// audio.cpp - PS Vita audio backend.
//
// Same software mixer as src/platform/linux/audio.cpp, same DirectSound
// contract (banks 1..80, slot != 0 loops, millibel volume, 1 = playing), but
// fed by a thread blocking on sceAudioOutOutput instead of an SDL callback.
//
// The BGM port is used rather than MAIN because MAIN is fixed at 48 kHz while
// every PC bank is 22050 Hz; BGM takes 22050 and 44100 natively, so nothing
// has to be resampled twice.
#include "Globals.h"
#include "platform/platform.h"
#include "marni/MarniSound.h"
#include "system/AssetPath.h"
#include "system/AudioFile.h"
#include "vita.h"

#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_outRate = 22050;
#define OUT_RATE     s_outRate
#define MAX_BANKS    81
#define GRAIN        512       // frames per sceAudioOutOutput, ~23 ms at 22050

namespace {

struct Bank {
    bool     active;
    bool     playing;
    bool     loop;
    BYTE*    wavData;
    BYTE*    pcm;
    DWORD    pcmSize;
    int      sampleRate;
    int      channels;
    int      bitsPerSample;
    int      vol;
    int      pan;
    int      slot;
    int      status;
    double   pos;
    size_t   loopBegin;
    size_t   loopEnd;
    bool     ps1;
    char     name[64];
};

Bank s_banks[MAX_BANKS];

SceKernelLwMutexWork s_lock;
bool   s_lockReady = false;
int    s_port      = -1;
SceUID s_thread    = -1;
volatile int s_running   = 0;
volatile int s_suspended = 0;

int16_t s_out[2][GRAIN * 2] __attribute__((aligned(64)));
int32_t s_acc[GRAIN * 2];

// Movie audio, from either decoder in video.cpp. It arrives in decoder-sized
// chunks at whatever rate the file was encoded with, so it goes through a ring
// and is resampled to OUT_RATE here. Head and tail are free-running frame
// counters; only their difference matters, and FEED_FRAMES being a power of
// two keeps the index a mask. The tail doubles as the FMV clock (frames played
// since the last reset); an aligned 32-bit read of it is atomic on the A9.
//
// 65536 frames is ~3 s at 22050 Hz. An AVI can interleave its audio well ahead
// of the picture, and anything the ring cannot take waits in video.cpp.
#define FEED_FRAMES 65536
int16_t  s_feed[FEED_FRAMES * 2];
unsigned s_feedHead = 0;
volatile unsigned s_feedTail = 0;
int      s_feedRate = 0;
float    s_feedFrac = 0.0f;

void Lock(void)
{
    if (s_lockReady) sceKernelLockLwMutex(&s_lock, 1, NULL);
}

void Unlock(void)
{
    if (s_lockReady) sceKernelUnlockLwMutex(&s_lock, 1);
}

float MillibelsToAmplitude(int mB)
{
    if (mB <= -10000) return 0.0f;
    if (mB >= 0) return 1.0f;
    return powf(10.0f, (float)mB / 2000.0f);
}

int32_t ReadSample(const Bank& b, size_t frame, int channel)
{
    size_t idx = frame * (size_t)b.channels + (size_t)channel;
    if (b.bitsPerSample == 8) {
        return ((int32_t)b.pcm[idx] - 128) << 8;
    }
    return (int32_t)(int16_t)((unsigned)b.pcm[idx * 2] | ((unsigned)b.pcm[idx * 2 + 1] << 8));
}

void Mix(int16_t* dst, int frames)
{
    int32_t* out = s_acc;
    memset(out, 0, (size_t)frames * 2 * sizeof(int32_t));

    if (s_suspended) {
        memset(dst, 0, (size_t)frames * 2 * sizeof(int16_t));
        return;
    }

    if (s_feedRate > 0) {
        const float step = (float)s_feedRate / (float)OUT_RATE;
        for (int f = 0; f < frames; ++f) {
            if (s_feedHead - s_feedTail < 2) break;
            const int16_t* a = &s_feed[(s_feedTail & (FEED_FRAMES - 1)) * 2];
            const int16_t* b = &s_feed[((s_feedTail + 1) & (FEED_FRAMES - 1)) * 2];
            out[f * 2 + 0] += a[0] + (int32_t)((float)(b[0] - a[0]) * s_feedFrac);
            out[f * 2 + 1] += a[1] + (int32_t)((float)(b[1] - a[1]) * s_feedFrac);
            s_feedFrac += step;
            while (s_feedFrac >= 1.0f && s_feedTail != s_feedHead) {
                s_feedFrac -= 1.0f;
                s_feedTail++;
            }
        }
    }

    for (int b = 1; b < MAX_BANKS; ++b) {
        Bank& bank = s_banks[b];
        if (!bank.active || !bank.playing || bank.pcm == NULL) continue;

        const int bytesPerSample = bank.bitsPerSample / 8;
        const size_t totalFrames = (bytesPerSample > 0 && bank.channels > 0)
            ? bank.pcmSize / ((size_t)bytesPerSample * (size_t)bank.channels)
            : 0;
        if (totalFrames == 0) { bank.playing = false; bank.status = 0; continue; }

        const double ratio = (double)bank.sampleRate / (double)OUT_RATE;
        const float amp = MillibelsToAmplitude(bank.vol);
        const float panL = (float)(10000 - bank.pan) / 20000.0f;
        const float panR = (float)(10000 + bank.pan) / 20000.0f;

        const size_t end = (bank.loop && bank.loopEnd != 0) ? bank.loopEnd : totalFrames;
        for (int f = 0; f < frames; ++f) {
            size_t i = (size_t)bank.pos;
            if (i >= end) {
                if (bank.loop) {
                    const size_t begin = bank.loopEnd != 0 ? bank.loopBegin : 0;
                    bank.pos -= (double)(end - begin);
                    i = (size_t)bank.pos;
                    if (i >= end) { bank.pos = (double)begin; i = begin; }
                }
                else { bank.playing = false; bank.status = 0; break; }
            }

            int32_t l, r;
            if (bank.ps1) {
                size_t j = i + 1;
                if (j >= end) j = bank.loop ? (bank.loopEnd != 0 ? bank.loopBegin : 0) : i;
                const float t = (float)(bank.pos - (double)i);
                const int ch1 = bank.channels == 1 ? 0 : 1;
                l = (int32_t)((1.0f - t) * ReadSample(bank, i, 0) + t * ReadSample(bank, j, 0));
                r = (int32_t)((1.0f - t) * ReadSample(bank, i, ch1) + t * ReadSample(bank, j, ch1));
            } else if (bank.channels == 1) {
                l = r = ReadSample(bank, i, 0);
            } else {
                l = ReadSample(bank, i, 0);
                r = ReadSample(bank, i, 1);
            }

            out[f * 2 + 0] += (int32_t)((float)l * amp * panL);
            out[f * 2 + 1] += (int32_t)((float)r * amp * panR);
            bank.pos += ratio;
        }
    }

    for (int i = 0; i < frames * 2; ++i) {
        int32_t v = out[i];
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        dst[i] = (int16_t)v;
    }
}

int AudioThread(SceSize, void*)
{
    int cur = 0;
    while (s_running) {
        Lock();
        Mix(s_out[cur], GRAIN);
        Unlock();
        // Blocks until the previous grain has been consumed, which is what
        // paces this loop - hence the double buffer.
        sceAudioOutOutput(s_port, s_out[cur]);
        cur ^= 1;
    }
    return sceKernelExitDeleteThread(0);
}

void StopBank(Bank& bank)
{
    bank.playing = false;
    bank.status = 0;
    bank.pos = 0.0;
}

void FreeBank(Bank& bank)
{
    StopBank(bank);
    free(bank.wavData);
    bank.wavData = NULL;
    bank.pcm = NULL;
    bank.pcmSize = 0;
    bank.active = false;
    bank.name[0] = '\0';
}

}  // namespace

// ---------------------------------------------------------------------------
// DirectSound
// ---------------------------------------------------------------------------

DirectSound::DirectSound(HWND hwnd)
{
    (void)hwnd;
    memset(this, 0, sizeof(*this));
    *(int*)((BYTE*)this + 0x10) = 1;
    m_bInitialized = 1;
}

void DirectSound::compact(void) {}

int DirectSound::GetStatus(int bank)
{
    if (bank <= 0 || bank >= MAX_BANKS) return 0;
    return s_banks[bank].status;
}

void DirectSound::Release(void)
{
    s_suspended = 1;
}

void DirectSound::Reload(void)
{
    s_suspended = 0;
}

void DirectSound::DestroySound(int bank)
{
    if (bank <= 0 || bank >= MAX_BANKS) return;
    Lock();
    FreeBank(s_banks[bank]);
    Unlock();
}

int DirectSound::NewDirectSoundBuffer(DWORD* /*wavBlk*/)
{
    return 1;
}

void DirectSound::StopSound(int bank)
{
    if (bank <= 0 || bank >= MAX_BANKS) return;
    Lock();
    StopBank(s_banks[bank]);
    Unlock();
}

void DirectSound::PlaySound(int bank, unsigned int slot)
{
    if (bank <= 0 || bank >= MAX_BANKS) return;
    Lock();
    Bank& b = s_banks[bank];
    if (b.active && b.pcm != NULL) {
        b.pos = 0.0;
        b.slot = (int)slot;
        b.loop = (slot != 0);
        b.playing = true;
        b.status = 1;
    }
    Unlock();
}

void DirectSound::SetVol(int bank, int vol)
{
    if (bank <= 0 || bank >= MAX_BANKS) return;
    if (vol > -1) vol = -1;
    if (vol < -9999) vol = -9999;
    Lock();
    s_banks[bank].vol = vol;
    Unlock();
}

void DirectSound::SetPan(int bank, int pan)
{
    if (bank <= 0 || bank >= MAX_BANKS) return;
    if (pan > 10000) pan = 10000;
    if (pan < -10000) pan = -10000;
    Lock();
    s_banks[bank].pan = pan;
    Unlock();
}

int DirectSound::GetVol(int bank)
{
    if (bank <= 0 || bank >= MAX_BANKS) return 0;
    Lock();
    int vol = s_banks[bank].vol;
    Unlock();
    return vol;
}

int DirectSound::CreateSound(const char* wavName)
{
    if (wavName == NULL) return 0;

    char resolved[260];
    const char* actualPath = ResolveAssetRoot(wavName, resolved, sizeof(resolved));

    char found[260];
    AudioFileData af;
    if (!AudioFile_Find(actualPath, found, sizeof(found)) || !AudioFile_Load(found, &af)) {
        static int s_failLogged = 0;
        if (s_failLogged < 4) {
            fprintf(stderr, "[AUDIO] could not load bank: %s\n", actualPath);
            if (++s_failLogged == 4) {
                fprintf(stderr, "[AUDIO] (further bank load failures suppressed)\n");
            }
        }
        return 0;
    }
    BYTE* wavData = af.buffer;

    Lock();

    int bank = 0;
    for (int i = 1; i < MAX_BANKS; ++i) {
        if (!s_banks[i].active) { bank = i; break; }
    }
    if (bank == 0) {
        Unlock();
        free(wavData);
        return 0;
    }

    Bank& b = s_banks[bank];
    FreeBank(b);
    b.wavData       = wavData;
    b.pcm           = af.pcm;
    b.pcmSize       = af.pcmSize;
    b.sampleRate    = af.sampleRate;
    b.channels      = af.channels;
    b.bitsPerSample = af.bitsPerSample;
    b.vol           = 0;
    b.pan           = 400;
    b.slot          = 0;
    b.status        = 0;
    b.playing       = false;
    b.loop          = false;
    b.pos           = 0.0;
    b.active        = true;
    b.ps1           = af.ps1 != 0;
    b.loopBegin     = af.loopBegin;
    b.loopEnd       = af.loopEnd > af.loopBegin ? af.loopEnd : 0;

    const char* slash = strrchr(wavName, '/');
    const char* backslash = strrchr(wavName, '\\');
    if (backslash != NULL && (slash == NULL || backslash > slash)) slash = backslash;
    strncpy(b.name, slash ? slash + 1 : wavName, sizeof(b.name) - 1);
    b.name[sizeof(b.name) - 1] = '\0';

    Unlock();
    return bank;
}

void DirectSound::ErrorRoutine(int code)
{
    (void)code;
}

// ---------------------------------------------------------------------------
// Engine lifecycle
// ---------------------------------------------------------------------------

void InitializeSoundSystem(void)
{
    DirectSound* mgr = new DirectSound(NULL);
    g_pDirectSound = mgr;
    g_SoundManager = (void*)mgr;

    if (!s_lockReady) {
        if (sceKernelCreateLwMutex(&s_lock, "re1_audio", SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0, NULL) < 0) {
            fprintf(stderr, "[AUDIO] could not create mixer lock\n");
            return;
        }
        s_lockReady = true;
    }

    s_outRate = AudioFile_TreeHasPs1Audio() ? 44100 : 22050;

    s_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, GRAIN, OUT_RATE,
                                 SCE_AUDIO_OUT_MODE_STEREO);
    if (s_port < 0) {
        fprintf(stderr, "[AUDIO] sceAudioOutOpenPort failed: 0x%08X\n", (unsigned)s_port);
        s_port = -1;
        return;
    }

    int vol[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
    sceAudioOutSetVolume(s_port,
                         (SceAudioOutChannelFlag)(SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH),
                         vol);

    // Highest user priority: a late grain is an audible click, a late game
    // frame is not.
    s_running = 1;
    s_thread = sceKernelCreateThread("re1_audio", AudioThread, 64, 0x4000, 0, 0, NULL);
    if (s_thread < 0) {
        fprintf(stderr, "[AUDIO] could not create mixer thread\n");
        s_running = 0;
        sceAudioOutReleasePort(s_port);
        s_port = -1;
        return;
    }
    sceKernelStartThread(s_thread, 0, NULL);
}

void CleanupSoundManagerResources(void)
{
    if (s_thread >= 0) {
        s_running = 0;
        sceKernelWaitThreadEnd(s_thread, NULL, NULL);
        s_thread = -1;
    }
    if (s_port >= 0) {
        sceAudioOutReleasePort(s_port);
        s_port = -1;
    }
    for (int i = 1; i < MAX_BANKS; ++i) FreeBank(s_banks[i]);
}

// ---------------------------------------------------------------------------
// FMV audio feed
// ---------------------------------------------------------------------------

// Queue up to `frames` of interleaved S16 PCM. Returns how many fit; the
// caller keeps the rest and offers it again once the mixer has drained some.
int plat_audio_feed_write(const short* pcm, int frames, int channels, int rate)
{
    if (pcm == NULL || frames <= 0 || channels <= 0) return 0;

    Lock();
    const unsigned room = FEED_FRAMES - (s_feedHead - s_feedTail);
    const int n = (unsigned)frames < room ? frames : (int)room;
    if (n > 0 && s_feedRate != rate) {
        s_feedRate = rate;
        s_feedFrac = 0.0f;
    }
    for (int i = 0; i < n; ++i) {
        int16_t* d = &s_feed[(s_feedHead & (FEED_FRAMES - 1)) * 2];
        d[0] = pcm[i * channels];
        d[1] = pcm[i * channels + (channels > 1 ? 1 : 0)];
        s_feedHead++;
    }
    Unlock();
    return n;
}

long plat_audio_feed_played(void)
{
    return (long)s_feedTail;
}

long plat_audio_feed_queued(void)
{
    return (long)(s_feedHead - s_feedTail);
}

void plat_audio_feed_reset(void)
{
    Lock();
    s_feedHead = s_feedTail = 0;
    s_feedFrac = 0.0f;
    s_feedRate = 0;
    Unlock();
}
