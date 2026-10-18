// assetcache.cpp - keeps the memory card out of room changes.
//
// A room change reads 25-40 files: the RDT, a pak per camera, the masks, a
// dozen sound banks that are mostly the same ones every time (the zombie set
// is reloaded in every room that has a zombie), and the effect sheet four
// times over. Each sound bank was also probed as .ogg first and then opened
// twice, and with mods or a DC tree every asset was probed in each of those
// folders. On the card a failed open still walks the whole directory, and
// Sound/ and Voice/ hold over 500 files each. All of that happened inside one
// frame of the door, which is what the black screen and the door hitch were.
//
// Three things here:
//
//   * Directory listings. plat_file_exists answers from a listing of the
//     folder, read once, so probes for files that are not there cost nothing.
//   * A data cache. Asset files up to 4 MB are kept after they are read, LRU,
//     up to kCacheBudget in all. LoadFile and plat_file_read_all copy out of it.
//   * Prefetch. A low-priority thread reads queued files into the cache. The
//     game queues the rooms behind every door of the room it is in once that
//     room is playable (port_prefetch_neighbours, DoorSystem.cpp), so by the
//     time a door is used its room is already in memory. Rooms remember which
//     files they loaded the last time (sound banks, enemy models), and those
//     are queued too.
//
// Only asset folders are cached: anything under SAVE/ or the config is read
// and written straight through.
#include "../platform.h"
#include "vita.h"

#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

const size_t kCacheBudget  = 24u * 1024u * 1024u;
const size_t kMaxCacheFile = 4u * 1024u * 1024u;

SceKernelLwMutexWork s_lock;
bool s_lockReady = false;

struct Lock {
    Lock()  { if (s_lockReady) sceKernelLockLwMutex(&s_lock, 1, NULL); }
    ~Lock() { if (s_lockReady) sceKernelUnlockLwMutex(&s_lock, 1); }
};

void EnsureInit(void);

// Lower case, forward slashes, no "./" segments: the key both the listings and
// the data cache use. The Vita's filesystem is case-insensitive, so this is
// also what decides two paths are the same file.
std::string Key(const char* path)
{
    std::string k;
    k.reserve(strlen(path));
    for (const char* p = path; *p != '\0'; ++p) {
        char c = *p == '\\' ? '/' : *p;
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c == '.' && (k.empty() || k.back() == '/') && (p[1] == '/' || p[1] == '\\')) {
            ++p;            // "./"
            continue;
        }
        if (c == '/' && !k.empty() && k.back() == '/') continue;
        k.push_back(c);
    }
    return k;
}

// The same path with its case kept, for the actual I/O.
std::string Native(const char* path)
{
    std::string n;
    n.reserve(strlen(path));
    for (const char* p = path; *p != '\0'; ++p) {
        char c = *p == '\\' ? '/' : *p;
        if (c == '.' && (n.empty() || n.back() == '/') && (p[1] == '/' || p[1] == '\\')) {
            ++p;
            continue;
        }
        if (c == '/' && !n.empty() && n.back() == '/') continue;
        n.push_back(c);
    }
    return n;
}

bool Cacheable(const std::string& key)
{
    return key.find("/save/") == std::string::npos &&
           key.find("config.ini") == std::string::npos &&
           key.find(".log") == std::string::npos;
}

// ---------------------------------------------------------------------------
// Directory listings
// ---------------------------------------------------------------------------

std::unordered_map<std::string, std::unordered_set<std::string>> s_dirs;

// `dir` is the native-case folder; listings are keyed by its lower case.
const std::unordered_set<std::string>& Listing(const std::string& dir)
{
    const std::string key = Key(dir.c_str());
    auto it = s_dirs.find(key);
    if (it != s_dirs.end()) return it->second;

    std::unordered_set<std::string>& names = s_dirs[key];
    SceUID d = sceIoDopen(dir.empty() ? "." : dir.c_str());
    if (d >= 0) {
        SceIoDirent e;
        memset(&e, 0, sizeof(e));
        while (sceIoDread(d, &e) > 0) {
            names.insert(Key(e.d_name));
            memset(&e, 0, sizeof(e));
        }
        sceIoDclose(d);
    }
    return names;
}

bool ExistsLocked(const std::string& native)
{
    const size_t slash = native.rfind('/');
    if (slash == std::string::npos) return Listing("").count(Key(native.c_str())) != 0;
    return Listing(native.substr(0, slash)).count(Key(native.c_str() + slash + 1)) != 0;
}

// ---------------------------------------------------------------------------
// Data cache
// ---------------------------------------------------------------------------

struct Entry {
    void*    data;
    size_t   size;
    unsigned lastUse;
};

std::unordered_map<std::string, Entry> s_cache;
size_t   s_cacheBytes = 0;
unsigned s_useClock = 0;

void EvictFor(size_t need)
{
    while (s_cacheBytes + need > kCacheBudget && !s_cache.empty()) {
        auto oldest = s_cache.begin();
        for (auto it = s_cache.begin(); it != s_cache.end(); ++it) {
            if (it->second.lastUse < oldest->second.lastUse) oldest = it;
        }
        s_cacheBytes -= oldest->second.size;
        free(oldest->second.data);
        s_cache.erase(oldest);
    }
}

// Takes ownership of `data` (a malloc'd copy) when it is kept.
void StoreLocked(const std::string& key, void* data, size_t size)
{
    if (s_cache.count(key) != 0 || size > kMaxCacheFile) { free(data); return; }
    EvictFor(size);
    s_cache[key] = Entry{ data, size, ++s_useClock };
    s_cacheBytes += size;
}

void Store(const std::string& key, const void* src, size_t size)
{
    if (size > kMaxCacheFile || !Cacheable(key)) return;
    void* copy = malloc(size ? size : 1);
    if (copy == NULL) return;
    memcpy(copy, src, size);
    Lock l;
    StoreLocked(key, copy, size);
}

// ---------------------------------------------------------------------------
// Learned room file lists and the prefetch thread
// ---------------------------------------------------------------------------

std::unordered_map<int, std::vector<std::string>> s_roomFiles;
int  s_learnKey = -1;
std::vector<std::string> s_learning;

std::vector<std::string> s_queue;
SceUID s_wake = -1;
SceUID s_thread = -1;

// Paths are kept native-case: they are opened again by the prefetch thread.
void Learn(const std::string& native)
{
    if (s_learnKey < 0) return;
    for (const std::string& f : s_learning) if (f == native) return;
    s_learning.push_back(native);
}

// sceIo rather than stdio: this runs beside the game's own reads.
void* ReadWhole(const char* path, size_t* outSize)
{
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return NULL;
    const SceOff size = sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    void* data = NULL;
    if (size >= 0 && (size_t)size <= kMaxCacheFile) {
        data = malloc((size_t)size ? (size_t)size : 1);
        if (data != NULL && sceIoRead(fd, data, (SceSize)size) != (int)size) {
            free(data);
            data = NULL;
        }
    }
    sceIoClose(fd);
    if (data != NULL) *outSize = (size_t)size;
    return data;
}

int PrefetchThread(SceSize, void*)
{
    for (;;) {
        sceKernelWaitSema(s_wake, 1, NULL);
        for (;;) {
            std::string key, native;
            {
                Lock l;
                if (s_queue.empty()) break;
                native = s_queue.front();
                s_queue.erase(s_queue.begin());
                key = Key(native.c_str());
                if (s_cache.count(key) != 0 || !ExistsLocked(native)) continue;
            }
            size_t size = 0;
            void* data = ReadWhole(native.c_str(), &size);
            if (data == NULL) continue;
            Lock l;
            StoreLocked(key, data, size);
        }
    }
    return 0;
}

void EnsureInit(void)
{
    if (s_lockReady) return;
    if (sceKernelCreateLwMutex(&s_lock, "re1_iocache", SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0, NULL) < 0) return;
    s_lockReady = true;
    s_wake = sceKernelCreateSema("re1_prefetch", 0, 0, 0x7FFFFFFF, NULL);
    // Below the game's own threads, so it only reads while they wait.
    s_thread = sceKernelCreateThread("re1_prefetch", PrefetchThread, 0x10000110, 0x8000, 0, 0, NULL);
    if (s_wake >= 0 && s_thread >= 0) sceKernelStartThread(s_thread, 0, NULL);
}

}  // namespace

// ---------------------------------------------------------------------------
// platform.h
// ---------------------------------------------------------------------------

BOOL plat_file_exists(const char* path)
{
    if (path == NULL) return FALSE;
    EnsureInit();
    const std::string key = Key(path);
    const std::string native = Native(path);
    if (!Cacheable(key)) {
        SceIoStat st;
        return sceIoGetstat(native.c_str(), &st) >= 0 ? TRUE : FALSE;
    }
    Lock l;
    if (s_cache.count(key) != 0) return TRUE;
    return ExistsLocked(native) ? TRUE : FALSE;
}

// ---------------------------------------------------------------------------
// vita.h
// ---------------------------------------------------------------------------

size_t vita_asset_load(const char* path, void* buffer)
{
    EnsureInit();
    const std::string key = Key(path);
    const std::string native = Native(path);
    if (Cacheable(key)) {
        Lock l;
        auto it = s_cache.find(key);
        if (it != s_cache.end()) {
            it->second.lastUse = ++s_useClock;
            memcpy(buffer, it->second.data, it->second.size);
            Learn(native);
            return it->second.size;
        }
        if (!ExistsLocked(native)) return (size_t)-1;
    }

    FILE* fp = fopen(native.c_str(), "rb");
    if (fp == NULL) return (size_t)-1;
    setvbuf(fp, NULL, _IONBF, 0);       // one device read (see LoadFile)
    fseek(fp, 0, SEEK_END);
    const long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    const size_t got = size > 0 ? fread(buffer, 1, (size_t)size, fp) : 0;
    fclose(fp);
    if (size < 0 || got != (size_t)size) return (size_t)-1;

    Store(key, buffer, (size_t)size);
    {
        Lock l;
        Learn(native);
    }
    return (size_t)size;
}

void* vita_asset_read_all(const char* path, size_t* outSize)
{
    EnsureInit();
    const std::string key = Key(path);
    const std::string native = Native(path);
    if (Cacheable(key)) {
        Lock l;
        auto it = s_cache.find(key);
        if (it != s_cache.end()) {
            // Same rounding as plat_file_read_all, the caller may rely on it.
            void* copy = malloc((it->second.size & ~(size_t)3) + 0x10);
            if (copy == NULL) return NULL;
            memcpy(copy, it->second.data, it->second.size);
            it->second.lastUse = ++s_useClock;
            Learn(native);
            if (outSize != NULL) *outSize = it->second.size;
            return copy;
        }
        if (!ExistsLocked(native)) return NULL;
    }
    return NULL;    // not cached: the caller reads it and hands it to vita_asset_store
}

void vita_asset_store(const char* path, const void* data, size_t size)
{
    EnsureInit();
    Store(Key(path), data, size);
    Lock l;
    Learn(Native(path));
}

void vita_asset_cache_reset(void)
{
    EnsureInit();
    Lock l;
    s_dirs.clear();
    for (auto& e : s_cache) free(e.second.data);
    s_cache.clear();
    s_cacheBytes = 0;
    s_queue.clear();
    s_roomFiles.clear();
}

// ---------------------------------------------------------------------------
// Globals.h (port_prefetch_*)
// ---------------------------------------------------------------------------

void port_prefetch_file(const char* path)
{
    EnsureInit();
    if (s_wake < 0) return;
    const std::string key = Key(path);
    if (!Cacheable(key)) return;
    const std::string native = Native(path);
    {
        Lock l;
        if (s_cache.count(key) != 0) return;
        for (const std::string& q : s_queue) if (q == native) return;
        s_queue.push_back(native);
    }
    sceKernelSignalSema(s_wake, 1);
}

void port_prefetch_learned(int roomKey)
{
    std::vector<std::string> files;
    {
        Lock l;
        auto it = s_roomFiles.find(roomKey);
        if (it == s_roomFiles.end()) return;
        files = it->second;
    }
    for (const std::string& f : files) port_prefetch_file(f.c_str());
}

void port_prefetch_learn(int roomKey)
{
    EnsureInit();
    Lock l;
    if (s_learnKey >= 0 && !s_learning.empty()) s_roomFiles[s_learnKey] = s_learning;
    s_learning.clear();
    s_learnKey = roomKey;
}
