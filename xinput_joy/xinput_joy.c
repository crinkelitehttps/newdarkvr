/*
 * dinput.dll: a synthetic legacy-DirectInput joystick device, backed by an XInput (Xbox) controller,
 * for Thief II (NewDark). The engine already has real, working analog-joystick support (original ~2000
 * Dark Engine code): it calls LoadLibraryA("dinput.dll") + GetProcAddress("DirectInputCreateA") itself
 * at startup, and its default binds (`joy_axisy` -> `+joyforward`, etc.) and Options -> Controls menu
 * ("Joystick: On", "Customize Controls...") already expect exactly this. No such DLL ships with the
 * install, so this feature has been dead code until now -- see docs/DEVLOG.md and HANDOFF.md.
 *
 * Unlike headlook.dll, this needs NO exe patching at all: the engine loads this DLL itself, by name.
 * bin/Thief2.orig.exe is untouched by this feature.
 *
 * Architecture:
 *   - DirectInputCreateA (exported) hands back a singleton IDirectInputA-shaped object.
 *   - Its EnumDevices/CreateDevice report exactly one fake joystick device (only if an XInput pad is
 *     actually connected), backed by a singleton IDirectInputDevice2A-shaped object.
 *   - GetDeviceState/Poll call XInputGetState (loaded dynamically: xinput1_4 -> xinput1_3 -> 9_1_0,
 *     matching this project's existing dynamic-LoadLibrary/GetProcAddress idiom) and fill a classic
 *     DIJOYSTATE-shaped buffer (xj_djoystate_t, see didefs.h) with:
 *       - left stick  -> lX/lY  (drives the engine's existing joy_axisx/+joyxaxis, joy_axisy/+joyforward
 *         binds -- this is the analog movement-speed control)
 *       - right stick -> lRz/lZ (drives joy_axisr/rudderturn by default)
 *       - a virtual button bank (see bank_layout() below) with a chording/"shift" scheme: one
 *         configurable physical button (default Left Shoulder) gives every other tracked button, plus
 *         the left stick's 4 directions, a second, distinct bindable identity while held -- so e.g.
 *         "A" and "LB+A" can be bound to two different actions in the game's own "Customize Controls..."
 *         screen, with zero engine-side changes.
 *   - D-pad -> the POV hat (rgdwPOV[0]), not the button bank -- free functionality (joy_hat* binds).
 *
 * Every vtable method is really implemented (not stubbed) and every call is logged (dinput.log, next to
 * this DLL) with args and, for structs we're not 100% sure of the engine's exact usage of yet, a hex
 * dump -- this DLL IS the reverse-engineering tool for Phase 0 (see DEVLOG/HANDOFF): unlike headlook.c,
 * where the unknown code lived inside the opaque exe, here every call the engine makes lands in our own
 * C code, so there is no need to guess from outside.
 *
 * Config: xinput_joy.ini next to this DLL, hot-reloaded every 2s (same idiom as headlook.ini).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "didefs.h"

/* ------------------------------------------------------------------ config and logging (headlook.c idiom) */

typedef enum { SB_LB, SB_RB, SB_LT, SB_RT, SB_LTHUMB, SB_RTHUMB, SB_BACK, SB_START } shift_button_t;

typedef struct {
    int enabled;
    int xinput_index;         /* 0-3: which XInput pad slot */
    int log_verbose;          /* 0 = quiet, 1 = call summary, 2 = full struct hex dumps */
    int invert_lx, invert_ly, invert_rx;
    shift_button_t shift_button;
    double trigger_threshold;      /* fraction 0..1 of trigger travel, for LT/RT-as-button */
    double stick_button_threshold; /* fraction 0..1 of stick travel, for left-stick-direction-as-button */
    int dpad_as_buttons;           /* 0 = D-pad is the POV hat only (default) */
    double stick_deadzone;         /* fraction 0..1, applied (with rescale) to the LEFT stick (movement) --
                                       the engine's own joystick_deadzone/rudder_deadzone weren't enough to
                                       stop drift in testing, so we no longer rely on it alone */
    double mouselook_deadzone;     /* separate, usually larger deadzone for the RIGHT stick (mouselook) --
                                       kept apart from stick_deadzone because a little residual movement
                                       creep is far less noticeable than the camera slowly drifting */
    int mouselook_enabled;         /* 1 = right stick drives synthetic mouse movement (yaw+pitch) instead
                                       of the classic joystick R/Z axes -- this engine's joystick support
                                       has no analog look/pitch bind at all, only mouselook responds to it */
    double mouselook_sensitivity;  /* pixels/poll-tick at full deflection; a rough guess, tune live */
    int mouselook_invert_y;
    int back_as_escape;            /* 1 = Back/View (select) presses Esc, instead of being joystick button joy8 */
    int rt_as_mouse1;              /* 1 = right trigger holds the left mouse button, instead of joystick button joy7 */
} config_t;

static config_t cfg;
static char g_dir[MAX_PATH];
static FILE *g_log;
static CRITICAL_SECTION g_lock; /* guards cfg + device acquire/format state; XInput itself needs no lock */

static void xj_log(const char *fmt, ...)
{
    va_list ap;
    SYSTEMTIME t;
    if (!g_log) return;
    GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

/* Phase-0 discovery helper: hex-dump a struct we're not 100% sure of the engine's usage of yet. */
static void xj_log_hex(const char *label, const void *p, size_t n)
{
    const unsigned char *b = (const unsigned char *)p;
    char line[3 * 32 + 1];
    size_t i, j, len;
    if (!g_log || cfg.log_verbose < 2 || !p) return;
    for (i = 0; i < n; i += 16) {
        len = (n - i < 16) ? (n - i) : 16;
        for (j = 0; j < len; j++) snprintf(line + j * 3, 4, "%02x ", b[i + j]);
        xj_log("  %s[%zu]: %s", label, i, line);
    }
}

static const char *guid_name(const GUID *g)
{
    if (!g) return "(null)";
    /* DirectInput property "GUIDs" (DIPROP_RANGE etc.) are really tiny integers cast to a pointer via
     * MAKEDIPROP -- NOT real memory addresses. IsEqualGUID() below unconditionally dereferences its
     * argument, so checking those small values first (plain pointer comparison, never dereferenced) is
     * required, not just tidy: without this, the first SetProperty/GetProperty call after Acquire reads
     * from an address like 0x4 and crashes -- confirmed via a native-Windows crash at this exact line
     * (dinput.dll+0x1d88 per Windows Event Viewer), fixed here. Any other small value (a property GUID
     * we haven't named) is still caught by the generic "< 0x10000" guard below, so this can't regress
     * the same way for a property we didn't think to list explicitly. */
    if ((UINT_PTR)g < 0x10000) {
        if (g == XJ_DIPROP_BUFFERSIZE) return "DIPROP_BUFFERSIZE";
        if (g == XJ_DIPROP_AXISMODE) return "DIPROP_AXISMODE";
        if (g == XJ_DIPROP_GRANULARITY) return "DIPROP_GRANULARITY";
        if (g == XJ_DIPROP_RANGE) return "DIPROP_RANGE";
        if (g == XJ_DIPROP_DEADZONE) return "DIPROP_DEADZONE";
        if (g == XJ_DIPROP_SATURATION) return "DIPROP_SATURATION";
        return "(unrecognized small/property value)";
    }
    if (IsEqualGUID(g, &XJ_IID_IUnknown)) return "IUnknown";
    if (IsEqualGUID(g, &XJ_IID_IDirectInputA)) return "IDirectInputA";
    if (IsEqualGUID(g, &XJ_IID_IDirectInput2A)) return "IDirectInput2A";
    if (IsEqualGUID(g, &XJ_IID_IDirectInput7A)) return "IDirectInput7A";
    if (IsEqualGUID(g, &XJ_IID_IDirectInputDeviceA)) return "IDirectInputDeviceA";
    if (IsEqualGUID(g, &XJ_IID_IDirectInputDevice2A)) return "IDirectInputDevice2A";
    if (IsEqualGUID(g, &XJ_IID_IDirectInputDevice7A)) return "IDirectInputDevice7A";
    if (IsEqualGUID(g, &XJ_GUID_Joystick)) return "GUID_Joystick";
    if (IsEqualGUID(g, &XJ_GUID_OurDevice)) return "GUID_OurDevice";
    return "(unrecognized GUID)";
}

static double ini_double(const char *ini, const char *key, double def)
{
    char buf[64], dbuf[64];
    snprintf(dbuf, sizeof dbuf, "%g", def);
    GetPrivateProfileStringA("xinput_joy", key, dbuf, buf, sizeof buf, ini);
    return atof(buf);
}

static shift_button_t parse_shift_button(const char *v)
{
    if (!_stricmp(v, "RB")) return SB_RB;
    if (!_stricmp(v, "LT")) return SB_LT;
    if (!_stricmp(v, "RT")) return SB_RT;
    if (!_stricmp(v, "LeftThumb")) return SB_LTHUMB;
    if (!_stricmp(v, "RightThumb")) return SB_RTHUMB;
    if (!_stricmp(v, "Back")) return SB_BACK;
    if (!_stricmp(v, "Start")) return SB_START;
    if (_stricmp(v, "LB") != 0) xj_log("unknown shift_button '%s' (use LB|RB|LT|RT|LeftThumb|RightThumb|Back|Start): defaulting to LB", v);
    return SB_LB;
}

static void load_config(void)
{
    char ini[MAX_PATH + 16], sv[32];
    snprintf(ini, sizeof ini, "%sxinput_joy.ini", g_dir);
    cfg.enabled       = (int)ini_double(ini, "enabled", 1) != 0;
    cfg.xinput_index  = (int)ini_double(ini, "xinput_index", 0);
    if (cfg.xinput_index < 0 || cfg.xinput_index > 3) cfg.xinput_index = 0;
    cfg.log_verbose   = (int)ini_double(ini, "log_verbose", 1);
    cfg.invert_lx     = (int)ini_double(ini, "invert_lx", 0) != 0;
    cfg.invert_ly     = (int)ini_double(ini, "invert_ly", 0) != 0;
    cfg.invert_rx     = (int)ini_double(ini, "invert_rx", 0) != 0;
    cfg.trigger_threshold      = ini_double(ini, "trigger_threshold", 0.5);
    cfg.stick_button_threshold = ini_double(ini, "stick_button_threshold", 0.5);
    cfg.dpad_as_buttons        = (int)ini_double(ini, "dpad_as_buttons", 0) != 0;
    cfg.stick_deadzone         = ini_double(ini, "stick_deadzone", 0.08);
    cfg.mouselook_deadzone     = ini_double(ini, "mouselook_deadzone", 0.15);
    cfg.mouselook_enabled      = (int)ini_double(ini, "mouselook_enabled", 1) != 0;
    cfg.mouselook_sensitivity  = ini_double(ini, "mouselook_sensitivity", 18.0);
    cfg.mouselook_invert_y     = (int)ini_double(ini, "mouselook_invert_y", 0) != 0;
    cfg.back_as_escape         = (int)ini_double(ini, "back_as_escape", 1) != 0;
    cfg.rt_as_mouse1           = (int)ini_double(ini, "rt_as_mouse1", 1) != 0;
    GetPrivateProfileStringA("xinput_joy", "shift_button", "LB", sv, sizeof sv, ini);
    cfg.shift_button = parse_shift_button(sv);
}

/* ------------------------------------------------------------------ XInput backend (dynamic load) */

typedef DWORD (WINAPI *pfn_XInputGetState_t)(DWORD, XINPUT_STATE *);
static pfn_XInputGetState_t p_XInputGetState;
static const char *g_xinput_dll_used;

static void load_xinput(void)
{
    static const char *cands[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll", NULL };
    int i;
    for (i = 0; cands[i]; i++) {
        HMODULE h = LoadLibraryA(cands[i]);
        if (!h) continue;
        p_XInputGetState = (pfn_XInputGetState_t)GetProcAddress(h, "XInputGetState");
        if (p_XInputGetState) { g_xinput_dll_used = cands[i]; xj_log("loaded %s for XInputGetState", cands[i]); return; }
        FreeLibrary(h);
    }
    xj_log("no XInput DLL found (tried xinput1_4/1_3/9_1_0) -- pad will always read as unplugged");
}

static BOOL pad_connected(void)
{
    XINPUT_STATE st;
    if (!p_XInputGetState) return FALSE;
    return p_XInputGetState((DWORD)cfg.xinput_index, &st) == ERROR_SUCCESS;
}

/* ------------------------------------------------------------------ button bank / shift-chording */

/* 11 "real button" targets (indices 0..10, each gets an unshifted virtual id joy1..joy11) followed by
 * 4 left-stick-direction targets (indices 11..14, no unshifted id of their own -- unshifted they stay
 * pure analog). All 15 get a shifted id at 11+i (joy12..joy26) while the modifier is held. See DEVLOG
 * plan doc "Buttons + shift/chording" for the reasoning and the budget (27 of 32 rgbButtons slots). */
typedef enum {
    T_A, T_B, T_X, T_Y, T_RB, T_LT, T_RT, T_BACK, T_START, T_LTHUMB, T_RTHUMB,
    T_LSTICK_UP, T_LSTICK_DOWN, T_LSTICK_LEFT, T_LSTICK_RIGHT,
    T_COUNT
} target_t;
#define T_BUTTON_COUNT 11          /* targets 0..T_BUTTON_COUNT-1 have a real unshifted identity */
#define SHIFT_BANK_OFFSET 11       /* virtual index of a shifted target i is SHIFT_BANK_OFFSET + i */
#define MODIFIER_ALONE_INDEX (SHIFT_BANK_OFFSET + T_COUNT)  /* = 26 -> joy27 */

/* If the configured shift button aliases one of our normal targets (e.g. shift_button=RB, and RB is
 * also T_RB), that target is excluded from normal processing -- the physical button plays modifier only. */
static int modifier_target_alias(shift_button_t sb)
{
    switch (sb) {
    case SB_RB:      return T_RB;
    case SB_LT:      return T_LT;
    case SB_RT:      return T_RT;
    case SB_LTHUMB:  return T_LTHUMB;
    case SB_RTHUMB:  return T_RTHUMB;
    case SB_BACK:    return T_BACK;
    case SB_START:   return T_START;
    default:         return -1; /* LB: not one of our targets */
    }
}

static BOOL modifier_state(shift_button_t sb, const XINPUT_GAMEPAD *gp, double trig_thresh)
{
    switch (sb) {
    case SB_LB:     return (gp->wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;
    case SB_RB:     return (gp->wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;
    case SB_LT:     return gp->bLeftTrigger  > (BYTE)(trig_thresh * 255);
    case SB_RT:     return gp->bRightTrigger > (BYTE)(trig_thresh * 255);
    case SB_LTHUMB: return (gp->wButtons & XINPUT_GAMEPAD_LEFT_THUMB) != 0;
    case SB_RTHUMB: return (gp->wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) != 0;
    case SB_BACK:   return (gp->wButtons & XINPUT_GAMEPAD_BACK) != 0;
    case SB_START:  return (gp->wButtons & XINPUT_GAMEPAD_START) != 0;
    }
    return FALSE;
}

static BOOL target_state(target_t t, const XINPUT_GAMEPAD *gp)
{
    SHORT stk_thresh = (SHORT)(cfg.stick_button_threshold * 32767);
    switch (t) {
    case T_A:      return (gp->wButtons & XINPUT_GAMEPAD_A) != 0;
    case T_B:      return (gp->wButtons & XINPUT_GAMEPAD_B) != 0;
    case T_X:      return (gp->wButtons & XINPUT_GAMEPAD_X) != 0;
    case T_Y:      return (gp->wButtons & XINPUT_GAMEPAD_Y) != 0;
    case T_RB:     return (gp->wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;
    case T_LT:     return gp->bLeftTrigger  > (BYTE)(cfg.trigger_threshold * 255);
    case T_RT:     return gp->bRightTrigger > (BYTE)(cfg.trigger_threshold * 255);
    case T_BACK:   return (gp->wButtons & XINPUT_GAMEPAD_BACK) != 0;
    case T_START:  return (gp->wButtons & XINPUT_GAMEPAD_START) != 0;
    case T_LTHUMB: return (gp->wButtons & XINPUT_GAMEPAD_LEFT_THUMB) != 0;
    case T_RTHUMB: return (gp->wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) != 0;
    case T_LSTICK_UP:    return gp->sThumbLY >  stk_thresh;
    case T_LSTICK_DOWN:  return gp->sThumbLY < -stk_thresh;
    case T_LSTICK_LEFT:  return gp->sThumbLX < -stk_thresh;
    case T_LSTICK_RIGHT: return gp->sThumbLX >  stk_thresh;
    default: return FALSE;
    }
}

/* Buttons sent as keyboard/mouse input by key_mouse_thread() instead: never reported as joystick buttons
 * (neither alone nor in a chord), so an old bind on them can't fire as well. Not when the button is the
 * chording modifier itself (shift_button), which keeps that role. */
static BOOL target_remapped(target_t t)
{
    if (t == T_BACK && cfg.back_as_escape && cfg.shift_button != SB_BACK) return TRUE;
    if (t == T_RT && cfg.rt_as_mouse1 && cfg.shift_button != SB_RT) return TRUE;
    return FALSE;
}

/* Fills rgbButtons[0..26] (32-wide array; caller zeroes it first). Returns whether the modifier is
 * currently held, so compute_axes() can suppress left-stick movement while chording. */
/* Reporting "modifier held alone" the instant the modifier goes down (the original design) made the
 * game's own bind-capture screen grab it immediately, before the user had any chance to also press a
 * second button to form a chord -- confirmed in testing. Fixed by deferring: while the modifier is
 * held, ONLY chord targets are reported (shifted ids), never modifier-alone; modifier-alone is reported
 * as a single one-tick "pulse" at the moment the modifier is RELEASED, and only if no target was ever
 * pressed during that hold (a chord happening at any point during the hold cancels it, even if that
 * target is released again before the modifier is). This means an action bound to modifier-alone only
 * ever sees a brief tap, never a sustained hold -- fine for a toggle/click action, not for one meant to
 * be held down continuously; flagged here deliberately, not fixable without losing the chord-vs-solo
 * disambiguation the release-wait itself is for. */
static BOOL compute_buttons(const XINPUT_GAMEPAD *gp, BYTE *rgbButtons)
{
    BOOL tstate[T_COUNT];
    BOOL mod_held = modifier_state(cfg.shift_button, gp, cfg.trigger_threshold);
    int alias = modifier_target_alias(cfg.shift_button);
    int i;
    static BOOL mod_prev_held;
    static BOOL mod_chorded;

    for (i = 0; i < T_COUNT; i++)
        tstate[i] = (i == alias || target_remapped((target_t)i)) ? FALSE : target_state((target_t)i, gp);

    if (mod_held && !mod_prev_held) mod_chorded = FALSE; /* modifier just went down: start a fresh hold */

    if (mod_held) {
        for (i = 0; i < T_COUNT; i++) {
            if (!tstate[i]) continue;
            mod_chorded = TRUE;
            rgbButtons[SHIFT_BANK_OFFSET + i] = 0x80;
        }
    } else {
        if (mod_prev_held && !mod_chorded) rgbButtons[MODIFIER_ALONE_INDEX] = 0x80; /* release-pulse */
        for (i = 0; i < T_BUTTON_COUNT; i++)
            if (tstate[i]) rgbButtons[i] = 0x80;
        /* stick-direction targets (i >= T_BUTTON_COUNT) have no unshifted id -- stay pure analog */
    }
    mod_prev_held = mod_held;
    return mod_held;
}

/* ------------------------------------------------------------------ axis mapping */

static LONG scale_axis(double frac /* -1..1 */, LONG lo, LONG hi)
{
    double mid = (lo + hi) / 2.0, half = (hi - lo) / 2.0;
    double v = mid + frac * half;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return (LONG)v;
}

/* Radial deadzone with rescale: values inside `dz` collapse to exactly 0 (fixes stick-center drift --
 * the engine's own joystick_deadzone/rudder_deadzone weren't enough in testing), and the remaining
 * travel is rescaled so full physical deflection still reaches -1..1 rather than topping out early. */
static double apply_deadzone(double frac /* -1..1 */, double dz)
{
    double a = frac < 0 ? -frac : frac;
    if (a <= dz) return 0.0;
    if (dz >= 1.0) return 0.0;
    a = (a - dz) / (1.0 - dz);
    return frac < 0 ? -a : a;
}

/* Right-stick-as-mouselook: relative SendInput, only while our own process's window has focus (so it
 * never moves the mouse elsewhere, e.g. after alt-tab). No analog look/pitch bind exists anywhere in
 * this engine's classic joystick support to hook instead -- mouselook is the only thing pitch responds
 * to at all, so this bypasses DirectInput entirely for the right stick. */
static void inject_mouselook(double rx, double ry)
{
    HWND fg;
    DWORD fg_pid;
    INPUT in;
    LONG dx, dy;

    if (rx == 0.0 && ry == 0.0) return;
    fg = GetForegroundWindow();
    if (!fg) return;
    GetWindowThreadProcessId(fg, &fg_pid);
    if (fg_pid != GetCurrentProcessId()) return;

    dx = (LONG)(rx * cfg.mouselook_sensitivity);
    dy = (LONG)((cfg.mouselook_invert_y ? ry : -ry) * cfg.mouselook_sensitivity); /* stick up -> look up -> mouse moves up (screen -Y) by default */
    if (dx == 0 && dy == 0) return;
    memset(&in, 0, sizeof in);
    in.type = INPUT_MOUSE;
    in.mi.dx = dx;
    in.mi.dy = dy;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &in, sizeof(INPUT));
}

/* ------------------------------------------------------------------ the synthetic joystick device */

typedef struct {
    const XJDeviceVtbl *lpVtbl;
    LONG refcount;
    BOOL acquired;
    DWORD data_format_size;         /* from SetDataFormat's dwDataSize; 0 = not set yet */
    XJ_DIPROPRANGE axis_range[4];   /* X, Y, Z, Rz -- defaults set in device_init() */
    xj_djoystate_t state;           /* latest polled state */
    DWORD state_seq;                /* bumped whenever Poll() actually changes something (for DI_NOEFFECT) */
} XJDeviceObj;

static XJDeviceObj g_device;
static BOOL g_device_inited;

static void device_poll_locked(XJDeviceObj *d)
{
    XINPUT_STATE xs;
    xj_djoystate_t ns;
    DWORD rc;
    static DWORD last_rc = (DWORD)-1;
    static unsigned fail_count;
    memset(&ns, 0, sizeof ns);

    rc = p_XInputGetState ? p_XInputGetState((DWORD)cfg.xinput_index, &xs) : (DWORD)-1;
    if (rc != ERROR_SUCCESS) {
        /* Throttled diagnostic: log on every rc change and every 300th repeat (~3s at 100Hz polling),
         * so a persistent failure is visible without flooding the log at verbose 2. */
        if (rc != last_rc || (++fail_count % 300) == 1)
            xj_log("device_poll_locked: XInputGetState(index=%d) failed, rc=%lu%s",
                   cfg.xinput_index, (unsigned long)rc, p_XInputGetState ? "" : " (no XInput DLL loaded)");
        last_rc = rc;
        /* pad gone: report a neutral, all-released state rather than stale data */
        d->state = ns;
        return;
    }
    if (last_rc != ERROR_SUCCESS) xj_log("device_poll_locked: XInputGetState(index=%d) succeeded again", cfg.xinput_index);
    last_rc = rc;

    {
        BOOL mod_held = compute_buttons(&xs.Gamepad, ns.rgbButtons);
        double lx = xs.Gamepad.sThumbLX / 32767.0;
        double ly = xs.Gamepad.sThumbLY / 32767.0;
        double rx = xs.Gamepad.sThumbRX / 32767.0;
        double ry = xs.Gamepad.sThumbRY / 32767.0;
        if (cfg.invert_lx) lx = -lx;
        if (cfg.invert_ly) ly = -ly;
        if (cfg.invert_rx) rx = -rx;
        lx = apply_deadzone(lx, cfg.stick_deadzone);
        ly = apply_deadzone(ly, cfg.stick_deadzone);
        rx = apply_deadzone(rx, cfg.mouselook_deadzone);
        ry = apply_deadzone(ry, cfg.mouselook_deadzone);
        if (mod_held) { lx = 0; ly = 0; } /* suppress movement while the stick is being used as a chord target */

        ns.lX  = scale_axis(lx,  d->axis_range[0].lMin, d->axis_range[0].lMax);
        ns.lY  = scale_axis(-ly, d->axis_range[1].lMin, d->axis_range[1].lMax); /* stick-forward (Y+) -> engine-forward */
        if (cfg.mouselook_enabled) {
            /* Right stick no longer feeds the classic joystick axes at all -- it drives synthetic mouse
             * movement instead (see inject_mouselook()), since this engine has no analog look/pitch bind
             * an axis could target. Report center so any old rudderturn bind on joy_axisr goes quiet
             * rather than fighting the mouse-driven turn. */
            ns.lZ = scale_axis(0, d->axis_range[2].lMin, d->axis_range[2].lMax);
            ns.lRz = scale_axis(0, d->axis_range[3].lMin, d->axis_range[3].lMax);
            inject_mouselook(rx, ry);
        } else {
            ns.lZ  = scale_axis(-ry, d->axis_range[2].lMin, d->axis_range[2].lMax);
            ns.lRz = scale_axis(rx,  d->axis_range[3].lMin, d->axis_range[3].lMax);
        }

        if (cfg.dpad_as_buttons) {
            /* left as a future extension point; default off, POV hat below covers the D-pad */
        }
        if (xs.Gamepad.wButtons & (XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT)) {
            int up = (xs.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_UP) != 0;
            int dn = (xs.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) != 0;
            int lt = (xs.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) != 0;
            int rt = (xs.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) != 0;
            /* standard DirectInput POV encoding: hundredths of a degree clockwise from north, diagonals included */
            if (up && rt) ns.rgdwPOV[0] = 4500;
            else if (dn && rt) ns.rgdwPOV[0] = 13500;
            else if (dn && lt) ns.rgdwPOV[0] = 22500;
            else if (up && lt) ns.rgdwPOV[0] = 31500;
            else if (up) ns.rgdwPOV[0] = 0;
            else if (rt) ns.rgdwPOV[0] = 9000;
            else if (dn) ns.rgdwPOV[0] = 18000;
            else if (lt) ns.rgdwPOV[0] = 27000;
        } else {
            ns.rgdwPOV[0] = (DWORD)-1; /* centered = no direction, per DirectInput convention */
        }
        ns.rgdwPOV[1] = ns.rgdwPOV[2] = ns.rgdwPOV[3] = (DWORD)-1;
    }

    if (memcmp(&d->state, &ns, sizeof ns) != 0) d->state_seq++;
    d->state = ns;
}

/* ---- IUnknown ---- */
static HRESULT WINAPI dev_QueryInterface(XJDevice *This, REFIID riid, void **ppv)
{
    xj_log("Device::QueryInterface(%s)", guid_name(riid));
    if (!ppv) return XJ_DIERR_INVALIDPARAM;
    if (IsEqualGUID(riid, &XJ_IID_IUnknown) || IsEqualGUID(riid, &XJ_IID_IDirectInputDeviceA) || IsEqualGUID(riid, &XJ_IID_IDirectInputDevice2A)) {
        *ppv = This;
        ((XJDeviceObj *)This)->refcount++;
        return XJ_DI_OK;
    }
    *ppv = NULL;
    return XJ_DIERR_NOINTERFACE;
}
static ULONG WINAPI dev_AddRef(XJDevice *This) { return (ULONG)++((XJDeviceObj *)This)->refcount; }
static ULONG WINAPI dev_Release(XJDevice *This)
{
    LONG r = --((XJDeviceObj *)This)->refcount;
    /* singleton: never actually freed, mirrors it being "the one pad" for the process lifetime */
    return (ULONG)(r < 0 ? 0 : r);
}

static HRESULT WINAPI dev_GetCapabilities(XJDevice *This, XJ_DIDEVCAPS *caps)
{
    (void)This;
    xj_log("Device::GetCapabilities (dwSize=%lu)", caps ? caps->dwSize : 0);
    if (!caps || caps->dwSize < sizeof(XJ_DIDEVCAPS) - 5 * sizeof(DWORD)) return XJ_DIERR_INVALIDPARAM;
    {
        DWORD size = caps->dwSize;
        memset(caps, 0, size);
        caps->dwSize = size;
        caps->dwFlags = XJ_DIDC_ATTACHED | XJ_DIDC_EMULATED;
        caps->dwDevType = XJ_DIDEVTYPE_JOYSTICK | (XJ_DIDEVTYPEJOYSTICK_GAMEPAD << 8);
        caps->dwAxes = 4;
        caps->dwButtons = MODIFIER_ALONE_INDEX + 1; /* 27 */
        caps->dwPOVs = 1;
    }
    return XJ_DI_OK;
}

/* Object table shared by EnumObjects/GetObjectInfo -- offsets match xj_djoystate_t exactly. */
typedef struct { const GUID *guid; DWORD ofs; DWORD type; const char *name; } obj_entry_t;
static const obj_entry_t g_objs[] = {
    { &XJ_GUID_XAxis,  offsetof(xj_djoystate_t, lX),  XJ_DIDFT_ABSAXIS | XJ_DIDFT_MAKEINSTANCE(0), "X Axis" },
    { &XJ_GUID_YAxis,  offsetof(xj_djoystate_t, lY),  XJ_DIDFT_ABSAXIS | XJ_DIDFT_MAKEINSTANCE(1), "Y Axis" },
    { &XJ_GUID_ZAxis,  offsetof(xj_djoystate_t, lZ),  XJ_DIDFT_ABSAXIS | XJ_DIDFT_MAKEINSTANCE(2), "Z Axis" },
    { &XJ_GUID_RzAxis, offsetof(xj_djoystate_t, lRz), XJ_DIDFT_ABSAXIS | XJ_DIDFT_MAKEINSTANCE(3), "Rudder" },
    { &XJ_GUID_POV,    offsetof(xj_djoystate_t, rgdwPOV[0]), XJ_DIDFT_POV | XJ_DIDFT_MAKEINSTANCE(0), "POV Hat" },
    /* 27 buttons; names left generic ("Button N") -- the engine supplies its own "joyN" bind labels */
};
#define OBJ_FIXED_COUNT (sizeof g_objs / sizeof g_objs[0])
#define OBJ_TOTAL_COUNT (OBJ_FIXED_COUNT + (MODIFIER_ALONE_INDEX + 1))

static void fill_object_instance(XJ_DIDEVICEOBJECTINSTANCEA *out, DWORD idx)
{
    DWORD size = out->dwSize;
    memset(out, 0, size);
    out->dwSize = size;
    if (idx < OBJ_FIXED_COUNT) {
        out->guidType = *g_objs[idx].guid;
        out->dwOfs = g_objs[idx].ofs;
        out->dwType = g_objs[idx].type;
        strncpy(out->tszName, g_objs[idx].name, sizeof(out->tszName) - 1);
    } else {
        DWORD b = idx - OBJ_FIXED_COUNT;
        out->guidType = XJ_GUID_Button;
        out->dwOfs = (DWORD)(offsetof(xj_djoystate_t, rgbButtons) + b);
        out->dwType = XJ_DIDFT_PSHBUTTON | XJ_DIDFT_MAKEINSTANCE(b);
        snprintf(out->tszName, sizeof(out->tszName), "Button %lu", b + 1);
    }
}

static HRESULT WINAPI dev_EnumObjects(XJDevice *This, XJ_LPDIENUMDEVICEOBJECTSCALLBACKA cb, void *pvRef, DWORD dwFlags)
{
    XJ_DIDEVICEOBJECTINSTANCEA inst;
    DWORD i;
    (void)This;
    xj_log("Device::EnumObjects(dwFlags=%#lx)", (unsigned long)dwFlags);
    for (i = 0; i < OBJ_TOTAL_COUNT; i++) {
        DWORD isAxisOrPov = i < OBJ_FIXED_COUNT;
        DWORD isButton = !isAxisOrPov;
        if (dwFlags != 0 /* DIDFT_ALL */) {
            DWORD wantAxis = dwFlags & XJ_DIDFT_AXIS, wantButton = dwFlags & XJ_DIDFT_BUTTON, wantPov = dwFlags & XJ_DIDFT_POV;
            if (isButton && !wantButton) continue;
            if (isAxisOrPov && i < 4 && !wantAxis) continue;
            if (isAxisOrPov && i == 4 && !wantPov) continue;
        }
        inst.dwSize = sizeof inst;
        fill_object_instance(&inst, i);
        if (cb && cb(&inst, pvRef) == XJ_DIENUM_STOP) break;
    }
    return XJ_DI_OK;
}

static HRESULT WINAPI dev_GetProperty(XJDevice *This, REFGUID rguidProp, XJ_DIPROPHEADER *pdiph)
{
    XJDeviceObj *d = (XJDeviceObj *)This;
    xj_log("Device::GetProperty(%s, dwObj=%lu, dwHow=%lu)", guid_name(rguidProp), pdiph ? pdiph->dwObj : 0, pdiph ? pdiph->dwHow : 0);
    if (!pdiph) return XJ_DIERR_INVALIDPARAM;
    if (rguidProp == XJ_DIPROP_RANGE && pdiph->dwHeaderSize == sizeof(XJ_DIPROPHEADER)) {
        XJ_DIPROPRANGE *r = (XJ_DIPROPRANGE *)pdiph;
        int axis = (pdiph->dwHow == XJ_DIPH_BYOFFSET) ?
            (pdiph->dwObj == offsetof(xj_djoystate_t, lX) ? 0 : pdiph->dwObj == offsetof(xj_djoystate_t, lY) ? 1 :
             pdiph->dwObj == offsetof(xj_djoystate_t, lZ) ? 2 : pdiph->dwObj == offsetof(xj_djoystate_t, lRz) ? 3 : -1) : -1;
        if (axis < 0) return XJ_DIERR_UNSUPPORTED;
        r->lMin = d->axis_range[axis].lMin;
        r->lMax = d->axis_range[axis].lMax;
        return XJ_DI_OK;
    }
    return XJ_DIERR_UNSUPPORTED;
}

static HRESULT WINAPI dev_SetProperty(XJDevice *This, REFGUID rguidProp, const XJ_DIPROPHEADER *pdiph)
{
    XJDeviceObj *d = (XJDeviceObj *)This;
    xj_log("Device::SetProperty(%s, dwObj=%lu, dwHow=%lu)", guid_name(rguidProp), pdiph ? pdiph->dwObj : 0, pdiph ? pdiph->dwHow : 0);
    xj_log_hex("SetProperty", pdiph, pdiph ? pdiph->dwHeaderSize + 16 : 0);
    if (!pdiph) return XJ_DIERR_INVALIDPARAM;
    if (rguidProp == XJ_DIPROP_RANGE) {
        const XJ_DIPROPRANGE *r = (const XJ_DIPROPRANGE *)pdiph;
        int axis = (pdiph->dwHow == XJ_DIPH_BYOFFSET) ?
            (pdiph->dwObj == offsetof(xj_djoystate_t, lX) ? 0 : pdiph->dwObj == offsetof(xj_djoystate_t, lY) ? 1 :
             pdiph->dwObj == offsetof(xj_djoystate_t, lZ) ? 2 : pdiph->dwObj == offsetof(xj_djoystate_t, lRz) ? 3 : -1) : -1;
        if (axis < 0) { xj_log("  SetProperty(RANGE): dwHow=%lu not DIPH_BYOFFSET or unrecognized offset -- ignored", pdiph->dwHow); return XJ_DIERR_UNSUPPORTED; }
        d->axis_range[axis].lMin = r->lMin;
        d->axis_range[axis].lMax = r->lMax;
        xj_log("  axis %d range now [%ld, %ld]", axis, (long)r->lMin, (long)r->lMax);
        return XJ_DI_OK;
    }
    /* DEADZONE/SATURATION/AXISMODE/BUFFERSIZE: acknowledged, not separately modeled (deadzone/sensitivity
     * shaping is left to the engine's own joystick_deadzone/joystick_sensitivity config, per the plan). */
    return XJ_DI_OK;
}

static HRESULT WINAPI dev_Acquire(XJDevice *This)
{
    XJDeviceObj *d = (XJDeviceObj *)This;
    xj_log("Device::Acquire");
    d->acquired = TRUE;
    return XJ_DI_OK;
}
static HRESULT WINAPI dev_Unacquire(XJDevice *This)
{
    XJDeviceObj *d = (XJDeviceObj *)This;
    xj_log("Device::Unacquire");
    d->acquired = FALSE;
    return XJ_DI_OK;
}

static HRESULT WINAPI dev_GetDeviceState(XJDevice *This, DWORD cbData, void *lpvData)
{
    XJDeviceObj *d = (XJDeviceObj *)This;
    if (!d->acquired) { xj_log("Device::GetDeviceState: not acquired"); return XJ_DIERR_NOTACQUIRED; }
    if (!lpvData) return XJ_DIERR_INVALIDPARAM;
    device_poll_locked(d); /* self-polls in case the engine never calls Poll() separately */
    memcpy(lpvData, &d->state, cbData < sizeof(d->state) ? cbData : sizeof(d->state));
    if (cfg.log_verbose >= 2) {
        xj_log("Device::GetDeviceState(cbData=%lu): X=%ld Y=%ld Z=%ld Rz=%ld POV=%lu btn0..10=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
               (unsigned long)cbData, (long)d->state.lX, (long)d->state.lY, (long)d->state.lZ, (long)d->state.lRz,
               (unsigned long)d->state.rgdwPOV[0],
               d->state.rgbButtons[0], d->state.rgbButtons[1], d->state.rgbButtons[2], d->state.rgbButtons[3],
               d->state.rgbButtons[4], d->state.rgbButtons[5], d->state.rgbButtons[6], d->state.rgbButtons[7],
               d->state.rgbButtons[8], d->state.rgbButtons[9], d->state.rgbButtons[10]);
    }
    return XJ_DI_OK;
}

/* Small ring buffer for buffered (GetDeviceData) access, populated by device_poll_locked() callers
 * that go through Poll(). Kept simple: only enabled once SetProperty(DIPROP_BUFFERSIZE) sets a nonzero
 * size, matching real DirectInput semantics (DIERR_NOTBUFFERED otherwise). */
static HRESULT WINAPI dev_GetDeviceData(XJDevice *This, DWORD cbObjectData, XJ_DIDEVICEOBJECTDATA *rgdod, DWORD *pdwInOut, DWORD dwFlags)
{
    (void)This; (void)cbObjectData; (void)rgdod; (void)dwFlags;
    xj_log("Device::GetDeviceData -- not buffered (no SetProperty(DIPROP_BUFFERSIZE) support yet; MVP uses polled access only)");
    if (pdwInOut) *pdwInOut = 0;
    return XJ_DIERR_NOTBUFFERED;
}

static HRESULT WINAPI dev_SetDataFormat(XJDevice *This, const XJ_DIDATAFORMAT *lpdf)
{
    XJDeviceObj *d = (XJDeviceObj *)This;
    DWORD i;
    if (!lpdf) return XJ_DIERR_INVALIDPARAM;
    xj_log("Device::SetDataFormat: dwObjSize=%lu dwFlags=%#lx dwDataSize=%lu dwNumObjs=%lu",
           (unsigned long)lpdf->dwObjSize, (unsigned long)lpdf->dwFlags, (unsigned long)lpdf->dwDataSize, (unsigned long)lpdf->dwNumObjs);
    if (lpdf->dwDataSize != sizeof(xj_djoystate_t))
        xj_log("  NOTE: engine's declared dwDataSize (%lu) != our xj_djoystate_t (%lu) -- see DEVLOG Phase 0; "
               "GetDeviceState still copies min(cbData, our struct size)", (unsigned long)lpdf->dwDataSize, (unsigned long)sizeof(xj_djoystate_t));
    if (cfg.log_verbose >= 2)
        for (i = 0; i < lpdf->dwNumObjs && i < 64; i++)
            xj_log("  rgodf[%lu]: pguid=%s dwOfs=%lu dwType=%#lx dwFlags=%#lx",
                   (unsigned long)i, guid_name(lpdf->rgodf[i].pguid), (unsigned long)lpdf->rgodf[i].dwOfs,
                   (unsigned long)lpdf->rgodf[i].dwType, (unsigned long)lpdf->rgodf[i].dwFlags);
    d->data_format_size = lpdf->dwDataSize;
    return XJ_DI_OK;
}

static HRESULT WINAPI dev_SetEventNotification(XJDevice *This, HANDLE hEvent)
{ (void)This; xj_log("Device::SetEventNotification(%p) -- ignored (polled access only)", (void *)hEvent); return XJ_DI_OK; }

static HRESULT WINAPI dev_SetCooperativeLevel(XJDevice *This, HWND hwnd, DWORD dwFlags)
{ (void)This; xj_log("Device::SetCooperativeLevel(hwnd=%p, dwFlags=%#lx)", (void *)hwnd, (unsigned long)dwFlags); return XJ_DI_OK; }

static HRESULT WINAPI dev_GetObjectInfo(XJDevice *This, XJ_DIDEVICEOBJECTINSTANCEA *pdidoi, DWORD dwObj, DWORD dwHow)
{
    DWORD idx;
    (void)This;
    if (!pdidoi) return XJ_DIERR_INVALIDPARAM;
    if (dwHow == XJ_DIPH_BYID) {
        DWORD type = XJ_DIDFT_GETTYPE(dwObj), inst = XJ_DIDFT_GETINSTANCE(dwObj);
        if (type & XJ_DIDFT_BUTTON) idx = OBJ_FIXED_COUNT + inst;
        else if (type & XJ_DIDFT_POV) idx = 4;
        else idx = inst; /* axis */
    } else if (dwHow == XJ_DIPH_BYOFFSET) {
        for (idx = 0; idx < OBJ_TOTAL_COUNT; idx++) {
            XJ_DIDEVICEOBJECTINSTANCEA tmp; tmp.dwSize = sizeof tmp;
            fill_object_instance(&tmp, idx);
            if (tmp.dwOfs == dwObj) break;
        }
    } else return XJ_DIERR_UNSUPPORTED;
    xj_log("Device::GetObjectInfo(dwObj=%lu, dwHow=%lu) -> idx=%lu", (unsigned long)dwObj, (unsigned long)dwHow, (unsigned long)idx);
    if (idx >= OBJ_TOTAL_COUNT) return XJ_DIERR_NOTFOUND;
    fill_object_instance(pdidoi, idx);
    return XJ_DI_OK;
}

static HRESULT WINAPI dev_GetDeviceInfo(XJDevice *This, XJ_DIDEVICEINSTANCEA *pdidi)
{
    DWORD size;
    (void)This;
    xj_log("Device::GetDeviceInfo");
    if (!pdidi) return XJ_DIERR_INVALIDPARAM;
    size = pdidi->dwSize;
    memset(pdidi, 0, size);
    pdidi->dwSize = size;
    pdidi->guidInstance = XJ_GUID_OurDevice;
    pdidi->guidProduct = XJ_GUID_Joystick;
    pdidi->dwDevType = XJ_DIDEVTYPE_JOYSTICK | (XJ_DIDEVTYPEJOYSTICK_GAMEPAD << 8);
    strncpy(pdidi->tszInstanceName, "Xbox Controller (XInput)", sizeof(pdidi->tszInstanceName) - 1);
    strncpy(pdidi->tszProductName, "Xbox Controller (XInput)", sizeof(pdidi->tszProductName) - 1);
    return XJ_DI_OK;
}

static HRESULT WINAPI dev_RunControlPanel(XJDevice *This, HWND hwndOwner, DWORD dwFlags)
{ (void)This; (void)hwndOwner; (void)dwFlags; xj_log("Device::RunControlPanel -- no-op"); return XJ_DI_OK; }

static HRESULT WINAPI dev_Initialize(XJDevice *This, HINSTANCE hinst, DWORD dwVersion, REFGUID rguid)
{ (void)This; (void)hinst; xj_log("Device::Initialize(dwVersion=%#lx, rguid=%s)", (unsigned long)dwVersion, guid_name(rguid)); return XJ_DI_OK; }

/* ---- force feedback: unsupported, no FF hardware to model ---- */
static HRESULT WINAPI dev_CreateEffect(XJDevice *This, REFGUID rguid, const void *lpeff, void **ppdeff, IUnknown *punkOuter)
{ (void)This; (void)rguid; (void)lpeff; (void)punkOuter; xj_log("Device::CreateEffect -- unsupported"); if (ppdeff) *ppdeff = NULL; return XJ_DIERR_UNSUPPORTED; }
static HRESULT WINAPI dev_EnumEffects(XJDevice *This, void *cb, void *pvRef, DWORD t) { (void)This; (void)cb; (void)pvRef; (void)t; return XJ_DI_OK; }
static HRESULT WINAPI dev_GetEffectInfo(XJDevice *This, void *pdei, REFGUID rguid) { (void)This; (void)pdei; (void)rguid; return XJ_DIERR_UNSUPPORTED; }
static HRESULT WINAPI dev_GetForceFeedbackState(XJDevice *This, DWORD *pdwOut) { (void)This; if (pdwOut) *pdwOut = 0; return XJ_DI_OK; }
static HRESULT WINAPI dev_SendForceFeedbackCommand(XJDevice *This, DWORD dwFlags) { (void)This; (void)dwFlags; return XJ_DI_OK; }
static HRESULT WINAPI dev_EnumCreatedEffectObjects(XJDevice *This, void *cb, void *pvRef, DWORD fl) { (void)This; (void)cb; (void)pvRef; (void)fl; return XJ_DI_OK; }
static HRESULT WINAPI dev_Escape(XJDevice *This, void *pesc) { (void)This; (void)pesc; return XJ_DIERR_UNSUPPORTED; }

static HRESULT WINAPI dev_Poll(XJDevice *This)
{
    XJDeviceObj *d = (XJDeviceObj *)This;
    DWORD before;
    if (!d->acquired) return XJ_DIERR_NOTACQUIRED;
    before = d->state_seq;
    device_poll_locked(d);
    return (d->state_seq != before) ? XJ_DI_OK : XJ_DI_NOEFFECT;
}
static HRESULT WINAPI dev_SendDeviceData(XJDevice *This, DWORD cbObjectData, const XJ_DIDEVICEOBJECTDATA *rgdod, DWORD *pdwInOut, DWORD fl)
{ (void)This; (void)cbObjectData; (void)rgdod; (void)fl; xj_log("Device::SendDeviceData -- unsupported"); if (pdwInOut) *pdwInOut = 0; return XJ_DIERR_UNSUPPORTED; }

static const XJDeviceVtbl g_device_vtbl = {
    dev_QueryInterface, dev_AddRef, dev_Release,
    dev_GetCapabilities, dev_EnumObjects, dev_GetProperty, dev_SetProperty,
    dev_Acquire, dev_Unacquire, dev_GetDeviceState, dev_GetDeviceData, dev_SetDataFormat,
    dev_SetEventNotification, dev_SetCooperativeLevel, dev_GetObjectInfo, dev_GetDeviceInfo,
    dev_RunControlPanel, dev_Initialize,
    dev_CreateEffect, dev_EnumEffects, dev_GetEffectInfo, dev_GetForceFeedbackState,
    dev_SendForceFeedbackCommand, dev_EnumCreatedEffectObjects, dev_Escape, dev_Poll, dev_SendDeviceData,
};

static void device_init(void)
{
    int i;
    if (g_device_inited) return;
    g_device_inited = TRUE;
    memset(&g_device, 0, sizeof g_device);
    g_device.lpVtbl = &g_device_vtbl;
    g_device.refcount = 1;
    for (i = 0; i < 4; i++) { g_device.axis_range[i].lMin = -1000; g_device.axis_range[i].lMax = 1000; } /* default; SetProperty overrides */
    g_device.state.rgdwPOV[0] = g_device.state.rgdwPOV[1] = g_device.state.rgdwPOV[2] = g_device.state.rgdwPOV[3] = (DWORD)-1;
}

/* ------------------------------------------------------------------ IDirectInputA */

static HRESULT WINAPI di_QueryInterface(XJDirectInput *This, REFIID riid, void **ppv)
{
    xj_log("DirectInput::QueryInterface(%s)", guid_name(riid));
    if (!ppv) return XJ_DIERR_INVALIDPARAM;
    if (IsEqualGUID(riid, &XJ_IID_IUnknown) || IsEqualGUID(riid, &XJ_IID_IDirectInputA) || IsEqualGUID(riid, &XJ_IID_IDirectInput2A) || IsEqualGUID(riid, &XJ_IID_IDirectInput7A)) {
        *ppv = This;
        return XJ_DI_OK;
    }
    *ppv = NULL;
    return XJ_DIERR_NOINTERFACE;
}
static ULONG WINAPI di_AddRef(XJDirectInput *This) { (void)This; return 1; }
static ULONG WINAPI di_Release(XJDirectInput *This) { (void)This; return 1; } /* singleton, lives for the process */

static HRESULT WINAPI di_CreateDevice(XJDirectInput *This, REFGUID rguid, XJDevice **out, IUnknown *pUnkOuter)
{
    (void)This; (void)pUnkOuter;
    xj_log("DirectInput::CreateDevice(%s)", guid_name(rguid));
    if (!out) return XJ_DIERR_INVALIDPARAM;
    /* Only one device exists; accept GUID_Joystick, our own instance GUID, or (leniently) anything else,
     * since some callers enumerate first and pass back exactly what EnumDevices gave them. */
    device_init();
    g_device.refcount++;
    *out = (XJDevice *)&g_device;
    return XJ_DI_OK;
}

static HRESULT WINAPI di_EnumDevices(XJDirectInput *This, DWORD dwDevType, XJ_LPDIENUMDEVICESCALLBACKA cb, void *pvRef, DWORD dwFlags)
{
    XJ_DIDEVICEINSTANCEA inst;
    (void)This;
    xj_log("DirectInput::EnumDevices(dwDevType=%lu, dwFlags=%#lx)", (unsigned long)dwDevType, (unsigned long)dwFlags);
    if (dwDevType != 0 && dwDevType != XJ_DIDEVTYPE_JOYSTICK) { xj_log("  filtered out (not a joystick request)"); return XJ_DI_OK; }
    if (!pad_connected()) { xj_log("  no XInput pad connected at index %d -- reporting zero devices", cfg.xinput_index); return XJ_DI_OK; }
    if ((dwFlags & XJ_DIEDFL_ATTACHEDONLY) && !pad_connected()) return XJ_DI_OK;
    memset(&inst, 0, sizeof inst);
    inst.dwSize = sizeof inst;
    inst.guidInstance = XJ_GUID_OurDevice;
    inst.guidProduct = XJ_GUID_Joystick;
    inst.dwDevType = XJ_DIDEVTYPE_JOYSTICK | (XJ_DIDEVTYPEJOYSTICK_GAMEPAD << 8);
    strncpy(inst.tszInstanceName, "Xbox Controller (XInput)", sizeof(inst.tszInstanceName) - 1);
    strncpy(inst.tszProductName, "Xbox Controller (XInput)", sizeof(inst.tszProductName) - 1);
    if (cb) cb(&inst, pvRef);
    return XJ_DI_OK;
}

static HRESULT WINAPI di_GetDeviceStatus(XJDirectInput *This, REFGUID rguidInstance)
{ (void)This; (void)rguidInstance; xj_log("DirectInput::GetDeviceStatus"); return pad_connected() ? XJ_DI_OK : XJ_DIERR_UNPLUGGED; }

static HRESULT WINAPI di_RunControlPanel(XJDirectInput *This, HWND hwndOwner, DWORD dwFlags)
{ (void)This; (void)hwndOwner; (void)dwFlags; xj_log("DirectInput::RunControlPanel -- no-op"); return XJ_DI_OK; }

static HRESULT WINAPI di_Initialize(XJDirectInput *This, HINSTANCE hinst, DWORD dwVersion)
{ (void)This; (void)hinst; xj_log("DirectInput::Initialize(dwVersion=%#lx)", (unsigned long)dwVersion); return XJ_DI_OK; }

static const XJDirectInputVtbl g_di_vtbl = {
    di_QueryInterface, di_AddRef, di_Release, di_CreateDevice, di_EnumDevices, di_GetDeviceStatus, di_RunControlPanel, di_Initialize,
};
static XJDirectInput g_di = { &g_di_vtbl };

/* ------------------------------------------------------------------ exported entry point */

static void ensure_lazy_init(void); /* defined below, in "config hot-reload + startup" */

__declspec(dllexport) HRESULT WINAPI DirectInputCreateA(HINSTANCE hinst, DWORD dwVersion, XJDirectInput **ppDI, IUnknown *punkOuter)
{
    (void)hinst; (void)punkOuter;
    ensure_lazy_init();
    xj_log("DirectInputCreateA(dwVersion=%#lx)", (unsigned long)dwVersion);
    if (!ppDI) return XJ_DIERR_INVALIDPARAM;
    *ppDI = &g_di;
    return XJ_DI_OK;
}

/* ------------------------------------------------------------------ buttons as keyboard / mouse */

/* Polls the pad itself at ~100 Hz, independently of the game's GetDeviceState calls (the game may not poll
 * the joystick at all while a menu is open, and those two buttons are for menus as much as for play).
 * Back -> Esc (press and release follow the button); right trigger -> left mouse button, held while the
 * trigger is held (so a bow draws / a sword swings as with the mouse). Sent with SendInput, only while this
 * game's window is in front; anything held is released when that stops, the pad goes away, or the option is
 * switched off. */
static BOOL game_in_front(void)
{
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (!fg) return FALSE;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

static void send_key(WORD vk, WORD scan, BOOL down)
{
    INPUT in;
    memset(&in, 0, sizeof in);
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = scan;
    in.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);   /* scancode: DirectInput keyboards see it too */
    SendInput(1, &in, sizeof(INPUT));
}

static void send_mouse1(BOOL down)
{
    INPUT in;
    memset(&in, 0, sizeof in);
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    SendInput(1, &in, sizeof(INPUT));
}

static DWORD WINAPI key_mouse_thread(LPVOID unused)
{
    BOOL esc_down = FALSE, m1_down = FALSE;
    (void)unused;
    for (;;) {
        XINPUT_STATE xs;
        BOOL ok, front, want_esc = FALSE, want_m1 = FALSE;
        int esc_on, m1_on;
        double thr;
        Sleep(10);
        EnterCriticalSection(&g_lock);
        esc_on = cfg.enabled && cfg.back_as_escape && cfg.shift_button != SB_BACK;
        m1_on = cfg.enabled && cfg.rt_as_mouse1 && cfg.shift_button != SB_RT;
        thr = cfg.trigger_threshold;
        ok = p_XInputGetState && p_XInputGetState((DWORD)cfg.xinput_index, &xs) == ERROR_SUCCESS;
        LeaveCriticalSection(&g_lock);
        front = game_in_front();
        if (ok && front) {
            want_esc = esc_on && (xs.Gamepad.wButtons & XINPUT_GAMEPAD_BACK) != 0;
            want_m1 = m1_on && xs.Gamepad.bRightTrigger > (BYTE)(thr * 255);
        }
        if (want_esc != esc_down) { send_key(VK_ESCAPE, 0x01, want_esc); esc_down = want_esc; if (cfg.log_verbose) xj_log("Back -> Esc %s", want_esc ? "down" : "up"); }
        if (want_m1 != m1_down) { send_mouse1(want_m1); m1_down = want_m1; if (cfg.log_verbose) xj_log("RT -> mouse1 %s", want_m1 ? "down" : "up"); }
    }
    return 0;
}

/* ------------------------------------------------------------------ config hot-reload + startup */

static DWORD WINAPI reload_thread(LPVOID unused)
{
    (void)unused;
    for (;;) {
        Sleep(2000);
        EnterCriticalSection(&g_lock);
        load_config();
        LeaveCriticalSection(&g_lock);
    }
    return 0;
}

/* Deliberately NOT done in DllMain: calling LoadLibraryA (load_xinput) and CreateThread while still
 * inside DllMain(DLL_PROCESS_ATTACH) -- i.e. still holding the loader lock -- is a well-known Windows
 * hazard (MSDN: "DllMain restrictions"). A first native test crashed dwm.exe (not Thief2.exe itself)
 * moments after the engine created its exclusive-fullscreen D3D9 device, with this exact pattern as the
 * only thing in DllMain beyond simple file I/O; removing it (deferring to first real use here, well
 * after the loader lock is released) fixed it. Guarded to run exactly once. */
static LONG g_lazy_inited;
static void ensure_lazy_init(void)
{
    if (InterlockedCompareExchange(&g_lazy_inited, 1, 0) != 0) return;
    load_xinput();
    device_init();
    CreateThread(NULL, 0, reload_thread, NULL, 0, NULL);
    CreateThread(NULL, 0, key_mouse_thread, NULL, 0, NULL);
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DWORD n = GetModuleFileNameA(hinst, g_dir, sizeof g_dir);
        char *slash;
        char logpath[MAX_PATH + 16];
        if (n == 0 || n >= sizeof g_dir) g_dir[0] = '\0';
        slash = strrchr(g_dir, '\\');
        if (slash) slash[1] = '\0'; else g_dir[0] = '\0';

        snprintf(logpath, sizeof logpath, "%sdinput.log", g_dir);
        g_log = fopen(logpath, "a");
        InitializeCriticalSection(&g_lock);
        load_config();
        xj_log("---- dinput.dll (xinput_joy) attached, dir=%s ----", g_dir);
        if (!cfg.enabled) { xj_log("disabled via ini (enabled=0)"); }
        DisableThreadLibraryCalls(hinst);
    } else if (reason == DLL_PROCESS_DETACH) {
        xj_log("---- detached ----");
        if (g_log) { fclose(g_log); g_log = NULL; }
    }
    return TRUE;
}
