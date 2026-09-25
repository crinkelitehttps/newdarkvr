/*
 * d3d9.dll stand-in for Thief II (NewDark): loads headlook.dll without patching Thief2.exe.
 *
 * The game LoadLibrary()s "d3d9.dll" by bare name, and d3d9 is not a KnownDLL, so a d3d9.dll in the game folder
 * is loaded instead of Windows' own. This one forwards every export to the real d3d9.dll (from the system
 * directory), or to d3d9_chain.dll in the same folder if present (for another d3d9 wrapper, e.g. ReShade renamed
 * to d3d9_chain.dll). When the game calls Direct3DCreate9(Ex), it loads headlook.dll from this folder and hands
 * it the new IDirect3D9 through headlook_d3d_created(), synchronously, before the game can create its device.
 *
 * Log: d3d9proxy.log next to this DLL (a few lines per run).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static char g_dir[MAX_PATH];
static FILE *g_log;
static HMODULE g_real;
static volatile LONG g_resolved;

static void plog(const char *fmt, ...)
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

/* Every export of Windows' d3d9.dll, in the order of the thunk table below. */
#define N_EXPORTS 17
static const char *const g_names[N_EXPORTS] = {
    "Direct3DCreate9", "Direct3DCreate9Ex", "Direct3DCreate9On12", "Direct3DCreate9On12Ex",
    "Direct3DShaderValidatorCreate9", "Direct3D9EnableMaximizedWindowedModeShim",
    "D3DPERF_BeginEvent", "D3DPERF_EndEvent", "D3DPERF_GetStatus", "D3DPERF_QueryRepeatFrame",
    "D3DPERF_SetMarker", "D3DPERF_SetOptions", "D3DPERF_SetRegion",
    "DebugSetLevel", "DebugSetMute", "PSGPError", "PSGPSampleTexture",
};
void *g_real_fn[N_EXPORTS];                       /* read by the asm thunks */

/* Load the real d3d9 (or the chained wrapper) and look up every export. Called on first use of any export,
 * never from DllMain (loader lock). */
void __stdcall proxy_resolve(void)
{
    char path[MAX_PATH + 32];
    int i;
    if (InterlockedCompareExchange(&g_resolved, 1, 0) != 0) {
        while (g_resolved != 2) Sleep(0);         /* another thread is resolving */
        return;
    }
    snprintf(path, sizeof path, "%sd3d9_chain.dll", g_dir);
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
        g_real = LoadLibraryA(path);
        plog("chaining to %s: %s", path, g_real ? "loaded" : "FAILED to load");
    }
    if (!g_real) {
        UINT n = GetSystemDirectoryA(path, MAX_PATH);   /* SysWOW64 for this 32-bit process on 64-bit Windows */
        snprintf(path + n, sizeof path - n, "\\d3d9.dll");
        g_real = LoadLibraryA(path);
        plog("real d3d9: %s: %s", path, g_real ? "loaded" : "FAILED to load");
    }
    for (i = 0; i < N_EXPORTS; i++) g_real_fn[i] = g_real ? (void *)GetProcAddress(g_real, g_names[i]) : NULL;
    g_resolved = 2;
}

/* Generic forwarding thunks: jump straight to the real function (any calling convention, any arguments).
 * An export the real DLL lacks returns 0 (only the Direct3DCreate9 pair is ever expected to be used here). */
#define THUNK(NAME, IDX)                                                              \
    __asm__(".text\n.globl _th_" #NAME "\n_th_" #NAME ":\n"                           \
            "  movl _g_real_fn+" #IDX "*4, %eax\n"                                     \
            "  testl %eax, %eax\n  jnz 1f\n"                                           \
            "  pushal\n  call _proxy_resolve@0\n  popal\n"                             \
            "  movl _g_real_fn+" #IDX "*4, %eax\n"                                     \
            "  testl %eax, %eax\n  jz 2f\n"                                            \
            "1: jmp *%eax\n"                                                           \
            "2: xorl %eax, %eax\n  ret\n");
THUNK(Direct3DCreate9On12, 2)
THUNK(Direct3DCreate9On12Ex, 3)
THUNK(Direct3DShaderValidatorCreate9, 4)
THUNK(Direct3D9EnableMaximizedWindowedModeShim, 5)
THUNK(D3DPERF_BeginEvent, 6)
THUNK(D3DPERF_EndEvent, 7)
THUNK(D3DPERF_GetStatus, 8)
THUNK(D3DPERF_QueryRepeatFrame, 9)
THUNK(D3DPERF_SetMarker, 10)
THUNK(D3DPERF_SetOptions, 11)
THUNK(D3DPERF_SetRegion, 12)
THUNK(DebugSetLevel, 13)
THUNK(DebugSetMute, 14)
THUNK(PSGPError, 15)
THUNK(PSGPSampleTexture, 16)

/* Load headlook.dll from this folder (once) and give it the IDirect3D9 the game just made. */
static void notify_headlook(void *d3d)
{
    static HMODULE hl;
    static void (__cdecl *created)(void *);
    static int tried;
    if (!tried) {
        char path[MAX_PATH + 32];
        tried = 1;
        snprintf(path, sizeof path, "%sheadlook.dll", g_dir);
        hl = LoadLibraryA(path);
        created = hl ? (void (__cdecl *)(void *))(void *)GetProcAddress(hl, "headlook_d3d_created") : NULL;
        plog("headlook.dll: %s%s", hl ? "loaded" : "not loaded (missing?)", hl && !created ? " but it has no headlook_d3d_created (old build?)" : "");
    }
    if (created && d3d) created(d3d);
}

typedef IDirect3D9 *(WINAPI *create9_t)(UINT);
typedef HRESULT (WINAPI *create9ex_t)(UINT, IDirect3D9Ex **);

IDirect3D9 *WINAPI proxy_Direct3DCreate9(UINT sdk)
{
    IDirect3D9 *d3d;
    if (g_resolved != 2) proxy_resolve();
    if (!g_real_fn[0]) { plog("Direct3DCreate9: no real function"); return NULL; }
    d3d = ((create9_t)g_real_fn[0])(sdk);
    plog("Direct3DCreate9(%u) -> %p", sdk, (void *)d3d);
    notify_headlook(d3d);
    return d3d;
}

HRESULT WINAPI proxy_Direct3DCreate9Ex(UINT sdk, IDirect3D9Ex **out)
{
    HRESULT hr;
    if (g_resolved != 2) proxy_resolve();
    if (!g_real_fn[1]) { plog("Direct3DCreate9Ex: no real function"); return E_NOTIMPL; }
    hr = ((create9ex_t)g_real_fn[1])(sdk, out);
    plog("Direct3DCreate9Ex(%u) -> %#lx", sdk, (unsigned long)hr);
    if (SUCCEEDED(hr) && out) notify_headlook(*out);
    return hr;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        char lp[MAX_PATH + 32], *slash;
        DisableThreadLibraryCalls(inst);
        GetModuleFileNameA(inst, g_dir, sizeof g_dir);
        slash = strrchr(g_dir, '\\');
        if (slash) slash[1] = 0; else g_dir[0] = 0;
        snprintf(lp, sizeof lp, "%sd3d9proxy.log", g_dir);
        g_log = fopen(lp, "w");
        plog("d3d9.dll stand-in loaded from %s", g_dir);
    }
    return TRUE;
}
