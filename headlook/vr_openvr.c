/*
 * vr_openvr.c: part of headlook.dll. Sends the two stereo eye images straight to a SteamVR headset through
 * OpenVR's compositor (IVRCompositor::Submit), and takes the head pose from the compositor too, so neither
 * Desktop+ nor hmd_bridge.exe is needed. Switched on with stereo=openvr (ini) or HEADLOOK_STEREO=openvr.
 * See docs/DEVLOG.md, "OpenVR direct submit", and tools/hmd_bridge/README.md.
 *
 * ROUGH CUT. Per frame, on the game's render thread:
 *   1. vr_begin_frame(): IVRCompositor::WaitGetPoses (this also paces the game to the headset) gives the
 *      head pose, turned into yaw/pitch for the existing render-copy hook. Roll and position are not drawn.
 *   2. The existing stereo code draws each eye; vr_submit_eye() copies that eye's Direct3D 9 render target
 *      to system memory (GetRenderTargetData + LockRect), uploads it into a Direct3D 11 texture on our own
 *      D3D11 device (OpenVR takes no D3D9 textures) and submits it. The CPU copy is the slow part.
 *   3. Each eye is submitted WITH the pose it was actually drawn at (yaw and pitch only, roll 0), so the
 *      compositor's reprojection makes up the rest (head roll, the time since WaitGetPoses).
 * Field of view: headlook.c widens the engine's own view for VR frames (the float at 0x7E62D8, 1/tan of half the
 * 4:3 horizontal FOV, default 1.0 = 90 deg; widescreen modes are Hor+, see image_tangents()) so the picture covers
 * the headset's whole field of view: vr_auto_scale(), or ini vr_view_scale. Each eye texture is a black canvas
 * big enough for both the picture and the headset eye's own field of view, with the game picture pasted at its
 * true angular position, and the texture bounds cut out the eye's field of view. So the scale is right; where
 * the picture is narrower than the headset (a manual vr_view_scale), there is a black border.
 *
 * OpenVR frame (from its docs): right-handed, +Y up, the headset looks along -Z; the pose matrix's columns are
 * the device's right, up and back axes in tracking space. The 32-bit openvr_api.dll is loaded from
 * openvr32\openvr_api.dll next to headlook.dll (the game folder's openvr_api.dll is hmd_bridge.exe's 64-bit one).
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <dxgi.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define OPENVR_API_NODLL
#include <openvr_capi.h>
#include "vr_openvr.h"
#include "vr_gpu.h"
#include "vr_wxr.h"

void hl_log(const char *fmt, ...);

#define DEG (180.0 / 3.14159265358979323846)
#define RAD (3.14159265358979323846 / 180.0)

typedef intptr_t (*init_fn)(EVRInitError *, EVRApplicationType);
typedef intptr_t (*iface_fn)(const char *, EVRInitError *);
typedef const char *(*errstr_fn)(EVRInitError);

static struct VR_IVRSystem_FnTable *g_sys;
static struct VR_IVRCompositor_FnTable *g_comp;
static struct VR_IVROverlay_FnTable *g_ovl;       /* for the HUD/menu panel; NULL if unavailable */
static volatile int g_ready;                      /* set last by vr_init: the tables and eye data are valid */
static float g_proj[2][4];                        /* per eye: left, right, top, bottom (tangents, y down) */
static int32_t g_adapter = -1;
static volatile int g_quit;                       /* SteamVR asked us to stop */
static char g_dir_vr[MAX_PATH];                   /* the DLL's folder (for dump files) */

/* Render-thread state. */
static ID3D11Device *g_d11;
static ID3D11DeviceContext *g_ctx;
static int g_d11_failed;
static ID3D11Texture2D *g_tex[2];
static VRTextureBounds_t g_bounds[2];
static UINT g_ox[2], g_oy[2];                     /* where the game picture sits in each eye's canvas */
static UINT g_img_w, g_img_h;
static float g_img_scale;
static int g_img_horplus;
static D3DFORMAT g_img_fmt;

/* Render-target formats the engine has been seen using, and the D3D11 format with the same memory layout.
 * 22/21: plain 8-bit. 113 (A16B16G16R16F, half floats in R,G,B,A order): the engine's HDR path, used when
 * d3d_disp_enable_hdr is on (seen 2026-09-24). */
typedef struct { D3DFORMAT d9; DXGI_FORMAT d11; UINT bpp; unsigned long long opaque_black, alpha_one; const char *name; } fmt_t;
static const fmt_t g_fmts[] = {
    { D3DFMT_X8R8G8B8,      DXGI_FORMAT_B8G8R8A8_UNORM,     4, 0xFF000000ull,         0xFF000000ull,         "X8R8G8B8" },
    { D3DFMT_A8R8G8B8,      DXGI_FORMAT_B8G8R8A8_UNORM,     4, 0xFF000000ull,         0xFF000000ull,         "A8R8G8B8" },
    { D3DFMT_A16B16G16R16F, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, 0x3C00000000000000ull, 0x3C00000000000000ull, "A16B16G16R16F (HDR)" },
};
static const fmt_t *find_fmt(D3DFORMAT f)
{
    size_t i;
    for (i = 0; i < sizeof g_fmts / sizeof g_fmts[0]; i++) if (g_fmts[i].d9 == f) return &g_fmts[i];
    return NULL;
}
static IDirect3DSurface9 *g_sysmem;               /* readback surface (system memory, survives device Reset) */
static D3DSURFACE_DESC g_sysmem_desc;
static HmdMatrix34_t g_drawn_pose;                /* the pose this frame is drawn at (sent with each eye) */
static int g_have_pose, g_have_ref, g_recentre = 1, g_key_was_down;
static double g_ref_yaw;
static unsigned long g_frames, g_submits, g_errs;
static int g_last_wait_err, g_last_submit_err;
/* Where the render thread is inside OpenVR/D3D11 right now, for the worker's "stuck" check and for tracing the
 * first frames step by step (a crash or hang then shows up as the last "->" line with no matching "ok"). */
static const char *volatile g_step = "idle";
static volatile DWORD g_step_since;
static unsigned long g_prof_frames;               /* frames in the current timing interval (see vr_status) */
static void release_caps(void);
static void submit_caps(IDirect3DDevice9 *dev);
static int g_cap_done, g_pending;
static unsigned long g_traced;                    /* frames started (WaitGetPoses calls); the first TRACE_FRAMES are traced */
#define TRACE_FRAMES 3

static void step(const char *what)
{
    g_step = what;
    g_step_since = GetTickCount();
    static int lines;                             /* hard cap, in case frames never get as far as Submit */
    if (g_traced <= TRACE_FRAMES && lines++ < 60) hl_log("openvr: frame %lu -> %s", g_traced, what);
}

typedef struct { double v[3]; } vec3;
static vec3 vec(double x, double y, double z) { vec3 r = {{ x, y, z }}; return r; }
static double dot(vec3 a, vec3 b) { return a.v[0] * b.v[0] + a.v[1] * b.v[1] + a.v[2] * b.v[2]; }
static vec3 cross(vec3 a, vec3 b)
{
    return vec(a.v[1] * b.v[2] - a.v[2] * b.v[1], a.v[2] * b.v[0] - a.v[0] * b.v[2], a.v[0] * b.v[1] - a.v[1] * b.v[0]);
}
static vec3 scale(vec3 a, double s) { return vec(a.v[0] * s, a.v[1] * s, a.v[2] * s); }
static double wrap180(double d) { while (d > 180.0) d -= 360.0; while (d < -180.0) d += 360.0; return d; }

/* Same maths as tools/hmd_bridge (checked by its --selftest): yaw + = turned right, pitch + = up. */
static void matrix_yaw_pitch(const float m[3][4], double *yaw, double *pitch)
{
    vec3 fwd = vec(-m[0][2], -m[1][2], -m[2][2]);
    double c = fwd.v[1];
    *yaw = atan2(fwd.v[0], -fwd.v[2]) * DEG;
    if (c > 1) c = 1;
    if (c < -1) c = -1;
    *pitch = asin(c) * DEG;
}

/* Rotation for yaw/pitch with roll 0 (hmd_bridge's angles_to_matrix with roll = 0), position kept from src. */
static void drawn_pose_matrix(double yaw, double pitch, const HmdMatrix34_t *src, HmdMatrix34_t *out)
{
    double y = yaw * RAD, p = pitch * RAD;
    vec3 fwd = vec(sin(y) * cos(p), sin(p), -cos(y) * cos(p));
    vec3 right = cross(fwd, vec(0, 1, 0)), up;
    double l = sqrt(dot(right, right));
    int i;
    right = l > 1e-6 ? scale(right, 1.0 / l) : vec(cos(y), 0, sin(y));
    up = cross(right, fwd);
    for (i = 0; i < 3; i++) {
        out->m[i][0] = (float)right.v[i];
        out->m[i][1] = (float)up.v[i];
        out->m[i][2] = (float)-fwd.v[i];
        out->m[i][3] = src->m[i][3];
    }
}

int vr_init(const char *dll_dir)
{
    char path[MAX_PATH + 64];
    HMODULE dll;
    init_fn p_init;
    iface_fn p_iface;
    errstr_fn p_errstr;
    EVRInitError err = EVRInitError_VRInitError_None;
    int eye;

    if (g_ready) return 1;
    snprintf(g_dir_vr, sizeof g_dir_vr, "%s", dll_dir);
    snprintf(path, sizeof path, "%sopenvr32\\openvr_api.dll", dll_dir);
    dll = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!dll) { hl_log("openvr: cannot load %s (error %lu)", path, GetLastError()); return 0; }
    p_init = (init_fn)(void *)GetProcAddress(dll, "VR_InitInternal");
    p_iface = (iface_fn)(void *)GetProcAddress(dll, "VR_GetGenericInterface");
    p_errstr = (errstr_fn)(void *)GetProcAddress(dll, "VR_GetVRInitErrorAsEnglishDescription");
    if (!p_init || !p_iface || !p_errstr) { hl_log("openvr: %s is missing expected exports", path); return 0; }

    p_init(&err, EVRApplicationType_VRApplication_Scene);
    if (err != EVRInitError_VRInitError_None) { hl_log("openvr: init failed: error %d: %s", (int)err, p_errstr(err)); return 0; }
    g_sys = (struct VR_IVRSystem_FnTable *)p_iface("FnTable:IVRSystem_026", &err);
    if (!g_sys || err != EVRInitError_VRInitError_None) { hl_log("openvr: no IVRSystem_026: error %d: %s", (int)err, p_errstr(err)); return 0; }
    g_comp = (struct VR_IVRCompositor_FnTable *)p_iface("FnTable:IVRCompositor_029", &err);
    if (!g_comp || err != EVRInitError_VRInitError_None) { hl_log("openvr: no IVRCompositor_029: error %d: %s", (int)err, p_errstr(err)); return 0; }

    g_ovl = (struct VR_IVROverlay_FnTable *)p_iface("FnTable:IVROverlay_028", &err);
    if (!g_ovl || err != EVRInitError_VRInitError_None) { hl_log("openvr: no IVROverlay_028 (error %d): no HUD panel", (int)err); g_ovl = NULL; }
    g_comp->SetTrackingSpace(ETrackingUniverseOrigin_TrackingUniverseSeated);
    for (eye = 0; eye < 2; eye++) {
        float l, r, t, b;
        g_sys->GetProjectionRaw((EVREye)eye, &l, &r, &t, &b);
        hl_log("openvr: eye %d raw projection left %.3f right %.3f top %.3f bottom %.3f (tangents)", eye, l, r, t, b);
        /* Valve's samples treat top < bottom (y down). Take min/max so a flipped sign cannot flip the picture. */
        g_proj[eye][0] = l < r ? l : r;
        g_proj[eye][1] = l < r ? r : l;
        g_proj[eye][2] = t < b ? t : b;
        g_proj[eye][3] = t < b ? b : t;
    }
    g_sys->GetDXGIOutputInfo(&g_adapter);
    {
        uint32_t w = 0, h = 0;
        g_sys->GetRecommendedRenderTargetSize(&w, &h);
        hl_log("openvr: connected as a scene app; headset on DXGI adapter %d; recommended %ux%u per eye (for comparison only)",
               (int)g_adapter, w, h);
    }
    g_ready = 1;
    return 1;
}

int vr_ready(void) { return g_ready && !g_quit; }

/* Render thread (all OpenVR calls after init stay on one thread): drain SteamVR's events so it does not think we
 * hung; notice a quit request. */
static void poll_events(void)
{
    struct VREvent_t ev;
    if (!g_ready || g_quit) return;
    while (g_sys->PollNextEvent(&ev, sizeof ev)) {
        if (ev.eventType == EVREventType_VREvent_Quit) {
            hl_log("openvr: SteamVR asked the app to quit: no more frames go to the headset (desktop view carries on)");
            g_quit = 1;
            return;
        }
    }
}

/* Controller buttons held on any connected XInput pad (OR of all four slots; XInput loaded on first use).
 * Used for recentre (L3 + R3) here and the HUD toggle in headlook.c. */
typedef struct { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } pad_state_t;   /* XINPUT_STATE */
unsigned vr_pad_buttons(void)
{
    static DWORD (WINAPI *volatile get)(DWORD, pad_state_t *);
    static volatile LONG tried;
    unsigned all = 0;
    DWORD i;
    if (InterlockedCompareExchange(&tried, 1, 0) == 0) {
        HMODULE x = LoadLibraryA("xinput1_4.dll");
        if (!x) x = LoadLibraryA("xinput9_1_0.dll");
        get = x ? (DWORD (WINAPI *)(DWORD, pad_state_t *))(void *)GetProcAddress(x, "XInputGetState") : NULL;
        hl_log("openvr: controller buttons (recentre L3+R3, HUD toggle) %s", get ? "available" : "unavailable (no XInput)");
    }
    all = wxr_pad_buttons();                      /* Quest Touch controllers under WinlatorXR, as Xbox buttons */
    if (!get) return all;
    for (i = 0; i < 4; i++) {
        pad_state_t st;
        if (get(i, &st) == 0) all |= st.buttons;
    }
    return all;
}
static int pad_recentre_down(void) { return (vr_pad_buttons() & 0x00C0) == 0x00C0; }   /* LEFT_THUMB | RIGHT_THUMB */

static volatile int g_menu_replace;               /* re-place the menu panel in front of the head */

/* Pause/Break, Scroll Lock or L3+R3: recentre the game view, and bring the menu panel in front of you. */
static void poll_recentre_keys(void)
{
    int down = ((GetAsyncKeyState(VK_PAUSE) | GetAsyncKeyState(VK_SCROLL)) & 0x8000) != 0 || pad_recentre_down();
    if (down && !g_key_was_down) { g_recentre = 1; g_menu_replace = 1; }   /* on the key's press, not while held */
    g_key_was_down = down;
}

int vr_begin_frame(IDirect3DDevice9 *dev, double *yaw_right_deg, double *pitch_up_deg)
{
    TrackedDevicePose_t poses[1];
    EVRCompositorError e;
    double yaw, pitch;

    if (!vr_ready()) return 0;
    vr_prof_tick();
    g_cap_done = 0;                                   /* a frame that never reached vr_end_frame */
    g_have_pose = 0;
    g_traced++;
    step("PollNextEvent");
    poll_events();
    if (g_quit) { step("idle"); return 0; }
    memset(poses, 0, sizeof poses);
    step("WaitGetPoses");
    {
        double t = vr_ms();
        e = g_comp->WaitGetPoses(poses, 1, NULL, 0);
        vr_prof(P_WAIT, vr_ms() - t);
    }
    step("idle");
    if (g_traced <= TRACE_FRAMES) hl_log("openvr: frame %lu    WaitGetPoses returned %d, pose valid %d, tracking result %d",
                                          g_traced, (int)e, (int)poses[0].bPoseIsValid, (int)poses[0].eTrackingResult);
    if (e != EVRCompositorError_VRCompositorError_None) {
        if ((int)e != g_last_wait_err) hl_log("openvr: WaitGetPoses error %d", (int)e);
        g_last_wait_err = (int)e;
        g_pending = 0;
        return 0;
    }
    g_last_wait_err = 0;
    /* Pipelined: the previous frame's eyes were left on the GPU; it has finished them by now, so the readback
     * doesn't stall. They go out with the pose they were drawn at, and the compositor reprojects the difference. */
    if (g_pending) { g_pending = 0; submit_caps(dev); }
    if (!poses[0].bPoseIsValid) return 0;

    poll_recentre_keys();

    matrix_yaw_pitch(poses[0].mDeviceToAbsoluteTracking.m, &yaw, &pitch);
    if (pitch > 89.0) pitch = 89.0;
    if (pitch < -89.0) pitch = -89.0;
    if (g_recentre) { g_ref_yaw = yaw; g_have_ref = 1; g_recentre = 0; hl_log("openvr: centred at tracking-space yaw %.1f", yaw); }
    drawn_pose_matrix(yaw, pitch, &poses[0].mDeviceToAbsoluteTracking, &g_drawn_pose);
    *yaw_right_deg = wrap180(yaw - g_ref_yaw);
    *pitch_up_deg = pitch;
    g_have_pose = 1;
    g_frames++;
    return 1;
}

static const GUID IID_IDXGIFactory1_ = { 0x770aae78, 0xf26f, 0x4dba, { 0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87 } };

static int make_d3d11(void)
{
    HMODULE d11 = LoadLibraryA("d3d11.dll"), dxgi = LoadLibraryA("dxgi.dll");
    PFN_D3D11_CREATE_DEVICE p_create = d11 ? (PFN_D3D11_CREATE_DEVICE)(void *)GetProcAddress(d11, "D3D11CreateDevice") : NULL;
    HRESULT (WINAPI *p_factory)(REFIID, void **) =
        dxgi ? (HRESULT (WINAPI *)(REFIID, void **))(void *)GetProcAddress(dxgi, "CreateDXGIFactory1") : NULL;
    IDXGIFactory1 *fac = NULL;
    IDXGIAdapter1 *ad = NULL;
    HRESULT hr;

    if (!p_create) { hl_log("openvr: cannot load d3d11.dll / D3D11CreateDevice"); return 0; }
    if (g_adapter >= 0 && p_factory && SUCCEEDED(p_factory(&IID_IDXGIFactory1_, (void **)&fac)) && fac) {
        if (FAILED(IDXGIFactory1_EnumAdapters1(fac, (UINT)g_adapter, &ad))) ad = NULL;
        IDXGIFactory1_Release(fac);
    }
    hr = p_create((IDXGIAdapter *)ad, ad ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
                  D3D11_SDK_VERSION, &g_d11, NULL, &g_ctx);
    if (ad) {
        DXGI_ADAPTER_DESC1 ds;
        if (SUCCEEDED(IDXGIAdapter1_GetDesc1(ad, &ds))) hl_log("openvr: D3D11 device on adapter %d: %ls", (int)g_adapter, ds.Description);
        IDXGIAdapter1_Release(ad);
    }
    if (FAILED(hr) || !g_d11) { hl_log("openvr: D3D11CreateDevice failed: %#lx", (unsigned long)hr); return 0; }
    hl_log("openvr: D3D11 device ready%s", ad ? "" : " (default adapter)");
    return 1;
}

/* The game picture's extent in tangents for the engine's view scale s (= 1/tan(half the 4:3 horizontal FOV)):
 * the engine's reference view is 4:3 (tan 1/s wide, 0.75/s high); in Hor+ widescreen the height stays and the
 * width grows with the aspect, otherwise (4:3 modes, or widescreen_lock_hfov) the width stays. Square pixels. */
static void image_tangents(UINT w, UINT h, float s, int horplus, double *tx, double *ty)
{
    if (horplus) { *ty = 0.75 / s; *tx = *ty * (double)w / (double)h; }
    else         { *tx = 1.0 / s;  *ty = *tx * (double)h / (double)w; }
}

/* The view scale that makes the game picture cover both headset eyes' full field of view (2% spare). Uses the
 * last picture size seen (4:3 until the first frame). */
float vr_auto_scale(int horplus)
{
    double need_x = 0, need_y = 0, aspect = g_img_w && g_img_h ? (double)g_img_w / (double)g_img_h : 4.0 / 3.0, sx, sy;
    int eye;
    for (eye = 0; eye < 2; eye++) {
        if (-g_proj[eye][0] > need_x) need_x = -g_proj[eye][0];
        if (g_proj[eye][1] > need_x) need_x = g_proj[eye][1];
        if (-g_proj[eye][2] > need_y) need_y = -g_proj[eye][2];
        if (g_proj[eye][3] > need_y) need_y = g_proj[eye][3];
    }
    if (need_x < 0.2 || need_y < 0.2) return 1.0f;    /* no headset data: the engine's default */
    need_x *= 1.02; need_y *= 1.02;
    if (horplus) { sy = 0.75 / need_y; sx = 0.75 * aspect / need_x; }
    else         { sx = 1.0 / need_x;  sy = 1.0 / (aspect * need_y); }
    return (float)(sx < sy ? sx : sy);                 /* the smaller scale is the wider view: covers both */
}

/* (Re)build both eye canvases for a game picture of w x h covering +-tx by +-ty (tangents). */
static int make_canvases(UINT w, UINT h, double tx, double ty, const fmt_t *fm)
{
    double ppt = (double)w * 0.5 / tx;
    int eye;

    for (eye = 0; eye < 2; eye++) {
        const float *p = g_proj[eye];
        double x0 = p[0] < -tx ? p[0] : -tx, x1 = p[1] > tx ? p[1] : tx;
        double y0 = p[2] < -ty ? p[2] : -ty, y1 = p[3] > ty ? p[3] : ty;
        UINT cw = (UINT)ceil((x1 - x0) * ppt), ch = (UINT)ceil((y1 - y0) * ppt);
        D3D11_TEXTURE2D_DESC td;
        D3D11_SUBRESOURCE_DATA init;
        unsigned char *black;
        size_t i;
        HRESULT hr;

        if (g_tex[eye]) { ID3D11Texture2D_Release(g_tex[eye]); g_tex[eye] = NULL; }
        if (cw > 8192 || ch > 8192) { hl_log("openvr: eye canvas %ux%u too large (view too narrow for this headset?)", cw, ch); return 0; }
        g_ox[eye] = (UINT)floor((-tx - x0) * ppt + 0.5);
        g_oy[eye] = (UINT)floor((-ty - y0) * ppt + 0.5);
        if (g_ox[eye] + w > cw) cw = g_ox[eye] + w;
        if (g_oy[eye] + h > ch) ch = g_oy[eye] + h;
        g_bounds[eye].uMin = (float)((p[0] - x0) * ppt / cw);
        g_bounds[eye].uMax = (float)((p[1] - x0) * ppt / cw);
        g_bounds[eye].vMin = (float)((p[2] - y0) * ppt / ch);
        g_bounds[eye].vMax = (float)((p[3] - y0) * ppt / ch);
        black = (unsigned char *)malloc((size_t)cw * ch * fm->bpp);
        if (!black) return 0;
        for (i = 0; i < (size_t)cw * ch; i++) memcpy(black + i * fm->bpp, &fm->opaque_black, fm->bpp);   /* little-endian */
        memset(&td, 0, sizeof td);
        td.Width = cw; td.Height = ch; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = fm->d11;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;        /* the compositor samples it */
        init.pSysMem = black; init.SysMemPitch = cw * fm->bpp; init.SysMemSlicePitch = 0;
        hr = ID3D11Device_CreateTexture2D(g_d11, &td, &init, &g_tex[eye]);
        free(black);
        if (FAILED(hr)) { hl_log("openvr: CreateTexture2D %ux%u failed: %#lx", cw, ch, (unsigned long)hr); g_tex[eye] = NULL; return 0; }
        hl_log("openvr: eye %d canvas %ux%u %s, game picture at (%u,%u) size %ux%u (tangents +-%.3f x +-%.3f), bounds u %.3f..%.3f v %.3f..%.3f",
               eye, cw, ch, fm->name, g_ox[eye], g_oy[eye], w, h, tx, ty,
               g_bounds[eye].uMin, g_bounds[eye].uMax, g_bounds[eye].vMin, g_bounds[eye].vMax);
    }
    g_img_w = w; g_img_h = h; g_img_fmt = fm->d9;
    return 1;
}

/* ---- Per-frame timing (render thread adds, worker logs averages every 5 s; unsynchronised, approximate). */
static const char *const g_prof_names[P_N] = { "frame", "wait", "drawL", "drawR", "capture", "readback", "alpha", "upload", "submit", "hud", "present" };
static double g_prof[P_N];

double vr_ms(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}
void vr_prof(int k, double ms) { if (k >= 0 && k < P_N) g_prof[k] += ms; }
void vr_prof_tick(void)                           /* render thread, once per 3D frame: the frame-to-frame time */
{
    static double last;
    double now = vr_ms();
    if (last > 0 && now - last < 1000) { vr_prof(P_FRAME, now - last); g_prof_frames++; }
    last = now;
}
void vr_prof_report(const char *tag)              /* worker: log the averages since the last report, then reset */
{
    char buf[512];
    int k, o;
    double sum = 0, n = (double)g_prof_frames;
    if (g_prof_frames == 0) return;
    o = snprintf(buf, sizeof buf, "%s: timing, ms per frame over %lu frames (%.1f fps):", tag, g_prof_frames, 1000.0 * n / (g_prof[P_FRAME] > 0 ? g_prof[P_FRAME] : 1));
    for (k = 0; k < P_N && o < (int)sizeof buf - 40; k++) {
        o += snprintf(buf + o, sizeof buf - o, " %s %.1f", g_prof_names[k], g_prof[k] / n);
        if (k != P_FRAME) sum += g_prof[k];
    }
    snprintf(buf + o, sizeof buf - o, " | other %.1f", (g_prof[P_FRAME] - sum) / n);
    hl_log("%s", buf);
    memset(g_prof, 0, sizeof g_prof);
    g_prof_frames = 0;
}

/* ---- Eye capture. After each pass the eye is copied on the GPU into its own render target: through a small pixel
 * shader that applies the brightness curve and makes it opaque 8-bit (vr_gpu_gamma), or with StretchRect, converting
 * the engine's 16-bit float HDR target to 8-bit if the driver can. Pipelined (vr_pipeline), the targets are read
 * back at the next frame's vr_begin_frame, when the GPU is long done with them, instead of stalling right after the
 * second pass. The targets are default-pool, so they are kept only while 3D frames keep coming and are released at
 * the device's Reset (vr_device_reset) and in menus: one outliving a Reset would make the engine's Reset fail. */
static IDirect3DSurface9 *g_cap[2];
static D3DSURFACE_DESC g_cap_desc[2];
static int g_cap_clean[2];                        /* went through the shader: curve applied, alpha already 1 */
static HmdMatrix34_t g_pend_pose;                 /* what the captured frame was drawn with */
static HmdMatrix34_t g_tex_pose;                  /* ... and what the eye textures now hold (idle frames resend it) */
static float g_pend_scale;
static int g_pend_horplus, g_pend_hdr;
static D3DFORMAT g_conv_src;                      /* source format the conversion check was made for */
static int g_conv_ok;
static float g_lut_gamma = -1, g_lut_black = -1;

static D3DFORMAT capture_format(IDirect3DDevice9 *dev, D3DFORMAT src, int convert)
{
    if (!convert || src != D3DFMT_A16B16G16R16F) return src;
    if (g_conv_src != src) {
        IDirect3D9 *d3d = NULL;
        D3DDEVICE_CREATION_PARAMETERS cp;
        g_conv_ok = 0;
        if (SUCCEEDED(IDirect3DDevice9_GetDirect3D(dev, &d3d)) && d3d) {
            if (SUCCEEDED(IDirect3DDevice9_GetCreationParameters(dev, &cp)))
                g_conv_ok = SUCCEEDED(IDirect3D9_CheckDeviceFormatConversion(d3d, cp.AdapterOrdinal, cp.DeviceType, src, D3DFMT_X8R8G8B8));
            IDirect3D9_Release(d3d);
        }
        g_conv_src = src;
        hl_log("openvr: GPU conversion of the HDR eye image to 8-bit %s", g_conv_ok ? "supported: using it" : "NOT supported: copying 16-bit");
    }
    return g_conv_ok ? D3DFMT_X8R8G8B8 : src;
}

static void release_caps(void)
{
    int i;
    for (i = 0; i < 2; i++) if (g_cap[i]) { IDirect3DSurface9_Release(g_cap[i]); g_cap[i] = NULL; }
    vr_gpu_reset();
    g_cap_done = 0;
    g_pending = 0;
}

void vr_capture_eye(IDirect3DDevice9 *dev, IDirect3DSurface9 *src, int eye, int convert, int gpu_curve)
{
    D3DSURFACE_DESC d;
    D3DFORMAT cf;
    HRESULT hr;
    int use_ps;
    double t0 = vr_ms();

    if (!vr_ready() || !g_have_pose || eye < 0 || eye > 1) return;
    IDirect3DSurface9_GetDesc(src, &d);
    /* The shader writes 8-bit: not when the 16-bit picture was asked for (vr_gpu_convert 0). */
    use_ps = gpu_curve && (convert || d.Format != D3DFMT_A16B16G16R16F) && vr_curve_available(dev);
    cf = use_ps ? D3DFMT_A8R8G8B8 : capture_format(dev, d.Format, convert);
    if (g_cap[eye] && (g_cap_desc[eye].Width != d.Width || g_cap_desc[eye].Height != d.Height || g_cap_desc[eye].Format != cf)) {
        IDirect3DSurface9_Release(g_cap[eye]); g_cap[eye] = NULL;
    }
    step("capture");
    hr = S_OK;
    if (!g_cap[eye]) {
        hr = IDirect3DDevice9_CreateRenderTarget(dev, d.Width, d.Height, cf, D3DMULTISAMPLE_NONE, 0, FALSE, &g_cap[eye], NULL);
        if (SUCCEEDED(hr) && g_cap[eye]) { g_cap_desc[eye] = d; g_cap_desc[eye].Format = cf; }
    }
    if (SUCCEEDED(hr) && g_cap[eye]) {
        if (use_ps) {
            if (!vr_curve_copy(dev, src, g_cap[eye], g_lut_gamma > 0 ? g_lut_gamma : 1.0f, g_lut_black > 0 ? g_lut_black : 0.0f))
                hr = E_FAIL;                          /* back to StretchRect + the CPU curve from the next frame */
        } else hr = IDirect3DDevice9_StretchRect(dev, src, NULL, g_cap[eye], NULL, D3DTEXF_NONE);
    }
    step("idle");
    if (FAILED(hr) || !g_cap[eye]) {
        if (g_errs++ < 5) hl_log("openvr: capture (eye %d, format %d -> %d) failed: %#lx%s", eye, (int)d.Format, (int)cf, (unsigned long)hr,
                                 !use_ps && cf != d.Format ? "; turning the 8-bit conversion off" : "");
        if (!use_ps && cf != d.Format) g_conv_ok = 0;
        if (g_cap[eye]) { IDirect3DSurface9_Release(g_cap[eye]); g_cap[eye] = NULL; }
    } else {
        g_cap_done |= 1 << eye;
        g_cap_clean[eye] = use_ps;
    }
    vr_prof(P_CAPTURE, vr_ms() - t0);
}

/* Brightness curve for the eye images: out = black + (1 - black) * in^(1/gamma). The eye images are taken
 * before the engine's own final colour correction (d3d_disp_sw_cc: the game's gamma/brightness settings apply
 * only to what reaches the monitor), and cheap headsets crush dark greys, so this is ours to set. */
static unsigned char g_lut[256];
static int g_lut_identity = 1;
void vr_set_picture(float gamma, float black)
{
    int i;
    if (gamma == g_lut_gamma && black == g_lut_black) return;
    if (gamma < 0.2f) gamma = 0.2f;
    if (gamma > 5.0f) gamma = 5.0f;
    if (black < 0.0f) black = 0.0f;
    if (black > 0.5f) black = 0.5f;
    for (i = 0; i < 256; i++) {
        double v = black + (1.0 - black) * pow(i / 255.0, 1.0 / gamma);
        g_lut[i] = (unsigned char)(v * 255.0 + 0.5);
    }
    g_lut_identity = gamma == 1.0f && black == 0.0f;
    g_lut_gamma = gamma; g_lut_black = black;
    hl_log("openvr: eye picture curve: gamma %.2f, black level %.2f", gamma, black);
}

static int submit_one(IDirect3DDevice9 *dev, IDirect3DSurface9 *src, int eye, float scale, int horplus, int hdr_linear, int clean,
                      const HmdMatrix34_t *pose)
{
    const fmt_t *fm;
    D3DSURFACE_DESC d;
    D3DLOCKED_RECT lr;
    D3D11_BOX box;
    VRTextureWithPose_t tex;
    EVRCompositorError e;
    HRESULT hr;
    UINT y, x;
    double t;

    if (!g_d11 && !g_d11_failed) { step("make_d3d11"); if (!make_d3d11()) g_d11_failed = 1; step("idle"); }
    if (!g_d11) return 0;

    IDirect3DSurface9_GetDesc(src, &d);
    fm = find_fmt(d.Format);
    if (!fm) {
        if (g_errs++ < 5) hl_log("openvr: render target format %d not handled", (int)d.Format);
        return 0;
    }
    if (d.Width != g_img_w || d.Height != g_img_h || scale != g_img_scale || horplus != g_img_horplus || d.Format != g_img_fmt ||
        !g_tex[0] || !g_tex[1]) {
        double tx, ty;
        image_tangents(d.Width, d.Height, scale, horplus, &tx, &ty);
        g_img_scale = scale; g_img_horplus = horplus;
        hl_log("openvr: drawing with view scale %.3f (%s), picture %ux%u", scale, horplus ? "Hor+" : "fixed width", d.Width, d.Height);
        if (!make_canvases(d.Width, d.Height, tx, ty, fm)) { g_img_w = d.Width; g_img_h = d.Height; g_img_fmt = d.Format; return 0; }
    }
    if (!g_tex[eye]) return 0;
    if (!g_sysmem || g_sysmem_desc.Width != d.Width || g_sysmem_desc.Height != d.Height || g_sysmem_desc.Format != d.Format) {
        if (g_sysmem) { IDirect3DSurface9_Release(g_sysmem); g_sysmem = NULL; }
        hr = IDirect3DDevice9_CreateOffscreenPlainSurface(dev, d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &g_sysmem, NULL);
        if (FAILED(hr) || !g_sysmem) { g_sysmem = NULL; if (g_errs++ < 5) hl_log("openvr: readback surface failed: %#lx", (unsigned long)hr); return 0; }
        g_sysmem_desc = d;
    }
    t = vr_ms();
    step(eye ? "GetRenderTargetData (right)" : "GetRenderTargetData (left)");
    hr = IDirect3DDevice9_GetRenderTargetData(dev, src, g_sysmem);
    if (FAILED(hr)) { if (g_errs++ < 5) hl_log("openvr: GetRenderTargetData failed: %#lx (multisampled target?)", (unsigned long)hr); return 0; }
    if (FAILED(IDirect3DSurface9_LockRect(g_sysmem, &lr, NULL, 0))) { if (g_errs++ < 5) hl_log("openvr: LockRect failed"); return 0; }
    vr_prof(P_READBACK, vr_ms() - t);
    t = vr_ms();
    for (y = 0; y < d.Height && !(clean && fm->bpp == 4); y++) {   /* X8 bytes may be 0, float alpha anything: make every pixel opaque */
        unsigned char *row = (unsigned char *)lr.pBits + (size_t)y * (size_t)lr.Pitch;
        if (fm->bpp == 4 && g_lut_identity) { UINT32 *px = (UINT32 *)row; for (x = 0; x < d.Width; x++) px[x] |= 0xFF000000u; }
        else if (fm->bpp == 4) {
            UINT32 *px = (UINT32 *)row;
            for (x = 0; x < d.Width; x++) {
                UINT32 v = px[x];
                px[x] = 0xFF000000u | ((UINT32)g_lut[(v >> 16) & 255] << 16) | ((UINT32)g_lut[(v >> 8) & 255] << 8) | g_lut[v & 255];
            }
        }
        else { unsigned long long *px = (unsigned long long *)row;
               for (x = 0; x < d.Width; x++) px[x] = (px[x] & 0x0000FFFFFFFFFFFFull) | fm->alpha_one; }
    }
    vr_prof(P_ALPHA, vr_ms() - t);
    t = vr_ms();
    step(eye ? "UpdateSubresource (right)" : "UpdateSubresource (left)");
    box.left = g_ox[eye]; box.top = g_oy[eye]; box.front = 0;
    box.right = g_ox[eye] + d.Width; box.bottom = g_oy[eye] + d.Height; box.back = 1;
    ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)g_tex[eye], 0, &box, lr.pBits, (UINT)lr.Pitch, 0);
    IDirect3DSurface9_UnlockRect(g_sysmem);
    ID3D11DeviceContext_Flush(g_ctx);
    vr_prof(P_UPLOAD, vr_ms() - t);

    memset(&tex, 0, sizeof tex);
    tex.handle = g_tex[eye];
    tex.eType = ETextureType_TextureType_DirectX;
    /* Whether the engine's HDR buffer holds gamma-encoded or linear values is not known; ini vr_hdr_linear
     * switches (live) if the float picture looks washed out or too dark. 8-bit is always gamma. */
    tex.eColorSpace = fm->bpp == 8 && hdr_linear ? EColorSpace_ColorSpace_Linear : EColorSpace_ColorSpace_Gamma;
    tex.mDeviceToAbsoluteTracking = *pose;
    t = vr_ms();
    step(eye ? "Submit (right)" : "Submit (left)");
    e = g_comp->Submit((EVREye)eye, (struct Texture_t *)&tex, &g_bounds[eye], EVRSubmitFlags_Submit_TextureWithPose);
    step("idle");
    vr_prof(P_SUBMIT, vr_ms() - t);
    if (g_traced <= TRACE_FRAMES) hl_log("openvr: frame %lu    Submit (eye %d) returned %d", g_traced, eye, (int)e);
    if (e != EVRCompositorError_VRCompositorError_None) {
        if ((int)e != g_last_submit_err) hl_log("openvr: Submit (eye %d) error %d", eye, (int)e);
        g_last_submit_err = (int)e;
        return 0;
    }
    g_last_submit_err = 0;
    g_submits++;
    return 1;
}

/* Read back, upload and submit both captured eyes with the pose they were drawn at, then hand the frame over. */
static void submit_caps(IDirect3DDevice9 *dev)
{
    int eye, n = 0;
    for (eye = 0; eye < 2; eye++)
        if (g_cap[eye]) n += submit_one(dev, g_cap[eye], eye, g_pend_scale, g_pend_horplus, g_pend_hdr, g_cap_clean[eye], &g_pend_pose);
    g_tex_pose = g_pend_pose;
    if (n == 2) {
        double t = vr_ms();
        step("PostPresentHandoff");
        g_comp->PostPresentHandoff();
        step("idle");
        vr_prof(P_SUBMIT, vr_ms() - t);
    }
}

/* After both passes: submit now, or leave the eyes on the GPU for the next vr_begin_frame (pipeline). */
void vr_end_frame(IDirect3DDevice9 *dev, float scale, int horplus, int hdr_linear, int pipeline)
{
    int done = g_cap_done;
    g_cap_done = 0;
    if (done != 3) return;
    g_pend_pose = g_drawn_pose;
    g_pend_scale = scale; g_pend_horplus = horplus; g_pend_hdr = hdr_linear;
    if (pipeline) { g_pending = 1; return; }
    submit_caps(dev);
}

/* ---- HUD / menu panel: a head-locked OpenVR overlay showing the game's back buffer. Render thread (Present). */
static VROverlayHandle_t g_hud;
static int g_hud_state;                           /* 0 = not made, 1 = made, -1 = failed */
static int g_hud_shown;
static ID3D11Texture2D *g_hud_tex;
static UINT g_hud_w, g_hud_h;
static IDirect3DSurface9 *g_hud_sys;
static D3DSURFACE_DESC g_hud_sys_desc;
static int g_hud_mode = -1;                       /* how it is placed now: 1 = in-game (head-locked), 0 = menu (world-locked) */
static double g_menu_yaw;                         /* head yaw the menu panel was placed at */
static int g_menu_pose_ok;                        /* ... from a valid pose */
static DWORD g_menu_away_since;                   /* when the head turned well away from it (0 = it's in view) */
static vr_hud_cfg_t g_hud_placed;                 /* the settings it was placed with */
static unsigned long g_hud_frames, g_hud_presents;
static IDirect3DSurface9 *g_hud_rt;              /* pipelined: GPU copy of the back buffer, read at the next Present */
static D3DSURFACE_DESC g_hud_rt_desc;
static int g_hud_pending, g_hud_pend_ingame;
static volatile int g_hud_dump;                   /* write the next panel image to hud_panel_N.bmp */
static unsigned g_hud_dump_no;

void vr_hud_hide(void)
{
    if (g_hud_state == 1 && g_hud_shown) { g_ovl->HideOverlay(g_hud); g_hud_shown = 0; }
}

void vr_hud_request_dump(void) { g_hud_dump = 1; }

/* A panel dist_m away along a direction pitched down_deg below the head's forward (yaw only, from the pose
 * matrix `head`, or straight ahead when head is NULL), facing the eye, upright. */
static void panel_matrix(const HmdMatrix34_t *head, float dist_m, float down_deg, HmdMatrix34_t *out)
{
    double yaw = 0, pitch, t = down_deg * RAD, px = 0, py = 0, pz = 0;
    vec3 dir, z, x, y;
    int i;
    if (head) {
        matrix_yaw_pitch(head->m, &yaw, &pitch);
        px = head->m[0][3]; py = head->m[1][3]; pz = head->m[2][3];
    }
    yaw *= RAD;
    dir = vec(sin(yaw) * cos(t), -sin(t), -cos(yaw) * cos(t));     /* yaw + = right, as matrix_yaw_pitch */
    z = scale(dir, -1.0);                                           /* the panel's normal points back at the eye */
    x = cross(vec(0, 1, 0), z);
    x = scale(x, 1.0 / sqrt(dot(x, x)));
    y = cross(z, x);
    for (i = 0; i < 3; i++) {
        out->m[i][0] = (float)x.v[i]; out->m[i][1] = (float)y.v[i]; out->m[i][2] = (float)z.v[i];
    }
    out->m[0][3] = (float)(px + dir.v[0] * dist_m);
    out->m[1][3] = (float)(py + dir.v[1] * dist_m);
    out->m[2][3] = (float)(pz + dir.v[2] * dist_m);
}

static void write_bmp32(const char *path, const unsigned char *bits, int pitch, UINT w, UINT h)
{
    FILE *f = fopen(path, "wb");
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;
    UINT y;
    if (!f) return;
    memset(&fh, 0, sizeof fh); memset(&ih, 0, sizeof ih);
    fh.bfType = 0x4D42; fh.bfOffBits = sizeof fh + sizeof ih; fh.bfSize = fh.bfOffBits + w * h * 4;
    ih.biSize = sizeof ih; ih.biWidth = (LONG)w; ih.biHeight = -(LONG)h; ih.biPlanes = 1; ih.biBitCount = 32;
    fwrite(&fh, sizeof fh, 1, f); fwrite(&ih, sizeof ih, 1, f);
    for (y = 0; y < h; y++) fwrite(bits + (size_t)y * (size_t)pitch, 4, w, f);
    fclose(f);
}

/* Read a copy of the back buffer (bb) back, make black see-through in a mission, and put it on the panel. */
static void hud_upload(IDirect3DDevice9 *dev, IDirect3DSurface9 *bb, int in_game)
{
    D3DSURFACE_DESC d;
    D3DLOCKED_RECT lr;
    Texture_t tex;
    UINT x, y;
    HRESULT hr;
    EVROverlayError oe;

    IDirect3DSurface9_GetDesc(bb, &d);
    if (!g_hud_sys || g_hud_sys_desc.Width != d.Width || g_hud_sys_desc.Height != d.Height || g_hud_sys_desc.Format != d.Format) {
        if (g_hud_sys) { IDirect3DSurface9_Release(g_hud_sys); g_hud_sys = NULL; }
        hr = IDirect3DDevice9_CreateOffscreenPlainSurface(dev, d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &g_hud_sys, NULL);
        if (FAILED(hr) || !g_hud_sys) { g_hud_sys = NULL; return; }
        g_hud_sys_desc = d;
    }
    if (!g_hud_tex || g_hud_w != d.Width || g_hud_h != d.Height) {
        D3D11_TEXTURE2D_DESC td;
        if (g_hud_tex) { ID3D11Texture2D_Release(g_hud_tex); g_hud_tex = NULL; }
        memset(&td, 0, sizeof td);
        td.Width = d.Width; td.Height = d.Height; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(g_d11, &td, NULL, &g_hud_tex))) { g_hud_tex = NULL; return; }
        g_hud_w = d.Width; g_hud_h = d.Height;
        hl_log("openvr: HUD panel texture %ux%u", d.Width, d.Height);
    }
    hr = IDirect3DDevice9_GetRenderTargetData(dev, bb, g_hud_sys);
    if (FAILED(hr)) { if (g_errs++ < 5) hl_log("openvr: HUD GetRenderTargetData failed: %#lx", (unsigned long)hr); return; }
    if (FAILED(IDirect3DSurface9_LockRect(g_hud_sys, &lr, NULL, 0))) return;
    for (y = 0; y < d.Height; y++) {
        UINT32 *px = (UINT32 *)((unsigned char *)lr.pBits + (size_t)y * (size_t)lr.Pitch);
        if (!in_game) { for (x = 0; x < d.Width; x++) px[x] |= 0xFF000000u; continue; }   /* menus: opaque */
        /* HUD over black: black is see-through; anything else is (nearly) solid, so dark HUD parts such as a
         * dim light gem still show. Alpha = 4 x the brightest channel, premultiplied (colour <= alpha holds). */
        for (x = 0; x < d.Width; x++) {
            UINT32 v = px[x], r = (v >> 16) & 255, g = (v >> 8) & 255, b = v & 255, a = r > g ? r : g;
            if (b > a) a = b;
            a *= 4;
            if (a > 255) a = 255;
            px[x] = (v & 0x00FFFFFFu) | (a << 24);
        }
    }
    if (g_hud_dump) {
        char path[MAX_PATH + 40];
        g_hud_dump = 0;
        snprintf(path, sizeof path, "%shud_panel_%u.bmp", g_dir_vr, ++g_hud_dump_no);
        write_bmp32(path, (const unsigned char *)lr.pBits, lr.Pitch, d.Width, d.Height);
        hl_log("openvr: wrote hud_panel_%u.bmp (%ux%u, %s)", g_hud_dump_no, d.Width, d.Height, in_game ? "in-game HUD" : "menu");
    }
    ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)g_hud_tex, 0, NULL, lr.pBits, (UINT)lr.Pitch, 0);
    IDirect3DSurface9_UnlockRect(g_hud_sys);
    ID3D11DeviceContext_Flush(g_ctx);
    tex.handle = g_hud_tex; tex.eType = ETextureType_TextureType_DirectX; tex.eColorSpace = EColorSpace_ColorSpace_Gamma;
    oe = g_ovl->SetOverlayTexture(g_hud, &tex);
    if (oe != 0 && g_errs++ < 5) hl_log("openvr: SetOverlayTexture failed: %d", (int)oe);
    if (!g_hud_shown) { g_ovl->ShowOverlay(g_hud); g_hud_shown = 1; }
    g_hud_frames++;
}

void vr_hud_present(IDirect3DDevice9 *dev, int in_game, const vr_hud_cfg_t *c)
{
    IDirect3DSurface9 *bb = NULL;
    D3DSURFACE_DESC d;
    HRESULT hr;
    EVROverlayError oe;
    int due;

    if (!vr_ready() || !g_ovl || g_hud_state < 0) return;
    g_hud_presents++;
    /* In a mission the panel only carries the HUD: refreshing it every Nth frame saves most of its cost (a
     * second GPU->CPU copy). Menus get every frame (nothing else to draw then, and the cursor stays smooth). */
    due = !(in_game && c->every > 1 && g_hud_presents % (unsigned long)c->every != 0 && !g_hud_dump && g_hud_mode == 1);
    if (!due && !g_hud_pending) return;
    if (!g_d11 && !g_d11_failed) { if (!make_d3d11()) g_d11_failed = 1; }
    if (!g_d11) return;
    if (g_hud_state == 0) {
        oe = g_ovl->CreateOverlay("thief2.headlook.hud", "Thief II HUD", &g_hud);
        if (oe != 0) { hl_log("openvr: CreateOverlay failed: %d (no HUD panel)", (int)oe); g_hud_state = -1; return; }
        g_ovl->SetOverlayFlag(g_hud, VROverlayFlags_IsPremultiplied, 1);
        g_hud_state = 1;
        hl_log("openvr: HUD panel overlay created");
    }
    if (!in_game && g_hud_mode == 0) {               /* menu panel: follow lazily if it's been out of view for a while */
        TrackedDevicePose_t pose;
        double yaw, pitch;
        memset(&pose, 0, sizeof pose);
        g_sys->GetDeviceToAbsoluteTrackingPose(ETrackingUniverseOrigin_TrackingUniverseSeated, 0.0f, &pose, 1);
        if (pose.bPoseIsValid) {
            matrix_yaw_pitch(pose.mDeviceToAbsoluteTracking.m, &yaw, &pitch);
            if (!g_menu_pose_ok) g_menu_replace = 1;  /* was placed without a pose (headset not tracking yet) */
            else if (fabs(wrap180(yaw - g_menu_yaw)) > 50.0) {
                if (!g_menu_away_since) g_menu_away_since = GetTickCount();
                else if (GetTickCount() - g_menu_away_since > 1500) g_menu_replace = 1;
            } else g_menu_away_since = 0;
        }
    }
    if (in_game != g_hud_mode || memcmp(c, &g_hud_placed, sizeof *c) != 0 || (!in_game && g_menu_replace)) {
        HmdMatrix34_t m;
        float wd = in_game ? c->hud_deg : c->menu_deg, dm = c->dist < 0.3f ? 0.3f : c->dist;
        if (wd < 10) wd = 10;
        if (wd > 150) wd = 150;
        float full_w = (float)(2.0 * dm * tan(wd * 0.5 * RAD)), show_w = full_w;
        VRTextureBounds_t tb;
        tb.uMin = tb.vMin = 0.0f; tb.uMax = tb.vMax = 1.0f;
        if (in_game) {                                /* the HUD: fixed to the head */
            panel_matrix(NULL, dm, c->hud_down, &m);
            if (c->gem_only && c->gem_x1 > c->gem_x0 && c->gem_y1 > c->gem_y0) {
                /* Just the light gem's part of the screen, at the same spot the full panel would show it. */
                float full_h = full_w * (g_hud_w ? (float)g_hud_h / (float)g_hud_w : 0.75f);
                float dx = ((c->gem_x0 + c->gem_x1) * 0.5f - 0.5f) * full_w;
                float dy = -((c->gem_y0 + c->gem_y1) * 0.5f - 0.5f) * full_h;
                int i;
                for (i = 0; i < 3; i++) m.m[i][3] += m.m[i][0] * dx + m.m[i][1] * dy;
                tb.uMin = c->gem_x0; tb.uMax = c->gem_x1; tb.vMin = c->gem_y0; tb.vMax = c->gem_y1;
                show_w = full_w * (c->gem_x1 - c->gem_x0);
            }
            g_ovl->SetOverlayTransformTrackedDeviceRelative(g_hud, 0, &m);   /* device 0 = the headset */
        } else {                                      /* menus, books: fixed in the room, in front of where you look now */
            TrackedDevicePose_t pose;
            memset(&pose, 0, sizeof pose);
            g_sys->GetDeviceToAbsoluteTrackingPose(ETrackingUniverseOrigin_TrackingUniverseSeated, 0.0f, &pose, 1);
            panel_matrix(pose.bPoseIsValid ? &pose.mDeviceToAbsoluteTracking : NULL, dm, c->menu_down, &m);
            g_ovl->SetOverlayTransformAbsolute(g_hud, ETrackingUniverseOrigin_TrackingUniverseSeated, &m);
            g_menu_pose_ok = pose.bPoseIsValid;
            g_menu_yaw = 0;
            if (pose.bPoseIsValid) { double p; matrix_yaw_pitch(pose.mDeviceToAbsoluteTracking.m, &g_menu_yaw, &p); }
            if (g_menu_replace) hl_log("openvr: menu panel re-placed in front of the head (yaw %.0f)", g_menu_yaw);
            g_menu_replace = 0;
            g_menu_away_since = 0;
        }
        g_ovl->SetOverlayTextureBounds(g_hud, &tb);
        g_ovl->SetOverlayWidthInMeters(g_hud, show_w);
        if (in_game != g_hud_mode || c->hud_deg != g_hud_placed.hud_deg || c->menu_deg != g_hud_placed.menu_deg ||
            c->dist != g_hud_placed.dist || c->hud_down != g_hud_placed.hud_down || c->menu_down != g_hud_placed.menu_down ||
            c->gem_only != g_hud_placed.gem_only)
            hl_log("openvr: panel placed for %s: %.0f deg wide at %.2f m, %.0f deg down, %s", in_game ? (c->gem_only ? "the light gem only" : "the HUD") : "menus",
                   wd, dm, in_game ? c->hud_down : c->menu_down, in_game ? "fixed to the head" : "fixed in the room");
        g_hud_mode = in_game;
        g_hud_placed = *c;
    }

    if (g_hud_pending) { g_hud_pending = 0; hud_upload(dev, g_hud_rt, g_hud_pend_ingame); }
    if (!due) return;
    if (FAILED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
    IDirect3DSurface9_GetDesc(bb, &d);
    if (d.Format != D3DFMT_X8R8G8B8 && d.Format != D3DFMT_A8R8G8B8) {
        if (g_errs++ < 5) hl_log("openvr: back buffer format %d not handled for the HUD panel", (int)d.Format);
        IDirect3DSurface9_Release(bb);
        return;
    }
    if (c->pipeline) {                            /* copy on the GPU now; read it back next Present, when it's done */
        if (g_hud_rt && (g_hud_rt_desc.Width != d.Width || g_hud_rt_desc.Height != d.Height || g_hud_rt_desc.Format != d.Format)) {
            IDirect3DSurface9_Release(g_hud_rt); g_hud_rt = NULL;
        }
        if (!g_hud_rt) {
            hr = IDirect3DDevice9_CreateRenderTarget(dev, d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &g_hud_rt, NULL);
            if (FAILED(hr) || !g_hud_rt) g_hud_rt = NULL; else g_hud_rt_desc = d;
        }
        if (g_hud_rt && SUCCEEDED(IDirect3DDevice9_StretchRect(dev, bb, NULL, g_hud_rt, NULL, D3DTEXF_NONE))) {
            g_hud_pending = 1;
            g_hud_pend_ingame = in_game;
            IDirect3DSurface9_Release(bb);
            return;
        }
    }
    hud_upload(dev, bb, in_game);                 /* not pipelined, or the GPU copy failed */
    IDirect3DSurface9_Release(bb);
}

/* Menus, loading, paused: the game draws no 3D view, so without this SteamVR gets no frames from us at all and
 * shows its own idle room (at launch our menu panel was never seen). Render thread, from Present: wait for the
 * headset like a game frame, then submit the last 3D frame with the pose it was drawn at (the paused world stays
 * put in the room), or black before the first map. */
static ID3D11Texture2D *g_black;
static unsigned long g_idle_frames;
void vr_idle_frame(IDirect3DDevice9 *dev)
{
    TrackedDevicePose_t p;
    int eye;
    if (!vr_ready()) return;
    /* Only once a 3D frame exists: sending black frames from launch made SteamVR open its dashboard ("Resume
     * game"), presumably when the frames then stopped while the first map loaded (user report 2026-09-25). */
    if (!g_tex[0] || !g_tex[1]) { release_caps(); return; }
    if (!g_d11 && !g_d11_failed) { if (!make_d3d11()) g_d11_failed = 1; }
    if (!g_d11) { release_caps(); return; }
    if (!g_black) {
        static UINT32 px[16 * 16];
        D3D11_TEXTURE2D_DESC td;
        D3D11_SUBRESOURCE_DATA init;
        int i;
        for (i = 0; i < 16 * 16; i++) px[i] = 0xFF000000u;
        memset(&td, 0, sizeof td);
        td.Width = td.Height = 16; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        init.pSysMem = px; init.SysMemPitch = 16 * 4; init.SysMemSlicePitch = 0;
        if (FAILED(ID3D11Device_CreateTexture2D(g_d11, &td, &init, &g_black))) { g_black = NULL; return; }
    }
    poll_events();
    if (g_quit) { release_caps(); return; }
    poll_recentre_keys();
    if (g_comp->WaitGetPoses(&p, 1, NULL, 0) != EVRCompositorError_VRCompositorError_None) { release_caps(); return; }
    if (g_pending) {                              /* the last 3D frame, still on the GPU (pipelined): send it first */
        g_pending = 0;
        submit_caps(dev);
        release_caps();                               /* no default-pool surfaces left lying around in menus (device Reset) */
        return;
    }
    release_caps();
    for (eye = 0; eye < 2; eye++) {
        if (g_tex[eye]) {
            VRTextureWithPose_t t;
            memset(&t, 0, sizeof t);
            t.handle = g_tex[eye]; t.eType = ETextureType_TextureType_DirectX; t.eColorSpace = EColorSpace_ColorSpace_Gamma;
            t.mDeviceToAbsoluteTracking = g_tex_pose;
            g_comp->Submit((EVREye)eye, (struct Texture_t *)&t, &g_bounds[eye], EVRSubmitFlags_Submit_TextureWithPose);
        } else {
            Texture_t t;
            t.handle = g_black; t.eType = ETextureType_TextureType_DirectX; t.eColorSpace = EColorSpace_ColorSpace_Gamma;
            g_comp->Submit((EVREye)eye, &t, NULL, EVRSubmitFlags_Submit_Default);
        }
    }
    g_comp->PostPresentHandoff();
    if (g_idle_frames++ == 0) hl_log("openvr: sending idle frames while the game shows menus (%s)", g_tex[0] ? "last 3D frame" : "black");
}

/* Render thread, just before the engine's device Reset: default-pool surfaces must all be gone or it fails. */
void vr_device_reset(void)
{
    static int logged;
    release_caps();
    if (g_hud_rt) { IDirect3DSurface9_Release(g_hud_rt); g_hud_rt = NULL; }
    g_hud_pending = 0;
    if (logged++ < 5) hl_log("openvr: device Reset: capture surfaces released");
}

/* Worker thread: reads counters only, never calls OpenVR. */
void vr_status(void)
{
    const char *st = g_step;
    if (!g_ready) return;
    hl_log("openvr: %lu poses, %lu eye images submitted, %lu HUD panel updates, %lu errors%s", g_frames, g_submits, g_hud_frames, g_errs,
           g_quit ? " (SteamVR quit)" : "");
    vr_prof_report("openvr");
    if (strcmp(st, "idle") != 0 && GetTickCount() - g_step_since > 2000)
        hl_log("openvr: render thread STUCK in %s for %lu ms", st, (unsigned long)(GetTickCount() - g_step_since));
}
