/*
 * headlook.dll: OpenTrack head-tracking for Thief 2 (NewDark), yaw/pitch/roll of the RENDER view only.
 *
 * Loaded at process start by the loader stub from patches/install_loader.py (LoadLibraryA at the exe's
 * entry point). See docs/DEVLOG.md, "Stage B map", for how the hook site was found.
 *
 * What it does:
 *   1. A worker thread receives OpenTrack's "UDP over network" packet (6 little-endian doubles:
 *      x, y, z in cm; yaw, pitch, roll in degrees) on a UDP port, and optionally the same 48-byte
 *      records over a TCP connection (for relaying through an ssh tunnel), smooths and clamps the
 *      angles and stores them as 16-bit angle units (65536 = 360 deg) in g_yaw / g_pitch / g_roll.
 *   2. At start-up it patches the 5 bytes at 0x5CEF38 in FUN_005cee30 (the per-frame scene render) to
 *      call hl_stub. There the renderer has just copied the camera struct's angles into a LOCAL location
 *      on its stack; the stub adds our offsets to that local copy only. The camera struct itself, which
 *      movement, mouse-look and projectile launch use, is never touched.
 *   3. The offsets are applied only while the camera struct's mode is 0 (camera on the player), so
 *      cutscenes and remote/security cameras are unaffected.
 *
 * Config: headlook.ini next to this DLL. Log: headlook.log next to this DLL.
 * Position (x, y, z) is received but not used yet.
 *
 * STEREO EXPERIMENT (off by default; see DEVLOG, "side-by-side stereo"): with stereo=sbs (ini) or the
 * HEADLOOK_STEREO=sbs environment variable (what `STEREO=sbs tools/run-wine.sh` sets) the DLL also
 *   4. replaces the `call FUN_005cee30` in the hardware-render branch of the frame handler (0x5CF2E2)
 *      with hl_scene, which draws the scene twice per frame, once per eye. Each pass shifts the render-only
 *      pose copy sideways by +-stereo_ipd/2 (through the same stub as head-look, g_eye_dx/dy), then the
 *      Direct3D 9 render target is copied into the left or right half of a temporary surface and the
 *      result is blitted back ("half" side-by-side: each eye squeezed to half width).
 *      The engine sends pre-transformed vertices (FVF 0x1c4) and never sets a viewport, so there is no
 *      projection to edit: the halves are made by copying, not by drawing into a sub-viewport.
 *      The device pointer is the global at 0xA36040. The wrapper is only patched in when stereo was
 *      requested at start-up, so the normal path is untouched otherwise.
 *
 * OPENVR DIRECT SUBMIT (stereo=openvr or HEADLOOK_STEREO=openvr; see vr_openvr.c): the same two eye passes,
 * but each eye image also goes straight to a SteamVR headset through IVRCompositor::Submit, and the head pose
 * comes from the compositor instead of UDP (the UDP pose is ignored while it does). The desktop window still
 * shows the side-by-side picture. If SteamVR cannot be reached it behaves like stereo=sbs and retries.
 *
 * REFERENCE MARKS (off by default; ini show_heading_marks): draws short tick marks at the screen edges
 * showing where the body's true heading and pitch are, so it is possible to tell where an arrow would
 * fly while head-look has the view rotated away from it (the head-look/stereo hook only ever changes the
 * RENDER-local copy, never the camera struct that movement, frob-by-struct and projectile launch would
 * use, so the offset we are already computing (g_yaw/g_pitch) *is* the error between the drawn view and
 * body-forward). Reuses the same call-site wrapper as stereo (so either feature, or both, installs it),
 * drawn as flat-coloured lines after the scene, with the D3D render state saved and restored around it.
 * The screen position is a plain perspective projection using ini `fov_deg` (a GUESS -- the engine's own
 * FOV is not confirmed statically, see DEVLOG); calibrate it by dialing in a known offset and comparing
 * where the crosshair (where the four ticks would cross if extended) sits against where an arrow lands.
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <winsock2.h>
#include <windows.h>
#include <d3d9.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vr_openvr.h"
#include "vr_wxr.h"

/* Link-time addresses in Thief2.exe (image base 0x400000, sha256 af56a109...); rebased at run time. */
#define LINK_BASE      0x400000u
#define HOOK_SITE_VA   0x5CEF38u   /* mov [esp+0x28], cx   (local heading store in FUN_005cee30) */
#define CAMPTR_VA      0xA2A988u   /* global holding a pointer to the camera struct (mode at +0)  */
#define SCENE_FN_VA    0x5CEE30u   /* FUN_005cee30: per-frame scene render                        */
#define WRAP_SITE_VA   0x5CF2E2u   /* `call 0x5CEE30` in the frame handler's hardware branch      */
#define D3DDEV_PTR_VA  0xA36040u   /* global holding the IDirect3DDevice9 pointer                 */
#define VIEWMAT_VA     0x92D8F4u   /* view matrix (9 floats) built from the local angles          */
#define VIEWSCALE_VA   0x7E62D8u   /* float 1/tan(half FOV at 4:3), default 1.0 (90 deg); set once by FUN_005cc900,
                                      read every frame in FUN_005cee30 (times the camera's zoom at +4)   */
#define OVL_CALL_VA    0x5CF06Fu   /* `call FUN_0058c080` in FUN_005cee30: draws the queued 2D/HUD overlays, then empties
                                      the queue (so only the first scene call of a frame draws them) */
#define OVL_FN_VA      0x58C080u
#define PROVIDER_VA    0xA36014u   /* the D3D display provider object; its vtable [0](arg) / [1]() bracket the scene */
#define PROV_ARG_VA    0xA33F44u   /* the argument FUN_005cee30 passes to provider vtable[0] */
#define HORPLUS_VA     0x7DF800u   /* int: 1 = widescreen mode widens the view sideways (Hor+), set in FUN_00689aa0
                                      unless widescreen_lock_hfov is set                                */

/* Read by hl_stub (asm below). Aligned 32-bit stores/loads are atomic on x86. */
volatile unsigned g_yaw = 0, g_pitch = 0, g_roll = 0;
volatile unsigned g_enabled = 1;
volatile unsigned g_eye_active = 0;              /* 1 while a stereo eye pass is being drawn */
volatile float g_eye_dx = 0.0f, g_eye_dy = 0.0f; /* sideways shift added to the local x, y for that pass */
unsigned g_camptr_addr = 0;

extern void hl_stub(void);

/* Entered by `call` from the patched site, so the return address is on the stack. Preserves all
 * registers and flags (the x87 pushes/pops below are balanced). At the site the renderer's local view
 * pose is: floats x,y,z at [esp+0x14],[esp+0x18],[esp+0x1c]; dword [esp+0x24] = bank | pitch<<16,
 * word [esp+0x28] = heading; +4 for our return address, +12 for the three pushes below. */
__asm__(
    ".text\n"
    ".globl _hl_stub\n"
    "_hl_stub:\n"
    ".intel_syntax noprefix\n"
    "pushfd\n"
    "push eax\n"
    "push edx\n"
    "mov word ptr [esp+0x38], cx\n"          /* the original instruction: store the heading */
    "mov eax, dword ptr [_g_camptr_addr]\n"
    "mov eax, dword ptr [eax]\n"             /* camera struct pointer */
    "test eax, eax\n"
    "jz 9f\n"
    "cmp dword ptr [eax], 0\n"               /* camera mode: only when it is on the player */
    "jne 9f\n"
    "cmp dword ptr [_g_enabled], 0\n"
    "je 5f\n"
    "mov eax, dword ptr [_g_yaw]\n"
    "add word ptr [esp+0x38], ax\n"          /* heading */
    "mov eax, dword ptr [_g_pitch]\n"
    "add word ptr [esp+0x36], ax\n"          /* pitch */
    "mov eax, dword ptr [_g_roll]\n"
    "add word ptr [esp+0x34], ax\n"          /* bank (roll) */
    "5:\n"
    "cmp dword ptr [_g_eye_active], 0\n"     /* stereo: shift the local position sideways */
    "je 9f\n"
    "fld dword ptr [esp+0x24]\n"
    "fadd dword ptr [_g_eye_dx]\n"
    "fstp dword ptr [esp+0x24]\n"
    "fld dword ptr [esp+0x28]\n"
    "fadd dword ptr [_g_eye_dy]\n"
    "fstp dword ptr [esp+0x28]\n"
    "9:\n"
    "pop edx\n"
    "pop eax\n"
    "popfd\n"
    "ret\n"
    ".att_syntax prefix\n");

/* ------------------------------------------------------------------ config and logging */

typedef struct {
    int    udp_port;      /* 0 = off */
    int    tcp_port;      /* 0 = off */
    double yaw_sign, pitch_sign, roll_sign;      /* +1 or -1 */
    double yaw_gain, pitch_gain, roll_gain;
    double max_yaw, max_pitch, max_roll;         /* degrees, symmetric clamp */
    double smoothing_ms;                         /* filter time constant; 0 = none */
    int    timeout_ms;                           /* no data for this long -> ease back to centre */
    int    use_roll;
    int    enabled;
    int    stereo;                               /* 0 = off, 1 = side-by-side, 2 = openvr; needs a restart to switch on */
    double stereo_ipd;                           /* eye separation in game units (see ini) */
    int    stereo_swap;                          /* swap left/right (cross-eyed viewing) */
    int    listen_loopback;                      /* 1 = bind the UDP/TCP listeners to 127.0.0.1 only (ports: restart to change) */
    int    show_marks;                            /* reference ticks for true heading/pitch; needs the wrapper, so restart to switch on */
    double fov_deg;                               /* GUESS: the engine's own FOV, for the ticks' screen position only */
    int    vr_hdr_linear;                         /* stereo=openvr with the engine's HDR buffer: 1 = send it as linear, 0 = as gamma */
    double vr_view_scale;                         /* stereo=openvr: engine view scale to draw with, 0 = auto (fill the headset) */
    int    vr_gpu_convert;                        /* stereo=openvr: convert the HDR eye image to 8-bit on the GPU before copying */
    int    vr_hud;                                /* stereo=openvr: show HUD and menus on a head-locked panel in the headset */
    double vr_hud_deg, vr_hud_down;               /* in-game HUD panel: width and tilt below straight ahead, degrees */
    double vr_menu_deg, vr_menu_down;             /* menus and books: the same (that panel is fixed in the room) */
    double vr_hud_dist;                           /* both panels' distance, metres */
    int    vr_hud_every;                          /* refresh the in-game HUD every Nth frame (it costs a GPU->CPU copy) */
    double vr_gamma, vr_black;                    /* headset picture: brightness curve (see vr_set_picture) */
    int    vr_idle_frames;                        /* keep SteamVR fed with the last 3D frame while the game shows menus */
    int    vr_hud_overlays;                       /* HUD overlays (shields, gem, item models) onto the panel: 0 off, 1 provider, 2 plain scene */
    int    vr_hud_key, vr_hud_pad;                /* HUD panel show/hide toggle: a virtual-key code, an XInput button mask (0 = none) */
    int    vr_gem_always;                         /* with the HUD toggled off, still show the light gem */
    double vr_gem_x0, vr_gem_y0, vr_gem_x1, vr_gem_y1;   /* where the light gem is on the screen (fractions, y down) */
    int    vr_pipeline;                           /* read the eyes (and the HUD) back a frame later, when the GPU is done */
    int    vr_gpu_gamma;                          /* brightness curve in a GPU pass instead of the CPU loop */
    double wxr_fov;                               /* stereo=wxr: field of view to draw, degrees (0 = the headset's own) */
    int    wxr_roll;                              /* stereo=wxr: draw head roll (WinlatorXR places the picture with it) */
    double wxr_ipd_scale;                         /* stereo=wxr: eye distance = the headset's IPD times this (1 = true scale) */
} config_t;

static config_t cfg;
static char g_dir[MAX_PATH];
static FILE *g_log;

void hl_log(const char *fmt, ...)
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

static double ini_double(const char *ini, const char *key, double def)
{
    char buf[64], dbuf[64];
    snprintf(dbuf, sizeof dbuf, "%g", def);
    GetPrivateProfileStringA("headlook", key, dbuf, buf, sizeof buf, ini);
    return atof(buf);
}

#define STEREO_SBS    1
#define STEREO_OPENVR 2
#define STEREO_WXR    3               /* WinlatorXR (standalone Quest etc.), see vr_wxr.c */

/* "sbs"/"on"/"1" -> 1; "openvr"/"vr" -> 2; "off"/"0"/"" -> 0; anything else is logged and treated as off. */
static int parse_stereo(const char *v, const char *where)
{
    if (!v[0] || !_stricmp(v, "off") || !strcmp(v, "0") || !_stricmp(v, "none")) return 0;
    if (!_stricmp(v, "sbs") || !_stricmp(v, "on") || !strcmp(v, "1")) return STEREO_SBS;
    if (!_stricmp(v, "openvr") || !_stricmp(v, "vr")) return STEREO_OPENVR;
    if (!_stricmp(v, "wxr") || !_stricmp(v, "winlatorxr") || !_stricmp(v, "quest")) return STEREO_WXR;
    hl_log("unknown stereo mode '%s' from %s (use off, sbs, openvr or wxr): treating as off", v, where);
    return 0;
}

static void load_config(void)
{
    char ini[MAX_PATH + 16], sv[32];
    snprintf(ini, sizeof ini, "%sheadlook.ini", g_dir);
    /* stereo mode: the HEADLOOK_STEREO environment variable (set by `STEREO=... tools/run-wine.sh`)
     * wins over the ini key, so the launcher can switch it without editing the ini. */
    if (GetEnvironmentVariableA("HEADLOOK_STEREO", sv, sizeof sv) > 0 && sv[0])
        cfg.stereo = parse_stereo(sv, "HEADLOOK_STEREO");
    else {
        GetPrivateProfileStringA("headlook", "stereo", "off", sv, sizeof sv, ini);
        cfg.stereo = parse_stereo(sv, "headlook.ini");
    }
    cfg.stereo_ipd   = ini_double(ini, "stereo_ipd", 0.21);
    cfg.stereo_swap  = (int)ini_double(ini, "stereo_swap", 0) != 0;
    cfg.listen_loopback = (int)ini_double(ini, "listen_loopback", 0) != 0;
    cfg.show_marks   = (int)ini_double(ini, "show_heading_marks", 1) != 0;
    cfg.fov_deg      = ini_double(ini, "fov_deg", 90);
    cfg.vr_hdr_linear = (int)ini_double(ini, "vr_hdr_linear", 0) != 0;
    cfg.vr_view_scale = ini_double(ini, "vr_view_scale", 0);
    cfg.vr_hud        = (int)ini_double(ini, "vr_hud", 1) != 0;
    cfg.vr_gpu_convert = (int)ini_double(ini, "vr_gpu_convert", 1) != 0;
    cfg.vr_hud_deg    = ini_double(ini, "vr_hud_deg", 45);
    cfg.vr_hud_down   = ini_double(ini, "vr_hud_down", 0);
    cfg.vr_menu_deg   = ini_double(ini, "vr_menu_deg", 50);
    cfg.vr_menu_down  = ini_double(ini, "vr_menu_down", 5);
    cfg.vr_hud_dist   = ini_double(ini, "vr_hud_dist", 1.5);
    cfg.vr_hud_every  = (int)ini_double(ini, "vr_hud_every", 3);
    cfg.vr_gamma      = ini_double(ini, "vr_gamma", 1.3);
    cfg.vr_black      = ini_double(ini, "vr_black", 0.0);
    cfg.vr_idle_frames = (int)ini_double(ini, "vr_idle_frames", 1) != 0;
    cfg.vr_hud_overlays = (int)ini_double(ini, "vr_hud_overlays", 2);
    cfg.vr_hud_key    = (int)ini_double(ini, "vr_hud_key", 0x48);           /* 'H' */
    cfg.vr_hud_pad    = (int)ini_double(ini, "vr_hud_pad", 0x8000);         /* XINPUT_GAMEPAD_Y */
    cfg.vr_gem_always = (int)ini_double(ini, "vr_gem_always", 1) != 0;
    cfg.vr_gem_x0     = ini_double(ini, "vr_gem_x0", 0.40);                 /* a guess: bottom centre */
    cfg.vr_gem_y0     = ini_double(ini, "vr_gem_y0", 0.82);
    cfg.vr_gem_x1     = ini_double(ini, "vr_gem_x1", 0.60);
    cfg.vr_gem_y1     = ini_double(ini, "vr_gem_y1", 1.00);
    cfg.vr_pipeline   = (int)ini_double(ini, "vr_pipeline", 1) != 0;
    cfg.vr_gpu_gamma  = (int)ini_double(ini, "vr_gpu_gamma", 1) != 0;
    cfg.wxr_fov       = ini_double(ini, "wxr_fov", 0);
    cfg.wxr_roll      = (int)ini_double(ini, "wxr_roll", 1) != 0;
    cfg.wxr_ipd_scale = ini_double(ini, "wxr_ipd_scale", 1.0);
    cfg.udp_port     = (int)ini_double(ini, "udp_port", 4242);
    cfg.tcp_port     = (int)ini_double(ini, "tcp_port", 4243);
    cfg.yaw_sign     = ini_double(ini, "yaw_sign", -1) < 0 ? -1 : 1;
    cfg.pitch_sign   = ini_double(ini, "pitch_sign", -1) < 0 ? -1 : 1;
    cfg.roll_sign    = ini_double(ini, "roll_sign", 1) < 0 ? -1 : 1;
    cfg.yaw_gain     = ini_double(ini, "yaw_gain", 1);
    cfg.pitch_gain   = ini_double(ini, "pitch_gain", 1);
    cfg.roll_gain    = ini_double(ini, "roll_gain", 1);
    cfg.max_yaw      = ini_double(ini, "max_yaw", 120);
    cfg.max_pitch    = ini_double(ini, "max_pitch", 70);
    cfg.max_roll     = ini_double(ini, "max_roll", 45);
    cfg.smoothing_ms = ini_double(ini, "smoothing_ms", 25);
    cfg.timeout_ms   = (int)ini_double(ini, "timeout_ms", 500);
    cfg.use_roll     = (int)ini_double(ini, "use_roll", 0) != 0;
    cfg.enabled      = (int)ini_double(ini, "enabled", 1) != 0;
}

/* ------------------------------------------------------------------ hook installation */

static int install_hook(void)
{
    static const unsigned char expect[9] = { 0x89, 0x44, 0x24, 0x24, 0x66, 0x89, 0x4C, 0x24, 0x28 };
    unsigned char *base = (unsigned char *)GetModuleHandleA(NULL);
    unsigned char *site = base + (HOOK_SITE_VA - LINK_BASE);
    DWORD old;
    int rel;

    if (memcmp(site - 4, expect, sizeof expect) != 0) {
        hl_log("hook NOT installed: unexpected bytes at site %p (different Thief2.exe build?)", (void *)site);
        return 0;
    }
    g_camptr_addr = (unsigned)(base + (CAMPTR_VA - LINK_BASE));
    rel = (int)((unsigned char *)hl_stub - (site + 5));
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) {
        hl_log("hook NOT installed: VirtualProtect failed (%lu)", GetLastError());
        return 0;
    }
    site[0] = 0xE8;
    memcpy(site + 1, &rel, 4);
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    hl_log("hook installed at %p (exe base %p), camera pointer slot %#x", (void *)site, (void *)base, g_camptr_addr);
    return 1;
}

/* ------------------------------------------------------------------ stereo (side-by-side) */

typedef void (__cdecl *scene_fn_t)(void);
static scene_fn_t g_scene_orig;
static IDirect3DDevice9 **g_dev_slot;
static const float *g_viewmat;                   /* the engine's view matrix, for the log only */
static int g_stereo_installed;
static volatile int g_stereo_on;                 /* live: 0 makes hl_scene a plain single call */
static volatile float g_ipd = 0.21f;
static volatile int g_swap;
static unsigned long g_stereo_frames, g_stereo_errs;
static int g_hook_ok;                            /* install_hook() succeeded: g_yaw/g_pitch are really being applied */
static volatile int g_marks_on;                  /* live: draw the heading/pitch reference ticks */
static volatile float g_fov_deg = 90.0f;
static volatile int g_vr_pose;                   /* the OpenVR pose owns g_yaw/g_pitch/g_roll (the UDP filter stops writing them) */
static int g_vr_frame;                           /* render thread: this frame's eyes go to the headset */
static float *g_viewscale;                       /* VIEWSCALE_VA, rebased */
static const int *g_horplus;                     /* HORPLUS_VA, rebased */
static float g_vr_scale;                         /* the effective view scale this VR frame is drawn with */
static int g_vr_horplus;
static volatile int g_dump_key;                  /* Insert pressed: dump this frame (the worker polls the key) */
static unsigned g_dump_no;
static volatile int g_hud_visible = 1;           /* the in-game HUD panel (toggled with vr_hud_key / vr_hud_pad) */
static int g_ovl_skip;                           /* render thread: the eye passes skip the HUD overlays (drawn after, once) */
static void (__cdecl *g_ovl_orig)(void);
static void draw_overlays_once(IDirect3DDevice9 *dev);
static float g_engine_scale = 1.0f;              /* the engine's own view scale this frame (for the HUD overlays) */
static volatile DWORD g_last_vr_tick;            /* last VR frame drawn with the HUD panel on: "in game" for vr_Present */
static unsigned to_units(double deg);

/* ---- One-shot diagnostic: create a file named stereo_dump.now next to the DLL and, on the next stereo
 * frame, the DLL (a) writes stereo_1_left_pass.bmp, stereo_2_right_pass.bmp, stereo_3_composite.bmp (the
 * render target after each step) and stereo_4_present.bmp (the back buffer at Present), and (b) logs the
 * order of the Direct3D calls that frame (run-length encoded). The device's method table is patched to do
 * this only on first use of the trigger, so normal runs are untouched. */
static volatile int g_dump_req;                  /* set by the worker thread when the trigger file appears */
static volatile int g_trace;                     /* 1 while the traced frame is running */
#define TR_MAX 4096
static struct { const char *name; unsigned n; } g_tr[TR_MAX];
static int g_tr_len;
static int g_dev_hooked;

static void tr(const char *name)
{
    if (g_tr_len > 0 && g_tr[g_tr_len - 1].name == name) { g_tr[g_tr_len - 1].n++; return; }
    if (g_tr_len < TR_MAX) { g_tr[g_tr_len].name = name; g_tr[g_tr_len].n = 1; g_tr_len++; }
}

static void dump_surface(IDirect3DDevice9 *dev, IDirect3DSurface9 *src, const char *name)
{
    D3DSURFACE_DESC d;
    IDirect3DSurface9 *sys = NULL;
    D3DLOCKED_RECT lr;
    char path[MAX_PATH + 32];
    FILE *f;
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;
    UINT y;
    HRESULT hr;

    IDirect3DSurface9_GetDesc(src, &d);
    if (d.Format != D3DFMT_X8R8G8B8 && d.Format != D3DFMT_A8R8G8B8 && d.Format != D3DFMT_A16B16G16R16F) { hl_log("dump %s: unsupported format %d", name, (int)d.Format); return; }
    hr = IDirect3DDevice9_CreateOffscreenPlainSurface(dev, d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, NULL);
    if (FAILED(hr) || !sys) { hl_log("dump %s: CreateOffscreenPlainSurface failed %#lx", name, (unsigned long)hr); return; }
    hr = IDirect3DDevice9_GetRenderTargetData(dev, src, sys);
    if (FAILED(hr)) { hl_log("dump %s: GetRenderTargetData failed %#lx", name, (unsigned long)hr); IDirect3DSurface9_Release(sys); return; }
    if (SUCCEEDED(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY))) {
        snprintf(path, sizeof path, "%sdump%u_%s", g_dir, g_dump_no, name);
        f = fopen(path, "wb");
        if (f) {
            memset(&fh, 0, sizeof fh); memset(&ih, 0, sizeof ih);
            fh.bfType = 0x4D42; fh.bfOffBits = sizeof fh + sizeof ih; fh.bfSize = fh.bfOffBits + d.Width * d.Height * 4;
            ih.biSize = sizeof ih; ih.biWidth = (LONG)d.Width; ih.biHeight = -(LONG)d.Height; ih.biPlanes = 1; ih.biBitCount = 32;
            fwrite(&fh, sizeof fh, 1, f); fwrite(&ih, sizeof ih, 1, f);
            for (y = 0; y < d.Height; y++) {
                const unsigned char *row = (const unsigned char *)lr.pBits + (size_t)y * (size_t)lr.Pitch;
                if (d.Format != D3DFMT_A16B16G16R16F) { fwrite(row, 4, d.Width, f); continue; }
                {   /* half floats R,G,B,A -> 8-bit B,G,R,X, clamped to 0..1 (no tone mapping) */
                    UINT x;
                    for (x = 0; x < d.Width; x++) {
                        unsigned char px[4];
                        int c;
                        for (c = 0; c < 3; c++) {
                            unsigned short hv = ((const unsigned short *)row)[x * 4 + c];
                            int ex = (hv >> 10) & 31, mant = hv & 1023;
                            float v = ex == 0 ? mant / 16777216.0f : (ex == 31 ? 1.0f : ldexpf(1.0f + mant / 1024.0f, ex - 15));
                            if (hv & 0x8000) v = 0;
                            if (v > 1) v = 1;
                            px[2 - c] = (unsigned char)(v * 255.0f + 0.5f);
                        }
                        px[3] = 255;
                        fwrite(px, 4, 1, f);
                    }
                }
            }
            fclose(f);
            hl_log("dump: wrote dump%u_%s (%ux%u, format %d)", g_dump_no, name, d.Width, d.Height, (int)d.Format);
        }
        IDirect3DSurface9_UnlockRect(sys);
    }
    IDirect3DSurface9_Release(sys);
}

static HRESULT (WINAPI *o_Present)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *);

static void trace_flush(const char *why)
{
    int i;
    g_trace = 0;
    hl_log("trace (%s): D3D calls from the start of that frame's scene draw (%d runs):", why, g_tr_len);
    for (i = 0; i < g_tr_len; i++) hl_log("  %-26s x%u", g_tr[i].name, g_tr[i].n);
    g_tr_len = 0;
}

static HRESULT WINAPI t_Present(IDirect3DDevice9 *d, const RECT *a, const RECT *b, HWND w, const RGNDATA *c)
{
    if (g_trace) {
        IDirect3DSurface9 *bb = NULL;
        tr("Present");
        if (SUCCEEDED(IDirect3DDevice9_GetBackBuffer(d, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
            dump_surface(d, bb, "stereo_4_present.bmp");
            IDirect3DSurface9_Release(bb);
        }
        trace_flush("ended at Present");
    }
    return o_Present(d, a, b, w, c);
}

/* One thunk per traced method; each records a name and forwards with the exact stdcall signature. */
#define TRACE_THUNK0(NAME, IDX)                                                              \
    static HRESULT (WINAPI *o_##NAME)(IDirect3DDevice9 *);                                   \
    static HRESULT WINAPI t_##NAME(IDirect3DDevice9 *d) { if (g_trace) tr(#NAME); return o_##NAME(d); }
TRACE_THUNK0(BeginScene, 41)
TRACE_THUNK0(EndScene, 42)

static HRESULT (WINAPI *o_Clear)(IDirect3DDevice9 *, DWORD, const D3DRECT *, DWORD, D3DCOLOR, float, DWORD);
static HRESULT WINAPI t_Clear(IDirect3DDevice9 *d, DWORD n, const D3DRECT *r, DWORD f, D3DCOLOR c, float z, DWORD s)
{ if (g_trace) tr("Clear"); return o_Clear(d, n, r, f, c, z, s); }

static HRESULT (WINAPI *o_SetRenderTarget)(IDirect3DDevice9 *, DWORD, IDirect3DSurface9 *);
static HRESULT WINAPI t_SetRenderTarget(IDirect3DDevice9 *d, DWORD i, IDirect3DSurface9 *s)
{ if (g_trace) tr("SetRenderTarget"); return o_SetRenderTarget(d, i, s); }

static HRESULT (WINAPI *o_StretchRect)(IDirect3DDevice9 *, IDirect3DSurface9 *, const RECT *, IDirect3DSurface9 *, const RECT *, D3DTEXTUREFILTERTYPE);
static HRESULT WINAPI t_StretchRect(IDirect3DDevice9 *d, IDirect3DSurface9 *a, const RECT *b, IDirect3DSurface9 *c, const RECT *e, D3DTEXTUREFILTERTYPE f)
{ if (g_trace) tr("StretchRect"); return o_StretchRect(d, a, b, c, e, f); }

static HRESULT (WINAPI *o_DrawPrimitive)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT);
static HRESULT WINAPI t_DrawPrimitive(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT s, UINT n)
{ if (g_trace) tr("DrawPrimitive"); return o_DrawPrimitive(d, t, s, n); }

static HRESULT (WINAPI *o_DrawIndexedPrimitive)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
static HRESULT WINAPI t_DrawIndexedPrimitive(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, INT b, UINT m, UINT nv, UINT s, UINT n)
{ if (g_trace) tr("DrawIndexedPrimitive"); return o_DrawIndexedPrimitive(d, t, b, m, nv, s, n); }

static HRESULT (WINAPI *o_DrawPrimitiveUP)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, const void *, UINT);
static HRESULT WINAPI t_DrawPrimitiveUP(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT n, const void *v, UINT s)
{ if (g_trace) tr("DrawPrimitiveUP"); return o_DrawPrimitiveUP(d, t, n, v, s); }

static HRESULT (WINAPI *o_DrawIndexedPrimitiveUP)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void *, D3DFORMAT, const void *, UINT);
static HRESULT WINAPI t_DrawIndexedPrimitiveUP(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT mv, UINT nv, UINT n, const void *i, D3DFORMAT f, const void *v, UINT s)
{ if (g_trace) tr("DrawIndexedPrimitiveUP"); return o_DrawIndexedPrimitiveUP(d, t, mv, nv, n, i, f, v, s); }

static int patch_vt(void **vt, int idx, void *thunk, void **orig_slot);

/* stereo=openvr: every Present hands the back buffer to the headset's HUD/menu panel. In a mission the back
 * buffer holds the HUD over black (stereo_frame leaves the scene out), shown with black as transparent; in menus
 * (no VR frame for 250 ms) it holds the menu, shown opaque. Installed by the worker once the device exists. */
typedef HRESULT (WINAPI *present_fn_t)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *);
static present_fn_t o_vr_Present;                /* what the slot held when we (last) patched it */
static present_fn_t g_first_present;             /* what it held the first time: used on re-entry (see below) */
static volatile unsigned long g_present_calls;
static int g_in_present;
static HRESULT WINAPI vr_Present(IDirect3DDevice9 *d, const RECT *a, const RECT *b, HWND w, const RGNDATA *c)
{
    HRESULT hr;
    /* Something else re-patched the slot after us and we re-patched over it (ensure_present_hook): if that
     * hook calls "the original" and that turns out to be us again, don't loop. */
    if (g_in_present) return g_first_present(d, a, b, w, c);
    g_present_calls++;
    g_in_present = 1;
    if (cfg.stereo == STEREO_WXR) {
        wxr_present_cfg_t pc;
        int in_game = GetTickCount() - g_last_vr_tick < 250;
        pc.hud = cfg.vr_hud && (g_hud_visible || cfg.vr_gem_always);
        pc.gem_only = !g_hud_visible;
        pc.gem_x0 = (float)cfg.vr_gem_x0; pc.gem_y0 = (float)cfg.vr_gem_y0;
        pc.gem_x1 = (float)cfg.vr_gem_x1; pc.gem_y1 = (float)cfg.vr_gem_y1;
        pc.hud_deg = (float)cfg.vr_hud_deg; pc.hud_down = (float)cfg.vr_hud_down; pc.dist = (float)cfg.vr_hud_dist;
        pc.gamma = (float)cfg.vr_gamma; pc.black = (float)cfg.vr_black;
        wxr_present(d, in_game, &pc);
    }
    if (cfg.stereo == STEREO_OPENVR && vr_ready()) {
        int in_game = GetTickCount() - g_last_vr_tick < 250;
        if (!in_game && cfg.vr_idle_frames) vr_idle_frame(d);  /* no 3D frame lately: menus, loading, paused */
        else if (!in_game) vr_device_reset();         /* no idle frames: still drop the kept eye surfaces in menus */
        if (cfg.vr_hud && in_game && !g_hud_visible && !cfg.vr_gem_always) vr_hud_hide();   /* toggled off: menus still show */
        else if (cfg.vr_hud) {
            double t = vr_ms();
            vr_hud_cfg_t hc;
            hc.hud_deg = (float)cfg.vr_hud_deg; hc.hud_down = (float)cfg.vr_hud_down;
            hc.menu_deg = (float)cfg.vr_menu_deg; hc.menu_down = (float)cfg.vr_menu_down;
            hc.dist = (float)cfg.vr_hud_dist; hc.every = cfg.vr_hud_every;
            hc.gem_only = in_game && !g_hud_visible;  /* toggled off: just the light gem (vr_gem_always) */
            hc.gem_x0 = (float)cfg.vr_gem_x0; hc.gem_y0 = (float)cfg.vr_gem_y0;
            hc.gem_x1 = (float)cfg.vr_gem_x1; hc.gem_y1 = (float)cfg.vr_gem_y1;
            hc.pipeline = cfg.vr_pipeline;
            vr_hud_present(d, in_game, &hc);
            vr_prof(P_HUD, vr_ms() - t);
        }
        else vr_hud_hide();
    }
    {
        double t = vr_ms();
        hr = o_vr_Present(d, a, b, w, c);
        vr_prof(P_PRESENT, vr_ms() - t);
    }
    g_in_present = 0;
    return hr;
}

/* Device Reset (slot 16): the engine resets its device when switching between menus and a mission. Our kept
 * default-pool surfaces (eye captures, the HUD copy; see vr_device_reset) must be released first or it fails. */
typedef HRESULT (WINAPI *reset_fn_t)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
static reset_fn_t o_vr_Reset, g_first_reset;
static int g_in_reset;
static HRESULT WINAPI vr_Reset(IDirect3DDevice9 *d, D3DPRESENT_PARAMETERS *pp)
{
    HRESULT hr;
    if (g_in_reset) return g_first_reset(d, pp);  /* re-entered through someone else's hook: see vr_Present */
    g_in_reset = 1;
    if (cfg.stereo == STEREO_WXR) wxr_device_reset(); else vr_device_reset();
    hr = o_vr_Reset(d, pp);
    g_in_reset = 0;
    if (FAILED(hr)) hl_log("openvr: device Reset failed: %#lx", (unsigned long)hr);
    return hr;
}

static const char *module_of(const void *addr, char *buf, size_t n)
{
    HMODULE m = NULL;
    snprintf(buf, n, "?");
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &m) && m) {
        char path[MAX_PATH], *sl;
        GetModuleFileNameA(m, path, sizeof path);
        sl = strrchr(path, '\\');
        snprintf(buf, n, "%s+%#lx", sl ? sl + 1 : path, (unsigned long)((const char *)addr - (const char *)m));
    }
    return buf;
}

/* The engine only fills 0xA36040 once a mission's 3D view starts, so menus before the first map never
 * reached the HUD panel. Catch the device when the game creates it instead: at start-up we make our own
 * IDirect3D9 (the class's method table lives in d3d9.dll and is shared with the game's) and hook
 * CreateDevice (slot 16) in it. */
typedef HRESULT (WINAPI *create_device_fn_t)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);
static create_device_fn_t o_CreateDevice;
static IDirect3DDevice9 *volatile g_created_dev;
static HRESULT WINAPI hk_CreateDevice(IDirect3D9 *d3d, UINT ad, D3DDEVTYPE ty, HWND w, DWORD fl, D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **out)
{
    HRESULT hr = o_CreateDevice(d3d, ad, ty, w, fl, pp, out);
    if (SUCCEEDED(hr) && out && *out) {
        g_created_dev = *out;
        hl_log("openvr: game created its D3D9 device %p (%ux%u)", (void *)*out, pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0);
    }
    return hr;
}

/* Hook CreateDevice in this IDirect3D9's method table, once. Two callers can race here (the start-up thread
 * below, and the game's own thread through headlook_d3d_created); patching twice would save our own hook as
 * "the original" and recurse, hence the lock and the already-hooked check. */
static void hook_d3d_object(IDirect3D9 *d3d, const char *how)
{
    static volatile LONG lock;
    void **vt;
    if (!d3d) return;
    while (InterlockedCompareExchange(&lock, 1, 0) != 0) Sleep(0);
    vt = *(void ***)d3d;
    if (vt[16] != (void *)hk_CreateDevice && patch_vt(vt, 16, (void *)hk_CreateDevice, (void **)&o_CreateDevice))
        hl_log("openvr: IDirect3D9::CreateDevice hooked (%s)", how);
    InterlockedExchange(&lock, 0);
}

static void hook_create_device(void)
{
    HMODULE d9 = LoadLibraryA("d3d9.dll");
    IDirect3D9 *(WINAPI *create)(UINT) = d9 ? (IDirect3D9 *(WINAPI *)(UINT))(void *)GetProcAddress(d9, "Direct3DCreate9") : NULL;
    IDirect3D9 *d3d = create ? create(D3D_SDK_VERSION) : NULL;
    if (!d3d) { hl_log("openvr: could not make an IDirect3D9 to hook CreateDevice (menus reach the headset only after a map loads)"); return; }
    hook_d3d_object(d3d, "own IDirect3D9 at start-up");
    IDirect3D9_Release(d3d);
}

/* Called by the d3d9.dll stand-in (d3d9proxy/) right after the game's Direct3DCreate9, on the game's thread,
 * before it can create its device. With the stand-in, Thief2.exe needs no patch: this DLL is loaded by it. */
__declspec(dllexport) void __cdecl headlook_d3d_created(void *d3d)
{
    if (cfg.stereo == STEREO_OPENVR || cfg.stereo == STEREO_WXR) hook_d3d_object((IDirect3D9 *)d3d, "the game's IDirect3D9, from the d3d9.dll stand-in");
}

/* Worker, every tick: (re)install vr_Present in the device's Present slot. The first build patched it once
 * and the HUD code then never ran (2026-09-24): something else evidently replaced the slot afterwards. */
static void ensure_present_hook(void)
{
    static int patches;
    void **vt;
    char was[MAX_PATH + 32];
    IDirect3DDevice9 *dev = g_created_dev ? g_created_dev : (g_dev_slot ? *g_dev_slot : NULL);
    if (!dev) return;
    vt = *(void ***)dev;
    if (vt[16] != (void *)vr_Reset) {
        static int rpatches;
        module_of(vt[16], was, sizeof was);
        if (patch_vt(vt, 16, (void *)vr_Reset, (void **)&o_vr_Reset)) {
            if (!g_first_reset) g_first_reset = o_vr_Reset;
            if (rpatches++ < 20) hl_log("openvr: Reset hook %s (slot held %s)", rpatches == 1 ? "installed" : "RE-installed", was);
        }
    }
    if (vt[17] == (void *)vr_Present) return;
    module_of(vt[17], was, sizeof was);
    if (!patch_vt(vt, 17, (void *)vr_Present, (void **)&o_vr_Present)) {
        if (patches++ < 5) hl_log("openvr: Present hook: VirtualProtect on the method table failed (%lu)", GetLastError());
        return;
    }
    if (!g_first_present) g_first_present = o_vr_Present;
    if (patches++ < 20)
        hl_log("openvr: Present hook %s (device %p, table %p; slot held %s)", patches == 1 ? "installed" : "RE-installed: the slot had been replaced",
               (void *)dev, (void *)vt, was);
}

static int patch_vt(void **vt, int idx, void *thunk, void **orig_slot)
{
    DWORD old;
    if (!VirtualProtect(&vt[idx], sizeof(void *), PAGE_READWRITE, &old)) return 0;
    *orig_slot = vt[idx];
    vt[idx] = thunk;
    VirtualProtect(&vt[idx], sizeof(void *), old, &old);
    return 1;
}

static void hook_device_for_trace(IDirect3DDevice9 *dev)
{
    void **vt = *(void ***)dev;
    if (g_dev_hooked) return;
    g_dev_hooked = 1;
    patch_vt(vt, 17, (void *)t_Present, (void **)&o_Present);
    patch_vt(vt, 41, (void *)t_BeginScene, (void **)&o_BeginScene);
    patch_vt(vt, 42, (void *)t_EndScene, (void **)&o_EndScene);
    patch_vt(vt, 43, (void *)t_Clear, (void **)&o_Clear);
    patch_vt(vt, 37, (void *)t_SetRenderTarget, (void **)&o_SetRenderTarget);
    patch_vt(vt, 34, (void *)t_StretchRect, (void **)&o_StretchRect);
    patch_vt(vt, 81, (void *)t_DrawPrimitive, (void **)&o_DrawPrimitive);
    patch_vt(vt, 82, (void *)t_DrawIndexedPrimitive, (void **)&o_DrawIndexedPrimitive);
    patch_vt(vt, 83, (void *)t_DrawPrimitiveUP, (void **)&o_DrawPrimitiveUP);
    patch_vt(vt, 84, (void *)t_DrawIndexedPrimitiveUP, (void **)&o_DrawIndexedPrimitiveUP);
    hl_log("trace: device method table patched (device %p, table %p)", (void *)dev, (void *)vt);
}

static void stereo_diag(const unsigned char *cam, const D3DSURFACE_DESC *d, double heading_deg, double rx, double ry)
{
    static DWORD next;
    DWORD now = GetTickCount();
    if ((int)(now - next) < 0) return;
    next = now + 5000;
    hl_log("stereo: %lu frames (%lu errors); target %ux%u fmt %d; ipd %.3f%s; view heading %.1f deg, eye axis (%.3f, %.3f)",
           g_stereo_frames, g_stereo_errs, d->Width, d->Height, (int)d->Format, (double)g_ipd,
           g_swap ? " (eyes swapped)" : "", heading_deg, rx, ry);
    if (g_viewmat && !IsBadReadPtr(g_viewmat, 36))
        hl_log("stereo: view matrix %.3f %.3f %.3f | %.3f %.3f %.3f | %.3f %.3f %.3f (camera struct heading %.1f)",
               g_viewmat[0], g_viewmat[1], g_viewmat[2], g_viewmat[3], g_viewmat[4], g_viewmat[5],
               g_viewmat[6], g_viewmat[7], g_viewmat[8],
               *(const unsigned short *)(cam + 0x18) * 360.0 / 65536.0);
}

/* ---- Reference marks (see the block comment near the top of the file). */
typedef struct { float x, y, z, rhw; D3DCOLOR c; } mark_vtx_t;
#define MARK_FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)

static short to_signed16(unsigned u) { return (short)(u & 0xFFFFu); }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void draw_reference_marks(IDirect3DDevice9 *dev, UINT width, UINT height)
{
    static const double D2R = 3.14159265358979323846 / 180.0;
    /* The offset actually being added to the drawn view right now (see the hl_stub asm): the same
     * quantity the ini's sign comments define as "+ turns the view left" / "+ tilts it down". */
    double yaw_deg   = clampf((float)(to_signed16(g_yaw)   * (360.0 / 65536.0)), -89.0f, 89.0f);
    double pitch_deg = clampf((float)(to_signed16(g_pitch) * (360.0 / 65536.0)), -89.0f, 89.0f);
    double hfov = clampf(g_fov_deg, 10.0f, 170.0f) * D2R * 0.5;
    /* One focal length in pixels serves both axes (square-pixel perspective: f = (w/2)/tan(hfov/2) =
     * (h/2)/tan(vfov/2) when vfov is the aspect-consistent angle for hfov), so there is no separate
     * vertical-FOV step. True heading appears to the RIGHT when the view has turned left (+yaw), i.e. in
     * the same direction as the offset itself; true pitch appears UP (smaller y) when the view tilted
     * down (+pitch). */
    double f_px = ((double)width / 2.0) / tan(hfov);
    float cx = clampf((float)(width / 2.0 + f_px * tan(yaw_deg * D2R)), -4.0f * width, 5.0f * width);
    float cy = clampf((float)(height / 2.0 - f_px * tan(pitch_deg * D2R)), -4.0f * height, 5.0f * height);
    float len = clampf((float)height * 0.035f, 6.0f, 60.0f);
    D3DCOLOR col = 0xFFFFC000;                                    /* opaque amber */
    mark_vtx_t v[8];
    DWORD old_zenable, old_blend, old_cull, old_cop, old_carg1, old_aop, old_fvf = 0;
    IDirect3DBaseTexture9 *old_tex = NULL;

    v[0].x = 0;                  v[0].y = cy; v[0].z = 0; v[0].rhw = 1; v[0].c = col;   /* left edge tick */
    v[1].x = len;                v[1].y = cy; v[1].z = 0; v[1].rhw = 1; v[1].c = col;
    v[2].x = (float)width - len; v[2].y = cy; v[2].z = 0; v[2].rhw = 1; v[2].c = col;   /* right edge tick */
    v[3].x = (float)width;       v[3].y = cy; v[3].z = 0; v[3].rhw = 1; v[3].c = col;
    v[4].x = cx; v[4].y = 0;                   v[4].z = 0; v[4].rhw = 1; v[4].c = col;  /* top edge tick */
    v[5].x = cx; v[5].y = len;                 v[5].z = 0; v[5].rhw = 1; v[5].c = col;
    v[6].x = cx; v[6].y = (float)height - len; v[6].z = 0; v[6].rhw = 1; v[6].c = col;  /* bottom edge tick */
    v[7].x = cx; v[7].y = (float)height;       v[7].z = 0; v[7].rhw = 1; v[7].c = col;

    /* Save and pin just enough state that the ticks draw as flat opaque lines regardless of whatever the
     * 3D scene left behind, then restore it so the engine's own 2D/HUD drawing that follows is unaffected. */
    /* A state block captures everything (FVF, shaders, all stages), so nothing the ticks set can leak into the
     * engine's next draw. The old code left the FVF set to the ticks' own (no texture coordinates): the prime
     * suspect for the "left eye drawn untextured" glitch (DEVLOG 2026-09-24). Made and released every call:
     * cheap, and nothing outlives the frame to upset a device Reset. */
    IDirect3DStateBlock9 *sb = NULL;
    if (FAILED(IDirect3DDevice9_CreateStateBlock(dev, D3DSBT_ALL, &sb))) sb = NULL;
    IDirect3DDevice9_GetFVF(dev, &old_fvf);
    IDirect3DDevice9_GetRenderState(dev, D3DRS_ZENABLE, &old_zenable);
    IDirect3DDevice9_GetRenderState(dev, D3DRS_ALPHABLENDENABLE, &old_blend);
    IDirect3DDevice9_GetRenderState(dev, D3DRS_CULLMODE, &old_cull);
    IDirect3DDevice9_GetTexture(dev, 0, &old_tex);
    IDirect3DDevice9_GetTextureStageState(dev, 0, D3DTSS_COLOROP, &old_cop);
    IDirect3DDevice9_GetTextureStageState(dev, 0, D3DTSS_COLORARG1, &old_carg1);
    IDirect3DDevice9_GetTextureStageState(dev, 0, D3DTSS_ALPHAOP, &old_aop);

    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, D3DZB_FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetTexture(dev, 0, NULL);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    IDirect3DDevice9_SetFVF(dev, MARK_FVF);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_LINELIST, 4, v, sizeof(mark_vtx_t));

    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, old_zenable);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, old_blend);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, old_cull);
    IDirect3DDevice9_SetTexture(dev, 0, old_tex);
    if (old_tex) IDirect3DBaseTexture9_Release(old_tex);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLOROP, old_cop);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLORARG1, old_carg1);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAOP, old_aop);
    IDirect3DDevice9_SetFVF(dev, old_fvf);
    if (sb) { IDirect3DStateBlock9_Apply(sb); IDirect3DStateBlock9_Release(sb); }
}

/* Draws both eyes. The engine draws the scene into its OWN offscreen render target: it calls SetRenderTarget
 * inside the scene call and afterwards StretchRects that surface onto the back buffer (seen in the D3D call
 * trace, DEVLOG 2026-09-21). So the surface to copy from, and to put the composite back into, is whatever
 * GetRenderTarget returns after each pass, not the one that was current on entry. The scene renderer is
 * always called at least once, so the caller must not draw again. */
static void stereo_frame(IDirect3DDevice9 *dev, const unsigned char *cam)
{
    IDirect3DSurface9 *cur = NULL, *comp = NULL;
    D3DSURFACE_DESC d;
    RECT half[2];
    double hd, rx, ry;
    float step;
    int pass, no_desktop_sbs = 0;
    HRESULT hr;

    /* WinlatorXR: the HUD is always drawn apart (and put into both eyes at Present), even with vr_hud 0 (then dropped). */
    g_ovl_skip = g_vr_frame && (cfg.vr_hud || cfg.stereo == STEREO_WXR) && cfg.vr_hud_overlays && g_ovl_orig;

    if (g_trace) trace_flush("no Present seen before the next frame");
    if (g_dump_req || g_dump_key) {                    /* start the one-shot diagnostic on this frame */
        g_dump_req = 0;
        g_dump_key = 0;
        g_dump_no++;
        hl_log("dump %u: view scale %.3f (engine's current %.3f), hor+ %d, vr frame %d",
               g_dump_no, (double)g_vr_scale, g_viewscale ? (double)*g_viewscale : -1.0, g_horplus ? *g_horplus : -1, g_vr_frame);
        hook_device_for_trace(dev);
        g_tr_len = 0;
        tr("-- hl_scene begins --");
        g_trace = 1;
    }

    /* Eye axis: heading is 0..65535 = 360 deg counter-clockwise; forward = (cos h, sin h), right = (sin h, -cos h).
     * Checked against the engine's view matrix in headlook.log: its second row is the left vector, (-sin h, cos h). */
    hd = (double)((*(const unsigned short *)(cam + 0x18) + (g_enabled ? g_yaw : 0u)) & 0xFFFFu) * (6.283185307179586 / 65536.0);
    rx = sin(hd);
    ry = -cos(hd);
    step = (g_swap ? -1.0f : 1.0f) * g_ipd * 0.5f;
    memset(&d, 0, sizeof d);
    if (g_vr_frame && cfg.stereo == STEREO_OPENVR) vr_set_picture((float)cfg.vr_gamma, (float)cfg.vr_black);   /* before the passes: the GPU curve uses it */

    for (pass = 0; pass < 2; pass++) {
        float s = pass == 0 ? -step : step;            /* left eye is the -right side */
        g_eye_dx = (float)(s * rx);
        g_eye_dy = (float)(s * ry);
        g_eye_active = 1;
        if (pass == 1) {                               /* second eye starts from an empty colour and depth buffer (the scene target is current now) */
            hr = IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0xFF000000, 1.0f, 0);
            if (FAILED(hr)) hr = IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF000000, 1.0f, 0);
            if (FAILED(hr) && g_stereo_errs++ < 3) hl_log("stereo: Clear failed: %#lx", (unsigned long)hr);
        }
        {
            double t = vr_ms();
            g_scene_orig();
            if (g_vr_frame) vr_prof(pass == 0 ? P_DRAW_L : P_DRAW_R, vr_ms() - t);
        }
        g_eye_active = 0;

        cur = NULL;
        if (FAILED(IDirect3DDevice9_GetRenderTarget(dev, 0, &cur)) || !cur) {
            if (g_stereo_errs++ < 3) hl_log("stereo: GetRenderTarget failed after pass %d", pass);
            break;
        }
        if (pass == 0) {
            IDirect3DSurface9_GetDesc(cur, &d);
            half[0].left = 0;                   half[0].top = 0; half[0].right = (LONG)(d.Width / 2); half[0].bottom = (LONG)d.Height;
            half[1].left = (LONG)(d.Width / 2); half[1].top = 0; half[1].right = (LONG)d.Width;       half[1].bottom = (LONG)d.Height;
        }
        if (pass == 0 && !(no_desktop_sbs = g_vr_frame && (cfg.vr_hud || cfg.stereo == STEREO_WXR) && !g_trace)) {
            /* Default-pool surface: made and released every frame, because one that outlives the frame would make
             * the engine's later device Reset (mode switches between menu and mission) fail. */
            hr = IDirect3DDevice9_CreateRenderTarget(dev, d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &comp, NULL);
            if (FAILED(hr) || !comp) {
                if (g_stereo_errs++ < 3) hl_log("stereo: CreateRenderTarget %ux%u fmt %d failed: %#lx", d.Width, d.Height, (int)d.Format, (unsigned long)hr);
                IDirect3DSurface9_Release(cur);
                break;                                 /* this frame stays a plain single (left-eye) view */
            }
        }
        /* Drawn at the pass's own full resolution, before the StretchRect below squeezes it into its
         * half along with the rest of the scene, so no separate stereo-squeeze math is needed here. Same
         * screen position both eyes: it marks a direction, not a point in space, so it should have no
         * parallax. */
        if (g_marks_on && !g_vr_frame) draw_reference_marks(dev, d.Width, d.Height);   /* not in VR: placed for the wrong FOV */
        if (g_vr_frame && cfg.stereo == STEREO_WXR) wxr_capture_eye(dev, cur, pass ^ (g_swap ? 1 : 0), (float)cfg.vr_gamma, (float)cfg.vr_black);
        else if (g_vr_frame) vr_capture_eye(dev, cur, pass ^ (g_swap ? 1 : 0), cfg.vr_gpu_convert, cfg.vr_gpu_gamma);   /* pass 0 is the right eye when swapped */
        if (g_trace) {
            tr(pass == 0 ? "-- left pass returned --" : "-- right pass returned --");
            dump_surface(dev, cur, pass == 0 ? "stereo_1_left_pass.bmp" : "stereo_2_right_pass.bmp");
        }
        if (comp) {                                    /* the desktop side-by-side picture (not built when it would be blacked out) */
            hr = IDirect3DDevice9_StretchRect(dev, cur, NULL, comp, &half[pass], D3DTEXF_LINEAR);
            if (FAILED(hr) && g_stereo_errs++ < 3) hl_log("stereo: StretchRect (eye %d into half) failed: %#lx", pass, (unsigned long)hr);
        }
        if (pass == 1) {
            if (no_desktop_sbs || !comp) {              /* HUD panel: leave the scene out of the back buffer (see vr_Present) */
                hr = IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0x00000000, 1.0f, 0);
            } else
            hr = IDirect3DDevice9_StretchRect(dev, comp, NULL, cur, NULL, D3DTEXF_NONE);
            if (FAILED(hr) && g_stereo_errs++ < 3) hl_log("stereo: StretchRect (composite back) failed: %#lx", (unsigned long)hr);
            if (g_trace) {
                tr("-- composite blitted back --");
                dump_surface(dev, cur, "stereo_3_composite.bmp");
            }
        }
        IDirect3DSurface9_Release(cur);
    }

    if (g_ovl_skip) {                                 /* the HUD overlays, once, onto the (cleared) panel target */
        g_ovl_skip = 0;
        draw_overlays_once(dev);
    }
    if (g_vr_frame) {
        if (cfg.stereo == STEREO_WXR) wxr_end_frame(g_vr_scale, g_vr_horplus);
        else vr_end_frame(dev, g_vr_scale, g_vr_horplus, cfg.vr_hdr_linear, cfg.vr_pipeline);
        g_last_vr_tick = GetTickCount();
    }
    g_stereo_frames++;
    stereo_diag(cam, &d, hd * 180.0 / 3.141592653589793, rx, ry);
    if (comp) IDirect3DSurface9_Release(comp);
}

/* ---- HUD overlays onto the panel. The engine draws its queued screen overlays (health shields, item frame, the
 * inventory/weapon models; DEVLOG 2026-09-25) once per frame at the end of the first scene call, into that eye's
 * picture only. In VR frames with the HUD panel on, both eye passes skip that call (the queue survives), and once
 * both eyes are captured and the scene target cleared, it is run once here at the engine's own view scale, inside
 * the engine's own scene brackets, so the overlays land on the target that becomes the panel. */
static void __cdecl hl_overlays(void)
{
    if (g_ovl_skip) return;
    g_ovl_orig();
}

typedef void (__attribute__((thiscall)) *prov_begin_t)(void *self, int arg);
typedef void (__attribute__((thiscall)) *prov_end_t)(void *self);

static void draw_overlays_once(IDirect3DDevice9 *dev)
{
    unsigned char *base = (unsigned char *)GetModuleHandleA(NULL);
    void *prov = *(void **)(base + (PROVIDER_VA - LINK_BASE));
    float saved = g_viewscale ? *g_viewscale : 0;
    static int logged;
    if (g_viewscale) *g_viewscale = g_engine_scale;
    if (cfg.vr_hud_overlays == 1 && prov) {
        void **vt = *(void ***)prov;
        ((prov_begin_t)vt[0])(prov, *(int *)(base + (PROV_ARG_VA - LINK_BASE)));
        g_ovl_orig();
        ((prov_end_t)vt[1])(prov);
    } else {
        IDirect3DDevice9_BeginScene(dev);
        g_ovl_orig();
        IDirect3DDevice9_EndScene(dev);
    }
    if (g_viewscale) *g_viewscale = saved;
    if (!logged++) hl_log("openvr: HUD overlays drawn once per frame onto the panel (mode %d: %s)", cfg.vr_hud_overlays,
                          cfg.vr_hud_overlays == 1 ? "engine's scene brackets" : "plain BeginScene/EndScene");
}

static int install_overlay_hook(void)
{
    unsigned char *base = (unsigned char *)GetModuleHandleA(NULL);
    unsigned char *site = base + (OVL_CALL_VA - LINK_BASE);
    unsigned char *fn = base + (OVL_FN_VA - LINK_BASE);
    DWORD old;
    int cur, rel;
    memcpy(&cur, site + 1, 4);
    if (site[0] != 0xE8 || site + 5 + cur != fn) {
        hl_log("openvr: HUD overlay hook NOT installed: unexpected code at %p", (void *)site);
        return 0;
    }
    g_ovl_orig = (void (__cdecl *)(void))fn;
    rel = (int)((unsigned char *)hl_overlays - (site + 5));
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy(site + 1, &rel, 4);
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    hl_log("openvr: HUD overlay hook installed at %p", (void *)site);
    return 1;
}

/* Replaces `call FUN_005cee30` in the hardware-render branch: used for stereo, for the reference marks, or
 * both (whichever was requested at start-up; see install_stereo()). Anything unusual (camera not on the
 * player, i.e. a cutscene or remote camera, no device yet) gets the original single draw with nothing added. */
static void __cdecl hl_scene(void)
{
    IDirect3DDevice9 *dev = g_dev_slot ? *g_dev_slot : NULL;
    const unsigned char *cam = NULL;
    int in_player_view;

    if (g_camptr_addr && !IsBadReadPtr((void *)g_camptr_addr, 4))
        cam = *(const unsigned char *const *)g_camptr_addr;
    in_player_view = dev && cam && !IsBadReadPtr(cam, 0x28) && *(const int *)cam == 0;

    if (in_player_view && g_stereo_on) {
        double yaw_r, pitch_u;
        g_vr_frame = 0;
        if (cfg.stereo == STEREO_OPENVR && vr_begin_frame(dev, &yaw_r, &pitch_u)) {
            /* Same sign convention as the OpenTrack packets hmd_bridge sent (yaw + = right, pitch + = up), so the
             * ini's yaw_sign/pitch_sign apply unchanged; gains, clamps, smoothing and roll do not (the headset
             * picture must match the head 1:1; the compositor reprojects the roll we do not draw). */
            g_yaw = to_units(yaw_r * cfg.yaw_sign);
            g_pitch = to_units(pitch_u * cfg.pitch_sign);
            g_roll = 0;
            g_vr_pose = 1;
            g_vr_frame = 1;
        } else if (cfg.stereo == STEREO_WXR) {
            double roll_r, ipd_m;
            if (wxr_begin_frame(&yaw_r, &pitch_u, &roll_r, &ipd_m)) {
                /* WinlatorXR places the picture at the full head pose it stored for this frame, roll included, so
                 * roll is drawn (the engine's bank; roll_sign flips it if it turns the wrong way). */
                g_yaw = to_units(yaw_r * cfg.yaw_sign);
                g_pitch = to_units(pitch_u * cfg.pitch_sign);
                g_roll = cfg.wxr_roll ? to_units(roll_r * cfg.roll_sign) : 0;
                g_ipd = (float)(ipd_m / 0.3048 * cfg.wxr_ipd_scale);   /* 1 game unit = 1 foot */
                g_vr_pose = 1;
                g_vr_frame = 1;
            }
        }
        if (g_vr_frame && g_viewscale && g_horplus) {
            /* Widen the engine's view to fill the headset: write its view scale for this frame only (restored
             * below, so menus, cutscenes and the non-VR path keep the engine's own). The camera zoom (bow, spyglass)
             * multiplies it; it is divided out so the headset picture keeps a fixed scale. */
            float engine_scale = *g_viewscale, zoom = *(const float *)(cam + 4);
            g_engine_scale = engine_scale;
            if (!(zoom > 0.05f && zoom < 50.0f)) zoom = 1.0f;
            g_vr_horplus = *g_horplus;
            if (cfg.stereo == STEREO_WXR) {
                IDirect3DSurface9 *bb = NULL;
                D3DSURFACE_DESC bd;
                memset(&bd, 0, sizeof bd);
                if (SUCCEEDED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) { IDirect3DSurface9_GetDesc(bb, &bd); IDirect3DSurface9_Release(bb); }
                g_vr_scale = cfg.vr_view_scale > 0 ? (float)cfg.vr_view_scale : wxr_view_scale(g_vr_horplus, bd.Width, bd.Height, (float)cfg.wxr_fov);
            } else
            g_vr_scale = cfg.vr_view_scale > 0 ? (float)cfg.vr_view_scale : vr_auto_scale(g_vr_horplus);
            *g_viewscale = g_vr_scale / zoom;
            stereo_frame(dev, cam);      /* draws both eyes, including the reference marks in each */
            *g_viewscale = engine_scale;
            return;
        }
        stereo_frame(dev, cam);          /* draws both eyes, including the reference marks in each */
        return;
    }
    g_eye_active = 0;
    g_scene_orig();
    if (in_player_view && g_marks_on) {
        IDirect3DSurface9 *cur = NULL;
        if (SUCCEEDED(IDirect3DDevice9_GetRenderTarget(dev, 0, &cur)) && cur) {
            D3DSURFACE_DESC d;
            IDirect3DSurface9_GetDesc(cur, &d);
            draw_reference_marks(dev, d.Width, d.Height);
            IDirect3DSurface9_Release(cur);
        }
    }
}

static int install_stereo(void)
{
    unsigned char *base = (unsigned char *)GetModuleHandleA(NULL);
    unsigned char *site = base + (WRAP_SITE_VA - LINK_BASE);
    unsigned char *fn = base + (SCENE_FN_VA - LINK_BASE);
    DWORD old;
    int cur, rel;

    memcpy(&cur, site + 1, 4);
    if (site[0] != 0xE8 || site + 5 + cur != fn) {
        hl_log("scene-call wrapper NOT installed: the call at %p is not a call to %p (different Thief2.exe build?)", (void *)site, (void *)fn);
        return 0;
    }
    g_scene_orig = (scene_fn_t)fn;
    g_dev_slot = (IDirect3DDevice9 **)(base + (D3DDEV_PTR_VA - LINK_BASE));
    g_viewmat = (const float *)(base + (VIEWMAT_VA - LINK_BASE));
    g_viewscale = (float *)(base + (VIEWSCALE_VA - LINK_BASE));
    g_horplus = (const int *)(base + (HORPLUS_VA - LINK_BASE));
    rel = (int)((unsigned char *)hl_scene - (site + 5));
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) {
        hl_log("scene-call wrapper NOT installed: VirtualProtect failed (%lu)", GetLastError());
        return 0;
    }
    memcpy(site + 1, &rel, 4);
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    hl_log("scene-call wrapper installed at %p (stereo and/or reference marks); device slot %p", (void *)site, (void *)g_dev_slot);
    return 1;
}

/* For the controller DLL (xinput_joy's dinput.dll): the Quest Touch controllers as an Xbox pad (XINPUT_STATE).
 * Returns 0 when not in WinlatorXR mode, 1 with fresh data, 2 in WinlatorXR mode without data yet (out neutral). */
__declspec(dllexport) int __cdecl headlook_xr_pad(void *xinput_state)
{
    if (cfg.stereo != STEREO_WXR) return 0;
    return wxr_xinput(xinput_state) ? 1 : 2;
}

/* For the backends' own dumps (vr_wxr.c): same files and numbering as the stereo diagnostic. */
void hl_dump_surface(IDirect3DDevice9 *dev, IDirect3DSurface9 *src, const char *name) { dump_surface(dev, src, name); }

/* ------------------------------------------------------------------ tracking data */

static double clampd(double v, double lim) { return v > lim ? lim : (v < -lim ? -lim : v); }
static unsigned to_units(double deg) { return (unsigned)((int)floor(deg * (65536.0 / 360.0) + 0.5)) & 0xFFFFu; }

static double tgt_yaw, tgt_pitch, tgt_roll;       /* degrees, latest packet, already signed/gained/clamped */
static double cur_yaw, cur_pitch, cur_roll;       /* degrees, smoothed */
static DWORD  last_rx;
static unsigned long n_packets, n_udp, n_tcp;

static void take_packet(const unsigned char *p, int via_tcp)
{
    double v[6];
    memcpy(v, p, sizeof v);
    if (!(fabs(v[3]) < 1e6 && fabs(v[4]) < 1e6 && fabs(v[5]) < 1e6)) return;    /* also rejects NaN */
    tgt_yaw   = clampd(v[3] * cfg.yaw_sign   * cfg.yaw_gain,   cfg.max_yaw);
    tgt_pitch = clampd(v[4] * cfg.pitch_sign * cfg.pitch_gain, cfg.max_pitch);
    tgt_roll  = cfg.use_roll ? clampd(v[5] * cfg.roll_sign * cfg.roll_gain, cfg.max_roll) : 0.0;
    last_rx = GetTickCount();
    if (via_tcp) n_tcp++; else n_udp++;
    if (++n_packets == 1)
        hl_log("first packet: yaw %.2f pitch %.2f roll %.2f (x %.1f y %.1f z %.1f)", v[3], v[4], v[5], v[0], v[1], v[2]);
}

static void step_filter(void)
{
    static LARGE_INTEGER freq, prev;
    LARGE_INTEGER now;
    double dt_ms = 10.0, a;

    QueryPerformanceCounter(&now);
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    else dt_ms = (double)(now.QuadPart - prev.QuadPart) * 1000.0 / (double)freq.QuadPart;
    prev = now;
    a = cfg.smoothing_ms > 0 ? 1.0 - exp(-dt_ms / cfg.smoothing_ms) : 1.0;
    if (GetTickCount() - last_rx > (DWORD)cfg.timeout_ms) { tgt_yaw = tgt_pitch = tgt_roll = 0; }
    cur_yaw   += (tgt_yaw   - cur_yaw)   * a;
    cur_pitch += (tgt_pitch - cur_pitch) * a;
    cur_roll  += (tgt_roll  - cur_roll)  * a;
    if (g_vr_pose) return;
    g_yaw   = to_units(cur_yaw);
    g_pitch = to_units(cur_pitch);
    g_roll  = to_units(cur_roll);
}

/* Diagnostic: the game's own camera struct (mode, position, angles), so the log shows whether the
 * underlying view moves (mouse, drift) separately from the offsets we add. Read-only. */
static const char *cam_desc(void)
{
    static char buf[128];
    const unsigned char *cam;
    buf[0] = 0;
    if (!g_camptr_addr || IsBadReadPtr((void *)g_camptr_addr, 4)) return buf;
    cam = *(const unsigned char *const *)g_camptr_addr;
    if (!cam || IsBadReadPtr(cam, 0x28)) return buf;
    snprintf(buf, sizeof buf, " | camera mode %d bank %.1f pitch %.1f heading %.1f",
             *(const int *)cam,
             *(const unsigned short *)(cam + 0x14) * 360.0 / 65536.0,
             *(const unsigned short *)(cam + 0x16) * 360.0 / 65536.0,
             *(const unsigned short *)(cam + 0x18) * 360.0 / 65536.0);
    return buf;
}

/* Cheap housekeeping on the worker thread: live config reload, a status line, data loss/return. */
static int game_window_in_front(void)
{
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (!fg) return 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

static void periodic(void)
{
    static DWORD next_reload, next_stats, next_dump_check, next_vr;
    static unsigned long last_total;
    static int stale;
    DWORD now = GetTickCount();
    int is_stale = n_packets > 0 && (now - last_rx) > (DWORD)cfg.timeout_ms;

    if ((int)(now - next_reload) >= 0) {
        int udp = cfg.udp_port, tcp = cfg.tcp_port;
        load_config();                               /* signs, gains, limits, smoothing, enabled: live */
        cfg.udp_port = udp;                          /* ports only change with a restart */
        cfg.tcp_port = tcp;
        g_enabled = (unsigned)cfg.enabled;
        /* Both only ever switch on if the wrapper was patched in at start (see startup()); toggling stereo
         * or show_heading_marks on for the first time needs a restart, toggling either off does not. */
        g_stereo_on = g_stereo_installed && cfg.stereo;
        g_marks_on  = g_hook_ok && g_stereo_installed && cfg.show_marks && cfg.enabled;
        g_ipd = (float)cfg.stereo_ipd;
        g_swap = cfg.stereo_swap;
        g_fov_deg = (float)cfg.fov_deg;
        next_reload = now + 2000;
    }
    if (cfg.stereo == STEREO_OPENVR || cfg.stereo == STEREO_WXR) {   /* HUD show/hide: vr_hud_key (default H) or vr_hud_pad (default Y) */
        static int tog_was_down;
        int down = (cfg.vr_hud_key && (GetAsyncKeyState(cfg.vr_hud_key) & 0x8000)) ||
                   (cfg.vr_hud_pad && (vr_pad_buttons() & (unsigned)cfg.vr_hud_pad) == (unsigned)cfg.vr_hud_pad);
        if (down && !tog_was_down && game_window_in_front()) {
            g_hud_visible = !g_hud_visible;
            hl_log("openvr: HUD panel %s", g_hud_visible ? "shown" : "hidden");
        }
        tog_was_down = down;
    }
    {                                                 /* same diagnostic from the Insert key (unbound in the game) */
        static int ins_was_down;
        int down = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
        if (down && !ins_was_down && g_stereo_on) {
            g_dump_key = 1;
            if (cfg.stereo == STEREO_WXR) wxr_request_dump();
            if (cfg.stereo == STEREO_OPENVR) vr_hud_request_dump();
            hl_log("stereo diagnostic requested (Insert key)");
        }
        ins_was_down = down;
    }
    if ((int)(now - next_dump_check) >= 0) {          /* stereo diagnostic trigger: a file named stereo_dump.now */
        char p[MAX_PATH + 32];
        snprintf(p, sizeof p, "%sstereo_dump.now", g_dir);
        if (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileA(p);
            hl_log("stereo diagnostic requested (%s)", g_stereo_on ? "will run on the next stereo frame" : "but stereo is not active: nothing will happen");
            g_dump_req = 1;
            if (cfg.stereo == STEREO_WXR) wxr_request_dump();
        }
        next_dump_check = now + 1000;
    }
    if ((cfg.stereo == STEREO_OPENVR || cfg.stereo == STEREO_WXR) && g_stereo_installed) ensure_present_hook();
    if (cfg.stereo == STEREO_WXR && g_stereo_installed && (int)(now - next_vr) >= 0) {
        wxr_status();                                 /* re-sends our mode every second, logs every 5 s */
        next_vr = now + 1000;
    }
    if (cfg.stereo == STEREO_OPENVR && g_stereo_installed && (int)(now - next_vr) >= 0) {
        if (!vr_ready()) vr_init(g_dir);                 /* SteamVR not up at start: keep trying */
        else { vr_status(); hl_log("openvr: %lu Present calls seen by the HUD hook", g_present_calls); }
        next_vr = now + 5000;
    }
    if ((int)(now - next_stats) >= 0) {
        unsigned long total = n_udp + n_tcp;
        if (total != last_total) {
            hl_log("rx %lu UDP + %lu TCP packets so far; applied yaw %.1f pitch %.1f roll %.1f deg%s",
                   n_udp, n_tcp, cur_yaw, cur_pitch, cur_roll, cam_desc());
            last_total = total;
        }
        next_stats = now + 5000;
    }
    if (is_stale && !stale)  { hl_log("no data for %d ms: easing back to centre", cfg.timeout_ms); stale = 1; }
    if (!is_stale && stale)  { hl_log("data resumed"); stale = 0; }
}

static SOCKET listen_on(int type, int port)
{
    SOCKADDR_IN a;
    SOCKET s = socket(AF_INET, type, 0);
    if (s == INVALID_SOCKET) { hl_log("socket(type %d) failed: %d", type, WSAGetLastError()); return s; }
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(cfg.listen_loopback ? INADDR_LOOPBACK : INADDR_ANY);
    a.sin_port = htons((u_short)port);
    if (bind(s, (SOCKADDR *)&a, sizeof a) != 0 || (type == SOCK_STREAM && listen(s, 1) != 0)) {
        hl_log("bind/listen on port %d failed: %d", port, WSAGetLastError());
        closesocket(s);
        return INVALID_SOCKET;
    }
    hl_log("listening on %s port %d", type == SOCK_DGRAM ? "UDP" : "TCP", port);
    return s;
}

static DWORD WINAPI worker(LPVOID arg)
{
    WSADATA wsa;
    SOCKET udp = INVALID_SOCKET, tcp_l = INVALID_SOCKET, tcp_c = INVALID_SOCKET;
    unsigned char acc[256];
    int acc_len = 0;
    (void)arg;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { hl_log("WSAStartup failed"); return 1; }
    if (cfg.udp_port > 0) udp   = listen_on(SOCK_DGRAM, cfg.udp_port);
    if (cfg.tcp_port > 0) tcp_l = listen_on(SOCK_STREAM, cfg.tcp_port);
    last_rx = GetTickCount();

    for (;;) {
        fd_set rs;
        struct timeval tv = { 0, 10000 };      /* 10 ms tick */
        int n;
        FD_ZERO(&rs);
        if (udp != INVALID_SOCKET)   FD_SET(udp, &rs);
        if (tcp_l != INVALID_SOCKET) FD_SET(tcp_l, &rs);
        if (tcp_c != INVALID_SOCKET) FD_SET(tcp_c, &rs);
        if (rs.fd_count == 0) { Sleep(10); step_filter(); periodic(); continue; }

        n = select(0, &rs, NULL, NULL, &tv);
        if (n > 0) {
            if (udp != INVALID_SOCKET && FD_ISSET(udp, &rs)) {
                unsigned char buf[512];
                int r = recv(udp, (char *)buf, sizeof buf, 0);
                if (r >= 48) take_packet(buf, 0);
            }
            if (tcp_l != INVALID_SOCKET && FD_ISSET(tcp_l, &rs)) {
                SOCKET c = accept(tcp_l, NULL, NULL);
                if (c != INVALID_SOCKET) {
                    if (tcp_c != INVALID_SOCKET) closesocket(tcp_c);
                    tcp_c = c; acc_len = 0;
                    hl_log("TCP client connected");
                }
            }
            if (tcp_c != INVALID_SOCKET && FD_ISSET(tcp_c, &rs)) {
                int r = recv(tcp_c, (char *)acc + acc_len, (int)sizeof acc - acc_len, 0);
                if (r <= 0) { hl_log("TCP client gone"); closesocket(tcp_c); tcp_c = INVALID_SOCKET; acc_len = 0; }
                else {
                    int off = 0;
                    acc_len += r;
                    while (acc_len - off >= 48) { take_packet(acc + off, 1); off += 48; }   /* fixed 48-byte records */
                    memmove(acc, acc + off, (size_t)(acc_len - off));
                    acc_len -= off;
                }
            }
        }
        step_filter();
        periodic();
    }
    return 0;
}

/* Crash logger: logs fatal-looking exceptions (address, module + offset) so a crash leaves a trace in
 * headlook.log. Vectored handlers see first-chance exceptions, some of which the game may handle itself, so
 * a line here is a lead, not proof; it never handles anything. */
static LONG CALLBACK crash_logger(PEXCEPTION_POINTERS ep)
{
    static volatile LONG n;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    void *addr = ep->ExceptionRecord->ExceptionAddress;
    HMODULE mod = NULL;
    char name[MAX_PATH] = "?";
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW &&
        code != EXCEPTION_PRIV_INSTRUCTION && code != 0xC0000409 /* stack buffer overrun / fail-fast */ &&
        code != 0xC0000374 /* heap corruption */)
        return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&n) > 10) return EXCEPTION_CONTINUE_SEARCH;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &mod) && mod)
        GetModuleFileNameA(mod, name, sizeof name);
    hl_log("EXCEPTION %#lx at %p (%s+%#lx)%s, thread %lu", (unsigned long)code, addr, strrchr(name, '\\') ? strrchr(name, '\\') + 1 : name,
           (unsigned long)((char *)addr - (char *)mod),
           code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2 ? (ep->ExceptionRecord->ExceptionInformation[0] ? " writing" : " reading") : "",
           GetCurrentThreadId());
    return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI startup(LPVOID arg)
{
    int hooked;
    (void)arg;
    /* The render-copy hook is needed for head-look and for the stereo eye shift; the marks need it too
     * (they show the error it is already computing), but only when head-look is actually on (cfg.enabled),
     * so they are not in this condition themselves. */
    hooked = (cfg.enabled || cfg.stereo) ? install_hook() : 0;
    g_hook_ok = hooked;
    if (hooked && (cfg.stereo || cfg.show_marks)) {
        g_stereo_installed = install_stereo();
        g_ipd = (float)cfg.stereo_ipd;
        g_swap = cfg.stereo_swap;
        g_fov_deg = (float)cfg.fov_deg;
        g_stereo_on = g_stereo_installed && cfg.stereo;
        g_marks_on  = g_stereo_installed && cfg.show_marks && cfg.enabled;
        if (g_stereo_installed && (cfg.stereo == STEREO_OPENVR || cfg.stereo == STEREO_WXR)) {
            AddVectoredExceptionHandler(1, crash_logger);
            hook_create_device();
            install_overlay_hook();
        }
        if (g_stereo_installed && cfg.stereo == STEREO_WXR) wxr_init(g_dir);
        if (g_stereo_installed && cfg.stereo == STEREO_OPENVR && !vr_init(g_dir))
            hl_log("openvr: not connected yet; drawing plain side-by-side and retrying every 10 s");
    }
    if (hooked)
        worker(NULL);
    else
        hl_log("idle (disabled or hook not installed)");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        char path[MAX_PATH], *slash;
        DisableThreadLibraryCalls(inst);
        GetModuleFileNameA(inst, path, sizeof path);
        slash = strrchr(path, '\\');
        if (slash) slash[1] = 0;
        snprintf(g_dir, sizeof g_dir, "%s", path);
        {
            char lp[MAX_PATH + 16];
            snprintf(lp, sizeof lp, "%sheadlook.log", g_dir);
            g_log = fopen(lp, "w");
        }
        load_config();
        hl_log("headlook.dll loaded; enabled=%d udp=%d tcp=%d smoothing=%.0fms use_roll=%d stereo=%d ipd=%.3f marks=%d fov=%.0f",
               cfg.enabled, cfg.udp_port, cfg.tcp_port, cfg.smoothing_ms, cfg.use_roll, cfg.stereo, cfg.stereo_ipd,
               cfg.show_marks, cfg.fov_deg);
        /* Real work happens on a thread: DllMain runs under the loader lock. */
        CloseHandle(CreateThread(NULL, 0, startup, NULL, 0, NULL));
    }
    return TRUE;
}
