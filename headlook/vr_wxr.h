/* vr_wxr.c: WinlatorXR XrAPI backend for headlook.dll (stereo=wxr). See the comment at the top of vr_wxr.c. */
#ifndef VR_WXR_H
#define VR_WXR_H
#include <d3d9.h>

int  wxr_init(const char *dll_dir);        /* any thread: writes the API version file, starts the UDP listener */
int  wxr_have_data(void);                  /* a pose packet arrived in the last second */
/* Render thread, once per 3D frame: waits (briefly) for a new headset frame, then gives its head pose in degrees
 * (yaw + = right, pitch + = up, roll + = right ear down) and the eye distance in metres. 1 = valid. */
int  wxr_begin_frame(double *yaw_right, double *pitch_up, double *roll_right, double *ipd_m);
float wxr_view_scale(int horplus, UINT w, UINT h, float fov_deg);   /* engine view scale covering fov_deg (0 = the headset's) */
void wxr_capture_eye(IDirect3DDevice9 *dev, IDirect3DSurface9 *src, int eye, float gamma, float black);   /* after each pass */
void wxr_end_frame(float scale, int horplus);         /* after both */

typedef struct {
    int   hud;                             /* draw the HUD (the back buffer's overlays and text) into both eyes */
    int   gem_only;                        /* ... just the light gem's part of it */
    float gem_x0, gem_y0, gem_x1, gem_y1;
    float hud_deg, hud_down, dist;         /* HUD width and tilt (degrees), and the distance it appears at (m) */
    float gamma, black;                    /* eye picture brightness curve */
} wxr_present_cfg_t;
/* Render thread, at Present. in_game: a 3D frame was drawn lately. Composes the side-by-side frame (eyes + HUD +
 * frame-sync pixel) into the back buffer, or switches the headset to its flat screen for menus. */
void wxr_present(IDirect3DDevice9 *dev, int in_game, const wxr_present_cfg_t *c);
void wxr_device_reset(void);               /* render thread, before the device's Reset */
void wxr_recentre(void);
void wxr_request_dump(void);              /* the next composed frame: write its HUD source and result as BMPs */
void wxr_status(void);                     /* worker thread, every few seconds; also re-sends the mode */

/* Touch controllers, for the controller DLL (Phase 2): the last packet's sticks and buttons. */
typedef struct {
    float lx, ly, rx, ry;                  /* thumbsticks, -1..1 */
    unsigned buttons;                      /* bit i = WinlatorXR button i (L_GRIP, L_MENU, ... R_TRIGGER; see vr_wxr.c) */
    unsigned long seq;                     /* packets received so far */
} wxr_input_t;
int  wxr_get_input(wxr_input_t *out);
/* The Touch controllers as an Xbox pad (XINPUT_STATE layout: DWORD packet, WORD buttons, BYTE LT, BYTE RT, SHORT
 * LX, LY, RX, RY). Returns 1 with fresh data, 0 without (out is then neutral). See vr_wxr.c for the mapping. */
int  wxr_xinput(void *xinput_state);
unsigned wxr_pad_buttons(void);            /* just the buttons of that (0 when not in WinlatorXR mode) */

#endif
