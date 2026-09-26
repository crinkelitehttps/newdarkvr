/* vr_openvr.c: OpenVR direct submit for headlook.dll (stereo=openvr). See the comment at the top of vr_openvr.c. */
#ifndef VR_OPENVR_H
#define VR_OPENVR_H
#include <d3d9.h>

int  vr_init(const char *dll_dir);          /* any thread; 1 = connected to SteamVR as a scene app */
int  vr_ready(void);
/* render thread, once per frame; 1 = pose valid. With pipelining it first submits the previous frame's eyes. */
int  vr_begin_frame(IDirect3DDevice9 *dev, double *yaw_right_deg, double *pitch_up_deg);
/* after each pass; 0 = left, 1 = right. gpu_curve: apply the brightness curve (and opaque alpha) in a GPU pass. */
void vr_capture_eye(IDirect3DDevice9 *dev, IDirect3DSurface9 *src, int eye, int convert, int gpu_curve);
/* after both: read back and submit now, or (pipeline) at the next vr_begin_frame, when the GPU is done with them */
void vr_end_frame(IDirect3DDevice9 *dev, float scale, int horplus, int hdr_linear, int pipeline);
void vr_device_reset(void);                 /* render thread, before the device's Reset: drop our default-pool surfaces */

/* Per-frame timing, logged as averages with the status line. */
enum { P_FRAME, P_WAIT, P_DRAW_L, P_DRAW_R, P_CAPTURE, P_READBACK, P_ALPHA, P_UPLOAD, P_SUBMIT, P_HUD, P_PRESENT, P_N };
double vr_ms(void);
void vr_prof(int k, double ms);
void vr_prof_tick(void);                    /* once per 3D frame */
void vr_prof_report(const char *tag);       /* worker: log the timing line and reset */
float vr_auto_scale(int horplus);           /* engine view scale that fills the headset's view */
void vr_set_picture(float gamma, float black);   /* eye image brightness curve (render thread, cheap if unchanged) */
void vr_idle_frame(IDirect3DDevice9 *dev);                 /* render thread, at Present when no 3D frame was drawn: keep SteamVR fed */
typedef struct {
    float hud_deg, hud_down;                /* in-game HUD panel: width and tilt below straight ahead (degrees), head-locked */
    float menu_deg, menu_down;              /* menus/books: the same, fixed in the room where you were looking */
    float dist;                             /* metres */
    int   every;                            /* in-game: refresh every Nth frame */
    int   gem_only;                         /* in-game, HUD toggled off: show just the light gem's corner of the panel */
    float gem_x0, gem_y0, gem_x1, gem_y1;   /* that part of the screen, as fractions (0..1, y down) */
    int   pipeline;                         /* copy the back buffer on the GPU now, read it back at the next Present */
} vr_hud_cfg_t;
void vr_hud_present(IDirect3DDevice9 *dev, int in_game, const vr_hud_cfg_t *c);   /* render thread, at Present */
void vr_hud_request_dump(void);
void vr_hud_hide(void);
unsigned vr_pad_buttons(void);              /* XInput buttons held on any pad (any thread) */
void vr_status(void);                       /* worker thread: counters, and a warning if the render thread is stuck */

#endif
