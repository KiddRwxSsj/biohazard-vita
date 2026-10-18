// hdpack.cpp - Classic REbirth HD texture packs on the Vita.
//
// TeamX's Resident Evil HD mod and the Seamless HD Project keep their images
// in hires/bgd (room backgrounds, 1280x960) and hires/mask (the cut-out sheet
// each camera draws over its background, 1024x1024 = the 256x256 texture page
// at 4x). Each file is named by a CRC the PC loader takes from its DirectDraw
// surface, which the Vita does not have. app0:hd_map.txt (vita/hd_map.txt,
// made by tools/vita/hd_map.py) says which game file each name stands for:
//
//     rc1000 DD17F52F         stage 1, room 00, camera 0 background
//     osp00001 C5157461       stage 1, room 00, camera 1 mask sheet
//
// A mod is used as an HD pack when it has a hires/ folder. Nothing here runs
// on the game's thread except handing the result to the GPU:
//
//   * Room load queues every camera's background and mask (port_hd_prefetch);
//     a background thread on the third core decodes them, scaled down to
//     [Vita] HdMaxHeight, into an LRU of ready pictures.
//   * A camera cut shows the original picture at once if its HD one is not
//     decoded yet, and port_hd_poll swaps the HD one in on the frame it is.
//   * [Vita] HdPreload: vita_hd_prepare decodes every picture of the active
//     packs once, before the game starts, into ux0:data/RE1/hdcache/<size>/
//     (zlib'd RGBA, already scaled). A cached picture is a read and an
//     inflate instead of a full-size WebP decode, and port_hd_poll then waits
//     for the picture on screen rather than showing the original first.
//   * A file that is not a sane WebP, or is bigger than kMaxSide in either
//     direction, is skipped rather than decoded - a pack made for a PC with
//     8K upscales would otherwise run the console out of memory. Everything
//     is decoded straight to the capped size, never at full size first.
#include "../platform.h"
#include "../../Globals.h"
#include "../../system/AssetPath.h"
#include "../../marni/MarniDX.h"
#include "../../marni/MarniSystem.h"
#include "vita.h"

#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <webp/decode.h>
#include <zlib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <set>
#include <string>
#include <unordered_map>
#include <vector>

void MarniVita_SetTextureSmooth(MarniHandle tex);

namespace {

const int    kMaxSide      = 4096;               // refuse anything larger than this
const size_t kMaxFileBytes = 16u * 1024u * 1024u;
const size_t kDecodedBudget = 32u * 1024u * 1024u;

SceKernelLwMutexWork s_lock;
bool   s_ready = false;          // there is at least one pack
SceUID s_wake = -1;
SceUID s_thread = -1;

struct Lock {
    Lock()  { sceKernelLockLwMutex(&s_lock, 1, NULL); }
    ~Lock() { sceKernelUnlockLwMutex(&s_lock, 1); }
};

std::unordered_map<std::string, std::string> s_map;     // "rc1000" -> "DD17F52F"
std::vector<std::string> s_roots;                        // ".../hires/", first wins

struct Job {
    std::string name;           // "rc1000" / "osp00001"
};

struct Picture {
    unsigned char* rgba;
    int w, h;
    unsigned lastUse;
};

std::vector<Job> s_queue;
std::unordered_map<std::string, Picture> s_done;
std::unordered_map<std::string, bool> s_failed;          // nothing usable for this name
size_t   s_doneBytes = 0;
unsigned s_clock = 0;

// What is on screen now and still waiting for its HD picture.
std::string s_wantBg;
std::string s_wantMask;
int s_wantMaskSlot = -1;

int MaxHeight(void)
{
    int h = g_vitaVideo.hdMaxHeight;
    if (h < 240) h = 240;
    if (h > 1088) h = 1088;
    return h;
}

bool IsMask(const std::string& name)
{
    return name.compare(0, 3, "osp") == 0;
}

std::string FindFile(const std::string& name)
{
    auto it = s_map.find(name);
    if (it == s_map.end()) return std::string();
    const char* kind = IsMask(name) ? "mask/" : "bgd/";
    for (const std::string& root : s_roots) {
        std::string path = root + kind + it->second + ".webp";
        if (plat_file_exists(path.c_str())) return path;
    }
    return std::string();
}

unsigned char* ReadFile(const std::string& path, size_t* size)
{
    SceUID fd = sceIoOpen(path.c_str(), SCE_O_RDONLY, 0);
    if (fd < 0) return NULL;
    const SceOff n = sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    unsigned char* data = NULL;
    if (n > 0 && (size_t)n <= kMaxFileBytes) {
        data = (unsigned char*)malloc((size_t)n);
        if (data != NULL && sceIoRead(fd, data, (SceSize)n) != (int)n) {
            free(data);
            data = NULL;
        }
    }
    sceIoClose(fd);
    *size = (size_t)n;
    return data;
}

// ---------------------------------------------------------------------------
// HdPreload cache: <kCacheRoot>/<MaxHeight>/<mod>_hires_bgd_<HASH>.rhd
// ---------------------------------------------------------------------------

const char kCacheRoot[] = "ux0:data/RE1/hdcache";

struct CacheHeader {
    char     magic[4];          // "RHD1"
    unsigned w, h;
    unsigned srcSize;           // the .webp it was made from, to notice a new pack
    unsigned maxH;
    unsigned zlen;              // zlib stream that follows
};

std::unordered_map<std::string, bool> s_cacheIndex;     // file names in CacheDir()

std::string CacheDir(void)
{
    char d[64];
    snprintf(d, sizeof(d), "%s/%d", kCacheRoot, MaxHeight());
    return d;
}

std::string CacheName(const std::string& src)
{
    const size_t at = src.find("/mods/");
    std::string r = at == std::string::npos ? src : src.substr(at + 6);
    const size_t dot = r.rfind('.');
    if (dot != std::string::npos) r.resize(dot);
    // Lower case: the card ignores case, the index below must too.
    for (char& c : r) {
        if (c == '/' || c == ':' || c == ' ') c = '_';
        else if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    }
    return r + ".rhd";
}

void ListCache(void)
{
    s_cacheIndex.clear();
    SceUID d = sceIoDopen(CacheDir().c_str());
    if (d < 0) return;
    SceIoDirent e;
    memset(&e, 0, sizeof(e));
    while (sceIoDread(d, &e) > 0) {
        if (!SCE_S_ISDIR(e.d_stat.st_mode)) {
            std::string n = e.d_name;
            for (char& c : n) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            s_cacheIndex[n] = true;
        }
        memset(&e, 0, sizeof(e));
    }
    sceIoDclose(d);
}

unsigned SourceSize(const std::string& path)
{
    SceIoStat st;
    if (sceIoGetstat(path.c_str(), &st) < 0) return 0;
    return (unsigned)st.st_size;
}

bool ReadCache(const std::string& src, Picture* out)
{
    const std::string name = CacheName(src);
    {
        Lock l;
        if (s_cacheIndex.count(name) == 0) return false;
    }
    size_t size = 0;
    unsigned char* data = ReadFile(CacheDir() + "/" + name, &size);
    if (data == NULL) return false;
    bool ok = false;
    CacheHeader hdr;
    if (size >= sizeof(hdr)) {
        memcpy(&hdr, data, sizeof(hdr));
        const size_t pixels = (size_t)hdr.w * hdr.h;
        if (memcmp(hdr.magic, "RHD1", 4) == 0 && hdr.maxH == (unsigned)MaxHeight() &&
            hdr.w > 0 && hdr.h > 0 && hdr.w <= kMaxSide && hdr.h <= kMaxSide &&
            sizeof(hdr) + hdr.zlen <= size && hdr.srcSize == SourceSize(src)) {
            unsigned char* rgba = (unsigned char*)WebPMalloc(pixels * 4);
            uLongf len = (uLongf)(pixels * 4);
            if (rgba != NULL && uncompress(rgba, &len, data + sizeof(hdr), hdr.zlen) == Z_OK &&
                len == pixels * 4) {
                out->rgba = rgba;
                out->w = (int)hdr.w;
                out->h = (int)hdr.h;
                ok = true;
            } else {
                WebPFree(rgba);
            }
        }
    }
    free(data);
    return ok;
}

// Returns the bytes written, 0 on failure.
size_t WriteCache(const std::string& src, const Picture& pic)
{
    const uLong raw = (uLong)pic.w * pic.h * 4;
    uLongf zlen = compressBound(raw);
    unsigned char* z = (unsigned char*)malloc(sizeof(CacheHeader) + zlen);
    if (z == NULL) return 0;
    size_t written = 0;
    if (compress2(z + sizeof(CacheHeader), &zlen, pic.rgba, raw, 1) == Z_OK) {
        CacheHeader hdr;
        memcpy(hdr.magic, "RHD1", 4);
        hdr.w = (unsigned)pic.w;
        hdr.h = (unsigned)pic.h;
        hdr.srcSize = SourceSize(src);
        hdr.maxH = (unsigned)MaxHeight();
        hdr.zlen = (unsigned)zlen;
        memcpy(z, &hdr, sizeof(hdr));
        const std::string name = CacheName(src);
        const std::string path = CacheDir() + "/" + name;
        const std::string tmp = path + ".part";
        SceUID fd = sceIoOpen(tmp.c_str(), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
        if (fd >= 0) {
            const size_t total = sizeof(hdr) + zlen;
            const bool full = sceIoWrite(fd, z, (SceSize)total) == (int)total;
            sceIoClose(fd);
            sceIoRemove(path.c_str());
            if (full && sceIoRename(tmp.c_str(), path.c_str()) >= 0) {
                Lock l;
                s_cacheIndex[name] = true;
                written = total;
            } else {
                sceIoRemove(tmp.c_str());
            }
        }
    }
    free(z);
    return written;
}

// Decode the WebP at `path` to at most MaxHeight() pixels high (backgrounds)
// or the same scale (masks, so a sheet matches the background it cuts out of).
bool DecodeWebp(const std::string& path, bool mask, Picture* out)
{
    size_t size = 0;
    unsigned char* data = ReadFile(path, &size);
    if (data == NULL) return false;

    bool ok = false;
    WebPDecoderConfig config;
    if (WebPInitDecoderConfig(&config) && WebPGetFeatures(data, size, &config.input) == VP8_STATUS_OK) {
        const int w = config.input.width, h = config.input.height;
        if (w > 0 && h > 0 && w <= kMaxSide && h <= kMaxSide) {
            const int cx = w, cy = h;
            // Backgrounds are 4x of 240 lines; masks use the scale their
            // background gets so the two stay in register.
            const int fullH = mask ? h * 240 / 256 : h;
            int ow = cx, oh = cy;
            if (fullH > MaxHeight()) {
                ow = (int)((long long)cx * MaxHeight() / fullH);
                oh = (int)((long long)cy * MaxHeight() / fullH);
                if (ow < 1) ow = 1;
                if (oh < 1) oh = 1;
                config.options.use_scaling = 1;
                config.options.scaled_width = ow;
                config.options.scaled_height = oh;
            }
            config.output.colorspace = MODE_RGBA;
            if (WebPDecode(data, size, &config) == VP8_STATUS_OK) {
                out->rgba = config.output.u.RGBA.rgba;      // ours now, freed with WebPFree
                out->w = config.output.width;
                out->h = config.output.height;
                ok = out->rgba != NULL;
                if (ok && !mask) {
                    // Some Seamless HD backgrounds carry an alpha channel; the
                    // background is always drawn opaque.
                    for (int i = 0; i < out->w * out->h; ++i) out->rgba[i * 4 + 3] = 0xFF;
                }
            } else {
                WebPFreeDecBuffer(&config.output);
            }
        }
    }
    free(data);
    return ok;
}

bool Decode(const Job& job, Picture* out)
{
    const std::string path = FindFile(job.name);
    if (path.empty()) return false;
    if (ReadCache(path, out)) return true;
    return DecodeWebp(path, IsMask(job.name), out);
}

void EvictFor(size_t need)
{
    while (s_doneBytes + need > kDecodedBudget && !s_done.empty()) {
        auto oldest = s_done.begin();
        for (auto it = s_done.begin(); it != s_done.end(); ++it) {
            if (it->first == s_wantBg || it->first == s_wantMask) continue;
            if (oldest->first == s_wantBg || oldest->first == s_wantMask ||
                it->second.lastUse < oldest->second.lastUse) {
                oldest = it;
            }
        }
        if (oldest->first == s_wantBg || oldest->first == s_wantMask) break;
        s_doneBytes -= (size_t)oldest->second.w * oldest->second.h * 4;
        WebPFree(oldest->second.rgba);
        s_done.erase(oldest);
    }
}

int DecodeThread(SceSize, void*)
{
    for (;;) {
        sceKernelWaitSema(s_wake, 1, NULL);
        for (;;) {
            Job job;
            {
                Lock l;
                if (s_queue.empty()) break;
                // What the screen is waiting for goes first.
                size_t pick = 0;
                for (size_t i = 0; i < s_queue.size(); ++i) {
                    if (s_queue[i].name == s_wantBg || s_queue[i].name == s_wantMask) { pick = i; break; }
                }
                job = s_queue[pick];
                s_queue.erase(s_queue.begin() + pick);
                if (s_done.count(job.name) != 0 || s_failed.count(job.name) != 0) continue;
            }
            Picture pic = { NULL, 0, 0, 0 };
            const bool ok = Decode(job, &pic);
            Lock l;
            if (!ok) {
                s_failed[job.name] = true;
                continue;
            }
            const size_t bytes = (size_t)pic.w * pic.h * 4;
            EvictFor(bytes);
            pic.lastUse = ++s_clock;
            s_done[job.name] = pic;
            s_doneBytes += bytes;
        }
    }
    return 0;
}

void Queue(const std::string& name)
{
    if (!s_ready || s_map.count(name) == 0) return;
    {
        Lock l;
        if (s_done.count(name) != 0 || s_failed.count(name) != 0) return;
        for (const Job& j : s_queue) if (j.name == name) return;
        s_queue.push_back(Job{ name });
    }
    sceKernelSignalSema(s_wake, 1);
}

void LoadMap(const char* path)
{
    FILE* f = fopen(path, "r");
    if (f == NULL) return;
    char line[128];
    while (fgets(line, sizeof(line), f) != NULL) {
        if (line[0] == '#') continue;
        char name[32], hash[32];
        if (sscanf(line, "%31s %31s", name, hash) != 2) continue;
        for (char* c = name; *c; ++c) if (*c >= 'A' && *c <= 'Z') *c = (char)(*c + 32);
        for (char* c = hash; *c; ++c) if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 32);
        s_map[name] = hash;
    }
    fclose(f);
}

std::string Lower(const char* s)
{
    std::string r(s);
    for (char& c : r) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    return r;
}

// A texture from a finished picture, or NULL. A mask sheet is the whole
// 256x256 page with the TIM in its top-left corner, so it is cut down to the
// TIM's share of it (pageW x pageH of 256) for a texture the size the
// game's UVs expect.
MarniHandle Upload(const std::string& name, int pageW = 256, int pageH = 256)
{
    Lock l;
    auto it = s_done.find(name);
    if (it == s_done.end()) return MARNI_NULL_HANDLE;
    it->second.lastUse = ++s_clock;
    const Picture& pic = it->second;
    MarniHandle tex = MARNI_NULL_HANDLE;
    if (pageW >= 256 && pageH >= 256) {
        MarniCreateTexture(pic.w, pic.h, 32, pic.rgba, &tex);
    } else {
        const int cw = pic.w * (pageW > 0 ? pageW : 256) / 256;
        const int ch = pic.h * (pageH > 0 ? pageH : 256) / 256;
        unsigned char* crop = (cw > 0 && ch > 0) ? (unsigned char*)malloc((size_t)cw * ch * 4) : NULL;
        if (crop == NULL) return MARNI_NULL_HANDLE;
        for (int y = 0; y < ch; ++y) {
            memcpy(crop + (size_t)y * cw * 4, pic.rgba + (size_t)y * pic.w * 4, (size_t)cw * 4);
        }
        MarniCreateTexture(cw, ch, 32, crop, &tex);
        free(crop);
    }
    if (tex != MARNI_NULL_HANDLE) MarniVita_SetTextureSmooth(tex);
    return tex;
}

}  // namespace

// ---------------------------------------------------------------------------
// vita.h
// ---------------------------------------------------------------------------

void vita_hd_init(void)
{
    if (!g_vitaVideo.hdTextures) return;

    // The roots, in [Mods] order. A DC session looks in a pack's bio1dc/
    // folder first: TeamX ships the Director's Cut camera angles there.
    const bool dc = strcmp(GetAssetModeName(), "") != 0;
    for (int i = 0; i < GetAssetModCount(); ++i) {
        const std::string mod = GetAssetModRoot(i);
        if (dc) {
            const std::string r = mod + "bio1dc/hires/";
            if (plat_file_exists((r + "bgd").c_str()) || plat_file_exists((r + "mask").c_str())) {
                s_roots.push_back(r);
            }
        }
        const std::string r = mod + "hires/";
        if (plat_file_exists((mod + "hires").c_str())) s_roots.push_back(r);
        // A pack can bring its own table for files the shipped one lacks.
        LoadMap((mod + "hd_map.txt").c_str());
    }
    if (s_roots.empty()) return;

    LoadMap("app0:hd_map.txt");
    if (s_map.empty()) return;
    ListCache();

    if (sceKernelCreateLwMutex(&s_lock, "re1_hd", SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0, NULL) < 0) return;
    s_wake = sceKernelCreateSema("re1_hd", 0, 0, 0x7FFFFFFF, NULL);
    // Third core, below the game: decoding never competes with a frame.
    s_thread = sceKernelCreateThread("re1_hd", DecodeThread, 0x10000110, 0x10000, 0,
                                     0x40000 /* SCE_KERNEL_CPU_MASK_USER_2 */, NULL);
    if (s_wake < 0 || s_thread < 0) return;
    sceKernelStartThread(s_thread, 0, NULL);
    s_ready = true;
}

BOOL vita_hd_active(void)
{
    return s_ready ? TRUE : FALSE;
}

// ---------------------------------------------------------------------------
// Globals.h (port_hd_*)
// ---------------------------------------------------------------------------

void port_hd_prefetch(const char* name)
{
    if (s_ready && name != NULL) Queue(Lower(name));
}

void port_hd_background(const char* name)
{
    if (!s_ready || name == NULL) return;
    const std::string key = Lower(name);
    {
        Lock l;
        s_wantBg = s_map.count(key) != 0 ? key : std::string();
    }
    if (s_wantBg.empty()) return;
    Queue(key);
}

void port_hd_mask(const char* name, int slot)
{
    if (!s_ready || name == NULL || slot < 0 || slot >= 256) return;
    const std::string key = Lower(name);
    {
        Lock l;
        s_wantMask = s_map.count(key) != 0 ? key : std::string();
        s_wantMaskSlot = slot;
    }
    if (s_wantMask.empty()) return;
    Queue(key);
}

// HdPreload: hold the frame until the picture on screen is ready (or known
// to be missing), so the original never shows first. Bounded, in case the
// card is very slow.
static void WaitFor(bool mask)
{
    for (int t = 0; t < 750; ++t) {             // 1.5 s at most
        {
            Lock l;
            const std::string& name = mask ? s_wantMask : s_wantBg;
            if (name.empty() || s_done.count(name) != 0 || s_failed.count(name) != 0) return;
        }
        sceKernelDelayThread(2000);
    }
}

void port_hd_poll(void)
{
    if (!s_ready) return;

    if (g_vitaVideo.hdPreload) {
        if (!s_wantBg.empty()) WaitFor(false);
        if (!s_wantMask.empty()) WaitFor(true);
    }

    if (!s_wantBg.empty() && g_displayImageSRV != MARNI_NULL_HANDLE) {
        MarniHandle tex = Upload(s_wantBg);
        if (tex != MARNI_NULL_HANDLE) {
            Marni_DX()->DestroyTexture(g_displayImageSRV);
            g_displayImageSRV = tex;
            Lock l;
            s_wantBg.clear();
        }
    }

    const int slot = s_wantMaskSlot;
    if (!s_wantMask.empty() && slot >= 0 && g_TexturePageSRV[slot] != MARNI_NULL_HANDLE) {
        MarniHandle tex = Upload(s_wantMask, g_TexturePageWidth[slot], g_TexturePageHeight[slot]);
        if (tex != MARNI_NULL_HANDLE) {
            // Width/height stay the TIM's: the cut-outs' UVs are divided by them.
            Marni_DX()->DestroyTexture(g_TexturePageSRV[slot]);
            g_TexturePageSRV[slot] = tex;
            Lock l;
            s_wantMask.clear();
        }
    }
}

// ---------------------------------------------------------------------------
// HdPreload: prepare the whole pack before play
// ---------------------------------------------------------------------------

namespace {

struct PrepJob {
    std::string path;
    bool mask;
};

std::vector<PrepJob> s_prep;
volatile int s_prepNext = 0;
volatile int s_prepDone = 0;
volatile int s_prepStop = 0;
volatile int s_prepRunning = 0;
volatile unsigned long long s_prepBytes = 0;

int PrepThread(SceSize, void*)
{
    for (;;) {
        if (s_prepStop) break;
        const int i = __atomic_fetch_add(&s_prepNext, 1, __ATOMIC_SEQ_CST);
        if (i >= (int)s_prep.size()) break;
        Picture pic = { NULL, 0, 0, 0 };
        if (DecodeWebp(s_prep[i].path, s_prep[i].mask, &pic)) {
            const size_t n = WriteCache(s_prep[i].path, pic);
            __atomic_fetch_add(&s_prepBytes, (unsigned long long)n, __ATOMIC_SEQ_CST);
            WebPFree(pic.rgba);
        }
        __atomic_fetch_add(&s_prepDone, 1, __ATOMIC_SEQ_CST);
    }
    __atomic_fetch_sub(&s_prepRunning, 1, __ATOMIC_SEQ_CST);
    return sceKernelExitDeleteThread(0);
}

// The cache of another HdMaxHeight is dead weight once the size changed.
void DropOtherSizes(void)
{
    SceUID d = sceIoDopen(kCacheRoot);
    if (d < 0) return;
    std::vector<std::string> stale;
    SceIoDirent e;
    memset(&e, 0, sizeof(e));
    char mine[16];
    snprintf(mine, sizeof(mine), "%d", MaxHeight());
    while (sceIoDread(d, &e) > 0) {
        if (SCE_S_ISDIR(e.d_stat.st_mode) && strcmp(e.d_name, mine) != 0 &&
            strcmp(e.d_name, ".") != 0 && strcmp(e.d_name, "..") != 0) {
            stale.push_back(std::string(kCacheRoot) + "/" + e.d_name);
        }
        memset(&e, 0, sizeof(e));
    }
    sceIoDclose(d);
    for (const std::string& dir : stale) {
        SceUID sd = sceIoDopen(dir.c_str());
        if (sd >= 0) {
            memset(&e, 0, sizeof(e));
            while (sceIoDread(sd, &e) > 0) {
                sceIoRemove((dir + "/" + e.d_name).c_str());
                memset(&e, 0, sizeof(e));
            }
            sceIoDclose(sd);
        }
        sceIoRmdir(dir.c_str());
    }
}

}  // namespace

void vita_hd_prepare(void)
{
    if (!s_ready || !g_vitaVideo.hdPreload) return;

    sceIoMkdir(kCacheRoot, 0777);
    DropOtherSizes();
    sceIoMkdir(CacheDir().c_str(), 0777);
    ListCache();

    // Every picture a camera can ask for that has no cache file yet. A file
    // whose pack was replaced is caught later by the size check and decoded
    // on the fly, then picked up here on the next start.
    std::set<std::string> seen;
    s_prep.clear();
    for (const auto& kv : s_map) {
        const std::string path = FindFile(kv.first);
        if (path.empty() || !seen.insert(path).second) continue;
        if (s_cacheIndex.count(CacheName(path)) != 0) continue;
        s_prep.push_back(PrepJob{ path, IsMask(kv.first) });
    }
    if (s_prep.empty()) return;

    s_prepNext = s_prepDone = s_prepStop = 0;
    s_prepBytes = 0;
    s_prepRunning = 2;
    // The game is not running yet: two cores decode, this one draws.
    const int cores[2] = { 0x20000, 0x40000 };      // SCE_KERNEL_CPU_MASK_USER_1 / _2
    for (int i = 0; i < 2; ++i) {
        const SceUID t = sceKernelCreateThread("re1_hdprep", PrepThread, 0x10000100, 0x20000, 0,
                                               cores[i], NULL);
        if (t < 0 || sceKernelStartThread(t, 0, NULL) < 0) {
            if (t >= 0) sceKernelDeleteThread(t);
            __atomic_fetch_sub(&s_prepRunning, 1, __ATOMIC_SEQ_CST);
        }
    }

    const int total = (int)s_prep.size();
    const SceUInt64 start = sceKernelGetProcessTimeWide();
    while (s_prepRunning > 0) {
        const int done = s_prepDone;
        const double secs = (double)(sceKernelGetProcessTimeWide() - start) / 1e6;
        char eta[48] = "";
        if (s_prepStop) {
            snprintf(eta, sizeof(eta), "Stopping... ");
        } else if (done > 0) {
            const int left = (int)(secs / done * (total - done) / 60.0 + 0.5);
            if (left < 1) snprintf(eta, sizeof(eta), "Less than a minute left. ");
            else snprintf(eta, sizeof(eta), "About %d min left. ", left);
        }
        char line[256];
        snprintf(line, sizeof(line),
                 "Done once, so backgrounds never pop in. %s%d MB written to ux0:data/RE1/hdcache/.",
                 eta, (int)(s_prepBytes >> 20));
        if (vita_mod_manager_progress("PREPARING HD BACKGROUNDS", line, done, total)) {
            s_prepStop = 1;     // the workers finish the picture they are on
        }
        sceKernelDelayThread(30000);
    }
    vita_mod_manager_progress_end();
    s_prep.clear();
}
