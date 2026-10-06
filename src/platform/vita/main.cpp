// main.cpp - PS Vita entry point.
//
// Counterpart of src/platform/linux/main.cpp: read config.ini, bring up
// vitaGL, start Marni, then drive main_loop() at the engine's 33 ms tick with
// the same limiter (see src/platform/win32/main.cpp:706-757 for the original).
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <vitaGL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "Globals.h"
#include "platform/platform.h"
#include "system/ConfigFile.h"
#include "marni/MarniDX.h"
#include "marni/MarniSystem.h"
#include "vita.h"

// Read by vita-elf-create, not by code. The scheduler's task stacks are
// separate heap allocations, so the main thread only needs room for the
// engine's own frames plus ffmpeg's.
unsigned int sceUserMainThreadStackSize = 1 * 1024 * 1024;

// newlib heap. Whatever is left after it goes to vitaGL's pools (textures,
// vertex data), minus the threshold passed to vglInitExtended below.
int _newlib_heap_size_user = 160 * 1024 * 1024;

static const int kFrameIntervalMs = 33;
static const BOOL kFrameLimiterEnabled = TRUE;

static BOOL s_glReady = FALSE;

// The game only stores and compares g_hWnd, but a NULL one reads as "no display
// yet" in IsGraphicsSystemReadyForOperation, so it needs to point somewhere.
static int s_display;

BOOL vita_gl_ready(void)
{
    return s_glReady;
}

int main(int, char**)
{
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    // Everything relative (config.ini fallbacks, re1_debug.log, the compile-time
    // ./assets roots) should land in the data folder, not app0:.
    mkdir("ux0:data", 0777);
    mkdir(VITA_DATA_DIR, 0777);
    chdir(VITA_DATA_DIR);

    crashlog_install();

    // Before vitaGL: the [Vita] MSAA setting is an init-time parameter.
    ConfigFile_EnsureExists();
    ConfigFile_Load();
    vita_load_video_settings();

    const SceGxmMultisampleMode msaa =
        g_vitaVideo.msaa == 4 ? SCE_GXM_MULTISAMPLE_4X :
        g_vitaVideo.msaa == 2 ? SCE_GXM_MULTISAMPLE_2X : SCE_GXM_MULTISAMPLE_NONE;

    // vitaGL takes every free byte of each memory type for its pools except
    // these thresholds. 16 MB of RAM is for the audio port, threads and the
    // message dialog; 8 MB of CDRAM is for AvPlayer's decoded MP4 frames.
    // The whole common-dialog budget (0x8C6000, same as vglInitExtended)
    // stays with the system, or sceMsgDialogInit has nothing to run in.
    //
    // Don't test the return value. This vitaGL build returns GL_TRUE only when
    // it had to clamp the requested size to sceDisplayGetMaximumFrameBufResolution,
    // so a normal 960x544 init comes back GL_FALSE. Treating that as an error
    // exited the process while vitaGL's splash thread was still drawing, and
    // that thread then faulted inside SceGxm.
    vglInitWithCustomThreshold(0, VITA_SCREEN_W, VITA_SCREEN_H,
                               16 * 1024 * 1024, 8 * 1024 * 1024, 0, 0x8C6000,
                               msaa);
    s_glReady = TRUE;

    // The panel is fixed; whatever [Display] says is for the PC builds.
    int boxW = 0, boxH = 0;
    vita_game_box(NULL, NULL, &boxW, &boxH);
    g_bFullScreen = TRUE;
    g_dwScreenWidth  = (DWORD)boxW;
    g_dwScreenHeight = (DWORD)boxH;
    vglWaitVblankStart(g_vitaVideo.vsync ? GL_TRUE : GL_FALSE);

    g_hWnd = &s_display;

    StartSoundSystemAsync(g_hWnd);

    // 0x004414f6
    TaskScheduler_Init();

    InitializeMarniSystem();
    if (!IsGraphicsSystemReadyForOperation()) {
        vita_show_message("Graphics init failed.\n\n"
                          "vitaGL needs ur0:data/libshacccg.suprx to compile its shaders. "
                          "Extract it with ShaRKBR33D and try again.");
        sceKernelExitProcess(1);
        return 1;
    }

    g_bWindowFocused = TRUE;
    g_bWindowActive  = TRUE;
    g_bQuitFlag      = FALSE;

    g_dwGameTimer1 = plat_time_ms();

    for (;;) {
        if (g_bQuitFlag) break;

        DWORD now = plat_time_ms();

        // 0x00441f0c
        if (kFrameLimiterEnabled) {
            BOOL bSkipFrame = FALSE;
            if (g_bUseFrameSkip == 1) {
                if (!g_bFrameSkipDetected) {
                    if ((int)(kFrameIntervalMs + g_dwGameTimer1) > (int)now &&
                        (int)(g_dwGameTimer1 - 0x10000) < (int)now) {
                        bSkipFrame = TRUE;
                    }
                }
            } else {
                if ((int)(g_dwGameTimer1 + 16) > (int)now &&
                    (int)(g_dwGameTimer1 - 0x10000) < (int)now) {
                    bSkipFrame = TRUE;
                }
            }
            if (bSkipFrame) {
                sceKernelDelayThread(1000);
                continue;
            }
        }

        g_dwGameTimer1 = now;

        // 0x00441dfe
        if (g_bMCINotifyEnabled) {
            if (g_bMCINotifyFlag) {
                UpdateVideoPlayback();
                continue;
            }
            g_bMCINotifyFlag = TRUE;
        }

        if (main_loop() == 0) break;

        if ((int)(g_dwSystemTimer1 + 1000) < (int)now) {
            g_dwSystemTimer1 = now;
        }
    }

    CleanupVideoConfigAndSaveAllSettings();

    MarniDX_DestroyGlobal();
    sceKernelExitProcess(0);
    return 0;
}
