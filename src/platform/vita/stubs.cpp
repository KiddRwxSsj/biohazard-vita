// stubs.cpp - message box and crash-log breadcrumbs.
//
// Userland gets no fault signals on the Vita (the kernel writes its own core
// dump to ux0:data/ and the shell shows the C2-12828-1 error), so crash.log is
// reduced to the step markers the shared code leaves on its way through init.
#include "Globals.h"
#include "platform/platform.h"
#include "vita.h"

#include <stdio.h>

#define CRASH_LOG VITA_DATA_DIR "/crash.log"

int ShowMessageBox(HWND hWnd, LPCSTR lpMsg, LPCSTR lpCaption, UINT uType)
{
    (void)hWnd;
    (void)lpCaption;
    (void)uType;
    vita_show_message(lpMsg);
    return 1;   // IDOK
}

void crashlog_install(void)
{
    FILE* f = fopen(CRASH_LOG, "a");
    if (f == NULL) return;
    fputs("\n---- session start ----\n", f);
    fclose(f);
}

void crashlog_mark(const char* step)
{
    if (step == NULL) return;
    FILE* f = fopen(CRASH_LOG, "a");
    if (f == NULL) return;
    fprintf(f, "[step] %s\n", step);
    fclose(f);
}
