// platform.cpp - PS Vita implementation of the platform services.
//
// Counterpart of src/platform/linux/platform.cpp. There is no keyboard and no
// window system here: keys exist only so the shared code keeps its contract,
// time comes from the process clock, and the game root is a fixed folder on
// ux0: instead of the executable's directory (app0: is read-only).
#include "../platform.h"
#include "vita.h"

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/message_dialog.h>
#include <psp2/common_dialog.h>
#include <vitaGL.h>

#include <errno.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// ---------------------------------------------------------------------------
// Keyboard
//
// Indexed by Win32 VK directly. Nothing feeds it on real hardware, but keeping
// the edge bit logic means anything injected later behaves like GetAsyncKeyState.
// ---------------------------------------------------------------------------
static unsigned char s_keyDown[256];
static unsigned char s_keyPrev[256];

void plat_key_event(int vk, BOOL down)
{
    if (vk < 0 || vk > 255) return;
    s_keyDown[vk] = down ? 1 : 0;
}

int plat_key_state(int vk)
{
    if (vk < 0 || vk > 255) return 0;
    int down = s_keyDown[vk];
    int edge = (down && !s_keyPrev[vk]) ? 1 : 0;
    s_keyPrev[vk] = (unsigned char)down;
    return (down ? 0x8000 : 0) | edge;
}

void plat_key_flush(void)
{
    for (int vk = 0; vk < 256; vk++) {
        plat_key_state(vk);
    }
}

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------

DWORD plat_time_ms(void)
{
    return (DWORD)(sceKernelGetProcessTimeWide() / 1000);
}

// ---------------------------------------------------------------------------
// Filesystem
//
// ux0: is exFAT and already case-insensitive, so unlike Linux there is no
// per-component lookup; only the separators need fixing. Relative paths are
// left alone - main() chdir()s into the data folder before anything opens a
// file, and newlib resolves them from there.
// ---------------------------------------------------------------------------

const char* plat_normalize_path(const char* path, char* buffer, size_t size)
{
    if (path == NULL || buffer == NULL || size == 0) return path;

    size_t i = 0;
    for (; path[i] != '\0' && i + 1 < size; ++i) {
        buffer[i] = (path[i] == '\\') ? '/' : path[i];
    }
    buffer[i] = '\0';
    return buffer;
}

BOOL plat_exe_dir(char* out, size_t size)
{
    if (out == NULL || size == 0) return FALSE;
    if (strlen(VITA_DATA_DIR) >= size) {
        out[0] = '\0';
        return FALSE;
    }
    strcpy(out, VITA_DATA_DIR);
    return TRUE;
}

BOOL plat_path_is_absolute(const char* path)
{
    if (path == NULL || path[0] == '\0') return FALSE;
    if (path[0] == '/' || path[0] == '\\') return TRUE;

    // Device prefix: "ux0:", "uma0:", "app0:"... a colon before any separator.
    for (const char* p = path; *p != '\0' && *p != '/' && *p != '\\'; ++p) {
        if (*p == ':') return TRUE;
    }
    return FALSE;
}

BOOL plat_mkdir(const char* path)
{
    char norm[1024];
    if (mkdir(plat_normalize_path(path, norm, sizeof(norm)), 0777) == 0) return TRUE;
    return (errno == EEXIST) ? TRUE : FALSE;
}

BOOL plat_file_write(const char* path, const void* data, size_t size)
{
    char norm[1024];
    FILE* f = fopen(plat_normalize_path(path, norm, sizeof(norm)), "wb");
    if (f == NULL) return FALSE;
    size_t written = fwrite(data, 1, size, f);
    fclose(f);
    return (written == size) ? TRUE : FALSE;
}

void* plat_file_read_all(const char* path, size_t* outSize)
{
    char norm[1024];
    FILE* f = fopen(plat_normalize_path(path, norm, sizeof(norm)), "rb");
    if (f == NULL) return NULL;
    setvbuf(f, NULL, _IONBF, 0);    // one read, not 1 KB at a time (see LoadFile)

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long fileSize = ftell(f);
    if (fileSize < 0) { fclose(f); return NULL; }
    rewind(f);

    size_t alignedSize = (size_t)((fileSize & ~3L) + 0x10);
    void* buffer = malloc(alignedSize);
    if (buffer == NULL) { fclose(f); return NULL; }

    size_t got = fread(buffer, 1, (size_t)fileSize, f);
    fclose(f);
    if (got != (size_t)fileSize) { free(buffer); return NULL; }

    if (outSize != NULL) *outSize = (size_t)fileSize;
    return buffer;
}

// No VirtualQuery or /proc/self/maps here, but the kernel will tell us which
// memblock an address lives in. Heap, .data and .bss are all memblocks, so this
// covers every buffer a TIM page can point into. If the lookup fails we answer
// "plenty" rather than 0: refusing would drop a texture the original drew.
size_t plat_readable_bytes(const void* p)
{
    if (p == NULL) return 0;

    SceKernelMemBlockInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (sceKernelGetMemBlockInfoByAddr((void*)p, &info) < 0) return 0x7fffffff;

    uintptr_t end = (uintptr_t)info.mappedBase + info.mappedSize;
    uintptr_t addr = (uintptr_t)p;
    return (addr < end) ? (size_t)(end - addr) : 0;
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

void plat_debug_output(const char* s)
{
    if (s == NULL) return;
    sceClibPrintf("%s", s);
}

BOOL plat_is_debugger_present(void)
{
    // sceClibPrintf only lands somewhere with a TTY plugin loaded. Debug builds
    // assume one is; release builds skip the formatting cost entirely.
#ifdef NDEBUG
    return FALSE;
#else
    return TRUE;
#endif
}

DWORD plat_env_get(const char* name, char* buffer, DWORD size)
{
    const char* v = getenv(name);
    if (v == NULL || buffer == NULL || size == 0) return 0;
    size_t n = strlen(v);
    if (n >= (size_t)size) n = (size_t)size - 1;
    memcpy(buffer, v, n);
    buffer[n] = '\0';
    return (DWORD)n;
}

void plat_audio_probe_and_cache_volume(void) {}
void plat_audio_restore_volume(void) {}

// ---------------------------------------------------------------------------
// Task stacks
//
// No mprotect for user memory, so the guard gaps are plain padding here. They
// are still laid out the same way so TaskScheduler's slot arithmetic holds.
// ---------------------------------------------------------------------------

void* plat_alloc_guarded_stacks(int count, size_t stackSize, size_t guardSize)
{
    size_t total = (size_t)count * (stackSize + guardSize) + guardSize;
    unsigned char* mem = (unsigned char*)memalign(16, total);
    if (mem == NULL) return NULL;
    memset(mem, 0, total);
    return mem + guardSize;
}

// ---------------------------------------------------------------------------
// Fatal errors
//
// Uses the system message dialog, which needs vitaGL to keep presenting frames
// while it is up; vglSwapBuffers(GL_TRUE) does the common-dialog update.
// ---------------------------------------------------------------------------

void vita_show_message(const char* text)
{
    if (text == NULL) text = "";
    sceClibPrintf("[RE1] %s\n", text);
    if (!vita_gl_ready()) return;

    SceMsgDialogUserMessageParam msg;
    memset(&msg, 0, sizeof(msg));
    msg.buttonType = SCE_MSG_DIALOG_BUTTON_TYPE_OK;
    msg.msg = (const SceChar8*)text;

    SceMsgDialogParam param;
    sceMsgDialogParamInit(&param);
    param.mode = SCE_MSG_DIALOG_MODE_USER_MSG;
    param.userMsgParam = &msg;

    if (sceMsgDialogInit(&param) < 0) return;
    while (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        vglSwapBuffers(GL_TRUE);
    }
    sceMsgDialogTerm();
}

void plat_fatal(const char* message)
{
    vita_show_message(message ? message : "(fatal)");
    sceKernelExitProcess(1);
}

// ---------------------------------------------------------------------------
// Window / cursor lifecycle - nothing to do, the display is always ours.
// ---------------------------------------------------------------------------

void plat_window_destroy(HWND) {}
void plat_cursor_show(BOOL) {}

// ---------------------------------------------------------------------------
// Pad button labels
//
// fontus.tim carries the PS1's own button symbols in the first row of the 8x14
// game-text font, and that encoding is ASCII for letters, so a code here is
// "row 0 index + 36" for a symbol and the plain letter otherwise. The bits
// follow input.cpp's mapping. Codes 40/41 are avoided on purpose:
// PrintText8x14 swaps those for 14x14 symbols, which is why L/R use letters.
// ---------------------------------------------------------------------------
unsigned char plat_pad_button_glyph(int bit)
{
    switch (bit) {
    case 8:  return 45;   // Cross
    case 9:  return 44;   // Circle
    case 10: return 46;   // Square
    case 11: return 43;   // Triangle
    case 12: return 'L';
    case 13: return 'R';
    case 14: return 'S';  // Select
    case 15: return 38;   // Start, the font's right-pointing triangle
    default: return 0;
    }
}
