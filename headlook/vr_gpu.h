/* vr_gpu.c: small Direct3D 9 helpers shared by the headset backends (vr_openvr.c, vr_wxr.c). Render thread only. */
#ifndef VR_GPU_H
#define VR_GPU_H
#include <d3d9.h>

/* A ps_2_0 shader from assembly text, via D3DXAssembleShader in d3dx9_43.dll (which the game imports). NULL on failure
 * (logged under `what`). */
IDirect3DPixelShader9 *vr_assemble_ps(IDirect3DDevice9 *dev, const char *src, const char *what);

/* Draw tex's [u0,v0]-[u1,v1] into dst's pixel rectangle [x0,y0]-[x1,y1] with pixel shader ps (c0 = its constant 0),
 * saving and restoring all device state around it. blend: 0 = replace, 1 = premultiplied alpha (ONE, INVSRCALPHA). */
int vr_draw_quad(IDirect3DDevice9 *dev, IDirect3DBaseTexture9 *tex, IDirect3DSurface9 *dst,
                 float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
                 IDirect3DPixelShader9 *ps, const float c0[4], int blend, int linear);

/* src through the brightness curve (out = black + (1 - black) * saturate(in)^(1/gamma), alpha 1) into dst, same size.
 * 0 if the shader or the draw is unavailable (the caller falls back to StretchRect). */
int vr_curve_copy(IDirect3DDevice9 *dev, IDirect3DSurface9 *src, IDirect3DSurface9 *dst, float gamma, float black);
int vr_curve_available(IDirect3DDevice9 *dev);

/* A texture holding a copy of src (src itself when it is already a texture's level 0), AddRef'd; NULL on failure.
 * slot: which kept copy to use (0 or 1), so two sources can be held at once. */
IDirect3DTexture9 *vr_texture_of(IDirect3DDevice9 *dev, IDirect3DSurface9 *src, int slot);

void vr_gpu_reset(void);                   /* before the device's Reset: release the kept default-pool copies */

#endif
