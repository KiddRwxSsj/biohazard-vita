// overlay.cpp - in-game settings panel.
//
// L+R+Select, or a tap in the top-right corner of the front touch panel, opens
// it. It is drawn over whatever the game presented, right before the swap, in
// the game's own 8x8 font from fontus.tim. While it is up the game keeps
// running but sees an idle pad; stopping it outright would mean freezing the
// task scheduler halfway through a frame. It has three pages - QUICK SAVES
// (the one it opens on), VIDEO and GAMEPLAY - flipped with L and R.
//
// Aspect, filter and gamma take effect on the next frame. MSAA is a vitaGL
// init parameter, so it is saved and used from the next boot.
//
// Quick save / quick load only raise requests (g_portQuick*, Globals.h); the
// game services them from game_loop, so they work during gameplay and nowhere
// else. A quick save is a regular RE1 save of the current room, the same as
// the typewriter would write, not a memory snapshot - loading rebuilds the
// room the way loading any save does, on the camera it was saved on. There are
// PORT_QUICK_SLOT_COUNT of them, savedat100.dat onwards, each with a thumbnail
// of the frame the game drew right after saving beside it (.thm). Delete takes
// a second press to confirm and removes both files.
//
// The HUD switches only set g_portHud; the game draws the HUD itself with the
// status screen's own art (port_hud_draw, MainMenu.cpp). EDIT HUD closes the
// panel and lets the player drag the two widgets with a finger and pinch them
// to size, or on a PS TV, pick one with Square, move it with the d-pad and
// size it with L/R. The layout goes to config.ini with the other settings.
#include "Globals.h"
#include "platform/platform.h"
#include "marni/MarniDX.h"
#include "marni/MarniSystem.h"
#include "system/AssetPath.h"
#include "vita.h"

#include <psp2/ctrl.h>
#include <psp2/touch.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

enum Item {
    ITEM_ASPECT, ITEM_FILTER, ITEM_GAMMA, ITEM_MSAA, ITEM_FRAME_RATE,
    ITEM_AUTO_RUN, ITEM_WEAPON_SWAP, ITEM_SKIP_DOORS, ITEM_HEALTH_HUD, ITEM_AMMO_HUD,
    ITEM_HUD_AUTO_HIDE, ITEM_EDIT_HUD, ITEM_MOD_MANAGER,
    ITEM_SLOT, ITEM_QUICK_SAVE, ITEM_QUICK_LOAD, ITEM_DELETE, ITEM_CLOSE, ITEM_COUNT
};

const char* const kLabels[ITEM_COUNT] = {
    "ASPECT", "FILTER", "GAMMA", "MSAA", "FRAME RATE",
    "AUTO RUN", "WEAPON SWAP", "SKIP DOORS", "STATUS HUD", "WEAPON HUD",
    "HUD AUTO HIDE", "EDIT HUD", "MOD MANAGER",
    "SLOT", "QUICK SAVE", "QUICK LOAD", "DELETE", "CLOSE"
};

enum Page { PAGE_SAVES, PAGE_VIDEO, PAGE_GAMEPLAY, PAGE_COUNT };

const char* const kPageNames[PAGE_COUNT] = { "QUICK SAVES", "VIDEO", "GAMEPLAY" };

// Rows per page, -1 terminated. CLOSE is on every page.
const int kPageItems[PAGE_COUNT][10] = {
    { ITEM_SLOT, ITEM_QUICK_SAVE, ITEM_QUICK_LOAD, ITEM_DELETE, ITEM_CLOSE, -1 },
    { ITEM_ASPECT, ITEM_FILTER, ITEM_GAMMA, ITEM_MSAA, ITEM_FRAME_RATE, ITEM_CLOSE, -1 },
    { ITEM_AUTO_RUN, ITEM_WEAPON_SWAP, ITEM_SKIP_DOORS, ITEM_HEALTH_HUD, ITEM_AMMO_HUD,
      ITEM_HUD_AUTO_HIDE, ITEM_EDIT_HUD, ITEM_MOD_MANAGER, ITEM_CLOSE, -1 },
};


const unsigned int kChord = SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_SELECT;
const int kCornerSize = 96;            // top-right touch hot spot, screen pixels

const int kThumbW = 160;
const int kThumbH = 120;

BOOL s_open        = FALSE;

// EDIT HUD state. Coordinates are in the 320x240 game picture, like the layout.
BOOL  s_editHud     = FALSE;
int   s_editSel     = 0;            // widget the d-pad and L/R act on
BOOL  s_dragging    = FALSE;
int   s_dragLastX   = 0;
int   s_dragLastY   = 0;
BOOL  s_pinching    = FALSE;
float s_pinchStartDist  = 0.0f;
int   s_pinchStartScale = 100;
BOOL s_waitRelease = FALSE;            // swallow the pad until the closing press is let go
int  s_page        = PAGE_SAVES;
int  s_cursor      = 0;           // row within the page
int  s_bootMsaa    = -1;               // what vitaGL was started with
unsigned int s_prevButtons = 0;
BOOL s_prevTouch   = FALSE;
BOOL s_touchReady  = FALSE;
SceTouchPanelInfo s_panel;

// game_loop bumps g_portGameplayFrame every frame; if it has stood still for a
// few polls the player is on the title, in a movie, a menu or a door.
DWORD s_lastGameplayFrame = 0;
int   s_gameplayIdle      = 1000;

BOOL  s_saving     = FALSE;
DWORD s_savingSince = 0;
int   s_savingSlot  = 0;           // the thumbnail goes to the slot the save was asked for
BOOL  s_confirmDelete = FALSE;
BOOL  s_confirmModManager = FALSE;
char  s_status[40] = "";
DWORD s_statusUntil = 0;

MarniHandle s_thumbTex[PORT_QUICK_SLOT_COUNT];
BOOL        s_thumbLoaded[PORT_QUICK_SLOT_COUNT];   // tried the file this session

void SetStatus(const char* text)
{
    snprintf(s_status, sizeof(s_status), "%s", text);
    s_statusUntil = plat_time_ms() + 2500;
}

BOOL InGameplay(void)
{
    return s_gameplayIdle < 5;
}

int PageRows(int page)
{
    int n = 0;
    while (n < 10 && kPageItems[page][n] >= 0) ++n;
    return n;
}

void QuickPath(int slot, char* out, size_t size, const char* ext)
{
    snprintf(out, size, "%ssavedat%d.%s", GetSaveRoot(), PORT_QUICK_SLOT_FIRST + slot + 1, ext);
}

// Whether each slot holds a save, so the panel does not hit the card every
// frame. -1 = not looked yet; reset when the panel opens and after a save or
// delete.
int s_slotExists[PORT_QUICK_SLOT_COUNT] = { -1, -1, -1 };

BOOL QuickSaveExists(int slot)
{
    if (s_slotExists[slot] < 0) {
        char path[300];
        QuickPath(slot, path, sizeof(path), "dat");
        FILE* f = fopen(path, "rb");
        s_slotExists[slot] = (f != NULL) ? 1 : 0;
        if (f != NULL) fclose(f);
    }
    return s_slotExists[slot] != 0;
}

// .thm: "RTHM", width and height as little-endian u16, then RGBA rows.
void LoadThumbnail(MarniDX* dx, int slot)
{
    s_thumbLoaded[slot] = TRUE;

    char path[300];
    QuickPath(slot, path, sizeof(path), "thm");
    FILE* f = fopen(path, "rb");
    if (f == NULL) return;

    unsigned char header[8];
    unsigned char* pixels = NULL;
    if (fread(header, 1, 8, f) == 8 && memcmp(header, "RTHM", 4) == 0) {
        const int w = header[4] | (header[5] << 8);
        const int h = header[6] | (header[7] << 8);
        const size_t bytes = (size_t)w * h * 4;
        if (w > 0 && h > 0 && w <= 512 && h <= 512) {
            pixels = (unsigned char*)malloc(bytes);
            if (pixels != NULL && fread(pixels, 1, bytes, f) == bytes) {
                if (s_thumbTex[slot] != MARNI_NULL_HANDLE) dx->DestroyTexture(s_thumbTex[slot]);
                s_thumbTex[slot] = dx->CreateTexture(w, h, 32, pixels, NULL, NULL);
            }
        }
    }
    free(pixels);
    fclose(f);
}

// Shrink the backbuffer to kThumbW x kThumbH, keep it as the panel texture and
// write it beside the save.
void CaptureThumbnail(MarniDX* dx, int slot)
{
    void* frame = NULL;
    DWORD fw = 0, fh = 0;
    if (!dx->CaptureBackbufferToRGBA(&frame, &fw, &fh) || frame == NULL) return;

    unsigned char* thumb = (unsigned char*)malloc((size_t)kThumbW * kThumbH * 4);
    if (thumb != NULL) {
        const unsigned char* src = (const unsigned char*)frame;
        for (int y = 0; y < kThumbH; ++y) {
            const unsigned char* row = src + (size_t)(y * fh / kThumbH) * fw * 4;
            unsigned char* d = thumb + (size_t)y * kThumbW * 4;
            for (int x = 0; x < kThumbW; ++x) {
                memcpy(d + x * 4, row + (size_t)(x * fw / kThumbW) * 4, 4);
                d[x * 4 + 3] = 255;
            }
        }

        if (s_thumbTex[slot] != MARNI_NULL_HANDLE) dx->DestroyTexture(s_thumbTex[slot]);
        s_thumbTex[slot] = dx->CreateTexture(kThumbW, kThumbH, 32, thumb, NULL, NULL);
        s_thumbLoaded[slot] = TRUE;

        char path[300];
        QuickPath(slot, path, sizeof(path), "thm");
        FILE* f = fopen(path, "wb");
        if (f != NULL) {
            const unsigned char header[8] = {
                'R', 'T', 'H', 'M',
                (unsigned char)(kThumbW & 0xFF), (unsigned char)(kThumbW >> 8),
                (unsigned char)(kThumbH & 0xFF), (unsigned char)(kThumbH >> 8),
            };
            fwrite(header, 1, sizeof(header), f);
            fwrite(thumb, 1, (size_t)kThumbW * kThumbH * 4, f);
            fclose(f);
        }
        free(thumb);
    }
    free(frame);
}

void EnsureTouch(void)
{
    if (s_touchReady) return;
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    memset(&s_panel, 0, sizeof(s_panel));
    sceTouchGetPanelInfo(SCE_TOUCH_PORT_FRONT, &s_panel);
    s_touchReady = TRUE;
}

// Front panel report -> screen pixels.
BOOL TouchToScreen(const SceTouchReport& r, int* x, int* y)
{
    const int spanX = s_panel.maxDispX - s_panel.minDispX;
    const int spanY = s_panel.maxDispY - s_panel.minDispY;
    if (spanX <= 0 || spanY <= 0) return FALSE;
    *x = (r.x - s_panel.minDispX) * VITA_SCREEN_W / spanX;
    *y = (r.y - s_panel.minDispY) * VITA_SCREEN_H / spanY;
    return TRUE;
}

// Screen pixels -> the 320x240 game picture the HUD layout is in.
void ScreenToGame(int sx, int sy, int* gx, int* gy)
{
    int bx, by, bw, bh;
    vita_game_box(&bx, &by, &bw, &bh);
    *gx = (sx - bx) * 320 / bw;
    *gy = (sy - by) * 240 / bh;
}

BOOL CornerTapped(void)
{
    EnsureTouch();

    SceTouchData touch;
    BOOL inCorner = FALSE;
    int x, y;
    if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1) > 0 && touch.reportNum > 0 &&
        TouchToScreen(touch.report[0], &x, &y)) {
        inCorner = (x >= VITA_SCREEN_W - kCornerSize && y < kCornerSize) ? TRUE : FALSE;
    }

    const BOOL tapped = inCorner && !s_prevTouch;
    s_prevTouch = inCorner;
    return tapped;
}

BOOL WidgetEnabled(int widget)
{
    return (g_portHud & (widget == 0 ? PORT_HUD_STATUS : PORT_HUD_WEAPON)) != 0;
}

int WidgetAt(int gx, int gy)
{
    for (int i = 1; i >= 0; --i) {
        if (!WidgetEnabled(i)) continue;
        int w, h;
        vita_hud_size(i, &w, &h);
        const PortHudWidget& l = g_portHudLayout[i];
        if (gx >= l.x && gx < l.x + w && gy >= l.y && gy < l.y + h) return i;
    }
    return -1;
}

// Resize about the widget's centre so it does not walk away while pinched.
void ScaleWidget(int widget, int scale)
{
    int w, h;
    vita_hud_size(widget, &w, &h);
    const int cx = g_portHudLayout[widget].x + w / 2;
    const int cy = g_portHudLayout[widget].y + h / 2;
    g_portHudLayout[widget].scale = (short)scale;
    vita_hud_clamp(widget);
    vita_hud_size(widget, &w, &h);
    g_portHudLayout[widget].x = (short)(cx - w / 2);
    g_portHudLayout[widget].y = (short)(cy - h / 2);
    vita_hud_clamp(widget);
}

void MoveWidget(int widget, int dx, int dy)
{
    g_portHudLayout[widget].x = (short)(g_portHudLayout[widget].x + dx);
    g_portHudLayout[widget].y = (short)(g_portHudLayout[widget].y + dy);
    vita_hud_clamp(widget);
}

void EditHudInput(unsigned int buttons, unsigned int pressed)
{
    EnsureTouch();

    SceTouchData touch;
    int count = 0;
    int gx[2] = { 0, 0 }, gy[2] = { 0, 0 };
    if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1) > 0) {
        for (unsigned i = 0; i < touch.reportNum && count < 2; ++i) {
            int sx, sy;
            if (!TouchToScreen(touch.report[i], &sx, &sy)) continue;
            ScreenToGame(sx, sy, &gx[count], &gy[count]);
            ++count;
        }
    }

    if (count == 0) {
        s_dragging = FALSE;
        s_pinching = FALSE;
    } else if (count == 1) {
        s_pinching = FALSE;
        if (!s_dragging) {
            const int hit = WidgetAt(gx[0], gy[0]);
            if (hit >= 0) {
                s_editSel = hit;
                s_dragging = TRUE;
            }
        } else {
            MoveWidget(s_editSel, gx[0] - s_dragLastX, gy[0] - s_dragLastY);
        }
        s_dragLastX = gx[0];
        s_dragLastY = gy[0];
    } else {
        // Two fingers: the gap between them sets the size of the widget the
        // first one grabbed (or the selected one, if it started off-widget).
        s_dragging = FALSE;
        const float ddx = (float)(gx[1] - gx[0]);
        const float ddy = (float)(gy[1] - gy[0]);
        const float dist = sqrtf(ddx * ddx + ddy * ddy);
        if (!s_pinching) {
            s_pinching = TRUE;
            s_pinchStartDist = dist;
            s_pinchStartScale = g_portHudLayout[s_editSel].scale;
        } else if (s_pinchStartDist > 4.0f) {
            ScaleWidget(s_editSel, (int)((float)s_pinchStartScale * dist / s_pinchStartDist + 0.5f));
        }
    }

    if ((pressed & SCE_CTRL_SQUARE) && WidgetEnabled(1 - s_editSel)) s_editSel = 1 - s_editSel;
    if (buttons & SCE_CTRL_UP)    MoveWidget(s_editSel, 0, -2);
    if (buttons & SCE_CTRL_DOWN)  MoveWidget(s_editSel, 0, 2);
    if (buttons & SCE_CTRL_LEFT)  MoveWidget(s_editSel, -2, 0);
    if (buttons & SCE_CTRL_RIGHT) MoveWidget(s_editSel, 2, 0);
    if (pressed & SCE_CTRL_LTRIGGER) ScaleWidget(s_editSel, g_portHudLayout[s_editSel].scale - 5);
    if (pressed & SCE_CTRL_RTRIGGER) ScaleWidget(s_editSel, g_portHudLayout[s_editSel].scale + 5);
    if (pressed & SCE_CTRL_TRIANGLE) vita_hud_reset_layout();

    if (pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_CROSS | SCE_CTRL_START)) {
        s_editHud = FALSE;
        s_dragging = FALSE;
        s_pinching = FALSE;
        s_waitRelease = TRUE;
        vita_save_video_settings();
    }
}

void DrawOutline(MarniDX* dx, int x, int y, int w, int h, DWORD color)
{
    dx->DrawRect(x, y, w, 2, color);
    dx->DrawRect(x, y + h - 2, w, 2, color);
    dx->DrawRect(x, y, 2, h, color);
    dx->DrawRect(x + w - 2, y, 2, h, color);
}

void DeleteSlot(int slot)
{
    char path[300];
    QuickPath(slot, path, sizeof(path), "dat");
    remove(path);
    QuickPath(slot, path, sizeof(path), "thm");
    remove(path);
    s_slotExists[slot] = 0;

    MarniDX* dx = Marni_DX();
    if (dx != NULL && s_thumbTex[slot] != MARNI_NULL_HANDLE) dx->DestroyTexture(s_thumbTex[slot]);
    s_thumbTex[slot] = MARNI_NULL_HANDLE;
    s_thumbLoaded[slot] = TRUE;
}

void Close(void)
{
    s_open = FALSE;
    s_waitRelease = TRUE;
    s_confirmDelete = FALSE;
    vita_save_video_settings();
}

void Change(int item, int dir)
{
    switch (item) {
    case ITEM_ASPECT:
        g_vitaVideo.aspect = (g_vitaVideo.aspect + dir + 3) % 3;
        break;
    case ITEM_FILTER:
        g_vitaVideo.linearFilter = !g_vitaVideo.linearFilter;
        break;
    case ITEM_GAMMA: {
        // Work in tenths so repeated steps land exactly on 1.0 again.
        int tenths = (int)(g_vitaVideo.gamma * 10.0f + 0.5f) + dir;
        if (tenths < 5) tenths = 5;
        if (tenths > 25) tenths = 25;
        g_vitaVideo.gamma = (float)tenths / 10.0f;
        break;
    }
    case ITEM_MSAA:
        g_vitaVideo.msaa = (dir > 0) ? (g_vitaVideo.msaa == 0 ? 2 : 4)
                                     : (g_vitaVideo.msaa == 4 ? 2 : 0);
        break;
    case ITEM_FRAME_RATE:
        g_vitaVideo.fps60 = !g_vitaVideo.fps60;
        g_portFrameInterp = g_vitaVideo.fps60 ? 1 : 0;
        break;
    case ITEM_SLOT:
        // The slot the next request means; a save already in flight keeps its own.
        g_portQuickSlot = (g_portQuickSlot + dir + PORT_QUICK_SLOT_COUNT) % PORT_QUICK_SLOT_COUNT;
        s_confirmDelete = FALSE;
        return;
    case ITEM_AUTO_RUN:    g_vitaGameplay.autoRun      = !g_vitaGameplay.autoRun;      break;
    case ITEM_WEAPON_SWAP:
        g_vitaGameplay.weaponSwitch = !g_vitaGameplay.weaponSwitch;
        if (g_vitaGameplay.weaponSwitch) SetStatus("RIGHT STICK CHANGES WEAPON");
        break;
    case ITEM_SKIP_DOORS:  g_vitaGameplay.skipDoors    = !g_vitaGameplay.skipDoors;    break;
    case ITEM_HEALTH_HUD:  g_vitaGameplay.healthHud    = !g_vitaGameplay.healthHud;    break;
    case ITEM_AMMO_HUD:    g_vitaGameplay.ammoHud      = !g_vitaGameplay.ammoHud;      break;
    case ITEM_HUD_AUTO_HIDE: g_vitaGameplay.hudAutoHide = !g_vitaGameplay.hudAutoHide; break;
    default:
        return;
    }
    if (item >= ITEM_AUTO_RUN) {
        vita_apply_gameplay_settings();
        return;
    }
    g_vitaVideoDirty = TRUE;
}

void Activate(int item)
{
    switch (item) {
    case ITEM_QUICK_SAVE:
        if (s_saving) return;
        if (!InGameplay()) { SetStatus("ONLY DURING GAMEPLAY"); return; }
        g_portQuickSaveResult = 0;
        g_portQuickSaveRequest = 1;
        s_saving = TRUE;
        s_savingSince = plat_time_ms();
        s_savingSlot = g_portQuickSlot;
        SetStatus("SAVING...");
        break;
    case ITEM_QUICK_LOAD:
        if (!QuickSaveExists(g_portQuickSlot)) { SetStatus("SLOT IS EMPTY"); return; }
        if (!InGameplay())       { SetStatus("ONLY DURING GAMEPLAY"); return; }
        g_portQuickLoadRequest = 1;
        Close();
        break;
    case ITEM_DELETE:
        if (!QuickSaveExists(g_portQuickSlot)) { SetStatus("SLOT IS EMPTY"); return; }
        if (s_saving) return;
        if (!s_confirmDelete) {
            s_confirmDelete = TRUE;
            SetStatus("PRESS X AGAIN TO DELETE");
            return;
        }
        DeleteSlot(g_portQuickSlot);
        s_confirmDelete = FALSE;
        SetStatus("DELETED");
        break;
    case ITEM_EDIT_HUD:
        if (!InGameplay())  { SetStatus("ONLY DURING GAMEPLAY"); return; }
        if (g_portHud == 0) { SetStatus("TURN A HUD ON FIRST"); return; }
        s_open = FALSE;
        s_editHud = TRUE;
        s_editSel = WidgetEnabled(0) ? 0 : 1;
        s_dragging = FALSE;
        s_pinching = FALSE;
        break;
    case ITEM_MOD_MANAGER:
        // Mods and the region are chosen before the game loads anything, so
        // this restarts the app into the manager.
        if (!s_confirmModManager) {
            s_confirmModManager = TRUE;
            SetStatus("X AGAIN: RESTART, UNSAVED PLAY IS LOST");
            return;
        }
        vita_mod_manager_relaunch();
        break;
    case ITEM_CLOSE:
        Close();
        break;
    default:
        Change(item, +1);
        break;
    }
}

void ValueText(int item, char* out, size_t size)
{
    switch (item) {
    case ITEM_ASPECT: {
        static const char* const kAspect[3] = { "4:3", "INTEGER", "STRETCH" };
        snprintf(out, size, "%s", kAspect[g_vitaVideo.aspect]);
        break;
    }
    case ITEM_FILTER: snprintf(out, size, "%s", g_vitaVideo.linearFilter ? "LINEAR" : "NEAREST"); break;
    case ITEM_GAMMA:  snprintf(out, size, "%.1f", g_vitaVideo.gamma); break;
    case ITEM_MSAA:
        if (g_vitaVideo.msaa == 0) snprintf(out, size, "OFF");
        else snprintf(out, size, "%dX", g_vitaVideo.msaa);
        break;
    case ITEM_FRAME_RATE: snprintf(out, size, "%s", g_vitaVideo.fps60 ? "60" : "30"); break;
    case ITEM_SLOT:   snprintf(out, size, "%d", g_portQuickSlot + 1); break;
    case ITEM_AUTO_RUN:    snprintf(out, size, "%s", g_vitaGameplay.autoRun ? "ON" : "OFF"); break;
    case ITEM_WEAPON_SWAP: snprintf(out, size, "%s", g_vitaGameplay.weaponSwitch ? "ON" : "OFF"); break;
    case ITEM_SKIP_DOORS:  snprintf(out, size, "%s", g_vitaGameplay.skipDoors ? "ON" : "OFF"); break;
    case ITEM_HEALTH_HUD:  snprintf(out, size, "%s", g_vitaGameplay.healthHud ? "ON" : "OFF"); break;
    case ITEM_AMMO_HUD:    snprintf(out, size, "%s", g_vitaGameplay.ammoHud ? "ON" : "OFF"); break;
    case ITEM_HUD_AUTO_HIDE: snprintf(out, size, "%s", g_vitaGameplay.hudAutoHide ? "ON" : "OFF"); break;
    default:          out[0] = '\0'; break;
    }
}

// fontus.tim's 8x8 region: ASCII from 0x20, 32 glyphs per 8-pixel row.
void DrawText(MarniDX* dx, float x, float y, const char* text, DWORD color, float cell = 16.0f)
{
    CMarniDirect3D* d3d = (CMarniDirect3D*)g_pMarniDirect3D;
    if (d3d == NULL || d3d->m_FontTexHandle == MARNI_NULL_HANDLE ||
        d3d->m_FontTexWidth <= 0 || d3d->m_FontTexHeight <= 0) {
        return;
    }

    const float tw = (float)d3d->m_FontTexWidth;
    const float th = (float)d3d->m_FontTexHeight;

    for (const char* c = text; *c != '\0'; ++c, x += cell) {
        const unsigned char ch = (unsigned char)*c;
        if (ch <= 0x20 || ch >= 0x80) continue;
        const float u = (float)((ch % 32) * 8);
        const float v = (float)(((ch - 0x20) / 32) * 8);
        dx->DrawSprite(x, y, cell, cell, u / tw, v / th, (u + 8.0f) / tw, (v + 8.0f) / th,
                       color, d3d->m_FontTexHandle, MARNI_SAMPLER_POINT, MARNI_BLEND_ALPHA);
    }
}

}  // namespace

BOOL vita_overlay_input(unsigned int buttons)
{
    if (s_bootMsaa < 0) s_bootMsaa = g_vitaVideo.msaa;

    if (g_portGameplayFrame != s_lastGameplayFrame) {
        s_lastGameplayFrame = g_portGameplayFrame;
        s_gameplayIdle = 0;
    } else if (s_gameplayIdle < 1000) {
        ++s_gameplayIdle;
    }

    // A request game_loop never picked up (the player went through a door or
    // the game ended in between) must not leave Quick Save stuck on "saving".
    if (s_saving && plat_time_ms() - s_savingSince > 3000) {
        g_portQuickSaveRequest = 0;
        s_saving = FALSE;
        SetStatus("CANNOT SAVE RIGHT NOW");
    }

    const unsigned int pressed = buttons & ~s_prevButtons;
    s_prevButtons = buttons;

    if (s_editHud) {
        EditHudInput(buttons, pressed);
        return TRUE;
    }

    // The LiveArea's Mod Manager link tapped while the game runs: open the
    // panel on the MOD MANAGER row instead of throwing the session away.
    if (vita_mod_manager_event()) {
        s_open = TRUE;
        s_page = PAGE_GAMEPLAY;
        s_cursor = 0;
        while (s_cursor < PageRows(s_page) && kPageItems[s_page][s_cursor] != ITEM_MOD_MANAGER) ++s_cursor;
        s_confirmModManager = TRUE;
        SetStatus("X: RESTART INTO THE MOD MANAGER");
        return TRUE;
    }

    const BOOL tapped = CornerTapped();
    const BOOL chord = (buttons & kChord) == kChord && (pressed & kChord) != 0;

    if (s_waitRelease) {
        if (buttons == 0) s_waitRelease = FALSE;
        return TRUE;
    }

    if (!s_open) {
        if (chord || tapped) {
            s_open = TRUE;
            s_page = PAGE_SAVES;      // quick saves are what the panel is opened for most
            s_cursor = 0;
            s_confirmDelete = FALSE;
            for (int i = 0; i < PORT_QUICK_SLOT_COUNT; ++i) s_slotExists[i] = -1;
            return TRUE;
        }
        return FALSE;
    }

    if (chord || tapped || (pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_START))) {
        Close();
        return TRUE;
    }

    if (pressed & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER)) {
        const int dir = (pressed & SCE_CTRL_RTRIGGER) ? 1 : PAGE_COUNT - 1;
        s_page = (s_page + dir) % PAGE_COUNT;
        s_cursor = 0;
        s_confirmDelete = FALSE;
        return TRUE;
    }

    const int rows = PageRows(s_page);
    if (pressed & (SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER)) {
        s_confirmDelete = FALSE;
        s_confirmModManager = FALSE;
    }
    if (pressed & SCE_CTRL_UP)    s_cursor = (s_cursor + rows - 1) % rows;
    if (pressed & SCE_CTRL_DOWN)  s_cursor = (s_cursor + 1) % rows;

    const int item = kPageItems[s_page][s_cursor];
    if (pressed & SCE_CTRL_LEFT)  Change(item, -1);
    if (pressed & SCE_CTRL_RIGHT) Change(item, +1);
    if (pressed & SCE_CTRL_CROSS) Activate(item);
    return TRUE;
}

BOOL vita_overlay_visible(void)
{
    return s_open || s_editHud;
}

// Called by MarniDX::Present with the game's frame complete and nothing of the
// panel drawn yet - the one moment the backbuffer is a clean screenshot.
void vita_overlay_before_draw(MarniDX* dx)
{
    if (!s_saving || g_portQuickSaveResult == 0 || dx == NULL) return;

    if (g_portQuickSaveResult > 0) {
        s_slotExists[s_savingSlot] = 1;
        CaptureThumbnail(dx, s_savingSlot);
        SetStatus("SAVED");
    } else {
        SetStatus("CANNOT SAVE RIGHT NOW");
    }
    g_portQuickSaveResult = 0;
    s_saving = FALSE;
}

void vita_overlay_draw(MarniDX* dx)
{
    if (dx == NULL) return;

    if (s_editHud) {
        int bx, by, bw, bh;
        vita_game_box(&bx, &by, &bw, &bh);
        for (int i = 0; i < 2; ++i) {
            if (!WidgetEnabled(i)) continue;
            int w, h;
            vita_hud_size(i, &w, &h);
            const int x = bx + g_portHudLayout[i].x * bw / 320;
            const int y = by + g_portHudLayout[i].y * bh / 240;
            DrawOutline(dx, x - 2, y - 2, w * bw / 320 + 4, h * bh / 240 + 4,
                        i == s_editSel ? 0xFFFFD040u : 0xFF808080u);
        }
        dx->DrawRect(bx + 8, by + 8, 452, 40, 0xC0000000u);
        DrawText(dx, (float)bx + 14.0f, (float)by + 12.0f,
                 "DRAG TO MOVE  PINCH TO RESIZE", 0xFFFFD040u, 12.0f);
        DrawText(dx, (float)bx + 14.0f, (float)by + 30.0f,
                 "SQUARE NEXT  TRIANGLE RESET  O DONE", 0xFFC8C8C8u, 12.0f);
        return;
    }

    if (!s_open) return;

    const BOOL saves = (s_page == PAGE_SAVES);
    const int slot = g_portQuickSlot;
    if (saves && !s_thumbLoaded[slot]) LoadThumbnail(dx, slot);

    const float w = saves ? 720.0f : 456.0f, h = 336.0f;
    const float x = (VITA_SCREEN_W - w) / 2.0f;
    const float y = (VITA_SCREEN_H - h) / 2.0f;

    dx->DrawRect((int)x, (int)y, (int)w, (int)h, 0xD8000000u);
    dx->DrawRect((int)x, (int)y, (int)w, 2, 0xFFB08040u);
    dx->DrawRect((int)x, (int)(y + h - 2), (int)w, 2, 0xFFB08040u);

    char title[24];
    snprintf(title, sizeof(title), "L %s R", kPageNames[s_page]);
    DrawText(dx, x + 24.0f, y + 16.0f, title, 0xFFB08040u);

    for (int row = 0; row < PageRows(s_page); ++row) {
        const int item = kPageItems[s_page][row];
        const float rowY = y + 56.0f + (float)row * 28.0f;
        const BOOL greyed = ((item == ITEM_QUICK_SAVE || item == ITEM_QUICK_LOAD) && !InGameplay()) ||
                            ((item == ITEM_QUICK_LOAD || item == ITEM_DELETE) && !QuickSaveExists(slot));
        const DWORD color = (row == s_cursor) ? 0xFFFFD040u : (greyed ? 0xFF707070u : 0xFFC8C8C8u);

        if (row == s_cursor) {
            dx->DrawRect((int)x + 12, (int)rowY - 4, 420, 24, 0x40FFD040u);
        }
        DrawText(dx, x + 24.0f, rowY, kLabels[item], color);

        char value[24];
        ValueText(item, value, sizeof(value));
        if (value[0] != '\0') {
            char line[32];
            snprintf(line, sizeof(line), "< %s >", value);
            DrawText(dx, x + 224.0f, rowY, line, color);
        }
    }

    if (saves) {
        // Quick save preview, 1.4x the stored 160x120.
        const float tx = x + 456.0f, ty = y + 56.0f, tw = 224.0f, th = 168.0f;
        char caption[16];
        snprintf(caption, sizeof(caption), "SLOT %d", slot + 1);
        DrawText(dx, tx, y + 16.0f, caption, 0xFFB08040u);
        dx->DrawRect((int)tx - 2, (int)ty - 2, (int)tw + 4, (int)th + 4, 0xFF404040u);
        if (s_thumbTex[slot] != MARNI_NULL_HANDLE) {
            dx->DrawSprite(tx, ty, tw, th, 0.0f, 0.0f, 1.0f, 1.0f, 0xFFFFFFFFu, s_thumbTex[slot],
                           MARNI_SAMPLER_LINEAR, MARNI_BLEND_DISABLE);
        } else {
            dx->DrawRect((int)tx, (int)ty, (int)tw, (int)th, 0xFF101010u);
            DrawText(dx, tx + 72.0f, ty + 76.0f, "EMPTY", 0xFF707070u);
        }
    }

    const float footY = y + h - 32.0f;
    if (s_status[0] != '\0' && (int)(s_statusUntil - plat_time_ms()) > 0) {
        DrawText(dx, x + 24.0f, footY, s_status, 0xFFFFD040u);
    } else if (g_vitaVideo.msaa != s_bootMsaa) {
        DrawText(dx, x + 24.0f, footY, "MSAA: RESTART TO APPLY", 0xFF8080FFu);
    }
}
