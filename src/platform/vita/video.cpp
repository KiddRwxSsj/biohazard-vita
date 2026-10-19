// video.cpp - PS Vita FMV backend.
//
// Two decoders behind the one plat_video_* surface:
//   - .mp4 (the asset migrator's H.264/AAC output, the DC .STR conversions and
//     the PS1 credit movies) goes to SceAvPlayer, i.e. the hardware decoder.
//   - .avi (the original Cinepak movies) goes through ffmpeg exactly like
//     src/platform/linux/video.cpp. The vitasdk ffmpeg build has the AVI
//     demuxer and Cinepak but no mov/H.264, so it cannot take the MP4s itself.
// If AvPlayer turns an .mp4 down, its .avi sibling is tried through ffmpeg.
//
// Both write the same RGBA buffer and MarniDX texture, so Present and the PS1
// credits overlay do not care which one decoded the frame. The notes below
// are about the ffmpeg path.
//
// Clock: the movie's audio is decoded as it is demuxed and fed to audio.cpp's
// mixer, whose consumption of it advances with the sound hardware. That count
// is the video clock, so the picture cannot drift from the sound. When a movie
// has no audio, or the audio falls behind, the wall clock stands in.
//
// The state machine's cut points are times; this backend turns them into
// frames with the stream's own rate, so a source-timed or PC-retimed movie
// still cuts at the same moment.
#include "../platform.h"

#include "../../Globals.h"
#include "../../marni/MarniDX.h"
#include "../../marni/MarniSystem.h"
#include "vita.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <psp2/avplayer.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/sysmodule.h>

#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

PlatVideoOverlayCallback s_overlayCallback = NULL;

// ---------------------------------------------------------------------------
// Start-up trace: ux0:data/RE1/fmv.log, one line per stage, stamped with the
// milliseconds since the movie was handed over. Opening a movie is the slow
// part on this hardware and this is the only way to see which step it is.
// ffmpeg warnings and AvPlayer events land in the same file, capped per movie
// so a decoder that complains every frame cannot fill the card.
// ---------------------------------------------------------------------------
FILE* s_trace      = NULL;
DWORD s_traceStart = 0;
int   s_traceLines = 0;
DWORD s_requestMs  = 0;   // plat_video_set_window_mode(TRUE), the state machine's first step

void Trace(const char* fmt, ...)
{
    if (s_trace == NULL || s_traceLines >= 200) return;
    ++s_traceLines;

    va_list ap;
    va_start(ap, fmt);
    fprintf(s_trace, "%7u ms  ", (unsigned)(plat_time_ms() - s_traceStart));
    vfprintf(s_trace, fmt, ap);
    va_end(ap);
    fputc('\n', s_trace);
    fflush(s_trace);
}

void FfLog(void*, int level, const char* fmt, va_list ap)
{
    if (level > AV_LOG_WARNING) return;
    char line[256];
    vsnprintf(line, sizeof(line), fmt, ap);
    size_t n = strlen(line);
    while (n > 0 && line[n - 1] == '\n') line[--n] = '\0';
    Trace("ffmpeg: %s", line);
}

void TraceBegin(const char* path)
{
    if (s_trace == NULL) {
        s_trace = fopen(VITA_DATA_DIR "/fmv.log", "a");
        av_log_set_callback(FfLog);
    }
    s_traceStart = plat_time_ms();
    s_traceLines = 0;
    Trace("---- open %s (requested %u ms ago)", path,
          s_requestMs ? (unsigned)(s_traceStart - s_requestMs) : 0u);
}

AVFormatContext* s_fmt     = NULL;
AVCodecContext*  s_vctx    = NULL;
AVCodecContext*  s_actx    = NULL;
SwrContext*      s_swr     = NULL;
int              s_vstream = -1;
int              s_astream = -1;
AVPacket*        s_pkt     = NULL;
AVFrame*         s_frame   = NULL;
AVFrame*         s_shown   = NULL;   // newest decoded picture, converted on demand
bool             s_shownFresh = false;
SwsContext*      s_sws     = NULL;

double s_fps    = 10.0;
int    s_width  = 0;
int    s_height = 0;

unsigned char* s_rgba      = NULL;   // last decoded frame, RGBA
int            s_rgbaPitch = 0;

MarniHandle s_tex  = MARNI_NULL_HANDLE;
int         s_texW = 0;
int         s_texH = 0;

int  s_frameIndex   = -1;     // index of the frame in s_rgba (absolute)
int  s_baseFrame    = 0;      // frame the current segment started at
int  s_playToFrame  = 0;      // 0 = play to the end
BOOL s_active       = FALSE;
BOOL s_endEvent     = FALSE;
BOOL s_eof          = FALSE;
BOOL s_flushed      = FALSE;   // the decoder's NULL-packet flush was sent
DWORD s_startTicks  = 0;      // wall clock, used only when the movie has no audio

AVFrame* s_aframe     = NULL;
BOOL     s_useAudio    = FALSE;
int      s_audioRate   = 0;      // source rate; the feed resamples
double   s_segStartSec = 0.0;    // audio before this is pre-roll from the seek
double   s_segEndSec   = 0.0;    // 0 = no cut point
short*   s_pend        = NULL;   // decoded PCM the feed ring had no room for yet
int      s_pendFrames  = 0;
int      s_pendCap     = 0;

// ---------------------------------------------------------------------------
// Teardown
// ---------------------------------------------------------------------------
void FreeAll(void)
{
    plat_audio_feed_reset();

    if (s_tex != MARNI_NULL_HANDLE) {
        MarniDX* dx = Marni_DX();
        if (dx != NULL) dx->DestroyTexture(s_tex);
        s_tex = MARNI_NULL_HANDLE;
    }
    s_texW = s_texH = 0;

    free(s_rgba);      s_rgba = NULL;      s_rgbaPitch = 0;
    free(s_pend);      s_pend = NULL;      s_pendFrames = s_pendCap = 0;
    s_useAudio = FALSE;
    s_audioRate = 0;

    if (s_swr)    { swr_free(&s_swr); }
    if (s_frame)  { av_frame_free(&s_frame); }
    if (s_shown)  { av_frame_free(&s_shown); }
    s_shownFresh = false;
    if (s_sws)    { sws_freeContext(s_sws); s_sws = NULL; }
    if (s_aframe) { av_frame_free(&s_aframe); }
    if (s_pkt)    { av_packet_free(&s_pkt); }
    if (s_vctx)   { avcodec_free_context(&s_vctx); }
    if (s_actx)   { avcodec_free_context(&s_actx); }
    if (s_fmt)    { avformat_close_input(&s_fmt); }

    s_vstream = -1;
    s_astream = -1;
    s_frameIndex = -1;
    s_baseFrame = 0;
    s_playToFrame = 0;
    s_eof = FALSE;
    s_flushed = FALSE;
    s_active = FALSE;
    s_endEvent = FALSE;
}

// ---------------------------------------------------------------------------
// Streaming audio
//
// Audio packets are decoded as the demuxer reaches them while it hunts for
// video, and pushed into audio.cpp's feed ring. The Linux backend decodes the
// whole track up front instead; that is one full pass over the file before the
// first frame, which on the memory card meant a 30-60 s freeze per movie.
//
// The resampler only converts the sample format to interleaved S16 stereo and
// keeps the source rate; the feed resamples to the mixer rate itself.
// ---------------------------------------------------------------------------
BOOL OpenAudio(void)
{
    if (s_actx == NULL || s_astream < 0) return FALSE;

    s_audioRate = s_actx->sample_rate;
    AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_STEREO;
    if (s_audioRate <= 0 ||
        swr_alloc_set_opts2(&s_swr, &outLayout, AV_SAMPLE_FMT_S16, s_audioRate,
                            &s_actx->ch_layout, s_actx->sample_fmt,
                            s_actx->sample_rate, 0, NULL) < 0 ||
        swr_init(s_swr) < 0) {
        swr_free(&s_swr);
        return FALSE;
    }

    s_aframe = av_frame_alloc();
    return s_aframe != NULL;
}

void FlushPendingAudio(void)
{
    if (s_pendFrames <= 0) return;
    const int n = plat_audio_feed_write(s_pend, s_pendFrames, 2, s_audioRate);
    if (n <= 0) return;
    s_pendFrames -= n;
    memmove(s_pend, s_pend + n * 2, (size_t)s_pendFrames * 2 * sizeof(short));
}

void QueueAudio(const short* pcm, int frames)
{
    if (frames <= 0) return;
    if (s_pendFrames + frames > s_pendCap) {
        const int want = (s_pendFrames + frames) * 2;
        short* grown = (short*)realloc(s_pend, (size_t)want * 2 * sizeof(short));
        if (grown == NULL) return;
        s_pend = grown;
        s_pendCap = want;
    }
    memcpy(s_pend + s_pendFrames * 2, pcm, (size_t)frames * 2 * sizeof(short));
    s_pendFrames += frames;
    FlushPendingAudio();
}

// Decode one audio packet and queue the part inside the current segment.
// After a seek the demuxer restarts at the keyframe before the target, so the
// head of what comes out belongs to the previous segment and is trimmed off;
// past a cut point nothing is queued at all.
void DecodeAudioPacket(AVPacket* pkt)
{
    if (s_swr == NULL || s_aframe == NULL) return;
    if (avcodec_send_packet(s_actx, pkt) < 0) return;

    static short* s_conv = NULL;
    static int s_convCap = 0;

    while (avcodec_receive_frame(s_actx, s_aframe) == 0) {
        const int in = s_aframe->nb_samples;
        if (in + 64 > s_convCap) {
            short* grown = (short*)realloc(s_conv, (size_t)(in + 64) * 2 * sizeof(short));
            if (grown == NULL) { av_frame_unref(s_aframe); return; }
            s_conv = grown;
            s_convCap = in + 64;
        }

        uint8_t* out[1] = { (uint8_t*)s_conv };
        int got = swr_convert(s_swr, out, s_convCap,
                              (const uint8_t**)s_aframe->extended_data, in);
        int skip = 0;

        const AVStream* st = s_fmt->streams[s_astream];
        if (got > 0 && s_aframe->pts != AV_NOPTS_VALUE && st->time_base.den > 0) {
            const double t0 = (double)s_aframe->pts * av_q2d(st->time_base);
            const double t1 = t0 + (double)got / (double)s_audioRate;
            if (t1 <= s_segStartSec || (s_segEndSec > 0.0 && t0 >= s_segEndSec)) {
                got = 0;
            } else {
                if (t0 < s_segStartSec) {
                    skip = (int)((s_segStartSec - t0) * s_audioRate);
                }
                if (s_segEndSec > 0.0 && t1 > s_segEndSec) {
                    got -= (int)((t1 - s_segEndSec) * s_audioRate);
                }
            }
        }

        if (got > skip) QueueAudio(s_conv + skip * 2, got - skip);
        av_frame_unref(s_aframe);
    }
}

// ---------------------------------------------------------------------------
// ConvertAndStore - pack the decoder's frame into the RGBA buffer.
//
// Cinepak (the game's own movies) decodes to packed RGB, a per-pixel shuffle;
// YUV420P (MPEG-4 / H.264 mod movies) goes through swscale's NEON path.
// Anything unexpected is reported once and skipped.
// ---------------------------------------------------------------------------
BOOL ConvertAndStore(void)
{
    AVFrame* frame = s_shown;
    if (s_width <= 0 || s_height <= 0) return FALSE;

    if (s_rgba == NULL) {
        s_rgbaPitch = s_width * 4;
        s_rgba = (unsigned char*)malloc((size_t)s_rgbaPitch * s_height);
        if (s_rgba == NULL) return FALSE;
    }

    const int srcStride = frame->linesize[0];
    const unsigned char* src = frame->data[0];
    unsigned char* dst = s_rgba;

    switch (frame->format) {
    case AV_PIX_FMT_RGB24:
        for (int y = 0; y < s_height; ++y) {
            const unsigned char* s = src + (size_t)y * srcStride;
            unsigned char* d = dst + (size_t)y * s_rgbaPitch;
            for (int x = 0; x < s_width; ++x) {
                d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255;
                s += 3; d += 4;
            }
        }
        return TRUE;
    case AV_PIX_FMT_BGR24:
        for (int y = 0; y < s_height; ++y) {
            const unsigned char* s = src + (size_t)y * srcStride;
            unsigned char* d = dst + (size_t)y * s_rgbaPitch;
            for (int x = 0; x < s_width; ++x) {
                d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = 255;
                s += 3; d += 4;
            }
        }
        return TRUE;
    case AV_PIX_FMT_RGBA:
        for (int y = 0; y < s_height; ++y) {
            memcpy(dst + (size_t)y * s_rgbaPitch, src + (size_t)y * srcStride,
                   (size_t)s_width * 4);
        }
        return TRUE;
    case AV_PIX_FMT_BGRA:
        for (int y = 0; y < s_height; ++y) {
            const unsigned char* s = src + (size_t)y * srcStride;
            unsigned char* d = dst + (size_t)y * s_rgbaPitch;
            for (int x = 0; x < s_width; ++x) {
                d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3];
                s += 4; d += 4;
            }
        }
        return TRUE;
    case AV_PIX_FMT_YUV420P:
        // swscale has a NEON path for exactly this (even height, width a
        // multiple of 16, no accurate rounding); the C loop below is ~3x slower.
        if ((s_width & 15) == 0 && (s_height & 1) == 0) {
            s_sws = sws_getCachedContext(s_sws, s_width, s_height, AV_PIX_FMT_YUV420P,
                                         s_width, s_height, AV_PIX_FMT_RGBA,
                                         SWS_POINT, NULL, NULL, NULL);
            if (s_sws != NULL) {
                int dstStride[1] = { s_rgbaPitch };
                uint8_t* dstPlane[1] = { dst };
                sws_scale(s_sws, frame->data, frame->linesize, 0, s_height, dstPlane, dstStride);
                return TRUE;
            }
        }
        {
            const unsigned char* yp = frame->data[0];
            const unsigned char* up = frame->data[1];
            const unsigned char* vp = frame->data[2];
            const int yStride = frame->linesize[0];
            const int uStride = frame->linesize[1];
            const int vStride = frame->linesize[2];
            for (int y = 0; y < s_height; ++y) {
                const unsigned char* yRow = yp + (size_t)y * yStride;
                const unsigned char* uRow = up + (size_t)(y / 2) * uStride;
                const unsigned char* vRow = vp + (size_t)(y / 2) * vStride;
                unsigned char* d = dst + (size_t)y * s_rgbaPitch;
                for (int x = 0; x < s_width; ++x) {
                    const int yy = yRow[x] - 16;
                    const int uu = uRow[x / 2] - 128;
                    const int vv = vRow[x / 2] - 128;
                    const int r = (298 * yy + 409 * vv + 128) >> 8;
                    const int g = (298 * yy - 100 * uu - 208 * vv + 128) >> 8;
                    const int b = (298 * yy + 516 * uu + 128) >> 8;
                    d[0] = (unsigned char)(r < 0 ? 0 : r > 255 ? 255 : r);
                    d[1] = (unsigned char)(g < 0 ? 0 : g > 255 ? 255 : g);
                    d[2] = (unsigned char)(b < 0 ? 0 : b > 255 ? 255 : b);
                    d[3] = 255;
                    d += 4;
                }
            }
        }
        return TRUE;
    default:
        Trace("unsupported pixel format %d", frame->format);
        return FALSE;
    }
}

// ---------------------------------------------------------------------------
// DecodeNextVideoFrame - advance the video decoder by one frame.
//
// The send/receive handshake has to be drained properly: a decoder whose input
// queue is full returns EAGAIN from send_packet and expects a receive first,
// and once the file ends the NULL-packet flush must happen exactly once.
// Getting either wrong spins forever - which is exactly what an early version
// of this did, roughly once in four runs.
// ---------------------------------------------------------------------------
BOOL DecodeNextVideoFrame(void)
{
    if (s_eof) return FALSE;

    for (;;) {
        int ret = avcodec_receive_frame(s_vctx, s_frame);
        if (ret == 0) {
            // Track the frame number from the timestamp. Counting decodes
            // instead only works from the start of the file: after a seek the
            // decoder starts at the preceding keyframe, so a count would land
            // `frame` pictures past the target - which is what made the
            // prologue cut run off the end of the movie and go black while the
            // audio, seeked by sample count, carried on correctly.
            AVStream* st = s_fmt->streams[s_vstream];
            if (s_frame->pts != AV_NOPTS_VALUE && st->time_base.den > 0) {
                const double sec = (double)s_frame->pts * av_q2d(st->time_base);
                s_frameIndex = (int)(sec * s_fps + 0.5);
            } else {
                s_frameIndex++;
            }
            // Converted only if it is the one on screen (ShowLatest): when
            // playback runs behind, the frames decoded to catch up are not.
            av_frame_unref(s_shown);
            av_frame_move_ref(s_shown, s_frame);
            s_shownFresh = true;
            return TRUE;
        }
        if (ret == AVERROR_EOF) { s_eof = TRUE; return FALSE; }
        if (ret != AVERROR(EAGAIN)) { s_eof = TRUE; return FALSE; }

        // The decoder wants more input. If the file is exhausted and the flush
        // packet has already gone in, there is nothing left to produce.
        if (s_flushed) { s_eof = TRUE; return FALSE; }

        int readRet = av_read_frame(s_fmt, s_pkt);
        while (readRet >= 0 && s_pkt->stream_index != s_vstream) {
            if (s_useAudio && s_pkt->stream_index == s_astream) {
                DecodeAudioPacket(s_pkt);
            }
            av_packet_unref(s_pkt);
            readRet = av_read_frame(s_fmt, s_pkt);
        }
        if (readRet < 0) {
            s_flushed = TRUE;
            avcodec_send_packet(s_vctx, NULL);
            continue;
        }

        ret = avcodec_send_packet(s_vctx, s_pkt);
        av_packet_unref(s_pkt);
        if (ret < 0 && ret != AVERROR(EAGAIN)) { s_eof = TRUE; return FALSE; }
        // EAGAIN means "receive first" - loop back and do exactly that.
    }
}

// ---------------------------------------------------------------------------
// SeekTo - position the video decoder (and the audio cursor) at `frame`.
// ---------------------------------------------------------------------------
BOOL SeekTo(int frame)
{
    if (frame < 0) frame = 0;

    s_segStartSec = (double)frame / s_fps;
    s_segEndSec = (s_playToFrame > 0) ? (double)s_playToFrame / s_fps : 0.0;
    plat_audio_feed_reset();
    s_pendFrames = 0;

    const int64_t ts = (int64_t)(s_segStartSec * (double)AV_TIME_BASE);
    av_seek_frame(s_fmt, -1, ts, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(s_vctx);
    if (s_actx != NULL) avcodec_flush_buffers(s_actx);
    s_eof = FALSE;
    s_flushed = FALSE;
    s_frameIndex = -1;

    // Decode forward to the requested frame so s_rgba holds it. The decoder
    // restarts at the keyframe before the target, so this walks until the
    // timestamp-derived index reaches it (frame 0 needs one decode).
    while (s_frameIndex < frame && !s_eof) {
        if (!DecodeNextVideoFrame()) break;
    }

    // The clock restarts with the segment, but frame numbers stay absolute:
    // a resumed segment begins at `frame`, not at 0.
    s_baseFrame = frame;
    s_startTicks = plat_time_ms();
    return TRUE;
}

// ---------------------------------------------------------------------------
// Present - upload the current frame and draw it over the whole back buffer.
// ---------------------------------------------------------------------------
void Present(int frameMs)
{
    if (s_shownFresh) {
        s_shownFresh = false;
        ConvertAndStore();
    }
    if (s_rgba == NULL || s_width <= 0 || s_height <= 0) return;

    MarniDX* dx = Marni_DX();
    if (dx == NULL) return;

    if (s_tex == MARNI_NULL_HANDLE || s_texW != s_width || s_texH != s_height) {
        if (s_tex != MARNI_NULL_HANDLE) dx->DestroyTexture(s_tex);
        int ow = 0, oh = 0;
        s_tex = dx->CreateTexture(s_width, s_height, 32, s_rgba, &ow, &oh);
        s_texW = s_width;
        s_texH = s_height;
        if (s_tex == MARNI_NULL_HANDLE) return;
    } else {
        dx->UpdateTexturePixels(s_tex, s_rgba, s_width, s_height, 32);
    }

    DWORD bw = 0, bh = 0;
    dx->GetBackBufferSize(&bw, &bh);
    if (bw == 0 || bh == 0) return;

    dx->Clear(0.0f, 0.0f, 0.0f, 1.0f);
    if (s_overlayCallback == NULL || !s_overlayCallback(s_tex, frameMs)) {
        dx->DrawSprite(0.0f, 0.0f, (float)bw, (float)bh,
                       0.0f, 0.0f, 1.0f, 1.0f,
                       0xFFFFFFFFu, s_tex, MARNI_SAMPLER_POINT, MARNI_BLEND_DISABLE);
    }
    dx->Present();
}

// .mp4 -> .avi when the sibling exists, for an MP4 AvPlayer turned down.
// Anything else goes to ffmpeg as-is and fails cleanly if it cannot be read.
const char* OpenPath(const char* path, char* buf, size_t size)
{
    const char* dot = strrchr(path, '.');
    if (dot == NULL || strcasecmp(dot, ".mp4") != 0) return path;

    const size_t stem = (size_t)(dot - path);
    if (stem + 5 > size) return path;
    memcpy(buf, path, stem);
    strcpy(buf + stem, ".avi");

    FILE* f = fopen(buf, "rb");
    if (f == NULL) return path;
    fclose(f);
    return buf;
}

int FfFrameMs(void)
{
    return s_frameIndex > 0 ? (int)((double)s_frameIndex * 1000.0 / s_fps + 0.5) : 0;
}

// TRUE when the container header already filled in what the decoders need:
// codec ids, the picture size and the PCM layout. An AVI's does.
BOOL HeaderHasStreamInfo(void)
{
    BOOL video = FALSE;
    for (unsigned i = 0; i < s_fmt->nb_streams; ++i) {
        const AVCodecParameters* par = s_fmt->streams[i]->codecpar;
        if (par->codec_type == AVMEDIA_TYPE_VIDEO) {
            if (par->codec_id == AV_CODEC_ID_NONE || par->width <= 0 || par->height <= 0) return FALSE;
            video = TRUE;
        } else if (par->codec_type == AVMEDIA_TYPE_AUDIO) {
            if (par->codec_id == AV_CODEC_ID_NONE || par->sample_rate <= 0 ||
                par->ch_layout.nb_channels <= 0) {
                return FALSE;
            }
        }
    }
    return video;
}

bool IsMp4(const char* path)
{
    const char* dot = strrchr(path, '.');
    return dot != NULL && strcasecmp(dot, ".mp4") == 0;
}

// ---------------------------------------------------------------------------
// SceAvPlayer
//
// AvPlayer paces itself: GetVideoData only hands out a picture once its
// timestamp is due on the player's own clock, and the audio comes off the
// same clock, so none of the ffmpeg path's frame/target bookkeeping applies.
// Audio is pulled on a thread (an AAC frame is 20-45 ms, the game ticks every
// 33) and pushed into audio.cpp's feed ring, which resamples it into the mix.
// ---------------------------------------------------------------------------

SceAvPlayerHandle s_av        = 0;
bool              s_avOpen    = false;
bool              s_avModule  = false;
SceUID            s_avThread  = -1;
volatile int      s_avRun     = 0;
volatile int      s_avPaused  = 0;
int               s_avPlayTo  = 0;
int               s_avFrameMs = 0;
int               s_avWaitTicks = 0;      // ticks spent before the first picture
volatile int      s_avEvent     = -1;     // last event id from the player thread
int               s_avEventSeen = -1;
volatile int      s_avWarning   = 0;      // data of the last warning event (0x20)
bool              s_avFailed    = false;  // stopped before the first picture
char              s_avPath[512];

unsigned char* s_nv12     = NULL;
size_t         s_nv12Size = 0;

void* AvAlloc(void*, uint32_t alignment, uint32_t size)
{
    return memalign(alignment, size);
}

void AvFree(void*, void* ptr)
{
    free(ptr);
}

// Picture buffers are written by the hardware decoder, so they come from CDRAM
// memblocks rather than the newlib heap. CDRAM is handed out in 256 KB units.
void* AvFrameAlloc(void*, uint32_t alignment, uint32_t size)
{
    if (alignment < 0x40000) alignment = 0x40000;
    size = (size + alignment - 1) & ~(alignment - 1);

    SceUID block = sceKernelAllocMemBlock("re1_fmv_frame", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
                                          size, NULL);
    if (block < 0) {
        Trace("CDRAM frame alloc of %u KB failed: 0x%08X", (unsigned)(size >> 10), (unsigned)block);
        return NULL;
    }

    void* base = NULL;
    sceKernelGetMemBlockBase(block, &base);
    return base;
}

void AvFrameFree(void*, void* ptr)
{
    SceUID block = sceKernelFindMemBlockByAddr(ptr, 0);
    if (block >= 0) sceKernelFreeMemBlock(block);
}

// Runs on AvPlayer's own thread, so it only records the id; AvTick writes it
// to the trace from the main thread.
void AvEvent(void*, int32_t eventId, int32_t, void* data)
{
    if (eventId == 0x20 && data != NULL) s_avWarning = *(const int32_t*)data;
    s_avEvent = eventId;
}

int AvAudioPump(SceSize, void*)
{
    SceAvPlayerFrameInfo info;
    while (s_avRun) {
        memset(&info, 0, sizeof(info));
        if (s_avPaused || !sceAvPlayerGetAudioData(s_av, &info) || info.pData == NULL) {
            sceKernelDelayThread(2000);
            continue;
        }

        const int channels = info.details.audio.channelCount > 0 ? info.details.audio.channelCount : 2;
        const int frames = (int)(info.details.audio.size / (sizeof(short) * channels));
        const int rate = (int)info.details.audio.sampleRate;

        // Ring full: hand over what fits and wait for the mixer to drain the
        // rest. A cut point (s_avPaused) drops the remainder, so nothing past
        // the cut is heard.
        const short* pcm = (const short*)info.pData;
        int done = 0;
        while (s_avRun && !s_avPaused && done < frames) {
            done += plat_audio_feed_write(pcm + done * channels, frames - done, channels, rate);
            if (done < frames) sceKernelDelayThread(2000);
        }
    }
    return sceKernelExitDeleteThread(0);
}

// NV12: full-res Y plane, then one interleaved U,V pair per 2x2 block. Same
// BT.601 limited-range integer math as the ffmpeg YUV420P case above.
//
// The decoder's buffer is CDRAM, which is slow to read a byte at a time from
// the CPU, so it is copied out in one go before the per-pixel pass.
bool AvConvertFrame(const SceAvPlayerFrameInfo& frame)
{
    const int w = (int)frame.details.video.width;
    const int h = (int)frame.details.video.height;
    if (w <= 0 || h <= 0 || frame.pData == NULL) return false;

    const size_t need = (size_t)w * h * 3 / 2;
    if (need > s_nv12Size) {
        unsigned char* grown = (unsigned char*)realloc(s_nv12, need);
        if (grown == NULL) return false;
        s_nv12 = grown;
        s_nv12Size = need;
    }
    memcpy(s_nv12, frame.pData, need);

    if (s_rgba == NULL || w != s_width || h != s_height) {
        free(s_rgba);
        s_width = w;
        s_height = h;
        s_rgbaPitch = w * 4;
        s_rgba = (unsigned char*)malloc((size_t)s_rgbaPitch * h);
        if (s_rgba == NULL) return false;
    }

    const unsigned char* yp = s_nv12;
    const unsigned char* uvp = s_nv12 + (size_t)w * h;
    for (int y = 0; y < h; ++y) {
        const unsigned char* yRow = yp + (size_t)y * w;
        const unsigned char* uvRow = uvp + (size_t)(y / 2) * w;
        unsigned char* d = s_rgba + (size_t)y * s_rgbaPitch;
        for (int x = 0; x < w; ++x) {
            // x & ~1 / x | 1 pick the U and V byte of this pixel's chroma pair.
            const int yy = yRow[x] - 16;
            const int uu = uvRow[x & ~1] - 128;
            const int vv = uvRow[x | 1] - 128;
            const int r = (298 * yy + 409 * vv + 128) >> 8;
            const int g = (298 * yy - 100 * uu - 208 * vv + 128) >> 8;
            const int b = (298 * yy + 516 * uu + 128) >> 8;
            d[0] = (unsigned char)(r < 0 ? 0 : r > 255 ? 255 : r);
            d[1] = (unsigned char)(g < 0 ? 0 : g > 255 ? 255 : g);
            d[2] = (unsigned char)(b < 0 ? 0 : b > 255 ? 255 : b);
            d[3] = 255;
            d += 4;
        }
    }
    return true;
}

void AvClose(void)
{
    if (!s_avOpen) return;

    s_avPaused = 1;
    if (s_avThread >= 0) {
        s_avRun = 0;
        sceKernelWaitThreadEnd(s_avThread, NULL, NULL);
        s_avThread = -1;
    }
    sceAvPlayerStop(s_av);
    sceAvPlayerClose(s_av);
    plat_audio_feed_reset();

    free(s_nv12);
    s_nv12 = NULL;
    s_nv12Size = 0;

    s_avOpen = false;
    s_av = 0;
}

BOOL AvOpen(const char* path, int playToMs)
{
    if (!s_avModule) {
        if (sceSysmoduleLoadModule(SCE_SYSMODULE_AVPLAYER) < 0) {
            Trace("could not load the AvPlayer module");
            return FALSE;
        }
        s_avModule = true;
    }

    SceAvPlayerInitData init;
    memset(&init, 0, sizeof(init));
    init.memoryReplacement.allocate          = AvAlloc;
    init.memoryReplacement.deallocate        = AvFree;
    init.memoryReplacement.allocateTexture   = AvFrameAlloc;
    init.memoryReplacement.deallocateTexture = AvFrameFree;
    init.basePriority               = 0xA0;
    init.numOutputVideoFrameBuffers = 2;
    init.autoStart                  = 1;
    init.eventReplacement.eventCallback = AvEvent;

    Trace("AvPlayer module ready");
    s_av = sceAvPlayerInit(&init);
    // The handle is a user-memory pointer (0x81..0x8F...), negative as an int.
    // Only 0 or a 0x80xxxxxx SCE error code means failure.
    if (s_av == 0 || ((unsigned)s_av >> 24) == 0x80) {
        Trace("sceAvPlayerInit failed: 0x%08X", (unsigned)s_av);
        s_av = 0;
        return FALSE;
    }
    Trace("sceAvPlayerInit done");
    if (sceAvPlayerAddSource(s_av, path) < 0) {
        Trace("AvPlayer could not open %s", path);
        sceAvPlayerClose(s_av);
        s_av = 0;
        return FALSE;
    }
    s_avOpen = true;

    plat_audio_feed_reset();
    s_avPaused = 0;
    s_avRun = 1;
    s_avThread = sceKernelCreateThread("re1_fmv_audio", AvAudioPump, 0x10000100, 0x4000, 0, 0, NULL);
    if (s_avThread >= 0) {
        sceKernelStartThread(s_avThread, 0, NULL);
    } else {
        s_avRun = 0;
    }

    s_avPlayTo  = playToMs;
    s_avFrameMs = 0;
    s_avWaitTicks = 0;
    s_avEvent = s_avEventSeen = -1;
    s_avWarning = 0;
    s_avFailed  = false;
    snprintf(s_avPath, sizeof(s_avPath), "%s", path);
    s_active    = TRUE;
    s_endEvent  = FALSE;

    Trace("playing %s through AvPlayer", path);
    return TRUE;
}

void AvPlayFrom(int fromMs)
{
    plat_audio_feed_reset();
    sceAvPlayerJumpToTime(s_av, (uint64_t)(fromMs > 0 ? fromMs : 0));
    sceAvPlayerResume(s_av);
    s_avPaused = 0;
    s_avPlayTo = 0;
    s_active   = TRUE;
    s_endEvent = FALSE;
}

void AvPause(void)
{
    s_avPaused = 1;
    sceAvPlayerPause(s_av);
    plat_audio_feed_reset();
}

void AvTick(void)
{
    SceAvPlayerFrameInfo frame;
    memset(&frame, 0, sizeof(frame));
    if (s_avEvent != s_avEventSeen) {
        s_avEventSeen = s_avEvent;
        Trace("AvPlayer event %d", s_avEventSeen);
    }

    if (sceAvPlayerGetVideoData(s_av, &frame) && AvConvertFrame(frame)) {
        if (s_avWaitTicks >= 0) {
            Trace("first AvPlayer picture %ux%u at %u ms stream time, after %d ticks",
                  (unsigned)frame.details.video.width, (unsigned)frame.details.video.height,
                  (unsigned)frame.timeStamp, s_avWaitTicks);
            s_avWaitTicks = -1;
        }
        s_avFrameMs = (int)frame.timeStamp;
    } else if (s_avWaitTicks >= 0) {
        ++s_avWaitTicks;
    }
    Present(s_avFrameMs);

    if (s_avPlayTo > 0 && (int)sceAvPlayerCurrentTime(s_av) >= s_avPlayTo) {
        AvPause();
        s_active = FALSE;
        s_endEvent = TRUE;
        return;
    }
    if (!sceAvPlayerIsActive(s_av)) {
        if (s_avWaitTicks >= 0) {
            // Stopped without a single picture: AvPlayer rejected the stream.
            Trace("AvPlayer stopped before the first picture (warning 0x%08X)",
                  (unsigned)s_avWarning);
            s_avFailed = true;
        }
        s_active = FALSE;
        s_endEvent = TRUE;
    }
}

}  // namespace

// ===========================================================================
// Backend entry points (see platform.h)
// ===========================================================================

void plat_video_set_overlay_callback(PlatVideoOverlayCallback callback)
{
    s_overlayCallback = callback;
}

BOOL plat_video_init(void)
{
    return TRUE;
}

static bool FitsAvPlayer(const char* path);
static bool s_avSkip = false;   // reopen through ffmpeg after an AvPlayer failure

BOOL plat_video_open_and_play(const char* path, int playToMs)
{
    AvClose();
    FreeAll();
    TraceBegin(path);

    if (IsMp4(path) && !s_avSkip && FitsAvPlayer(path) && AvOpen(path, playToMs)) {
        return TRUE;
    }

    char aviPath[512];
    path = OpenPath(path, aviPath, sizeof(aviPath));

    // ffmpeg reads "ux0:" as a URL scheme, and this build only has the file
    // protocol, so a bare device path fails with "protocol not found" and every
    // movie got skipped. Naming the protocol explicitly makes it strip "file:"
    // and hand the rest to open() untouched.
    char url[520];
    snprintf(url, sizeof(url), "file:%s", path);

    if (avformat_open_input(&s_fmt, url, NULL, NULL) < 0) {
        Trace("ffmpeg could not open %s", path);
        FreeAll();
        return FALSE;
    }
    Trace("avformat_open_input done (%s)", s_fmt->iformat ? s_fmt->iformat->name : "?");

    // avformat_find_stream_info took a flat ~36 s in this ffmpeg build on every
    // movie, the 5-second logo as much as the intro (fmv.log), so it is not
    // reading the file - and it has nothing to add for an AVI anyway: the
    // header carries the codecs, the picture size, the PCM layout and the frame
    // rate as the video time base. It only runs if the header left a gap.
    if (HeaderHasStreamInfo()) {
        Trace("stream info taken from the header");
    } else {
        s_fmt->probesize = 256 * 1024;
        s_fmt->max_analyze_duration = AV_TIME_BASE / 2;
        if (avformat_find_stream_info(s_fmt, NULL) < 0) {
            Trace("no stream info in %s", path);
            FreeAll();
            return FALSE;
        }
        Trace("avformat_find_stream_info done");
    }

    s_vstream = av_find_best_stream(s_fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    s_astream = av_find_best_stream(s_fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (s_vstream < 0) {
        Trace("no video stream in %s", path);
        FreeAll();
        return FALSE;
    }

    // Video decoder
    {
        AVStream* st = s_fmt->streams[s_vstream];
        const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
        if (codec == NULL) { FreeAll(); return FALSE; }
        s_vctx = avcodec_alloc_context3(codec);
        if (s_vctx == NULL) { FreeAll(); return FALSE; }
        if (avcodec_parameters_to_context(s_vctx, st->codecpar) < 0 ||
            avcodec_open2(s_vctx, codec, NULL) < 0) {
            FreeAll();
            return FALSE;
        }
        s_width  = s_vctx->width;
        s_height = s_vctx->height;

        s_fps    = 10.0;   // the PC movies' rate, if nothing below knows better
        if (st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0) {
            s_fps = av_q2d(st->avg_frame_rate);
        } else if (st->r_frame_rate.num > 0 && st->r_frame_rate.den > 0) {
            s_fps = av_q2d(st->r_frame_rate);
        } else if (st->time_base.num > 0 && st->time_base.den > 0) {
            // AVI video ticks once per frame (dwScale/dwRate), so the time
            // base is the frame period. That is all a header-only open gets.
            const double fps = 1.0 / av_q2d(st->time_base);
            if (fps >= 1.0 && fps <= 120.0) s_fps = fps;
        }
        if (s_fps <= 0.0) s_fps = 10.0;
    }

    // Audio decoder (optional)
    if (s_astream >= 0) {
        AVStream* st = s_fmt->streams[s_astream];
        const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
        if (codec != NULL) {
            s_actx = avcodec_alloc_context3(codec);
            if (s_actx == NULL ||
                avcodec_parameters_to_context(s_actx, st->codecpar) < 0 ||
                avcodec_open2(s_actx, codec, NULL) < 0) {
                avcodec_free_context(&s_actx);
                s_astream = -1;
            }
        } else {
            s_astream = -1;
        }
    }

    s_pkt   = av_packet_alloc();
    s_frame = av_frame_alloc();
    s_shown = av_frame_alloc();
    if (s_pkt == NULL || s_frame == NULL || s_shown == NULL) { FreeAll(); return FALSE; }

    s_useAudio = OpenAudio();
    Trace("decoders open: video %s, audio %s",
          s_vctx->codec ? s_vctx->codec->name : "?",
          s_useAudio ? s_actx->codec->name : "none");

    // The cut point arrives as a time; turn it into this movie's frames (the
    // stream rate is known now, and a converted movie's rate need not be the
    // originals' 10 fps).
    s_playToFrame = (playToMs > 0)
        ? (int)((double)playToMs / 1000.0 * s_fps + 0.5) : 0;
    s_active = TRUE;
    s_endEvent = FALSE;
    SeekTo(0);
    Present(FfFrameMs());

    Trace("playing %s (%dx%d, %.2f fps, audio %s)",
            path, s_width, s_height, s_fps, s_useAudio ? "on" : "off");
    return TRUE;
}

void plat_video_play_from(int fromMs)
{
    if (s_avOpen) {
        AvPlayFrom(fromMs);
        return;
    }
    if (s_fmt == NULL) return;
    const int fromFrame = (int)((double)fromMs / 1000.0 * s_fps + 0.5);
    s_playToFrame = 0;
    s_endEvent = FALSE;
    s_active = TRUE;
    SeekTo(fromFrame);
    Trace("play_from(%dms -> frame %d) index %d eof=%d",
            fromMs, fromFrame, s_frameIndex, (int)s_eof);
    Present(FfFrameMs());
}

void plat_video_stop(void)
{
    if (s_avOpen) AvPause();
    s_active = FALSE;
}

void plat_video_close(void)
{
    if (s_avOpen || s_fmt != NULL) Trace("close");
    AvClose();
    FreeAll();
}

BOOL plat_video_is_active(void)
{
    return s_active;
}

BOOL plat_video_take_end_event(void)
{
    if (!s_endEvent) return FALSE;
    s_endEvent = FALSE;
    return TRUE;
}

static void TickFfmpeg(void);

void plat_video_tick(void)
{
    if (!s_active) return;

    const DWORD t0 = plat_time_ms();
    if (s_avOpen) {
        AvTick();
        if (s_avFailed) {
            // Play it in software rather than skip the movie.
            char path[512];
            snprintf(path, sizeof(path), "%s", s_avPath);
            const int playTo = s_avPlayTo;
            Trace("falling back to ffmpeg");
            s_avSkip = true;
            plat_video_open_and_play(path, playTo);
            s_avSkip = false;
        }
    } else if (s_fmt != NULL) TickFfmpeg();

    const DWORD spent = plat_time_ms() - t0;
    if (spent > 100) Trace("slow tick: %u ms", (unsigned)spent);
}

static void TickFfmpeg(void)
{
    FlushPendingAudio();

    // Target frame: the mixer's consumption of the movie audio when it is in
    // step with real time, the wall clock otherwise. The audio is the better
    // clock, but it stalls if the demuxer has not reached the next audio chunk
    // yet, and the picture must not stall with it.
    const double wallSeconds = (double)(plat_time_ms() - s_startTicks) / 1000.0;
    double seconds = wallSeconds;
    if (s_useAudio) {
        const double audioSeconds = (double)plat_audio_feed_played() / (double)s_audioRate;
        if (audioSeconds >= wallSeconds - 0.5 && audioSeconds <= wallSeconds + 0.25) {
            seconds = audioSeconds;
        }
    }

    const int target = s_baseFrame + (int)(seconds * s_fps);

    // Decode forward to the frame due now (the decode itself advances the
    // index from the frame's timestamp).
    while (s_frameIndex < target && !s_eof) {
        if (!DecodeNextVideoFrame()) break;
    }
    Present(FfFrameMs());

    // End of the segment: the requested cut point, or the last frame once the
    // clock has moved past it.
    if (s_playToFrame > 0 && s_frameIndex >= s_playToFrame) {
        plat_audio_feed_reset();
        s_pendFrames = 0;
        s_active = FALSE;
        s_endEvent = TRUE;
        return;
    }

    // At the end of the file, let the last of the audio play out before the
    // state machine closes the movie and the feed with it - up to a second,
    // in case the mixer is suspended and never drains it.
    if (s_eof && target > s_frameIndex) {
        const BOOL drained = !s_useAudio ||
                             (s_pendFrames == 0 && plat_audio_feed_queued() <= 0);
        if (drained || target > s_frameIndex + (int)s_fps) {
            s_active = FALSE;
            s_endEvent = TRUE;
        }
    }
}

void plat_video_set_window_mode(BOOL forVideo)
{
    if (forVideo) s_requestMs = plat_time_ms();
}

// ---------------------------------------------------------------------------
// MP4 probe
//
// AvPlayer takes any MP4 it can parse and only finds out at decode time that
// the stream is beyond the hardware, then ends without a picture: a movie pack
// encoded as H.264 High 4:4:4 (the common "lossless-looking" export) made
// every cutscene vanish. The avcC box says which profile and chroma format the
// stream is, so look before handing it over. HEVC is out entirely.
// ---------------------------------------------------------------------------

static unsigned Be32(const unsigned char* p)
{
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3];
}

// 1 = H.264 the hardware takes, 0 = something it does not, -1 = no answer.
static int ProbeMp4(FILE* f, int* height)
{
    // Top-level boxes until moov; moov can sit at either end of the file.
    long pos = 0;
    unsigned char hdr[16];
    for (int guard = 0; guard < 64; ++guard) {
        if (fseek(f, pos, SEEK_SET) != 0 || fread(hdr, 1, 8, f) != 8) return -1;
        unsigned long long size = Be32(hdr);
        long hdrLen = 8;
        if (size == 1) {
            if (fread(hdr + 8, 1, 8, f) != 8) return -1;
            size = ((unsigned long long)Be32(hdr + 8) << 32) | Be32(hdr + 12);
            hdrLen = 16;
        }
        if (memcmp(hdr + 4, "moov", 4) == 0) {
            const unsigned long long body = size ? size - hdrLen : 0;
            if (body == 0 || body > 8u * 1024u * 1024u) return -1;
            unsigned char* moov = (unsigned char*)malloc((size_t)body);
            if (moov == NULL) return -1;
            int verdict = -1;
            if (fread(moov, 1, (size_t)body, f) == body) {
                for (size_t i = 0; i + 12 <= body; ++i) {
                    // avc1 sample entry: height 26 bytes after the fourcc.
                    if (height != NULL && memcmp(moov + i, "avc1", 4) == 0 && i + 32 <= body) {
                        *height = (moov[i + 30] << 8) | moov[i + 31];
                    }
                    if (memcmp(moov + i, "hvc1", 4) == 0 || memcmp(moov + i, "hev1", 4) == 0) {
                        verdict = 0;
                        break;
                    }
                    if (memcmp(moov + i, "avcC", 4) == 0 && moov[i + 4] == 1) {
                        const unsigned profile = moov[i + 5];
                        // Baseline, Main, Extended, High. High 10/4:2:2/4:4:4
                        // (110/122/244) and the rest need what the chip lacks.
                        verdict = (profile == 66 || profile == 77 || profile == 88 ||
                                   profile == 100) ? 1 : 0;
                        break;
                    }
                }
            }
            free(moov);
            return verdict;
        }
        if (size == 0) return -1;                   // box runs to the end
        pos += (long)size;
    }
    return -1;
}

// Any other container goes to ffmpeg, which in this build has Cinepak, MPEG-4
// part 2 and PCM but no H.264: TeamX's HD movies are H.264 in AVI. The AVI
// header names the codec, so ask the demuxer only - no stream probing.
static BOOL FfmpegCanDecode(const char* path)
{
    char url[300];
    snprintf(url, sizeof(url), "file:%s", path);
    AVFormatContext* fmt = NULL;
    if (avformat_open_input(&fmt, url, NULL, NULL) < 0) return FALSE;
    BOOL ok = TRUE;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        const AVCodecParameters* par = fmt->streams[i]->codecpar;
        // Only the light codecs decode in real time on the CPU. H.264 (with
        // tools/vita/build_ffmpeg.sh's FFmpeg) is there for the mod manager's
        // movie conversion, not for playback.
        if (par->codec_type == AVMEDIA_TYPE_VIDEO &&
            (avcodec_find_decoder(par->codec_id) == NULL ||
             (par->codec_id != AV_CODEC_ID_CINEPAK && par->codec_id != AV_CODEC_ID_MPEG4 &&
              par->codec_id != AV_CODEC_ID_H263 && par->codec_id != AV_CODEC_ID_SVQ1))) {
            ok = FALSE;
        }
    }
    avformat_close_input(&fmt);
    return ok;
}

BOOL plat_video_can_play(const char* path)
{
    if (path == NULL) return FALSE;
    if (!IsMp4(path)) return FfmpegCanDecode(path);
    FILE* f = fopen(path, "rb");
    if (f == NULL) return FALSE;
    const int verdict = ProbeMp4(f, NULL);
    fclose(f);
    return verdict != 0 ? TRUE : FALSE;
}

// SceAvPlayer stops without a picture on anything taller than 720 lines (the
// HD mod's 1280x960), so those skip it. The Vita's AVC decoder refuses them
// too (sceVideodecInitLibrary 0x80620802): OPTIMIZE scales them to 640x480.
static bool FitsAvPlayer(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (f == NULL) return true;
    int height = 0;
    ProbeMp4(f, &height);
    fclose(f);
    if (height > 720) Trace("%d lines: too tall for AvPlayer", height);
    return height <= 720;
}
