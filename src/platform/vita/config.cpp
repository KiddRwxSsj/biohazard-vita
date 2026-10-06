// config.cpp - the settings-save hook and the single-instance guard.
//
// Settings live in config.ini via the shared src/system/ConfigFile.cpp.
// The Vita never runs two copies of an app, so the instance check is free.
#include "../platform.h"

#include "../../Globals.h"
#include "../../system/ConfigFile.h"

// CleanupVideoConfigAndSaveAllSettings (0x00497ea0)
void CleanupVideoConfigAndSaveAllSettings(void)
{
    if (g_bHasFinalizedSettings) {
        return;
    }
    g_bHasFinalizedSettings = TRUE;
    ConfigFile_Save();
}

BOOL plat_single_instance_check(void)
{
    return TRUE;
}
