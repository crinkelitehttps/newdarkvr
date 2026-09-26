/*
 * vr_wxr.c: part of headlook.dll. Standalone Quest (and other Android headsets) through WinlatorXR's "XrAPI",
 * switched on with stereo=wxr (ini) or HEADLOOK_STEREO=wxr. WinlatorXR runs the game in Wine + Box64 on the headset
 * and shows its window; with the XrAPI the game becomes the VR app. The protocol, as implemented in WinlatorXR's own
 * source (app/src/main/java/com/winlator/xr/api/XrVersion0*.java, cpp/xr/renderer.c; version 0.5):
 *   - We write "0.5" to Z:\tmp\xr\version, which makes WinlatorXR listen on UDP 7278 for our state:
 *     "L_HAPTICS R_HAPTICS MODE_VR MODE_3D FOVX FOVY" (MODE_VR 1 = VR, 2 = its flat screen; MODE_3D 1 = side by side;
 *     FOVX/FOVY: the full horizontal/vertical angle our picture covers, in degrees, or 0 for the headset's own).
 *   - Once MODE_VR > 0 it sends, every headset frame, to UDP 7872 (and 7873): "client0", then 29 floats (left and
 *     right controller: quaternion x y z w, thumbstick x y, position x y z; head: quaternion, position, IPD (m),
 *     FOVX, FOVY, HMD_SYNC), a string of 19 T/F button states, 9 more floats (height, grip quaternions) and "TF"
 *     flags (immersive, SBS). OpenXR frame: right-handed, +Y up, looking along -Z.
 *   - HMD_SYNC steps 0, 12, ... 252: WinlatorXR keeps the head pose it sent with each value. We draw with that pose,
 *     then paint the value as the red channel of the frame's top-left pixel (green 0, alpha > 0). It reads the pixel
 *     back and places our picture at the stored pose, so its reprojection covers the delay. That pose includes head
 *     roll, so roll is drawn too (the engine's bank angle), unlike the OpenVR path.
 * Per frame: wxr_begin_frame waits (up to ~20 ms) for a new HMD_SYNC and gives the pose; the existing stereo code
 * draws both eyes and captures them (wxr_capture_eye, through the brightness curve); at Present, where the back
 * buffer holds the engine's HUD over black (see stereo_frame), wxr_present copies that HUD aside, puts the eyes side
 * by side into the back buffer, draws the HUD into both halves at a comfortable depth, and paints the sync pixel.
 * Menus, books and loading screens: MODE_VR 2, WinlatorXR's own flat screen.
 * ROUGH CUT, desk-tested only against tools/wxr_fake.py (no headset). Each eye is drawn at the full back buffer size
 * and squeezed into half its width (the engine's picture always has the back buffer's shape); a later step could
 * use alternate-eye frames (MODE_3D 2) instead.
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <winsock2.h>
#include <windows.h>
#include <d3d9.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vr_wxr.h"
#include "vr_openvr.h"                            /* vr_ms, vr_prof*, vr_pad_buttons */
#include "vr_gpu.h"

void hl_log(const char *fmt, ...);
void hl_dump_surface(IDirect3DDevice9 *dev, IDirect3DSurface9 *src, const char *name);

#define DEG (180.0 / 3.14159265358979323846)
#define RAD (3.14159265358979323846 / 180.0)
#define API_DIR "Z:\\tmp\\xr"
#define PORT_IN 7872
#define PORT_OUT 7278
#define SYNC_WAIT_MS 20

typedef struct {
    float hq[4], hpos[3], ipd, fovx, fovy;
    int sync;
    float lstick[2], rstick[2];
    unsigned buttons;
} pkt_t;

static CRITICAL_SECTION g_cs;
static HANDLE g_evt;                              /* auto-reset: a packet with a new HMD_SYNC arrived */
static pkt_t g_pkt;
static volatile int g_have;
static volatile DWORD g_last_rx;
static volatile unsigned long g_rx, g_bad, g_syncs;
static SOCKET g_tx = INVALID_SOCKET;
static struct sockaddr_in g_to;
static int g_port;
static volatile int g_ready;

/* Mode we want WinlatorXR in; the worker re-sends it every second, Present on every change. */
static volatile int g_mode_vr = 2, g_mode_3d = 0;
static volatile float g_send_fovx, g_send_fovy;
static int g_sent_vr = -1, g_sent_3d = -1;
static float g_sent_fovx = -1, g_sent_fovy = -1;
static volatile DWORD g_fov0_since;               /* since when we've asked for the headset's own FOV (0 0) */
static volatile float g_native_fov;               /* the headset's own FOV (largest of the two, degrees; incl. WinlatorXR's 1.1 margin) */

/* Render-thread state. */
static int g_used_sync = -1, g_frame_sync, g_eyes_sync;
static double g_ref_yaw;
static int g_have_ref, g_recentre_req, g_key_was_down;
static double g_ipd_m = 0.064;
static IDirect3DSurface9 *g_eye[2];
static D3DSURFACE_DESC g_eye_desc[2];
static int g_eye_done, g_have_frame;
static double g_tx_t = 1, g_ty_t = 1;             /* the eye picture's half extent, in tangents */
static IDirect3DTexture9 *g_hudtex;
static D3DSURFACE_DESC g_hudtex_desc;
static IDirect3DPixelShader9 *g_hud_ps;
static IDirect3DDevice9 *g_hud_ps_dev;
static int g_hud_ps_failed;
static unsigned long g_frames, g_composed;
static volatile int g_dump;

void wxr_request_dump(void) { g_dump = 1; }

static void send_mode(void)
{
    char m[96];
    int n;
    if (g_tx == INVALID_SOCKET) return;
    n = snprintf(m, sizeof m, "0 0 %d %d %.2f %.2f", g_mode_vr, g_mode_3d, (double)g_send_fovx, (double)g_send_fovy);
    sendto(g_tx, m, n, 0, (const struct sockaddr *)&g_to, sizeof g_to);
    if (g_mode_vr != g_sent_vr || g_mode_3d != g_sent_3d || g_send_fovx != g_sent_fovx || g_send_fovy != g_sent_fovy)
        hl_log("wxr: -> \"%s\" (%s)", m, g_mode_vr == 1 ? "VR, side by side" : "flat screen for menus");
    if (g_send_fovx <= 0 && g_sent_fovx != 0) g_fov0_since = GetTickCount();
    g_sent_vr = g_mode_vr; g_sent_3d = g_mode_3d; g_sent_fovx = g_send_fovx; g_sent_fovy = g_send_fovy;
}

/* "client0 f*29 TF... [f*9] [TF]" -> g_pkt. Tokenised by hand (strtok isn't thread-safe and the game may use it). */
static void parse(char *s)
{
    char *tok[64];
    int n = 0, i = 0, k;
    float f[29];
    pkt_t p;
    while (*s && n < 64) {
        while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') *s++ = 0;
        if (!*s) break;
        tok[n++] = s;
        while (*s && *s != ' ' && *s != '\t' && *s != '\r' && *s != '\n') s++;
    }
    if (n > 0 && !strncmp(tok[0], "client", 6)) i = 1;
    if (n - i < 30) { g_bad++; return; }
    for (k = 0; k < 29; k++) f[k] = (float)strtod(tok[i + k], NULL);
    memset(&p, 0, sizeof p);
    p.lstick[0] = f[4]; p.lstick[1] = f[5];
    p.rstick[0] = f[13]; p.rstick[1] = f[14];
    memcpy(p.hq, &f[18], sizeof p.hq);
    memcpy(p.hpos, &f[22], sizeof p.hpos);
    p.ipd = f[25]; p.fovx = f[26]; p.fovy = f[27]; p.sync = (int)(f[28] + 0.5f);
    for (k = 0; tok[i + 29][k] && k < 32; k++) if (tok[i + 29][k] == 'T') p.buttons |= 1u << k;
    EnterCriticalSection(&g_cs);
    k = !g_have || p.sync != g_pkt.sync;
    g_pkt = p;
    g_have = 1;
    g_last_rx = GetTickCount();
    g_rx++;
    if (k) g_syncs++;
    LeaveCriticalSection(&g_cs);
    if (k) SetEvent(g_evt);
    /* Learn the headset's own FOV while we're not overriding it (the packet echoes what we set). */
    if (g_sent_fovx == 0 && GetTickCount() - g_fov0_since > 500) {
        float fv = p.fovx > p.fovy ? p.fovx : p.fovy;
        if (fv > 30 && fv < 170) {
            if (fabsf(fv - g_native_fov) > 0.5f) hl_log("wxr: headset's own field of view %.1f x %.1f deg", (double)p.fovx, (double)p.fovy);
            g_native_fov = fv;
        }
    }
}

static DWORD WINAPI rx_thread(LPVOID arg)
{
    SOCKET s = (SOCKET)(UINT_PTR)arg;
    char buf[2048];
    for (;;) {
        int n = recv(s, buf, sizeof buf - 1, 0);
        if (n <= 0) { Sleep(10); continue; }
        buf[n] = 0;
        parse(buf);
    }
    return 0;
}

int wxr_init(const char *dll_dir)
{
    WSADATA wsa;
    SOCKET s;
    struct sockaddr_in a;
    FILE *f;
    char line[256];
    int port;
    (void)dll_dir;

    if (g_ready) return 1;
    InitializeCriticalSection(&g_cs);
    g_evt = CreateEventA(NULL, FALSE, FALSE, NULL);
    WSAStartup(MAKEWORD(2, 2), &wsa);

    /* What WinlatorXR wrote about the device (manufacturer, product, Android version, patch level, screen size). */
    if ((f = fopen(API_DIR "\\system", "r"))) {
        int i = 0;
        while (fgets(line, sizeof line, f) && i++ < 8) { line[strcspn(line, "\r\n")] = 0; hl_log("wxr: system: %s", line); }
        fclose(f);
    } else hl_log("wxr: no " API_DIR "\\system (not running under WinlatorXR, or its XR mode is off)");
    CreateDirectoryA("Z:\\tmp", NULL);
    CreateDirectoryA(API_DIR, NULL);
    if ((f = fopen(API_DIR "\\version", "w"))) { fputs("0.5\n", f); fclose(f); hl_log("wxr: wrote " API_DIR "\\version (XrAPI 0.5)"); }
    else hl_log("wxr: could NOT write " API_DIR "\\version (%lu): WinlatorXR won't talk to us", GetLastError());

    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    for (port = PORT_IN; port <= PORT_IN + 1; port++) {
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET; a.sin_port = htons((u_short)port); a.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(s, (struct sockaddr *)&a, sizeof a) == 0) break;
    }
    if (port > PORT_IN + 1) { hl_log("wxr: cannot bind UDP %d or %d (%d)", PORT_IN, PORT_IN + 1, WSAGetLastError()); closesocket(s); return 0; }
    g_port = port;
    g_tx = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    memset(&g_to, 0, sizeof g_to);
    g_to.sin_family = AF_INET; g_to.sin_port = htons(PORT_OUT); g_to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CreateThread(NULL, 0, rx_thread, (LPVOID)(UINT_PTR)s, 0, NULL);
    g_ready = 1;
    send_mode();
    hl_log("wxr: listening on UDP %d, sending our mode to %d", g_port, PORT_OUT);
    return 1;
}

int wxr_have_data(void) { return g_ready && g_have && GetTickCount() - g_last_rx < 1000; }

void wxr_recentre(void) { g_recentre_req = 1; }

static void poll_recentre_keys(void)
{
    int down = ((GetAsyncKeyState(VK_PAUSE) | GetAsyncKeyState(VK_SCROLL)) & 0x8000) != 0 || (vr_pad_buttons() & 0x00C0) == 0x00C0;
    if (down && !g_key_was_down) g_recentre_req = 1;
    g_key_was_down = down;
}

/* Head orientation (rotates head space into the tracking space) -> yaw right, pitch up, roll right-ear-down. */
static void quat_angles(const float *q, double *yaw, double *pitch, double *roll)
{
    double x = q[0], y = q[1], z = q[2], w = q[3];
    double fx = -2 * (x * z + w * y), fy = -2 * (y * z - w * x), fz = -(1 - 2 * (x * x + y * y));   /* R * (0,0,-1) */
    double rx = 1 - 2 * (y * y + z * z), ry = 2 * (x * y + w * z), rz = 2 * (x * z - w * y);        /* R * (1,0,0) */
    double l = sqrt(fx * fx + fy * fy + fz * fz), hx, hz, ux, uy, uz;
    if (l < 1e-9) { *yaw = *pitch = *roll = 0; return; }
    fx /= l; fy /= l; fz /= l;
    *yaw = atan2(fx, -fz) * DEG;
    *pitch = asin(fy > 1 ? 1 : (fy < -1 ? -1 : fy)) * DEG;
    hx = cos(*yaw * RAD); hz = sin(*yaw * RAD);   /* the level right vector for this heading */
    ux = -hz * fy; uy = hz * fx - hx * fz; uz = hx * fy;   /* up without roll = cross(level right, forward) */
    *roll = atan2(-(rx * ux + ry * uy + rz * uz), rx * hx + rz * hz) * DEG;
}

static double wrap180(double d) { while (d > 180.0) d -= 360.0; while (d < -180.0) d += 360.0; return d; }

int wxr_begin_frame(double *yaw_right, double *pitch_up, double *roll_right, double *ipd_m)
{
    pkt_t p;
    double t0, yaw, pitch, roll;
    int cur;

    vr_prof_tick();
    g_eye_done = 0;
    if (!wxr_have_data()) return 0;
    /* Pace to the headset: wait for a headset frame we haven't drawn yet (its pose is what gets stored). */
    t0 = vr_ms();
    for (;;) {
        double left;
        EnterCriticalSection(&g_cs); cur = g_pkt.sync; LeaveCriticalSection(&g_cs);
        if (cur != g_used_sync) break;
        left = SYNC_WAIT_MS - (vr_ms() - t0);
        if (left <= 0) break;
        WaitForSingleObject(g_evt, (DWORD)left + 1);
    }
    vr_prof(P_WAIT, vr_ms() - t0);
    EnterCriticalSection(&g_cs); p = g_pkt; LeaveCriticalSection(&g_cs);
    g_used_sync = p.sync;
    g_frame_sync = p.sync;

    poll_recentre_keys();
    quat_angles(p.hq, &yaw, &pitch, &roll);
    if (!g_have_ref || g_recentre_req) {
        g_ref_yaw = yaw; g_have_ref = 1; g_recentre_req = 0;
        hl_log("wxr: centred at tracking yaw %.1f", yaw);
    }
    if (p.ipd > 0.04f && p.ipd < 0.09f) g_ipd_m = p.ipd;
    *yaw_right = wrap180(yaw - g_ref_yaw);
    *pitch_up = pitch > 89 ? 89 : (pitch < -89 ? -89 : pitch);
    *roll_right = roll;
    *ipd_m = g_ipd_m;
    g_frames++;
    if (g_frames <= 3 || g_frames % 2000 == 0)
        hl_log("wxr: frame %lu: sync %d, yaw %.1f pitch %.1f roll %.1f, ipd %.1f mm", g_frames, p.sync, *yaw_right, *pitch_up, roll, g_ipd_m * 1000);
    return 1;
}

/* The engine picture's extent for view scale s: the reference view is 4:3 (tan 1/s wide, 0.75/s high); Hor+ keeps
 * the height and widens with the aspect, otherwise the width stays (see vr_openvr.c, image_tangents). */
static void tangents(UINT w, UINT h, float s, int horplus, double *tx, double *ty)
{
    if (horplus) { *ty = 0.75 / s; *tx = *ty * (double)w / (double)h; }
    else         { *tx = 1.0 / s;  *ty = *tx * (double)h / (double)w; }
}

float wxr_view_scale(int horplus, UINT w, UINT h, float fov_deg)
{
    double fov = fov_deg > 10 ? fov_deg : (g_native_fov > 10 ? g_native_fov : 100.0), need, aspect, sx, sy;
    if (fov > 160) fov = 160;
    need = tan(fov * 0.5 * RAD);
    aspect = w && h ? (double)w / (double)h : 2.0;
    if (horplus) { sy = 0.75 / need; sx = 0.75 * aspect / need; }
    else         { sx = 1.0 / need;  sy = 1.0 / (aspect * need); }
    return (float)(sx < sy ? sx : sy);             /* the smaller scale is the wider view: covers both */
}

void wxr_capture_eye(IDirect3DDevice9 *dev, IDirect3DSurface9 *src, int eye, float gamma, float black)
{
    D3DSURFACE_DESC d;
    HRESULT hr = S_OK;
    double t0 = vr_ms();
    if (eye < 0 || eye > 1) return;
    IDirect3DSurface9_GetDesc(src, &d);
    if (g_eye[eye] && (g_eye_desc[eye].Width != d.Width || g_eye_desc[eye].Height != d.Height)) { IDirect3DSurface9_Release(g_eye[eye]); g_eye[eye] = NULL; }
    if (!g_eye[eye]) {
        hr = IDirect3DDevice9_CreateRenderTarget(dev, d.Width, d.Height, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &g_eye[eye], NULL);
        if (FAILED(hr) || !g_eye[eye]) { g_eye[eye] = NULL; { static int n; if (n++ < 5) hl_log("wxr: eye target %ux%u failed: %#lx", d.Width, d.Height, (unsigned long)hr); } return; }
        IDirect3DSurface9_GetDesc(g_eye[eye], &g_eye_desc[eye]);
    }
    if (!vr_curve_copy(dev, src, g_eye[eye], gamma, black))
        hr = IDirect3DDevice9_StretchRect(dev, src, NULL, g_eye[eye], NULL, D3DTEXF_NONE);
    if (FAILED(hr)) { static int n; if (n++ < 5) hl_log("wxr: eye copy failed: %#lx", (unsigned long)hr); return; }
    g_eye_done |= 1 << eye;
    vr_prof(P_CAPTURE, vr_ms() - t0);
}

void wxr_end_frame(float scale, int horplus)
{
    double tx, ty;
    if (g_eye_done != 3) return;
    g_eye_done = 0;
    tangents(g_eye_desc[0].Width, g_eye_desc[0].Height, scale, horplus, &tx, &ty);
    g_tx_t = tx; g_ty_t = ty;
    g_send_fovx = (float)(2 * atan(tx) * DEG);
    g_send_fovy = (float)(2 * atan(ty) * DEG);
    g_eyes_sync = g_frame_sync;
    g_have_frame = 1;
}

/* HUD over black -> premultiplied alpha: black is see-through, anything brighter than 1/4 is solid (as the OpenVR
 * panel does it: a dim light gem still shows). c0.x = 4. */
static const char g_hud_src[] =
    "ps_2_0\n"
    "dcl t0.xy\n"
    "dcl_2d s0\n"
    "texld r0, t0, s0\n"
    "max r1.x, r0.x, r0.y\n"
    "max r1.x, r1.x, r0.z\n"
    "mul_sat r0.w, r1.x, c0.x\n"
    "mov oC0, r0\n";

static int hud_shader(IDirect3DDevice9 *dev)
{
    if (g_hud_ps && g_hud_ps_dev == dev) return 1;
    if (g_hud_ps_failed) return 0;
    g_hud_ps = vr_assemble_ps(dev, g_hud_src, "HUD alpha");
    g_hud_ps_dev = dev;
    if (!g_hud_ps) g_hud_ps_failed = 1;
    return g_hud_ps != NULL;
}

static void release_frame(void)
{
    int i;
    for (i = 0; i < 2; i++) if (g_eye[i]) { IDirect3DSurface9_Release(g_eye[i]); g_eye[i] = NULL; }
    if (g_hudtex) { IDirect3DTexture9_Release(g_hudtex); g_hudtex = NULL; }
    g_eye_done = 0;
    g_have_frame = 0;
}

void wxr_device_reset(void)
{
    release_frame();
    vr_gpu_reset();
}

void wxr_present(IDirect3DDevice9 *dev, int in_game, const wxr_present_cfg_t *c)
{
    IDirect3DSurface9 *bb = NULL, *lvl = NULL;
    D3DSURFACE_DESC d;
    RECT half[2];
    D3DRECT px;
    RECT sync_rc;
    double t0 = vr_ms();
    int eye, hud;
    HRESULT hr;

    if (!g_ready) return;
    g_mode_vr = in_game ? 1 : 2;
    g_mode_3d = in_game ? 1 : 0;
    if (!in_game) { g_send_fovx = g_send_fovy = 0; release_frame(); }   /* no default-pool surfaces kept in menus */
    if (g_mode_vr != g_sent_vr || g_mode_3d != g_sent_3d || (in_game && g_send_fovx != g_sent_fovx)) send_mode();
    if (!in_game || !g_have_frame || !g_eye[0] || !g_eye[1]) return;
    if (FAILED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
    IDirect3DSurface9_GetDesc(bb, &d);

    /* 1. The HUD: the back buffer holds the engine's overlays and text over black; keep a copy to draw from. */
    hud = c->hud && hud_shader(dev);
    if (hud) {
        if (g_hudtex && (g_hudtex_desc.Width != d.Width || g_hudtex_desc.Height != d.Height || g_hudtex_desc.Format != d.Format)) {
            IDirect3DTexture9_Release(g_hudtex); g_hudtex = NULL;
        }
        if (!g_hudtex) {
            hr = IDirect3DDevice9_CreateTexture(dev, d.Width, d.Height, 1, D3DUSAGE_RENDERTARGET, d.Format, D3DPOOL_DEFAULT, &g_hudtex, NULL);
            if (FAILED(hr) || !g_hudtex) { g_hudtex = NULL; hud = 0; }
            else g_hudtex_desc = d;
        }
        if (hud && SUCCEEDED(IDirect3DTexture9_GetSurfaceLevel(g_hudtex, 0, &lvl)) && lvl) {
            if (FAILED(hr = IDirect3DDevice9_StretchRect(dev, bb, NULL, lvl, NULL, D3DTEXF_NONE))) { hud = 0; hl_log("wxr: HUD copy failed: %#lx", (unsigned long)hr); }
            else if (g_dump) hl_dump_surface(dev, lvl, "wxr_hud_src.bmp");
            IDirect3DSurface9_Release(lvl);
        } else hud = 0;
    }

    /* 2. The eyes, side by side (each squeezed to half the width). */
    half[0].left = 0; half[0].top = 0; half[0].right = (LONG)(d.Width / 2); half[0].bottom = (LONG)d.Height;
    half[1].left = (LONG)(d.Width / 2); half[1].top = 0; half[1].right = (LONG)d.Width; half[1].bottom = (LONG)d.Height;
    for (eye = 0; eye < 2; eye++)
        IDirect3DDevice9_StretchRect(dev, g_eye[eye], NULL, bb, &half[eye], D3DTEXF_LINEAR);

    /* 3. The HUD into each half, as a panel c->hud_deg wide at c->dist metres, c->hud_down degrees below straight
     * ahead: each eye sees it shifted by half the eye distance over that distance (in tangents), so it has depth. */
    if (hud) {
        double hw = tan((c->hud_deg < 10 ? 10 : (c->hud_deg > 150 ? 150 : c->hud_deg)) * 0.5 * RAD);
        double hh = hw * (double)d.Height / (double)d.Width;
        double cy = -tan(c->hud_down * RAD), dist = c->dist < 0.3f ? 0.3 : c->dist, disp = g_ipd_m * 0.5 / dist;
        double ew = d.Width * 0.5, eh = d.Height;
        float c0[4] = { 4.0f, 0, 0, 0 };
        float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
        if (c->gem_only && c->gem_x1 > c->gem_x0 && c->gem_y1 > c->gem_y0) { u0 = c->gem_x0; v0 = c->gem_y0; u1 = c->gem_x1; v1 = c->gem_y1; }
        for (eye = 0; eye < 2; eye++) {
            double cx = eye == 0 ? disp : -disp;
            double X0 = eye * ew + (cx - hw + g_tx_t) / (2 * g_tx_t) * ew, X1 = eye * ew + (cx + hw + g_tx_t) / (2 * g_tx_t) * ew;
            double Y0 = (g_ty_t - (cy + hh)) / (2 * g_ty_t) * eh, Y1 = (g_ty_t - (cy - hh)) / (2 * g_ty_t) * eh;
            if (g_dump || g_composed == 0)
                hl_log("wxr: HUD quad, eye %d: x %.0f..%.0f y %.0f..%.0f of %ux%u (eye picture +-%.2f x +-%.2f tangents)",
                       eye, X0, X1, Y0, Y1, d.Width, d.Height, g_tx_t, g_ty_t);
            if (!vr_draw_quad(dev, (IDirect3DBaseTexture9 *)g_hudtex, bb,
                         (float)(X0 + u0 * (X1 - X0)), (float)(Y0 + v0 * (Y1 - Y0)), (float)(X0 + u1 * (X1 - X0)), (float)(Y0 + v1 * (Y1 - Y0)),
                         u0, v0, u1, v1, g_hud_ps, c0, 1, 1)) { static int n; if (n++ < 5) hl_log("wxr: HUD quad draw failed (eye %d)", eye); }
        }
    }

    /* 4. The frame-sync pixel, last: red = the HMD_SYNC these eyes were drawn with. */
    sync_rc.left = 0; sync_rc.top = 0; sync_rc.right = 2; sync_rc.bottom = 2;
    hr = IDirect3DDevice9_ColorFill(dev, bb, &sync_rc, D3DCOLOR_ARGB(255, g_eyes_sync & 255, 0, 0));
    if (FAILED(hr)) {                              /* ColorFill refused the back buffer: a Clear on it instead */
        IDirect3DSurface9 *rt = NULL;
        IDirect3DDevice9_GetRenderTarget(dev, 0, &rt);
        IDirect3DDevice9_SetRenderTarget(dev, 0, bb);
        px.x1 = 0; px.y1 = 0; px.x2 = 2; px.y2 = 2;
        IDirect3DDevice9_Clear(dev, 1, &px, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, g_eyes_sync & 255, 0, 0), 1.0f, 0);
        if (rt) { IDirect3DDevice9_SetRenderTarget(dev, 0, rt); IDirect3DSurface9_Release(rt); }
    }
    if (g_dump) { g_dump = 0; hl_dump_surface(dev, bb, "wxr_composed.bmp"); hl_log("wxr: wrote wxr_hud_src.bmp / wxr_composed.bmp (hud %d)", hud); }
    IDirect3DSurface9_Release(bb);
    g_composed++;
    vr_prof(P_HUD, vr_ms() - t0);
}

int wxr_get_input(wxr_input_t *out)
{
    if (!g_ready || !g_have) return 0;
    EnterCriticalSection(&g_cs);
    out->lx = g_pkt.lstick[0]; out->ly = g_pkt.lstick[1];
    out->rx = g_pkt.rstick[0]; out->ry = g_pkt.rstick[1];
    out->buttons = g_pkt.buttons;
    out->seq = g_rx;
    LeaveCriticalSection(&g_cs);
    return GetTickCount() - g_last_rx < 1000;
}

/* Touch -> Xbox. WinlatorXR's button order (XrInterface.ControllerButton): 0 L_GRIP, 1 L_MENU, 2 L_THUMBSTICK_PRESS,
 * 3-6 L stick left/right/up/down, 7 L_TRIGGER, 8 L_X, 9 L_Y, 10 R_A, 11 R_B, 12 R_GRIP, 13 R_THUMBSTICK_PRESS,
 * 14-17 R stick left/right/up/down, 18 R_TRIGGER. Mapped like the Xbox pad the controller DLL (xinput_joy) is
 * tuned for: A/B/X/Y as labelled, left grip = LB (its chord button), right grip = RB, triggers = LT/RT (all or
 * nothing: WinlatorXR only sends them as buttons), stick clicks = L3/R3. The left menu button is left out:
 * WinlatorXR itself always turns it into Esc. Sticks come as 0.1 steps. */
typedef struct { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } xstate_t;
static SHORT stick(float v) { if (v > 1) v = 1; if (v < -1) v = -1; return (SHORT)(v * 32767.0f); }
int wxr_xinput(void *out)
{
    xstate_t *x = (xstate_t *)out;
    pkt_t p;
    unsigned b;
    int fresh;
    memset(x, 0, sizeof *x);
    if (!g_ready || !g_have) return 0;
    EnterCriticalSection(&g_cs); p = g_pkt; fresh = GetTickCount() - g_last_rx < 1000; x->packet = (DWORD)g_rx; LeaveCriticalSection(&g_cs);
    if (!fresh) return 0;
    b = p.buttons;
    if (b & (1u << 10)) x->buttons |= 0x1000;     /* A */
    if (b & (1u << 11)) x->buttons |= 0x2000;     /* B */
    if (b & (1u << 8))  x->buttons |= 0x4000;     /* X */
    if (b & (1u << 9))  x->buttons |= 0x8000;     /* Y */
    if (b & (1u << 0))  x->buttons |= 0x0100;     /* left grip -> LB */
    if (b & (1u << 12)) x->buttons |= 0x0200;     /* right grip -> RB */
    if (b & (1u << 2))  x->buttons |= 0x0040;     /* L3 */
    if (b & (1u << 13)) x->buttons |= 0x0080;     /* R3 */
    if (b & (1u << 7))  x->lt = 255;
    if (b & (1u << 18)) x->rt = 255;
    x->lx = stick(p.lstick[0]); x->ly = stick(p.lstick[1]);
    x->rx = stick(p.rstick[0]); x->ry = stick(p.rstick[1]);
    return 1;
}

unsigned wxr_pad_buttons(void)
{
    xstate_t x;
    return wxr_xinput(&x) ? x.buttons : 0;
}

/* Worker, every second: re-send our mode (UDP may drop it; WinlatorXR may have started after us); log every 5th. */
void wxr_status(void)
{
    static unsigned n;
    if (!g_ready) return;
    send_mode();
    if (n++ % 5) return;
    hl_log("wxr: %lu packets (%lu new headset frames, %lu unreadable), last %lu ms ago; %lu frames drawn, %lu composed; mode %d/%d, our FOV %.1f x %.1f",
           g_rx, g_syncs, g_bad, g_have ? (unsigned long)(GetTickCount() - g_last_rx) : 0ul, g_frames, g_composed,
           g_mode_vr, g_mode_3d, (double)g_send_fovx, (double)g_send_fovy);
    vr_prof_report("wxr");
}
