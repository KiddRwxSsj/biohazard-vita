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
//   * The mod manager's OPTIMIZE turns a pack's WebPs into HASH.jpg (bgd) /
//     HASH.rhd (mask) at HdMaxHeight and deletes them. Those load fast, so
//     port_hd_poll waits for them instead of showing the original first.
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
#include <turbojpeg.h>
#include <zlib.h>

#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>

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

bool Ext(const std::string& path, const char* ext)
{
    const size_t n = strlen(ext);
    return path.size() >= n && strcasecmp(path.c_str() + path.size() - n, ext) == 0;
}

// The pack's file for `name`: an optimized .jpg/.rhd first, else the .webp.
std::string FindFile(const std::string& name)
{
    auto it = s_map.find(name);
    if (it == s_map.end()) return std::string();
    const bool mask = IsMask(name);
    const char* kind = mask ? "mask/" : "bgd/";
    for (const std::string& root : s_roots) {
        const std::string base = root + kind + it->second;
        const std::string fast = base + (mask ? ".rhd" : ".jpg");
        if (plat_file_exists(fast.c_str())) return fast;
        const std::string webp = base + ".webp";
        if (plat_file_exists(webp.c_str())) return webp;
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
// Optimized packs (mod manager, OPTIMIZE): beside or instead of HASH.webp,
//   bgd/HASH.jpg   the background, already scaled, as a JPEG,
//   mask/HASH.rhd  the mask sheet, already scaled: "RHD1" header + zlib RGBA.
// Both load several times faster than the WebP they replace.
// ---------------------------------------------------------------------------

struct RhdHeader {
    char     magic[4];          // "RHD1"
    unsigned w, h;
    unsigned reserved;
    unsigned maxH;              // HdMaxHeight it was made for
    unsigned zlen;              // zlib stream that follows
};

bool ReadRhd(const std::string& path, Picture* out)
{
    size_t size = 0;
    unsigned char* data = ReadFile(path, &size);
    if (data == NULL) return false;
    bool ok = false;
    RhdHeader hdr;
    if (size >= sizeof(hdr)) {
        memcpy(&hdr, data, sizeof(hdr));
        const size_t pixels = (size_t)hdr.w * hdr.h;
        if (memcmp(hdr.magic, "RHD1", 4) == 0 && hdr.w > 0 && hdr.h > 0 &&
            hdr.w <= kMaxSide && hdr.h <= kMaxSide && sizeof(hdr) + hdr.zlen <= size) {
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

bool WriteAtomically(const std::string& path, const void* a, size_t na, const void* b, size_t nb)
{
    const std::string tmp = path + ".part";
    SceUID fd = sceIoOpen(tmp.c_str(), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) return false;
    bool full = sceIoWrite(fd, a, (SceSize)na) == (int)na;
    if (full && nb > 0) full = sceIoWrite(fd, b, (SceSize)nb) == (int)nb;
    sceIoClose(fd);
    sceIoRemove(path.c_str());
    if (full && sceIoRename(tmp.c_str(), path.c_str()) >= 0) return true;
    sceIoRemove(tmp.c_str());
    return false;
}

bool WriteRhd(const std::string& path, const Picture& pic)
{
    const uLong raw = (uLong)pic.w * pic.h * 4;
    uLongf zlen = compressBound(raw);
    unsigned char* z = (unsigned char*)malloc(zlen);
    if (z == NULL) return false;
    bool ok = false;
    if (compress2(z, &zlen, pic.rgba, raw, 6) == Z_OK) {
        RhdHeader hdr;
        memcpy(hdr.magic, "RHD1", 4);
        hdr.w = (unsigned)pic.w;
        hdr.h = (unsigned)pic.h;
        hdr.reserved = 0;
        hdr.maxH = (unsigned)MaxHeight();
        hdr.zlen = (unsigned)zlen;
        ok = WriteAtomically(path, &hdr, sizeof(hdr), z, zlen);
    }
    free(z);
    return ok;
}

bool ReadJpeg(const std::string& path, Picture* out)
{
    size_t size = 0;
    unsigned char* data = ReadFile(path, &size);
    if (data == NULL) return false;
    bool ok = false;
    tjhandle tj = tjInitDecompress();
    int w = 0, h = 0, sub = 0, cs = 0;
    if (tj != NULL && tjDecompressHeader3(tj, data, (unsigned long)size, &w, &h, &sub, &cs) == 0 &&
        w > 0 && h > 0 && w <= kMaxSide && h <= kMaxSide) {
        unsigned char* rgba = (unsigned char*)WebPMalloc((size_t)w * h * 4);
        if (rgba != NULL && tjDecompress2(tj, data, (unsigned long)size, rgba, w, 0, h,
                                          TJPF_RGBA, TJFLAG_FASTDCT) == 0) {
            out->rgba = rgba;
            out->w = w;
            out->h = h;
            ok = true;
        } else {
            WebPFree(rgba);
        }
    }
    if (tj != NULL) tjDestroy(tj);
    free(data);
    return ok;
}

bool WriteJpeg(const std::string& path, const Picture& pic)
{
    tjhandle tj = tjInitCompress();
    if (tj == NULL) return false;
    unsigned char* jpg = NULL;
    unsigned long size = 0;
    bool ok = tjCompress2(tj, pic.rgba, pic.w, 0, pic.h, TJPF_RGBA, &jpg, &size,
                          TJSAMP_444, 92, TJFLAG_ACCURATEDCT) == 0;
    ok = ok && WriteAtomically(path, jpg, size, NULL, 0);
    tjFree(jpg);
    tjDestroy(tj);
    return ok;
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
    if (Ext(path, ".jpg")) return ReadJpeg(path, out);
    if (Ext(path, ".rhd")) return ReadRhd(path, out);
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

// A picture from an optimized pack loads in a few frames: hold the frame
// for it rather than show the original background first (pop-in). A plain
// .webp can take half a second, so that one is swapped in when it is ready.
static void WaitFor(bool mask)
{
    std::string name;
    {
        Lock l;
        name = mask ? s_wantMask : s_wantBg;
    }
    const std::string path = FindFile(name);
    if (!Ext(path, ".jpg") && !Ext(path, ".rhd")) return;
    for (int t = 0; t < 500; ++t) {             // 1 s at most
        {
            Lock l;
            const std::string& now = mask ? s_wantMask : s_wantBg;
            if (now.empty() || s_done.count(now) != 0 || s_failed.count(now) != 0) return;
        }
        sceKernelDelayThread(2000);
    }
}

void port_hd_poll(void)
{
    if (!s_ready) return;

    if (!s_wantBg.empty()) WaitFor(false);
    if (!s_wantMask.empty()) WaitFor(true);

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
// Mod manager: OPTIMIZE
// ---------------------------------------------------------------------------

BOOL vita_hd_optimize_picture(const char* webpPath, BOOL mask)
{
    const std::string src = webpPath;
    const size_t dot = src.rfind('.');
    if (dot == std::string::npos) return FALSE;
    const std::string dst = src.substr(0, dot) + (mask ? ".rhd" : ".jpg");
    Picture pic = { NULL, 0, 0, 0 };
    if (!DecodeWebp(src, mask ? true : false, &pic)) return FALSE;
    const bool ok = mask ? WriteRhd(dst, pic) : WriteJpeg(dst, pic);
    WebPFree(pic.rgba);
    if (!ok) return FALSE;
    sceIoRemove(src.c_str());
    return TRUE;
}

// The HD PRELOAD cache of the previous version: optimization replaced it.
void vita_hd_drop_old_cache(void)
{
    const char* root = "ux0:data/RE1/hdcache";
    SceUID d = sceIoDopen(root);
    if (d < 0) return;
    std::vector<std::string> dirs;
    SceIoDirent e;
    memset(&e, 0, sizeof(e));
    while (sceIoDread(d, &e) > 0) {
        if (e.d_name[0] != '.') dirs.push_back(std::string(root) + "/" + e.d_name);
        memset(&e, 0, sizeof(e));
    }
    sceIoDclose(d);
    for (const std::string& dir : dirs) {
        SceUID sd = sceIoDopen(dir.c_str());
        if (sd >= 0) {
            memset(&e, 0, sizeof(e));
            while (sceIoDread(sd, &e) > 0) {
                if (e.d_name[0] != '.') sceIoRemove((dir + "/" + e.d_name).c_str());
                memset(&e, 0, sizeof(e));
            }
            sceIoDclose(sd);
        }
        sceIoRmdir(dir.c_str());
        sceIoRemove(dir.c_str());
    }
    sceIoRmdir(root);
}
