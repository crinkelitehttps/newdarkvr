/*
 * vr_gpu.c: part of headlook.dll. Direct3D 9 helpers for the headset backends: pixel shaders assembled at run time,
 * and a textured quad drawn with the engine's device state saved and restored around it (a D3DSBT_ALL state block,
 * plus the render target and depth-stencil, which state blocks don't cover). Render thread only.
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>
#include "vr_gpu.h"

void hl_log(const char *fmt, ...);

typedef struct dxbuf dxbuf;
typedef struct {
    HRESULT (WINAPI *QueryInterface)(dxbuf *, REFIID, void **);
    ULONG (WINAPI *AddRef)(dxbuf *);
    ULONG (WINAPI *Release)(dxbuf *);
    void *(WINAPI *GetBufferPointer)(dxbuf *);
    DWORD (WINAPI *GetBufferSize)(dxbuf *);
} dxbuf_vt;
struct dxbuf { const dxbuf_vt *vt; };
typedef HRESULT (WINAPI *assemble_fn)(const char *, UINT, const void *, void *, DWORD, dxbuf **, dxbuf **);

IDirect3DPixelShader9 *vr_assemble_ps(IDirect3DDevice9 *dev, const char *src, const char *what)
{
    static assemble_fn assemble;
    static int no_d3dx;
    dxbuf *code = NULL, *errs = NULL;
    IDirect3DPixelShader9 *ps = NULL;
    HRESULT hr;

    if (no_d3dx) return NULL;
    if (!assemble) {
        HMODULE dx = LoadLibraryA("d3dx9_43.dll");
        assemble = dx ? (assemble_fn)(void *)GetProcAddress(dx, "D3DXAssembleShader") : NULL;
        if (!assemble) { hl_log("gpu: no d3dx9_43.dll / D3DXAssembleShader: no shaders"); no_d3dx = 1; return NULL; }
    }
    hr = assemble(src, (UINT)strlen(src), NULL, NULL, 0, &code, &errs);
    if (FAILED(hr) || !code)
        hl_log("gpu: %s shader failed to assemble (%#lx): %s", what, (unsigned long)hr, errs ? (const char *)errs->vt->GetBufferPointer(errs) : "?");
    else if (FAILED(hr = IDirect3DDevice9_CreatePixelShader(dev, (const DWORD *)code->vt->GetBufferPointer(code), &ps)) || !ps) {
        hl_log("gpu: %s shader: CreatePixelShader failed (%#lx)", what, (unsigned long)hr);
        ps = NULL;
    } else hl_log("gpu: %s shader ready", what);
    if (code) code->vt->Release(code);
    if (errs) errs->vt->Release(errs);
    return ps;
}

int vr_draw_quad(IDirect3DDevice9 *dev, IDirect3DBaseTexture9 *tex, IDirect3DSurface9 *dst,
                 float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
                 IDirect3DPixelShader9 *ps, const float c0[4], int blend, int linear)
{
    IDirect3DStateBlock9 *sb = NULL;
    IDirect3DSurface9 *rt = NULL, *ds = NULL;
    struct { float x, y, z, rhw, u, v; } q[4] = {
        { x0 - 0.5f, y0 - 0.5f, 0, 1, u0, v0 }, { x1 - 0.5f, y0 - 0.5f, 0, 1, u1, v0 },
        { x0 - 0.5f, y1 - 0.5f, 0, 1, u0, v1 }, { x1 - 0.5f, y1 - 0.5f, 0, 1, u1, v1 } };
    D3DTEXTUREFILTERTYPE f = linear ? D3DTEXF_LINEAR : D3DTEXF_POINT;
    HRESULT hr;
    int in_scene;

    if (FAILED(hr = IDirect3DDevice9_CreateStateBlock(dev, D3DSBT_ALL, &sb)) || !sb) { static int n; if (n++ < 5) hl_log("gpu: CreateStateBlock failed: %#lx", (unsigned long)hr); return 0; }
    IDirect3DDevice9_GetRenderTarget(dev, 0, &rt);
    if (FAILED(IDirect3DDevice9_GetDepthStencilSurface(dev, &ds))) ds = NULL;

    IDirect3DDevice9_SetRenderTarget(dev, 0, dst);
    IDirect3DDevice9_SetDepthStencilSurface(dev, NULL);
    IDirect3DDevice9_SetVertexShader(dev, NULL);
    IDirect3DDevice9_SetPixelShader(dev, ps);
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_TEX1);
    IDirect3DDevice9_SetTexture(dev, 0, tex);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MINFILTER, f);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, f);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_SRGBTEXTURE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZWRITEENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHATESTENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_STENCILENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SCISSORTESTENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_FOGENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SRGBWRITEENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_COLORWRITEENABLE, 0xF);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, blend ? TRUE : FALSE);
    if (blend) {
        IDirect3DDevice9_SetRenderState(dev, D3DRS_BLENDOP, D3DBLENDOP_ADD);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_ONE);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    }
    if (c0) IDirect3DDevice9_SetPixelShaderConstantF(dev, 0, c0, 1);
    in_scene = SUCCEEDED(IDirect3DDevice9_BeginScene(dev));   /* fails if a scene is still open: draw inside it */
    hr = IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof q[0]);
    if (in_scene) IDirect3DDevice9_EndScene(dev);

    if (rt) { IDirect3DDevice9_SetRenderTarget(dev, 0, rt); IDirect3DSurface9_Release(rt); }
    IDirect3DDevice9_SetDepthStencilSurface(dev, ds);
    if (ds) IDirect3DSurface9_Release(ds);
    IDirect3DStateBlock9_Apply(sb);               /* after SetRenderTarget, which resets the viewport */
    IDirect3DStateBlock9_Release(sb);
    if (FAILED(hr)) { static int n; if (n++ < 5) hl_log("gpu: quad draw failed: %#lx", (unsigned long)hr); return 0; }
    return 1;
}

static const GUID IID_IDirect3DTexture9_ = { 0x85c31227, 0x3de5, 0x4f00, { 0x9b, 0x3a, 0xf1, 0x1a, 0xc3, 0x8c, 0x18, 0xb5 } };
static IDirect3DTexture9 *g_copy[2];              /* kept copies of sources that aren't textures (default pool) */
static D3DSURFACE_DESC g_copy_desc[2];

IDirect3DTexture9 *vr_texture_of(IDirect3DDevice9 *dev, IDirect3DSurface9 *src, int slot)
{
    IDirect3DTexture9 *tex = NULL;
    IDirect3DSurface9 *lvl = NULL;
    D3DSURFACE_DESC sd;
    HRESULT hr;

    if (SUCCEEDED(IDirect3DSurface9_GetContainer(src, &IID_IDirect3DTexture9_, (void **)&tex)) && tex) return tex;
    slot &= 1;
    IDirect3DSurface9_GetDesc(src, &sd);
    if (g_copy[slot] && (g_copy_desc[slot].Width != sd.Width || g_copy_desc[slot].Height != sd.Height || g_copy_desc[slot].Format != sd.Format)) {
        IDirect3DTexture9_Release(g_copy[slot]); g_copy[slot] = NULL;
    }
    if (!g_copy[slot]) {
        hr = IDirect3DDevice9_CreateTexture(dev, sd.Width, sd.Height, 1, D3DUSAGE_RENDERTARGET, sd.Format, D3DPOOL_DEFAULT, &g_copy[slot], NULL);
        if (FAILED(hr) || !g_copy[slot]) { g_copy[slot] = NULL; hl_log("gpu: copy texture %ux%u fmt %d failed: %#lx", sd.Width, sd.Height, (int)sd.Format, (unsigned long)hr); return NULL; }
        g_copy_desc[slot] = sd;
        hl_log("gpu: sampling a copy of a %ux%u surface (it isn't a texture)", sd.Width, sd.Height);
    }
    if (FAILED(IDirect3DTexture9_GetSurfaceLevel(g_copy[slot], 0, &lvl)) || !lvl) return NULL;
    hr = IDirect3DDevice9_StretchRect(dev, src, NULL, lvl, NULL, D3DTEXF_NONE);
    IDirect3DSurface9_Release(lvl);
    if (FAILED(hr)) { static int n; if (n++ < 5) hl_log("gpu: copying a surface for sampling failed: %#lx", (unsigned long)hr); return NULL; }
    IDirect3DTexture9_AddRef(g_copy[slot]);
    return g_copy[slot];
}

/* The curve: out.rgb = black + (1 - black) * saturate(in.rgb)^(1/gamma), out.a = 1; c0 = (1/gamma, 1 - black, black, 0). */
static const char g_curve_src[] =
    "ps_2_0\n"
    "def c1, 0, 0, 0, 1\n"
    "dcl t0.xy\n"
    "dcl_2d s0\n"
    "texld r0, t0, s0\n"
    "mov_sat r0, r0\n"
    "pow r1.x, r0.x, c0.x\n"
    "pow r1.y, r0.y, c0.x\n"
    "pow r1.z, r0.z, c0.x\n"
    "mad r1.xyz, r1, c0.y, c0.z\n"
    "mov r1.w, c1.w\n"
    "mov oC0, r1\n";
static IDirect3DPixelShader9 *g_curve;
static IDirect3DDevice9 *g_curve_dev;
static int g_curve_failed;

int vr_curve_available(IDirect3DDevice9 *dev)
{
    if (g_curve && g_curve_dev == dev) return 1;
    if (g_curve_failed) return 0;
    g_curve = vr_assemble_ps(dev, g_curve_src, "brightness curve");   /* a new device: the old one's shader went with it */
    g_curve_dev = dev;
    if (!g_curve) g_curve_failed = 1;
    return g_curve != NULL;
}

int vr_curve_copy(IDirect3DDevice9 *dev, IDirect3DSurface9 *src, IDirect3DSurface9 *dst, float gamma, float black)
{
    IDirect3DTexture9 *tex;
    D3DSURFACE_DESC dd;
    float c0[4];
    int ok;

    if (!vr_curve_available(dev)) return 0;
    if (gamma < 0.2f) gamma = 0.2f;
    if (gamma > 5.0f) gamma = 5.0f;
    if (black < 0.0f) black = 0.0f;
    if (black > 0.5f) black = 0.5f;
    c0[0] = 1.0f / gamma; c0[1] = 1.0f - black; c0[2] = black; c0[3] = 0.0f;
    if (!(tex = vr_texture_of(dev, src, 0))) return 0;
    IDirect3DSurface9_GetDesc(dst, &dd);
    ok = vr_draw_quad(dev, (IDirect3DBaseTexture9 *)tex, dst, 0, 0, (float)dd.Width, (float)dd.Height, 0, 0, 1, 1, g_curve, c0, 0, 0);
    IDirect3DTexture9_Release(tex);
    if (!ok) { hl_log("gpu: curve pass failed: back to the plain copy"); g_curve_failed = 1; }
    return ok;
}

void vr_gpu_reset(void)
{
    int i;
    for (i = 0; i < 2; i++) if (g_copy[i]) { IDirect3DTexture9_Release(g_copy[i]); g_copy[i] = NULL; }
}
