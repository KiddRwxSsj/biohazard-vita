// settings.cpp - config.ini [Vita].
//
// The video half is what dxcfg.exe configures on the PC. dxcfg's display mode
// and presentation have no meaning on a fixed 960x544 panel, and anisotropic
// filtering does nothing without mipmaps, which the game's textures never had;
// what carries over is aspect handling, the scaling filter, vsync, gamma and
// MSAA. The gameplay half is the port's quality-of-life switches, all off by
// default so the game plays as shipped until the player turns one on.
#include "../platform.h"
#include "../../Globals.h"
#include "../../system/ConfigFile.h"
#include "vita.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

VitaVideoSettings g_vitaVideo = { VITA_ASPECT_4_3, FALSE, TRUE, 1.0f, 0 };
BOOL g_vitaVideoDirty = FALSE;
VitaGameplaySettings g_vitaGameplay = { FALSE, FALSE, FALSE, FALSE, FALSE };

// The layout MainMenu.cpp starts with, kept before config.ini overrides it so
// EDIT HUD can put it back.
static PortHudWidget s_defaultHudLayout[2];
static BOOL s_haveDefaultHudLayout = FALSE;

static const int kHudNativeW[2] = { PORT_HUD_STATUS_W, PORT_HUD_WEAPON_W };
static const int kHudNativeH[2] = { PORT_HUD_STATUS_H, PORT_HUD_WEAPON_H };

void vita_hud_clamp(int widget)
{
    PortHudWidget& w = g_portHudLayout[widget];
    if (w.scale < 50) w.scale = 50;
    if (w.scale > 150) w.scale = 150;
    const int width  = kHudNativeW[widget] * w.scale / 100;
    const int height = kHudNativeH[widget] * w.scale / 100;
    if (w.x > 320 - width)  w.x = (short)(320 - width);
    if (w.y > 240 - height) w.y = (short)(240 - height);
    if (w.x < 0) w.x = 0;
    if (w.y < 0) w.y = 0;
}

void vita_hud_size(int widget, int* w, int* h)
{
    if (w) *w = kHudNativeW[widget] * g_portHudLayout[widget].scale / 100;
    if (h) *h = kHudNativeH[widget] * g_portHudLayout[widget].scale / 100;
}

void vita_hud_reset_layout(void)
{
    if (!s_haveDefaultHudLayout) return;
    g_portHudLayout[0] = s_defaultHudLayout[0];
    g_portHudLayout[1] = s_defaultHudLayout[1];
}

static void ReadHudWidget(const char* key, int widget)
{
    char value[32];
    int x, y, scale;
    if (!ConfigFile_ReadString("Vita", key, value, sizeof(value))) return;
    if (sscanf(value, "%d , %d , %d", &x, &y, &scale) != 3) return;
    g_portHudLayout[widget].x = (short)x;
    g_portHudLayout[widget].y = (short)y;
    g_portHudLayout[widget].scale = (short)scale;
    vita_hud_clamp(widget);
}

static const char kVitaSection[] =
    "\n"
    "[Vita]\n"
    "; Aspect: 4:3 (default, black bars left and right), Integer (640x480,\n"
    "; an exact 2x of the original picture, sharpest), Stretch (fills the screen)\n"
    "Aspect=4:3\n"
    "; Filter: Nearest (default, original pixels) or Linear (smoothed)\n"
    "Filter=Nearest\n"
    "; VSync: 1 = on (default), 0 = off\n"
    "VSync=1\n"
    "; Gamma: 1.0 = unchanged, above 1.0 brightens the dark areas\n"
    "Gamma=1.0\n"
    "; Antialiasing (MSAA): 0 (default), 2 or 4. 4 costs GPU time and memory.\n"
    "MSAA=0\n"
    "; Gameplay extras, 1 = on. AutoRun: push forward to run, hold run to walk.\n"
    "; WeaponSwitch: flick the right stick to change weapon. SkipDoors: cut the\n"
    "; door animation out entirely. HealthHud: the status screen's portrait and\n"
    "; ECG in play. AmmoHud: its equipped weapon box with the round count.\n"
    "AutoRun=0\n"
    "WeaponSwitch=0\n"
    "SkipDoors=0\n"
    "HealthHud=0\n"
    "AmmoHud=0\n"
    "; Where the two HUD widgets sit: x,y of the top-left corner in the 320x240\n"
    "; picture, then size in percent (50-150). Easier set from the overlay's\n"
    "; EDIT HUD, which drags and pinches them.\n"
    "HudStatus=6,202,80\n"
    "HudWeapon=266,204,80\n";

// A fresh config.ini comes from the shared template, which knows nothing about
// the Vita. Append the section once so the options are there to edit; a
// section the player already wrote, in any letter case, is left alone.
static BOOL HasVitaSection(void)
{
    FILE* f = fopen(ConfigFile_Find(), "r");
    if (f == NULL) return FALSE;

    char line[256];
    BOOL found = FALSE;
    while (!found && fgets(line, sizeof(line), f) != NULL) {
        const char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        found = (strncasecmp(p, "[Vita]", 6) == 0) ? TRUE : FALSE;
    }
    fclose(f);
    return found;
}

static void EnsureVitaSection(void)
{
    if (HasVitaSection()) return;

    FILE* f = fopen(ConfigFile_Find(), "a");
    if (f == NULL) return;
    fputs(kVitaSection, f);
    fclose(f);
}

void vita_load_video_settings(void)
{
    EnsureVitaSection();

    char value[32];
    if (ConfigFile_ReadString("Vita", "Aspect", value, sizeof(value))) {
        if (strcasecmp(value, "Integer") == 0)      g_vitaVideo.aspect = VITA_ASPECT_INTEGER;
        else if (strcasecmp(value, "Stretch") == 0) g_vitaVideo.aspect = VITA_ASPECT_STRETCH;
        else                                        g_vitaVideo.aspect = VITA_ASPECT_4_3;
    }
    if (ConfigFile_ReadString("Vita", "Filter", value, sizeof(value))) {
        g_vitaVideo.linearFilter = (strcasecmp(value, "Linear") == 0) ? TRUE : FALSE;
    }
    g_vitaVideo.vsync = ConfigFile_ReadInt("Vita", "VSync", 1) ? TRUE : FALSE;

    if (ConfigFile_ReadString("Vita", "Gamma", value, sizeof(value))) {
        float gamma = (float)atof(value);
        g_vitaVideo.gamma = (gamma >= 0.5f && gamma <= 2.5f) ? gamma : 1.0f;
    }

    int msaa = ConfigFile_ReadInt("Vita", "MSAA", 0);
    g_vitaVideo.msaa = (msaa >= 4) ? 4 : (msaa >= 2) ? 2 : 0;

    g_vitaGameplay.autoRun      = ConfigFile_ReadInt("Vita", "AutoRun", 0) ? TRUE : FALSE;
    g_vitaGameplay.weaponSwitch = ConfigFile_ReadInt("Vita", "WeaponSwitch", 0) ? TRUE : FALSE;
    g_vitaGameplay.skipDoors    = ConfigFile_ReadInt("Vita", "SkipDoors", 0) ? TRUE : FALSE;
    g_vitaGameplay.healthHud    = ConfigFile_ReadInt("Vita", "HealthHud", 0) ? TRUE : FALSE;
    g_vitaGameplay.ammoHud      = ConfigFile_ReadInt("Vita", "AmmoHud", 0) ? TRUE : FALSE;

    if (!s_haveDefaultHudLayout) {
        s_defaultHudLayout[0] = g_portHudLayout[0];
        s_defaultHudLayout[1] = g_portHudLayout[1];
        s_haveDefaultHudLayout = TRUE;
    }
    ReadHudWidget("HudStatus", 0);
    ReadHudWidget("HudWeapon", 1);
    vita_apply_gameplay_settings();
}

void vita_apply_gameplay_settings(void)
{
    g_portAutoRun   = g_vitaGameplay.autoRun ? 1 : 0;
    g_portSkipDoors = g_vitaGameplay.skipDoors ? 1 : 0;
    g_portHud       = (g_vitaGameplay.healthHud ? PORT_HUD_STATUS : 0) |
                      (g_vitaGameplay.ammoHud ? PORT_HUD_WEAPON : 0);
}

// Write g_vitaVideo back into the [Vita] section, in place. Every other line,
// comments included, is copied through untouched; keys the section lacks are
// added at its end, and a missing section is appended whole.
void vita_save_video_settings(void)
{
    static const char* const kAspect[3] = { "4:3", "Integer", "Stretch" };
    static const char* const kKeys[] = {
        "Aspect", "Filter", "VSync", "Gamma", "MSAA",
        "AutoRun", "WeaponSwitch", "SkipDoors", "HealthHud", "AmmoHud",
        "HudStatus", "HudWeapon",
    };
    const int kKeyCount = (int)(sizeof(kKeys) / sizeof(kKeys[0]));
    char values[sizeof(kKeys) / sizeof(kKeys[0])][16];
    snprintf(values[0], sizeof(values[0]), "%s", kAspect[g_vitaVideo.aspect]);
    snprintf(values[1], sizeof(values[1]), "%s", g_vitaVideo.linearFilter ? "Linear" : "Nearest");
    snprintf(values[2], sizeof(values[2]), "%d", g_vitaVideo.vsync ? 1 : 0);
    snprintf(values[3], sizeof(values[3]), "%.1f", g_vitaVideo.gamma);
    snprintf(values[4], sizeof(values[4]), "%d", g_vitaVideo.msaa);
    snprintf(values[5], sizeof(values[5]), "%d", g_vitaGameplay.autoRun ? 1 : 0);
    snprintf(values[6], sizeof(values[6]), "%d", g_vitaGameplay.weaponSwitch ? 1 : 0);
    snprintf(values[7], sizeof(values[7]), "%d", g_vitaGameplay.skipDoors ? 1 : 0);
    snprintf(values[8], sizeof(values[8]), "%d", g_vitaGameplay.healthHud ? 1 : 0);
    snprintf(values[9], sizeof(values[9]), "%d", g_vitaGameplay.ammoHud ? 1 : 0);
    for (int i = 0; i < 2; ++i) {
        snprintf(values[10 + i], sizeof(values[10 + i]), "%d,%d,%d",
                 g_portHudLayout[i].x, g_portHudLayout[i].y, g_portHudLayout[i].scale);
    }

    const char* path = ConfigFile_Find();
    FILE* in = fopen(path, "r");
    if (in == NULL) return;

    fseek(in, 0, SEEK_END);
    long size = ftell(in);
    rewind(in);
    char* text = (char*)malloc((size_t)size + 1);
    if (text == NULL) { fclose(in); return; }
    size = (long)fread(text, 1, (size_t)size, in);
    text[size] = '\0';
    fclose(in);

    FILE* out = fopen(path, "w");
    if (out == NULL) { free(text); return; }

    BOOL inVita = FALSE, sawVita = FALSE;
    BOOL written[sizeof(kKeys) / sizeof(kKeys[0])] = {};

    auto emitMissing = [&]() {
        for (int k = 0; k < kKeyCount; ++k) {
            if (!written[k]) fprintf(out, "%s=%s\n", kKeys[k], values[k]);
            written[k] = TRUE;
        }
    };

    char* line = text;
    while (line != NULL) {
        char* next = strchr(line, '\n');
        if (next != NULL) *next++ = '\0';
        else if (*line == '\0') break;   // nothing after the final newline

        const char* p = line;
        while (*p == ' ' || *p == '\t') ++p;

        if (*p == '[') {
            if (inVita) emitMissing();
            inVita = (strncasecmp(p, "[Vita]", 6) == 0) ? TRUE : FALSE;
            if (inVita) sawVita = TRUE;
            fprintf(out, "%s\n", line);
        } else {
            int key = -1;
            if (inVita) {
                for (int k = 0; k < kKeyCount && key < 0; ++k) {
                    const size_t n = strlen(kKeys[k]);
                    const char* q = p + n;
                    while (*q == ' ' || *q == '\t') ++q;
                    if (strncasecmp(p, kKeys[k], n) == 0 && *q == '=') key = k;
                }
            }
            if (key < 0) {
                fprintf(out, "%s\n", line);
            } else if (!written[key]) {
                // A duplicate later in the section is dropped, so the value
                // the reader picks up is always this one.
                fprintf(out, "%s=%s\n", kKeys[key], values[key]);
                written[key] = TRUE;
            }
        }
        line = next;
    }

    if (inVita) emitMissing();
    if (!sawVita) {
        fputs("\n[Vita]\n", out);
        emitMissing();
    }

    fclose(out);
    free(text);
}

void vita_game_box(int* x, int* y, int* w, int* h)
{
    int bw, bh;
    switch (g_vitaVideo.aspect) {
    case VITA_ASPECT_INTEGER: bw = 640;           bh = 480;           break;
    case VITA_ASPECT_STRETCH: bw = VITA_SCREEN_W; bh = VITA_SCREEN_H; break;
    default:
        // 544 * 4/3 = 725.3; 724 keeps the width even so both bars are 118 px.
        bw = 724;
        bh = VITA_SCREEN_H;
        break;
    }
    if (x) *x = (VITA_SCREEN_W - bw) / 2;
    if (y) *y = (VITA_SCREEN_H - bh) / 2;
    if (w) *w = bw;
    if (h) *h = bh;
}
