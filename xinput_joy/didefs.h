/*
 * Hand-rolled legacy (pre-DX8) DirectInput ABI: just enough of IDirectInputA and
 * IDirectInputDevice2A for xinput_joy.c to implement a synthetic joystick device backed by XInput.
 *
 * Deliberately NOT #include <dinput.h>: this way the exact byte layout we memcpy into the engine's
 * buffer (xj_djoystate_t) is fully under our control and trivial to widen if Phase-0 discovery (see
 * DEVLOG) shows the engine wants a different struct. Every slot order, struct layout, GUID value and
 * error code below was cross-checked against mingw's own /usr/i686-w64-mingw32/include/dinput.h (the
 * real Microsoft-published ABI) this session -- not guessed from memory.
 */
#ifndef XINPUT_JOY_DIDEFS_H
#define XINPUT_JOY_DIDEFS_H

#include <windows.h>
#include <initguid.h>

/* WIN32_LEAN_AND_MEAN excludes <unknwn.h>, so IUnknown isn't declared; we never call through it (no
 * real aggregation support needed), so an opaque forward declaration is enough for the pointer types
 * below. */
#ifndef __IUnknown_FWD_DEFINED__
#define __IUnknown_FWD_DEFINED__
typedef struct IUnknown IUnknown;
#endif

/* ------------------------------------------------------------------ GUIDs (verbatim from dinput.h) */

DEFINE_GUID(XJ_IID_IUnknown,             0x00000000,0x0000,0x0000,0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46);
DEFINE_GUID(XJ_IID_IDirectInputA,        0x89521360,0xAA8A,0x11CF,0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00);
DEFINE_GUID(XJ_IID_IDirectInput2A,       0x5944E662,0xAA8A,0x11CF,0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00);
DEFINE_GUID(XJ_IID_IDirectInput7A,       0x9A4CB684,0x236D,0x11D3,0x8E,0x9D,0x00,0xC0,0x4F,0x68,0x44,0xAE);
DEFINE_GUID(XJ_IID_IDirectInputDeviceA,  0x5944E680,0xC92E,0x11CF,0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00);
DEFINE_GUID(XJ_IID_IDirectInputDevice2A, 0x5944E682,0xC92E,0x11CF,0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00);
DEFINE_GUID(XJ_IID_IDirectInputDevice7A, 0x57D7C6BC,0x2356,0x11D3,0x8E,0x9D,0x00,0xC0,0x4F,0x68,0x44,0xAE);

DEFINE_GUID(XJ_GUID_XAxis,   0xA36D02E0,0xC9F3,0x11CF,0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00);
DEFINE_GUID(XJ_GUID_YAxis,   0xA36D02E1,0xC9F3,0x11CF,0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00);
DEFINE_GUID(XJ_GUID_ZAxis,   0xA36D02E2,0xC9F3,0x11CF,0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00);
DEFINE_GUID(XJ_GUID_RxAxis,  0xA36D02F4,0xC9F3,0x11CF,0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00);
DEFINE_GUID(XJ_GUID_RyAxis,  0xA36D02F5,0xC9F3,0x11CF,0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00);
DEFINE_GUID(XJ_GUID_RzAxis,  0xA36D02E3,0xC9F3,0x11CF,0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00);
DEFINE_GUID(XJ_GUID_Slider,  0xA36D02E4,0xC9F3,0x11CF,0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00);
DEFINE_GUID(XJ_GUID_Button,  0xA36D02F0,0xC9F3,0x11CF,0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00);
DEFINE_GUID(XJ_GUID_POV,     0xA36D02F2,0xC9F3,0x11CF,0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00);
DEFINE_GUID(XJ_GUID_Unknown, 0xA36D02F3,0xC9F3,0x11CF,0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00);
DEFINE_GUID(XJ_GUID_Joystick,0x6F1D2B70,0xD5A0,0x11CF,0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00); /* device class */
/* Our fake device instance GUID -- arbitrary, stable, only needs to be self-consistent. */
DEFINE_GUID(XJ_GUID_OurDevice,0x1c8f1a00,0x7a3e,0x4a2b,0x9d,0x11,0x58,0x69,0x6e,0x70,0x75,0x74);

/* Real DirectInput property "GUIDs" are just small integers cast through MAKEDIPROP -- not real GUIDs,
 * so a pointer comparison (not a byte comparison) is how the engine will pass and we'll recognize them. */
#define XJ_DIPROP_BUFFERSIZE ((const GUID *)(UINT_PTR)1)
#define XJ_DIPROP_AXISMODE   ((const GUID *)(UINT_PTR)2)
#define XJ_DIPROP_GRANULARITY ((const GUID *)(UINT_PTR)3)
#define XJ_DIPROP_RANGE      ((const GUID *)(UINT_PTR)4)
#define XJ_DIPROP_DEADZONE   ((const GUID *)(UINT_PTR)5)
#define XJ_DIPROP_SATURATION ((const GUID *)(UINT_PTR)6)

/* ------------------------------------------------------------------ error codes / return values */

#define XJ_DI_OK             S_OK
#define XJ_DI_NOEFFECT       S_FALSE
#define XJ_DIERR_INVALIDPARAM  E_INVALIDARG
#define XJ_DIERR_NOINTERFACE   E_NOINTERFACE
#define XJ_DIERR_GENERIC       E_FAIL
#define XJ_DIERR_OUTOFMEMORY    E_OUTOFMEMORY
#define XJ_DIERR_UNSUPPORTED    E_NOTIMPL
#define XJ_DIERR_NOTINITIALIZED     MAKE_HRESULT(SEVERITY_ERROR, FACILITY_WIN32, ERROR_NOT_READY)
#define XJ_DIERR_ALREADYINITIALIZED MAKE_HRESULT(SEVERITY_ERROR, FACILITY_WIN32, ERROR_ALREADY_INITIALIZED)
#define XJ_DIERR_OTHERAPPHASPRIO    E_ACCESSDENIED
#define XJ_DIERR_INPUTLOST      MAKE_HRESULT(SEVERITY_ERROR, FACILITY_WIN32, ERROR_READ_FAULT)
#define XJ_DIERR_ACQUIRED       MAKE_HRESULT(SEVERITY_ERROR, FACILITY_WIN32, ERROR_BUSY)
#define XJ_DIERR_NOTACQUIRED    MAKE_HRESULT(SEVERITY_ERROR, FACILITY_WIN32, ERROR_INVALID_ACCESS)
#define XJ_DIERR_NOTBUFFERED    ((HRESULT)0x80040207L)
#define XJ_DIERR_UNPLUGGED      ((HRESULT)0x80040209L)
#define XJ_DIERR_NOTFOUND       MAKE_HRESULT(SEVERITY_ERROR, FACILITY_WIN32, ERROR_FILE_NOT_FOUND)

/* ------------------------------------------------------------------ flags / enums (verbatim) */

#define XJ_DIENUM_STOP          0
#define XJ_DIENUM_CONTINUE      1

#define XJ_DIEDFL_ALLDEVICES     0x00000000
#define XJ_DIEDFL_ATTACHEDONLY   0x00000001

#define XJ_DIDEVTYPE_DEVICE      1
#define XJ_DIDEVTYPE_MOUSE       2
#define XJ_DIDEVTYPE_KEYBOARD    3
#define XJ_DIDEVTYPE_JOYSTICK    4
#define XJ_DIDEVTYPEJOYSTICK_GAMEPAD 4

#define XJ_DIDC_ATTACHED         0x00000001
#define XJ_DIDC_EMULATED         0x00000004

#define XJ_DIDFT_RELAXIS         0x00000001
#define XJ_DIDFT_ABSAXIS         0x00000002
#define XJ_DIDFT_AXIS            0x00000003
#define XJ_DIDFT_PSHBUTTON       0x00000004
#define XJ_DIDFT_BUTTON          0x0000000C
#define XJ_DIDFT_POV             0x00000010
#define XJ_DIDFT_ANYINSTANCE     0x00FFFF00
#define XJ_DIDFT_MAKEINSTANCE(n) ((WORD)(n) << 8)
#define XJ_DIDFT_GETTYPE(n)      LOBYTE(n)
#define XJ_DIDFT_GETINSTANCE(n)  LOWORD((n) >> 8)

#define XJ_DIPH_DEVICE   0
#define XJ_DIPH_BYOFFSET 1
#define XJ_DIPH_BYID     2

#define XJ_DISCL_EXCLUSIVE    0x00000001
#define XJ_DISCL_NONEXCLUSIVE 0x00000002
#define XJ_DISCL_FOREGROUND   0x00000004
#define XJ_DISCL_BACKGROUND   0x00000008

/* ------------------------------------------------------------------ structs (field layout verbatim) */

typedef struct {
    DWORD dwSize, dwFlags, dwDevType, dwAxes, dwButtons, dwPOVs;
    DWORD dwFFSamplePeriod, dwFFMinTimeResolution, dwFirmwareRevision, dwHardwareRevision, dwFFDriverVersion;
} XJ_DIDEVCAPS;

typedef struct { const GUID *pguid; DWORD dwOfs, dwType, dwFlags; } XJ_DIOBJECTDATAFORMAT;
typedef struct { DWORD dwSize, dwObjSize, dwFlags, dwDataSize, dwNumObjs; XJ_DIOBJECTDATAFORMAT *rgodf; } XJ_DIDATAFORMAT;

typedef struct { DWORD dwOfs, dwData, dwTimeStamp, dwSequence; } XJ_DIDEVICEOBJECTDATA;

typedef struct {
    DWORD dwSize; GUID guidInstance, guidProduct; DWORD dwDevType;
    CHAR tszInstanceName[MAX_PATH]; CHAR tszProductName[MAX_PATH];
    GUID guidFFDriver; WORD wUsagePage, wUsage;
} XJ_DIDEVICEINSTANCEA;

typedef struct {
    DWORD dwSize; GUID guidType; DWORD dwOfs, dwType, dwFlags; CHAR tszName[MAX_PATH];
    DWORD dwFFMaxForce, dwFFForceResolution; WORD wCollectionNumber, wDesignatorIndex, wUsagePage, wUsage;
    DWORD dwDimension; WORD wExponent, wReportId;
} XJ_DIDEVICEOBJECTINSTANCEA;

typedef struct { DWORD dwSize, dwHeaderSize, dwObj, dwHow; } XJ_DIPROPHEADER;
typedef struct { XJ_DIPROPHEADER diph; DWORD dwData; } XJ_DIPROPDWORD;
typedef struct { XJ_DIPROPHEADER diph; LONG lMin, lMax; } XJ_DIPROPRANGE;

/* Byte-identical to Microsoft's classic DIJOYSTATE (52 bytes): lX,lY,lZ,lRx,lRy,lRz, 2 sliders,
 * 4 POV dwords, 32 buttons. Phase-0 discovery may show the engine wants something else (DIJOYSTATE2,
 * 128 buttons) -- SetDataFormat's logged dwDataSize is the source of truth, not this typedef; see
 * xj_state_buf in xinput_joy.c, which is sized generously and only reports what SetDataFormat asked for. */
typedef struct {
    LONG lX, lY, lZ, lRx, lRy, lRz;
    LONG rglSlider[2];
    DWORD rgdwPOV[4];
    BYTE rgbButtons[32];
} xj_djoystate_t;

typedef WINBOOL (CALLBACK *XJ_LPDIENUMDEVICESCALLBACKA)(const XJ_DIDEVICEINSTANCEA *, void *);
typedef WINBOOL (CALLBACK *XJ_LPDIENUMDEVICEOBJECTSCALLBACKA)(const XJ_DIDEVICEOBJECTINSTANCEA *, void *);

/* ------------------------------------------------------------------ vtables */

typedef struct XJDirectInputVtbl XJDirectInputVtbl;
typedef struct { const XJDirectInputVtbl *lpVtbl; } XJDirectInput;

typedef struct XJDeviceVtbl XJDeviceVtbl;
typedef struct { const XJDeviceVtbl *lpVtbl; } XJDevice; /* forward decl only; real def has private fields in xinput_joy.c */

/* IDirectInputA: 8 slots (3 IUnknown + 5 own). Confirmed against dinput.h's DECLARE_INTERFACE_. */
struct XJDirectInputVtbl {
    HRESULT (WINAPI *QueryInterface)(XJDirectInput *This, REFIID riid, void **ppv);
    ULONG   (WINAPI *AddRef)(XJDirectInput *This);
    ULONG   (WINAPI *Release)(XJDirectInput *This);
    HRESULT (WINAPI *CreateDevice)(XJDirectInput *This, REFGUID rguid, XJDevice **out, IUnknown *pUnkOuter);
    HRESULT (WINAPI *EnumDevices)(XJDirectInput *This, DWORD dwDevType, XJ_LPDIENUMDEVICESCALLBACKA cb, void *pvRef, DWORD dwFlags);
    HRESULT (WINAPI *GetDeviceStatus)(XJDirectInput *This, REFGUID rguidInstance);
    HRESULT (WINAPI *RunControlPanel)(XJDirectInput *This, HWND hwndOwner, DWORD dwFlags);
    HRESULT (WINAPI *Initialize)(XJDirectInput *This, HINSTANCE hinst, DWORD dwVersion);
};

/* IDirectInputDevice2A: 27 slots. Slots 0-17 are plain IDirectInputDeviceA (confirmed against
 * dinput.h); IDirectInputDevice2A does NOT simply append Poll/SendDeviceData right after Initialize --
 * it inserts the force-feedback methods first (18-24), THEN Poll(25)/SendDeviceData(26). Getting this
 * wrong would make Poll live at the wrong vtable offset for anything that QI's for
 * IID_IDirectInputDevice2A specifically -- verified line-by-line against dinput.h this session. */
struct XJDeviceVtbl {
    HRESULT (WINAPI *QueryInterface)(XJDevice *This, REFIID riid, void **ppv);
    ULONG   (WINAPI *AddRef)(XJDevice *This);
    ULONG   (WINAPI *Release)(XJDevice *This);
    HRESULT (WINAPI *GetCapabilities)(XJDevice *This, XJ_DIDEVCAPS *caps);
    HRESULT (WINAPI *EnumObjects)(XJDevice *This, XJ_LPDIENUMDEVICEOBJECTSCALLBACKA cb, void *pvRef, DWORD dwFlags);
    HRESULT (WINAPI *GetProperty)(XJDevice *This, REFGUID rguidProp, XJ_DIPROPHEADER *pdiph);
    HRESULT (WINAPI *SetProperty)(XJDevice *This, REFGUID rguidProp, const XJ_DIPROPHEADER *pdiph);
    HRESULT (WINAPI *Acquire)(XJDevice *This);
    HRESULT (WINAPI *Unacquire)(XJDevice *This);
    HRESULT (WINAPI *GetDeviceState)(XJDevice *This, DWORD cbData, void *lpvData);
    HRESULT (WINAPI *GetDeviceData)(XJDevice *This, DWORD cbObjectData, XJ_DIDEVICEOBJECTDATA *rgdod, DWORD *pdwInOut, DWORD dwFlags);
    HRESULT (WINAPI *SetDataFormat)(XJDevice *This, const XJ_DIDATAFORMAT *lpdf);
    HRESULT (WINAPI *SetEventNotification)(XJDevice *This, HANDLE hEvent);
    HRESULT (WINAPI *SetCooperativeLevel)(XJDevice *This, HWND hwnd, DWORD dwFlags);
    HRESULT (WINAPI *GetObjectInfo)(XJDevice *This, XJ_DIDEVICEOBJECTINSTANCEA *pdidoi, DWORD dwObj, DWORD dwHow);
    HRESULT (WINAPI *GetDeviceInfo)(XJDevice *This, XJ_DIDEVICEINSTANCEA *pdidi);
    HRESULT (WINAPI *RunControlPanel)(XJDevice *This, HWND hwndOwner, DWORD dwFlags);
    HRESULT (WINAPI *Initialize)(XJDevice *This, HINSTANCE hinst, DWORD dwVersion, REFGUID rguid);
    /* ---- IDirectInputDevice2A additions: force feedback (stubbed, unsupported -- no FF hardware to model) */
    HRESULT (WINAPI *CreateEffect)(XJDevice *This, REFGUID rguid, const void *lpeff, void **ppdeff, IUnknown *punkOuter);
    HRESULT (WINAPI *EnumEffects)(XJDevice *This, void *lpCallback, void *pvRef, DWORD dwEffType);
    HRESULT (WINAPI *GetEffectInfo)(XJDevice *This, void *pdei, REFGUID rguid);
    HRESULT (WINAPI *GetForceFeedbackState)(XJDevice *This, DWORD *pdwOut);
    HRESULT (WINAPI *SendForceFeedbackCommand)(XJDevice *This, DWORD dwFlags);
    HRESULT (WINAPI *EnumCreatedEffectObjects)(XJDevice *This, void *lpCallback, void *pvRef, DWORD fl);
    HRESULT (WINAPI *Escape)(XJDevice *This, void *pesc);
    HRESULT (WINAPI *Poll)(XJDevice *This);
    HRESULT (WINAPI *SendDeviceData)(XJDevice *This, DWORD cbObjectData, const XJ_DIDEVICEOBJECTDATA *rgdod, DWORD *pdwInOut, DWORD fl);
};

#endif /* XINPUT_JOY_DIDEFS_H */
