// input.cpp - PS Vita input backend.
//
// Counterpart of src/platform/linux/input.cpp. The built-in pad (or the PSTV's
// paired controller, which sceCtrl reports on the same port) is always present,
// so joysticks[0] is permanently enabled and gets the same Marni mask the
// XInput/SDL backends build - see docs/GAMEPAD_INPUT.md for the bit layout.
#include "Globals.h"
#include "platform/platform.h"
#include "marni/MarniInput.h"
#include "marni/MarniXInput.h"

#include "vita.h"

#include <psp2/ctrl.h>

// Sticks report 0..255 with 128 at rest. 64 is roughly the same share of the
// range as XInput's 10000/32767 deadzone the other backends use.
static const int kStickDeadzone = 64;

// Right stick weapon flick: fire past 80 from centre, re-arm once back inside
// 40, so one flick is one weapon however long the stick is held over.
static const int kFlickFire  = 80;
static const int kFlickReset = 40;
static BOOL s_flickArmed = TRUE;

static void ReadWeaponFlick(const SceCtrlData& pad)
{
    const int rx = (int)pad.rx - 128;
    if (!s_flickArmed) {
        if (rx > -kFlickReset && rx < kFlickReset) s_flickArmed = TRUE;
        return;
    }
    if (!g_vitaGameplay.weaponSwitch) return;
    if (rx >= kFlickFire || rx <= -kFlickFire) {
        g_portWeaponCycleRequest = (rx > 0) ? 1 : -1;
        s_flickArmed = FALSE;
    }
}

static DWORD BuildPadMask(void)
{
    SceCtrlData pad;
    if (sceCtrlPeekBufferPositive(0, &pad, 1) < 0) return 0;

    if (vita_overlay_input(pad.buttons)) return 0;
    ReadWeaponFlick(pad);

    // L+R+Select is the overlay chord. Hide Select from the game while both
    // shoulders are down so the chord does not also open the options screen.
    unsigned int b = pad.buttons;
    if ((b & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER)) == (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER)) {
        b &= ~(unsigned int)SCE_CTRL_SELECT;
    }
    const int lx = (int)pad.lx - 128;
    const int ly = (int)pad.ly - 128;

    DWORD mask = 0;
    if ((b & SCE_CTRL_UP)    || ly < -kStickDeadzone) mask |= 1;
    if ((b & SCE_CTRL_DOWN)  || ly >  kStickDeadzone) mask |= 2;
    if ((b & SCE_CTRL_LEFT)  || lx < -kStickDeadzone) mask |= 4;
    if ((b & SCE_CTRL_RIGHT) || lx >  kStickDeadzone) mask |= 8;

    if (mask & 1) mask &= ~2u;
    if (mask & 4) mask &= ~8u;

    // Face buttons by position, same as the SDL backend maps a DualShock:
    // Cross = A (action), Circle = B (cancel/run), Square = X, Triangle = Y.
    if (b & SCE_CTRL_CROSS)    mask |= MARNI_XI_BTN_A;
    if (b & SCE_CTRL_CIRCLE)   mask |= MARNI_XI_BTN_B;
    if (b & SCE_CTRL_SQUARE)   mask |= MARNI_XI_BTN_X;
    if (b & SCE_CTRL_TRIANGLE) mask |= MARNI_XI_BTN_Y;

    // The handheld's shoulders come through as the "trigger" bits; they are
    // the only shoulders it has, so they take the LB/RB slots.
    if (b & SCE_CTRL_LTRIGGER) mask |= MARNI_XI_BTN_LB;
    if (b & SCE_CTRL_RTRIGGER) mask |= MARNI_XI_BTN_RB;
    if (b & SCE_CTRL_SELECT)   mask |= MARNI_XI_BTN_BACK;
    if (b & SCE_CTRL_START)    mask |= MARNI_XI_BTN_START;

    return mask;
}

void CMarniDirectInput::UpdateKeyboardInputState(MasterInputState* pState)
{
    pState->keyboardCurr = 0;
    for (int i = 0; i < 32; i++) {
        if (plat_key_state(pState->keyMap[i]) & 0x8000) {
            pState->keyboardCurr |= (1u << i);
        }
    }
}

void CMarniDirectInput::UpdateAllInputStates(MasterInputState* pState)
{
    UpdateKeyboardInputState(pState);

    DWORD oldPrev = pState->keyboardPrev;
    pState->keyboardRepeat   = oldPrev;
    pState->keyboardPrev     = pState->keyboardCurr;
    pState->frameFlag        = 1;
    pState->keyboardNewPress = (~oldPrev) & pState->keyboardCurr;

    JoystickEntry* pJoy = &pState->joysticks[0];
    DWORD padMask = BuildPadMask();

    pJoy->enabled   = 1;
    pJoy->prevPress = pJoy->currPress;
    pJoy->currPress = padMask;
    pJoy->newPress  = (~pJoy->prevPress) & padMask;
}

void CMarniDirectInput::InitJoysticks(MasterInputState* pState)
{
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    pState->joystickCount = 1u;
}

bool MarniPadIsConnected(void)
{
    return true;
}

namespace MarniXInput {
bool IsConnected() { return true; }
}
