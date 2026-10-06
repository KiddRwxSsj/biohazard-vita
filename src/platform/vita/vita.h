// vita.h - glue shared by the PS Vita platform files. Not part of platform.h:
// nothing outside src/platform/vita/ and MarniDX_Vita.cpp should need it.
#pragma once

#include "../types.h"

// Game root on the memory card. Assets (USA/, JPN/, DC/), config.ini and SAVE/
// all hang off this, exactly like the executable's directory on PC.
#define VITA_DATA_DIR "ux0:data/RE1"

// The native panel. The game draws into a 4:3 box centred inside it.
#define VITA_SCREEN_W 960
#define VITA_SCREEN_H 544

// config.ini [Vita] - the console's take on the PC's dxcfg.exe. Read once at
// boot (MSAA has to be known before vitaGL starts), see vita_load_video_settings.
enum VitaAspect {
    VITA_ASPECT_4_3,        // 724x544 centred, pillarboxed (default)
    VITA_ASPECT_INTEGER,    // 640x480 centred, an exact 2x of the 320x240 game
    VITA_ASPECT_STRETCH,    // the whole 960x544 panel
};

struct VitaVideoSettings {
    int   aspect;           // VitaAspect
    BOOL  linearFilter;     // force bilinear on every draw
    BOOL  vsync;
    float gamma;            // 1.0 = untouched
    int   msaa;             // 0, 2 or 4
};

extern VitaVideoSettings g_vitaVideo;

// config.ini [Vita], gameplay half. vita_apply_gameplay_settings mirrors it
// into the game's g_portAutoRun / g_portSkipDoors / g_portHud.
struct VitaGameplaySettings {
    BOOL autoRun;
    BOOL weaponSwitch;      // right stick flick cycles the equipped weapon
    BOOL skipDoors;
    BOOL healthHud;         // status row: portrait + ECG
    BOOL ammoHud;           // status row: equipped weapon + count
};

extern VitaGameplaySettings g_vitaGameplay;

void vita_apply_gameplay_settings(void);

// settings.cpp - HUD layout helpers for EDIT HUD. Widget 0 = status, 1 = weapon.
void vita_hud_clamp(int widget);
void vita_hud_size(int widget, int* w, int* h);
void vita_hud_reset_layout(void);

// Set when the overlay changes g_vitaVideo; MarniDX_Vita applies it between
// frames (MarniVita_ApplyVideoSettings) so no frame is drawn half and half.
extern BOOL g_vitaVideoDirty;

void vita_load_video_settings(void);
void vita_save_video_settings(void);
void MarniVita_ApplyVideoSettings(void);

// overlay.cpp - the in-game settings panel. vita_overlay_input takes the raw
// sceCtrl buttons once per frame and returns TRUE while it owns the pad.
class MarniDX;
BOOL vita_overlay_input(unsigned int buttons);
BOOL vita_overlay_visible(void);
void vita_overlay_before_draw(MarniDX* dx);
void vita_overlay_draw(MarniDX* dx);

// Where the game is drawn on the panel, in GL viewport terms (y from the bottom).
void vita_game_box(int* x, int* y, int* w, int* h);

// True once vitaGL is up; the message dialog needs a live display to show on.
BOOL vita_gl_ready(void);

// Blocking OK-only system dialog.
void vita_show_message(const char* text);

// audio.cpp - the movie audio feed, resampled into the game's mixer. Both FMV
// decoders write into it; _played is frames consumed since the last reset, at
// the rate the data was written with, and is what the ffmpeg path clocks on.
int  plat_audio_feed_write(const short* pcm, int frames, int channels, int rate);
void plat_audio_feed_reset(void);
long plat_audio_feed_played(void);
long plat_audio_feed_queued(void);
