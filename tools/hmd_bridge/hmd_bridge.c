/*
 * hmd_bridge: reads the head pose of a VR headset through OpenVR (SteamVR) and sends it as OpenTrack
 * "UDP over network" packets (6 little-endian doubles: x, y, z in cm; yaw, pitch, roll in degrees) to
 * headlook.dll, which is already listening on UDP 4242. Windows only; 64-bit; needs openvr_api.dll
 * next to the .exe (tools/hmd_bridge/build.sh fetches it).
 *
 * Output uses OpenTrack's sign convention, which headlook.ini's default yaw_sign/pitch_sign (-1, -1)
 * expect: yaw POSITIVE = head turned right, pitch POSITIVE = looking up, roll POSITIVE = head tilted to
 * the right (roll is off in headlook by default and its sign has never been checked with a real head).
 * Position is sent relative to the last recentre (x right, y up, z back, cm); headlook receives but
 * does not use it yet.
 *
 * Recentre: the first valid pose is taken as "straight ahead". Press Pause/Break or Scroll Lock (works
 * from any window, so also while the game is fullscreen; Thief binds F9 to quick-load, so not F9) or C/Space
 * in this window to recentre; Q in this window quits.
 *
 * OpenVR frame (from its docs): right-handed, +Y up, the headset looks along -Z. The pose matrix's
 * columns are the device's right, up and back axes expressed in tracking space.
 *
 * "--selftest" checks the maths against known rotations and sends one fixed packet, no headset needed.
 */
#define OPENVR_API_NODLL
#include <winsock2.h>
#include <windows.h>
#include <conio.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openvr_capi.h>

#define DEG (180.0 / 3.14159265358979323846)
#define RAD (3.14159265358979323846 / 180.0)

/* hmd_bridge.log next to the exe (or in the current directory): connection state, centring, a status line
 * every 5 s. Lets a helper read what happened without copying console text. */
static FILE *g_log;
static void blog(const char *fmt, ...)
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

typedef struct { double v[3]; } vec3;

static vec3 vec(double x, double y, double z) { vec3 r = {{ x, y, z }}; return r; }
static double dot(vec3 a, vec3 b) { return a.v[0] * b.v[0] + a.v[1] * b.v[1] + a.v[2] * b.v[2]; }
static vec3 cross(vec3 a, vec3 b)
{
    return vec(a.v[1] * b.v[2] - a.v[2] * b.v[1], a.v[2] * b.v[0] - a.v[0] * b.v[2], a.v[0] * b.v[1] - a.v[1] * b.v[0]);
}
static vec3 scale(vec3 a, double s) { return vec(a.v[0] * s, a.v[1] * s, a.v[2] * s); }
static vec3 add(vec3 a, vec3 b) { return vec(a.v[0] + b.v[0], a.v[1] + b.v[1], a.v[2] + b.v[2]); }
static double len(vec3 a) { return sqrt(dot(a, a)); }
static double wrap180(double d) { while (d > 180.0) d -= 360.0; while (d < -180.0) d += 360.0; return d; }

/* Absolute head angles (degrees) from a pose matrix; yaw is relative to tracking space's -Z. */
static void matrix_to_angles(const float m[3][4], double *yaw, double *pitch, double *roll)
{
    vec3 right = vec(m[0][0], m[1][0], m[2][0]);
    vec3 fwd = vec(-m[0][2], -m[1][2], -m[2][2]);
    vec3 r0, u0;
    double c;

    *yaw = atan2(fwd.v[0], -fwd.v[2]) * DEG;                  /* toward +X (right) is positive */
    c = fwd.v[1]; if (c > 1) c = 1; if (c < -1) c = -1;
    *pitch = asin(c) * DEG;                                   /* up is positive */
    r0 = cross(fwd, vec(0, 1, 0));                            /* the level right vector for this heading */
    if (len(r0) < 1e-4) { *roll = 0; return; }                /* looking straight up or down: roll undefined */
    r0 = scale(r0, 1.0 / len(r0));
    u0 = cross(r0, fwd);
    *roll = atan2(-dot(right, u0), dot(right, r0)) * DEG;     /* right ear down is positive */
}

/* The inverse, used by the self test to build matrices from angles by an independent route. */
static void angles_to_matrix(double yaw, double pitch, double roll, float m[3][4])
{
    double y = yaw * RAD, p = pitch * RAD, r = roll * RAD;
    vec3 fwd = vec(sin(y) * cos(p), sin(p), -cos(y) * cos(p));
    vec3 r0 = cross(fwd, vec(0, 1, 0)), u0, right, up;
    r0 = scale(r0, 1.0 / len(r0));
    u0 = cross(r0, fwd);
    right = add(scale(r0, cos(r)), scale(u0, -sin(r)));
    up = add(scale(r0, sin(r)), scale(u0, cos(r)));
    memset(m, 0, sizeof(float) * 12);
    for (int i = 0; i < 3; i++) { m[i][0] = (float)right.v[i]; m[i][1] = (float)up.v[i]; m[i][2] = (float)-fwd.v[i]; }
}

static SOCKET g_sock = INVALID_SOCKET;
static SOCKADDR_IN g_dest;

static void send_pose(double x, double y, double z, double yaw, double pitch, double roll)
{
    double v[6] = { x, y, z, yaw, pitch, roll };            /* OpenTrack order; little-endian on x86 */
    sendto(g_sock, (const char *)v, sizeof v, 0, (const SOCKADDR *)&g_dest, sizeof g_dest);
}

static int selftest(void)
{
    struct { double y, p, r; } cases[] = {
        { 0, 0, 0 }, { 30, 0, 0 }, { -45, 0, 0 }, { 170, 0, 0 }, { 0, 20, 0 }, { 0, -35, 0 },
        { 0, 0, 15 }, { 60, 25, -10 }, { -120, -40, 20 },
    };
    int fails = 0;
    float m[3][4];
    double y, p, r, phi;

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        angles_to_matrix(cases[i].y, cases[i].p, cases[i].r, m);
        matrix_to_angles(m, &y, &p, &r);
        int ok = fabs(wrap180(y - cases[i].y)) < 0.01 && fabs(p - cases[i].p) < 0.01 && fabs(r - cases[i].r) < 0.01;
        printf("  round trip yaw %6.1f pitch %6.1f roll %6.1f -> %7.2f %7.2f %7.2f  %s\n",
               cases[i].y, cases[i].p, cases[i].r, y, p, r, ok ? "ok" : "FAIL");
        fails += !ok;
    }
    /* Independent check with a textbook rotation: turning right by 30 degrees is a rotation of -30 degrees
     * about +Y in a right-handed Y-up frame; the matrix is [[c 0 s][0 1 0][-s 0 c]] with phi = -30. */
    phi = -30.0 * RAD;
    memset(m, 0, sizeof m);
    m[0][0] = (float)cos(phi); m[0][2] = (float)sin(phi); m[1][1] = 1; m[2][0] = (float)-sin(phi); m[2][2] = (float)cos(phi);
    matrix_to_angles(m, &y, &p, &r);
    printf("  textbook rotation about +Y by -30 deg -> yaw %.2f pitch %.2f roll %.2f (expect yaw +30: turned right)  %s\n",
           y, p, r, (fabs(y - 30.0) < 0.01 && fabs(p) < 0.01 && fabs(r) < 0.01) ? "ok" : "FAIL");
    fails += !(fabs(y - 30.0) < 0.01 && fabs(p) < 0.01 && fabs(r) < 0.01);
    /* Looking up: the forward axis (-Z column) gains +Y. */
    memset(m, 0, sizeof m);
    phi = 20.0 * RAD;                                        /* rotation about +X by +20 pitches the nose up */
    m[0][0] = 1; m[1][1] = (float)cos(phi); m[1][2] = (float)-sin(phi); m[2][1] = (float)sin(phi); m[2][2] = (float)cos(phi);
    matrix_to_angles(m, &y, &p, &r);
    printf("  textbook rotation about +X by +20 deg -> yaw %.2f pitch %.2f roll %.2f (expect pitch +20: looking up)  %s\n",
           y, p, r, (fabs(p - 20.0) < 0.01 && fabs(y) < 0.01 && fabs(r) < 0.01) ? "ok" : "FAIL");
    fails += !(fabs(p - 20.0) < 0.01 && fabs(y) < 0.01 && fabs(r) < 0.01);
    return fails;
}

typedef intptr_t (*init_fn)(EVRInitError *, EVRApplicationType);
typedef void (*shutdown_fn)(void);
typedef intptr_t (*iface_fn)(const char *, EVRInitError *);
typedef const char *(*errstr_fn)(EVRInitError);

int main(int argc, char **argv)
{
    const char *host = "127.0.0.1";
    int port = 4242, launch = 0, do_selftest = 0;
    double predict_ms = 15.0;
    WSADATA wsa;
    HMODULE dll;
    init_fn p_init;
    shutdown_fn p_shutdown;
    iface_fn p_iface;
    errstr_fn p_errstr;
    struct VR_IVRSystem_FnTable *sys = NULL;
    EVRInitError err = EVRInitError_VRInitError_None, last_err = (EVRInitError)-1;
    int have_ref = 0, recentre = 1, hotkey_was_down = 0;
    double ref_yaw = 0;
    vec3 ref_pos = vec(0, 0, 0);
    unsigned long sent = 0;
    DWORD next_status = 0, next_blog = 0;
    double yaw = 0, pitch = 0, roll = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--host") && i + 1 < argc) host = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--predict-ms") && i + 1 < argc) predict_ms = atof(argv[++i]);
        else if (!strcmp(argv[i], "--launch")) launch = 1;
        else if (!strcmp(argv[i], "--selftest")) do_selftest = 1;
        else {
            printf("hmd_bridge: head pose from OpenVR -> OpenTrack UDP packets for headlook.dll\n"
                   "  --host H        destination (default 127.0.0.1)\n"
                   "  --port N        destination UDP port (default 4242)\n"
                   "  --predict-ms M  predict the pose M ms ahead to hide latency (default 15, 0 = off)\n"
                   "  --launch        start SteamVR if it is not running (default: wait for it)\n"
                   "  --selftest      check the maths and send one packet, no headset needed\n"
                   "keys: Pause/Break or Scroll Lock (global) or C/Space (this window) = recentre, Q = quit\n");
            return 2;
        }
    }
    g_log = fopen("hmd_bridge.log", "w");
    blog("hmd_bridge starting: host %s port %d predict %.0f ms launch %d selftest %d", host, port, predict_ms, launch, do_selftest);
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { fprintf(stderr, "WSAStartup failed\n"); blog("WSAStartup failed"); return 1; }
    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&g_dest, 0, sizeof g_dest);
    g_dest.sin_family = AF_INET;
    g_dest.sin_port = htons((u_short)port);
    g_dest.sin_addr.s_addr = inet_addr(host);
    if (g_sock == INVALID_SOCKET || g_dest.sin_addr.s_addr == INADDR_NONE) { fprintf(stderr, "bad socket or host '%s'\n", host); return 1; }

    if (do_selftest) {
        int fails = selftest();
        send_pose(1.5, -2.5, 3.5, 30.0, 10.0, 5.0);
        printf("sent one fixed packet to %s:%d (x 1.5 y -2.5 z 3.5 yaw 30 pitch 10 roll 5)\n%s\n", host, port, fails ? "SELFTEST FAILED" : "selftest ok");
        return fails ? 1 : 0;
    }

    dll = LoadLibraryA("openvr_api.dll");
    if (!dll) { fprintf(stderr, "cannot load openvr_api.dll (it must be next to hmd_bridge.exe; run build.sh to fetch it)\n"); blog("cannot load openvr_api.dll (error %lu)", GetLastError()); return 1; }
    p_init = (init_fn)(void *)GetProcAddress(dll, "VR_InitInternal");
    p_shutdown = (shutdown_fn)(void *)GetProcAddress(dll, "VR_ShutdownInternal");
    p_iface = (iface_fn)(void *)GetProcAddress(dll, "VR_GetGenericInterface");
    p_errstr = (errstr_fn)(void *)GetProcAddress(dll, "VR_GetVRInitErrorAsEnglishDescription");
    if (!p_init || !p_shutdown || !p_iface || !p_errstr) { fprintf(stderr, "openvr_api.dll is missing expected exports\n"); return 1; }
    timeBeginPeriod(1);

    printf("hmd_bridge -> %s:%d. %s\n", host, port, launch ? "Starting SteamVR if needed." : "Waiting for SteamVR to be running.");
    for (;;) {                                               /* connect to the runtime, retrying until it is there */
        err = EVRInitError_VRInitError_None;
        p_init(&err, launch ? EVRApplicationType_VRApplication_Utility : EVRApplicationType_VRApplication_Background);
        if (err == EVRInitError_VRInitError_None) {
            sys = (struct VR_IVRSystem_FnTable *)p_iface("FnTable:" "IVRSystem_026", &err);
            if (sys && err == EVRInitError_VRInitError_None) break;
            p_shutdown();
        }
        if (err != last_err) { printf("  OpenVR: %s (retrying every 2 s)\n", p_errstr(err)); blog("OpenVR init: error %d: %s", (int)err, p_errstr(err)); last_err = err; }
        if (_kbhit() && (_getch() | 0x20) == 'q') return 0;
        Sleep(2000);
    }
    blog("connected to OpenVR (IVRSystem_026 function table)");
    printf("Connected to OpenVR. Look straight ahead: the first pose is taken as the centre.\n"
           "Recentre with Pause/Break or Scroll Lock (from any window) or C/Space here; Q here quits.\n");

    for (;;) {
        TrackedDevicePose_t pose;
        memset(&pose, 0, sizeof pose);
        sys->GetDeviceToAbsoluteTrackingPose(ETrackingUniverseOrigin_TrackingUniverseSeated, (float)(predict_ms / 1000.0), &pose, 1);

        if (pose.bPoseIsValid && pose.eTrackingResult == ETrackingResult_TrackingResult_Running_OK) {
            double abs_yaw;
            vec3 pos = vec(pose.mDeviceToAbsoluteTracking.m[0][3], pose.mDeviceToAbsoluteTracking.m[1][3], pose.mDeviceToAbsoluteTracking.m[2][3]);
            matrix_to_angles(pose.mDeviceToAbsoluteTracking.m, &abs_yaw, &pitch, &roll);
            if (recentre) { ref_yaw = abs_yaw; ref_pos = pos; have_ref = 1; recentre = 0; printf("  centred (yaw %.1f in tracking space)\n", abs_yaw); blog("centred at tracking-space yaw %.1f", abs_yaw); }
            yaw = wrap180(abs_yaw - ref_yaw);
            if (have_ref) {
                vec3 d = vec(pos.v[0] - ref_pos.v[0], pos.v[1] - ref_pos.v[1], pos.v[2] - ref_pos.v[2]);
                vec3 rf = vec(sin(ref_yaw * RAD), 0, -cos(ref_yaw * RAD));       /* reference forward */
                vec3 rr = vec(cos(ref_yaw * RAD), 0, sin(ref_yaw * RAD));        /* reference right */
                send_pose(dot(d, rr) * 100.0, d.v[1] * 100.0, -dot(d, rf) * 100.0, yaw, pitch, roll);
                sent++;
            }
        }
        if (_kbhit()) {
            int k = _getch() | 0x20;
            if (k == 'q') break;
            if (k == 'c' || k == ' ') recentre = 1;
        }
        {
            int down = ((GetAsyncKeyState(VK_PAUSE) | GetAsyncKeyState(VK_SCROLL)) & 0x8000) != 0;
            if (down && !hotkey_was_down) recentre = 1;       /* on the key's press, not while it is held */
            hotkey_was_down = down;
        }
        if ((int)(GetTickCount() - next_status) >= 0) {
            printf("\r  yaw %6.1f  pitch %6.1f  roll %6.1f   sent %lu   %s          ", yaw, pitch, roll, sent,
                   pose.bPoseIsValid ? "tracking" : "NO POSE");
            fflush(stdout);
            next_status = GetTickCount() + 500;
        }
        if ((int)(GetTickCount() - next_blog) >= 0) {
            blog("yaw %.1f pitch %.1f roll %.1f sent %lu pose %s", yaw, pitch, roll, sent, pose.bPoseIsValid ? "valid" : "INVALID");
            next_blog = GetTickCount() + 5000;
        }
        Sleep(4);                                            /* ~250 Hz; the headset itself updates at its refresh rate */
    }
    printf("\nquitting\n");
    blog("quitting; %lu packets sent", sent);
    p_shutdown();
    return 0;
}
