

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "render_dx9_imgui.h"

#ifndef DDRAW_WRAPPER_ENABLE_LOGGING
#define DDRAW_WRAPPER_ENABLE_LOGGING 0
#endif

#define DD_OK                      0x00000000L
#define DDERR_GENERIC              0x80004005L
#define DDERR_UNSUPPORTED          0x80004001L
#define DDERR_INVALIDPARAMS        0x80070057L
#define DDERR_OUTOFMEMORY          0x8007000EL
#define DDERR_SURFACELOST          0x887601C2L
#define DDERR_WASSTILLDRAWING      0x887601C4L
#define DDERR_NOTLOCKED            0x887601C8L
#define DDERR_ALREADYLOCKED        0x887601CAL

#define DDSCAPS_BACKBUFFER         0x00000004
#define DDSCAPS_COMPLEX            0x00000008
#define DDSCAPS_FLIP               0x00000010
#define DDSCAPS_OFFSCREENPLAIN     0x00000040
#define DDSCAPS_PRIMARYSURFACE     0x00000200
#define DDSCAPS_SYSTEMMEMORY       0x00000800
#define DDSCAPS_VIDEOMEMORY        0x00004000

#define DDSD_CAPS                  0x00000001
#define DDSD_HEIGHT                0x00000002
#define DDSD_WIDTH                 0x00000004
#define DDSD_PITCH                 0x00000008
#define DDSD_BACKBUFFERCOUNT       0x00000020
#define DDSD_LPSURFACE             0x00000800
#define DDSD_PIXELFORMAT           0x00001000

#define DDLOCK_WAIT                0x00000001
#define DDLOCK_READONLY            0x00000010
#define DDLOCK_WRITEONLY           0x00000020
#define DDLOCK_NOSYSLOCK           0x00000800

#define DDSCL_FULLSCREEN           0x00000001
#define DDSCL_ALLOWMODEX           0x00000004
#define DDSCL_NORMAL               0x00000008
#define DDSCL_EXCLUSIVE            0x00000010
#define DDSCL_ALLOWREBOOT          0x00000002

#define DDPCAPS_8BIT               0x00000004
#define DDPCAPS_ALLOW256           0x00000040

#define DDBLTFAST_NOCOLORKEY       0x00000000
#define DDBLTFAST_SRCCOLORKEY      0x00000001
#define DDBLTFAST_DESTCOLORKEY     0x00000002
#define DDBLTFAST_WAIT             0x00000010

#define DDBLT_COLORFILL            0x00000400
#define DDBLT_KEYSRC               0x00008000
#define DDBLT_KEYDEST              0x00004000
#define DDBLT_WAIT                 0x01000000

#define DDCKEY_COLORSPACE          0x00000001
#define DDCKEY_DESTBLT             0x00000002
#define DDCKEY_DESTOVERLAY         0x00000004
#define DDCKEY_SRCBLT              0x00000008
#define DDCKEY_SRCOVERLAY          0x00000010

/* Must match the Windows SDK struct byte-for-byte: 108 bytes on 32-bit. */
#pragma pack(push, 4)

typedef struct { DWORD dwLow; DWORD dwHigh; } MY_DDCOLORKEY;

typedef struct {          /* 32 bytes, matches DDPIXELFORMAT */
    DWORD dwSize;
    DWORD dwFlags;
    DWORD dwFourCC;
    DWORD dwRGBBitCount;
    DWORD dwRBitMask;
    DWORD dwGBitMask;
    DWORD dwBBitMask;
    DWORD dwRGBAlphaBitMask;
} MY_DDPIXELFORMAT;

typedef struct { DWORD dwCaps; } MY_DDSCAPS;

typedef struct {
    DWORD           dwSize;
    DWORD           dwFlags;
    DWORD           dwHeight;
    DWORD           dwWidth;
    LONG            lPitch;
    DWORD           dwBackBufferCount;
    DWORD           dwMipMapCount;
    DWORD           dwAlphaBitDepth;
    DWORD           dwReserved;
    DWORD           lpSurface;
    MY_DDCOLORKEY   ddckCKDestOverlay;
    MY_DDCOLORKEY   ddckCKDestBlt;
    MY_DDCOLORKEY   ddckCKSrcOverlay;
    MY_DDCOLORKEY   ddckCKSrcBlt;
    MY_DDPIXELFORMAT ddpfPixelFormat;
    MY_DDSCAPS      ddsCaps;
} MY_DDSURFACEDESC;

#pragma pack(pop)

typedef char _ASSERT_DDSURFACEDESC_SIZE[(sizeof(MY_DDSURFACEDESC) == 108) ? 1 : -1];

typedef struct MyDDraw     MyDDraw;
typedef struct MyDDSurface MyDDSurface;
typedef struct MyDDPalette MyDDPalette;

struct MyDDPalette {
    void   *lpVtbl;
    LONG    refCount;
    RGBQUAD entries[256];
};

struct MyDDSurface {
    void        *lpVtbl;
    LONG         refCount;
    int          width, height, pitch;
    BYTE        *pixels;
    BOOL         isPrimary;
    BOOL         isBackBuffer;
    MyDDSurface *attachedBackBuffer;
    MyDDPalette *palette;
    MyDDraw     *owner;
    BOOL         locked;
    BOOL         hasColorKeySrc;
    DWORD        colorKeySrcLow, colorKeySrcHigh;
};

struct MyDDraw {
    void       *lpVtbl;
    LONG        refCount;
    HWND        hWnd;
    WNDPROC     origWndProc;
    int         gameWidth, gameHeight;
    BOOL        borderless;
    RECT        savedWinRect;
    LONG        savedStyle, savedExStyle;
    MyDDSurface *primarySurface;
};

typedef struct EfzDdrawOverlayFrame {
    DWORD         structSize;
    BYTE         *pixels;
    int           width;
    int           height;
    int           pitch;
    HWND          hWnd;
    const RGBQUAD *palette;
    DWORD         presentSerial;
} EfzDdrawOverlayFrame;

typedef void (__stdcall *EfzDdrawOverlayCallback)(
    const EfzDdrawOverlayFrame *frame,
    void *userData);

static MyDDraw         *g_dd        = NULL;
static FILE            *g_log       = NULL;
static CRITICAL_SECTION g_logCS;
static BOOL             g_logCSInit = FALSE;
static HINSTANCE        g_hInst     = NULL;
static HHOOK            g_kbHook    = NULL;
static BOOL             g_appActive = TRUE;
static BOOL             g_dinputHooked = FALSE;
static BOOL             g_hasEverFlipped = FALSE;

#if DDRAW_WRAPPER_ENABLE_LOGGING
static void LOG(const char *fmt, ...);
#else
#define LOG(...) ((void)0)
#endif

#define DIERR_INPUTLOST          0x8007001EL
#define DIERR_NOTACQUIRED        0x8007000CL
#define DISCL_EXCLUSIVE_DI       0x00000001
#define DISCL_NONEXCLUSIVE_DI    0x00000002
#define DISCL_FOREGROUND_DI      0x00000004
#define DISCL_BACKGROUND_DI      0x00000008
#define DISCL_NOWINKEY_DI        0x00000010

typedef HRESULT (WINAPI *PFN_DirectInputCreateEx)(
    HINSTANCE hinst, DWORD dwVersion, REFIID riidltf, void **ppvOut,
    void *punkOuter);
typedef HRESULT (WINAPI *PFN_DirectInputCreateA)(
    HINSTANCE hinst, DWORD dwVersion, void **ppvOut, void *punkOuter);

typedef struct DInputProxy DInputProxy;
typedef struct DInputDeviceProxy DInputDeviceProxy;

struct DInputProxy {
    void *lpVtbl;
    LONG  refCount;
    void *real;
};

struct DInputDeviceProxy {
    void *lpVtbl;
    LONG  refCount;
    void *real;
    DWORD coopFlags;
    HWND  hWnd;
    DWORD stateCalls;
    DWORD stateFailures;
    DWORD stateInactiveSuppressions;
    DWORD pollCalls;
    DWORD pollFailures;
    DWORD pollInactiveSuppressions;
};

static PFN_DirectInputCreateEx g_realDirectInputCreateEx = NULL;
static PFN_DirectInputCreateA  g_realDirectInputCreateA = NULL;
static DWORD g_focusSerial = 0;
static DWORD g_lastFocusTick = 0;
static DWORD g_presentSerial = 0;
static DWORD g_flipSerial = 0;
static const DWORD kInputFocusSettleMs = 100;
static BOOL  g_renderTraceAfterFocus = FALSE;
static int   g_renderTraceBudget = 0;
static int   g_wndTraceBudget = 0;
static EfzDdrawOverlayCallback g_overlayCallback = NULL;
static void *g_overlayCallbackUserData = NULL;
static void __stdcall ToggleWindowModeFromPresenter(void *userData);

void __stdcall EFZDDRAW_RegisterOverlayCallback(
    EfzDdrawOverlayCallback callback,
    void *userData)
{
    g_overlayCallback = callback;
    g_overlayCallbackUserData = userData;
    LOG("Overlay callback %s callback=%p user=%p",
        callback ? "registered" : "cleared",
        (void *)callback,
        userData);
}

typedef struct {
    int  fps_limit;
    BOOL borderless;
    int  window_scale;
    BOOL linear_scale;
} WrapperConfig;

static WrapperConfig g_cfg = { 60, FALSE, 1, FALSE };

#define CURSOR_IDLE_HIDE_MS 2500u
static DWORD g_lastMouseMoveTick = 0;
static BOOL  g_cursorHidden      = FALSE;

static LARGE_INTEGER g_qpcFreq;
static LARGE_INTEGER g_nextFrameQpc;
static BOOL          g_qpcInit          = FALSE;
static BOOL          g_fpsPacedThisFlip = FALSE;
/* Interlocked; the render thread resets its own QPC deadline. */
static volatile LONG g_runtimeFpsLimit = 60;
static volatile LONG g_fpsLimitGeneration = 1;
static LONG          g_appliedFpsLimitGeneration = 0;

static HRESULT WINAPI Hook_DirectInputCreateEx(
    HINSTANCE hinst, DWORD dwVersion, REFIID riidltf, void **ppvOut,
    void *punkOuter);
static HRESULT WINAPI Hook_DirectInputCreateA(
    HINSTANCE hinst, DWORD dwVersion, void **ppvOut, void *punkOuter);

static BOOL hr_failed(HRESULT hr)
{
    return hr < 0;
}

static BOOL should_log_counter(DWORD count)
{
    return count <= 8 || (count % 120) == 0;
}

static BOOL is_foreground_for_hwnd(HWND hwnd)
{
    HWND foreground;
    HWND hwndRoot;
    HWND foregroundRoot;
    DWORD foregroundPid = 0;

    if (hwnd == NULL || !IsWindow(hwnd))
        return FALSE;

    foreground = GetForegroundWindow();
    if (foreground == NULL || !IsWindow(foreground))
        return FALSE;

    GetWindowThreadProcessId(foreground, &foregroundPid);
    if (foregroundPid != GetCurrentProcessId())
        return FALSE;

    if (foreground == hwnd || IsChild(hwnd, foreground))
        return TRUE;

    hwndRoot = GetAncestor(hwnd, GA_ROOTOWNER);
    foregroundRoot = GetAncestor(foreground, GA_ROOTOWNER);
    if (hwndRoot == NULL)
        hwndRoot = hwnd;
    if (foregroundRoot == NULL)
        foregroundRoot = foreground;

    return hwndRoot == foregroundRoot;
}

static BOOL should_suppress_dinput_device(DInputDeviceProxy *dev)
{
    if (EFZDX9_Presenter_ShouldSuppressGameInput())
        return TRUE;

    if (!g_appActive)
        return TRUE;

    if (g_lastFocusTick != 0 && GetTickCount() - g_lastFocusTick < kInputFocusSettleMs)
        return TRUE;

    if (dev != NULL && dev->hWnd != NULL)
        return !is_foreground_for_hwnd(dev->hWnd);

    if (g_dd != NULL && g_dd->hWnd != NULL)
        return !is_foreground_for_hwnd(g_dd->hWnd);

    return FALSE;
}

static HRESULT call_real_release(void *real)
{
    typedef HRESULT (__stdcall *Fn)(void *);
    return ((Fn)(*(void ***)real)[2])(real);
}

static DInputDeviceProxy *wrap_dinput_device(void *real)
{
    static void *deviceVtbl[32];
    DInputDeviceProxy *dev;

    if (!real) return NULL;

    dev = (DInputDeviceProxy *)calloc(1, sizeof(*dev));
    if (!dev) return NULL;

    dev->lpVtbl = deviceVtbl;
    dev->refCount = 1;
    dev->real = real;
    dev->coopFlags = DISCL_FOREGROUND_DI;
    return dev;
}

static HRESULT __stdcall DIDev_QueryInterface(DInputDeviceProxy *dev,
    REFIID riid, void **ppv)
{
    if (!ppv) return DDERR_INVALIDPARAMS;
    *ppv = dev;
    InterlockedIncrement(&dev->refCount);
    return DD_OK;
}

static ULONG __stdcall DIDev_AddRef(DInputDeviceProxy *dev)
{
    return (ULONG)InterlockedIncrement(&dev->refCount);
}

static ULONG __stdcall DIDev_Release(DInputDeviceProxy *dev)
{
    LONG ref = InterlockedDecrement(&dev->refCount);
    if (ref == 0) {
        call_real_release(dev->real);
        free(dev);
    }
    return (ULONG)ref;
}

static HRESULT __stdcall DIDev_GetCapabilities(DInputDeviceProxy *dev, void *caps)
{
    typedef HRESULT (__stdcall *Fn)(void *, void *);
    return ((Fn)(*(void ***)dev->real)[3])(dev->real, caps);
}

static HRESULT __stdcall DIDev_EnumObjects(DInputDeviceProxy *dev,
    void *callback, void *ref, DWORD flags)
{
    typedef HRESULT (__stdcall *Fn)(void *, void *, void *, DWORD);
    return ((Fn)(*(void ***)dev->real)[4])(dev->real, callback, ref, flags);
}

static HRESULT __stdcall DIDev_GetProperty(DInputDeviceProxy *dev,
    REFGUID prop, void *header)
{
    typedef HRESULT (__stdcall *Fn)(void *, REFGUID, void *);
    return ((Fn)(*(void ***)dev->real)[5])(dev->real, prop, header);
}

static HRESULT __stdcall DIDev_SetProperty(DInputDeviceProxy *dev,
    REFGUID prop, void *header)
{
    typedef HRESULT (__stdcall *Fn)(void *, REFGUID, void *);
    return ((Fn)(*(void ***)dev->real)[6])(dev->real, prop, header);
}

static HRESULT __stdcall DIDev_Acquire(DInputDeviceProxy *dev)
{
    typedef HRESULT (__stdcall *Fn)(void *);
    HRESULT hr = ((Fn)(*(void ***)dev->real)[7])(dev->real);
    LOG("DI Acquire dev=%p real=%p hwnd=%p flags=%08X active=%d hr=%08X",
        (void *)dev, dev->real, (void *)dev->hWnd, dev->coopFlags,
        g_appActive, (DWORD)hr);
    return hr;
}

static HRESULT __stdcall DIDev_Unacquire(DInputDeviceProxy *dev)
{
    typedef HRESULT (__stdcall *Fn)(void *);
    HRESULT hr = ((Fn)(*(void ***)dev->real)[8])(dev->real);
    LOG("DI Unacquire dev=%p real=%p hwnd=%p flags=%08X active=%d hr=%08X",
        (void *)dev, dev->real, (void *)dev->hWnd, dev->coopFlags,
        g_appActive, (DWORD)hr);
    return hr;
}

static void neutralize_dinput_state(void *data, DWORD cbData)
{
    ZeroMemory(data, cbData);

    /* POV neutral is 0xFFFFFFFF; zeroing it fabricates DPad Up. */
    if (cbData == 80u) {
        BYTE *bytes = (BYTE *)data;
        DWORD neutralPov = 0xFFFFFFFFu;
        DWORD index;
        for (index = 0; index < 4u; ++index) {
            CopyMemory(bytes + 32u + (index * sizeof(DWORD)),
                &neutralPov,
                sizeof(neutralPov));
        }
    }
}

static HRESULT __stdcall DIDev_GetDeviceState(DInputDeviceProxy *dev,
    DWORD cbData, void *data)
{
    typedef HRESULT (__stdcall *GetStateFn)(void *, DWORD, void *);
    typedef HRESULT (__stdcall *AcquireFn)(void *);
    HRESULT hr;

    if (!data) {
        return ((GetStateFn)(*(void ***)dev->real)[9])(dev->real, cbData, data);
    }

    dev->stateCalls++;
    if (should_suppress_dinput_device(dev)) {
        neutralize_dinput_state(data, cbData);
        dev->stateInactiveSuppressions++;
        if (should_log_counter(dev->stateInactiveSuppressions)) {
            LOG("DI GetDeviceState inactive-neutral dev=%p call=%u inactiveNeutral=%u cb=%u hwnd=%p fg=%p focusSerial=%u",
                (void *)dev, dev->stateCalls, dev->stateInactiveSuppressions,
                cbData, (void *)dev->hWnd, (void *)GetForegroundWindow(),
                g_focusSerial);
        }
        return DD_OK;
    }

    hr = ((GetStateFn)(*(void ***)dev->real)[9])(dev->real, cbData, data);
    if (hr_failed(hr)) {
        HRESULT acquireHr;
        neutralize_dinput_state(data, cbData);
        dev->stateFailures++;
        acquireHr = ((AcquireFn)(*(void ***)dev->real)[7])(dev->real);
        if (should_log_counter(dev->stateFailures)) {
            LOG("DI GetDeviceState fail-neutral dev=%p call=%u fail=%u cb=%u hr=%08X acquireHr=%08X active=%d hwnd=%p fg=%p focusSerial=%u",
                (void *)dev, dev->stateCalls, dev->stateFailures, cbData,
                (DWORD)hr, (DWORD)acquireHr, g_appActive, (void *)dev->hWnd,
                (void *)GetForegroundWindow(), g_focusSerial);
        }
        return DD_OK;
    }

    if (dev->stateCalls <= 8 || g_renderTraceAfterFocus) {
        LOG("DI GetDeviceState ok dev=%p call=%u cb=%u active=%d hwnd=%p focusSerial=%u",
            (void *)dev, dev->stateCalls, cbData, g_appActive,
            (void *)dev->hWnd, g_focusSerial);
    }
    return hr;
}

static HRESULT __stdcall DIDev_GetDeviceData(DInputDeviceProxy *dev,
    DWORD cbObjectData, void *objectData, DWORD *inOut, DWORD flags)
{
    typedef HRESULT (__stdcall *Fn)(void *, DWORD, void *, DWORD *, DWORD);
    if (inOut != NULL && should_suppress_dinput_device(dev)) {
        *inOut = 0;
        return DD_OK;
    }
    return ((Fn)(*(void ***)dev->real)[10])(dev->real, cbObjectData, objectData,
                                          inOut, flags);
}

static HRESULT __stdcall DIDev_SetDataFormat(DInputDeviceProxy *dev, void *fmt)
{
    typedef HRESULT (__stdcall *Fn)(void *, void *);
    return ((Fn)(*(void ***)dev->real)[11])(dev->real, fmt);
}

static HRESULT __stdcall DIDev_SetEventNotification(DInputDeviceProxy *dev,
    HANDLE event)
{
    typedef HRESULT (__stdcall *Fn)(void *, HANDLE);
    return ((Fn)(*(void ***)dev->real)[12])(dev->real, event);
}

static HRESULT __stdcall DIDev_SetCooperativeLevel(DInputDeviceProxy *dev,
    HWND hWnd, DWORD flags)
{
    typedef HRESULT (__stdcall *Fn)(void *, HWND, DWORD);
    HRESULT hr;
    DWORD origFlags = flags;
    dev->hWnd = hWnd;

    flags = (flags & ~(DISCL_EXCLUSIVE_DI | DISCL_BACKGROUND_DI | DISCL_NOWINKEY_DI))
        | DISCL_NONEXCLUSIVE_DI
        | DISCL_FOREGROUND_DI;

    dev->coopFlags = flags;
    hr = ((Fn)(*(void ***)dev->real)[13])(dev->real, hWnd, flags);
    LOG("DI SetCooperativeLevel dev=%p real=%p hwnd=%p origFlags=%08X flags=%08X active=%d hr=%08X",
        (void *)dev, dev->real, (void *)hWnd, origFlags, flags, g_appActive, (DWORD)hr);
    return hr;
}

static HRESULT __stdcall DIDev_GetObjectInfo(DInputDeviceProxy *dev,
    void *objectInfo, DWORD object, DWORD how)
{
    typedef HRESULT (__stdcall *Fn)(void *, void *, DWORD, DWORD);
    return ((Fn)(*(void ***)dev->real)[14])(dev->real, objectInfo, object, how);
}

static HRESULT __stdcall DIDev_GetDeviceInfo(DInputDeviceProxy *dev,
    void *deviceInfo)
{
    typedef HRESULT (__stdcall *Fn)(void *, void *);
    return ((Fn)(*(void ***)dev->real)[15])(dev->real, deviceInfo);
}

static HRESULT __stdcall DIDev_RunControlPanel(DInputDeviceProxy *dev,
    HWND owner, DWORD flags)
{
    typedef HRESULT (__stdcall *Fn)(void *, HWND, DWORD);
    return ((Fn)(*(void ***)dev->real)[16])(dev->real, owner, flags);
}

static HRESULT __stdcall DIDev_Initialize(DInputDeviceProxy *dev,
    HINSTANCE hinst, DWORD version, REFGUID guid)
{
    typedef HRESULT (__stdcall *Fn)(void *, HINSTANCE, DWORD, REFGUID);
    return ((Fn)(*(void ***)dev->real)[17])(dev->real, hinst, version, guid);
}

static HRESULT __stdcall DIDev_CreateEffect(DInputDeviceProxy *dev,
    REFGUID guid, void *effect, void **out, void *outer)
{
    typedef HRESULT (__stdcall *Fn)(void *, REFGUID, void *, void **, void *);
    return ((Fn)(*(void ***)dev->real)[18])(dev->real, guid, effect, out, outer);
}

static HRESULT __stdcall DIDev_EnumEffects(DInputDeviceProxy *dev,
    void *callback, void *ref, DWORD type)
{
    typedef HRESULT (__stdcall *Fn)(void *, void *, void *, DWORD);
    return ((Fn)(*(void ***)dev->real)[19])(dev->real, callback, ref, type);
}

static HRESULT __stdcall DIDev_GetEffectInfo(DInputDeviceProxy *dev,
    void *effectInfo, REFGUID guid)
{
    typedef HRESULT (__stdcall *Fn)(void *, void *, REFGUID);
    return ((Fn)(*(void ***)dev->real)[20])(dev->real, effectInfo, guid);
}

static HRESULT __stdcall DIDev_GetForceFeedbackState(DInputDeviceProxy *dev,
    DWORD *state)
{
    typedef HRESULT (__stdcall *Fn)(void *, DWORD *);
    return ((Fn)(*(void ***)dev->real)[21])(dev->real, state);
}

static HRESULT __stdcall DIDev_SendForceFeedbackCommand(DInputDeviceProxy *dev,
    DWORD flags)
{
    typedef HRESULT (__stdcall *Fn)(void *, DWORD);
    return ((Fn)(*(void ***)dev->real)[22])(dev->real, flags);
}

static HRESULT __stdcall DIDev_EnumCreatedEffectObjects(DInputDeviceProxy *dev,
    void *callback, void *ref, DWORD flags)
{
    typedef HRESULT (__stdcall *Fn)(void *, void *, void *, DWORD);
    return ((Fn)(*(void ***)dev->real)[23])(dev->real, callback, ref, flags);
}

static HRESULT __stdcall DIDev_Escape(DInputDeviceProxy *dev, void *escape)
{
    typedef HRESULT (__stdcall *Fn)(void *, void *);
    return ((Fn)(*(void ***)dev->real)[24])(dev->real, escape);
}

static HRESULT __stdcall DIDev_Poll(DInputDeviceProxy *dev)
{
    typedef HRESULT (__stdcall *Fn)(void *);
    HRESULT hr;

    dev->pollCalls++;
    if (should_suppress_dinput_device(dev)) {
        dev->pollInactiveSuppressions++;
        if (should_log_counter(dev->pollInactiveSuppressions)) {
            LOG("DI Poll inactive-ok dev=%p call=%u inactivePoll=%u hwnd=%p fg=%p focusSerial=%u",
                (void *)dev, dev->pollCalls, dev->pollInactiveSuppressions,
                (void *)dev->hWnd, (void *)GetForegroundWindow(),
                g_focusSerial);
        }
        return DD_OK;
    }

    hr = ((Fn)(*(void ***)dev->real)[25])(dev->real);
    if (hr_failed(hr)) {
        dev->pollFailures++;
        if (should_log_counter(dev->pollFailures)) {
            LOG("DI Poll fail-masked dev=%p call=%u fail=%u hr=%08X active=%d hwnd=%p fg=%p focusSerial=%u",
                (void *)dev, dev->pollCalls, dev->pollFailures, (DWORD)hr,
                g_appActive, (void *)dev->hWnd, (void *)GetForegroundWindow(),
                g_focusSerial);
        }
        return DD_OK;
    }
    if (dev->pollCalls <= 8 || g_renderTraceAfterFocus) {
        LOG("DI Poll ok dev=%p call=%u active=%d hwnd=%p focusSerial=%u",
            (void *)dev, dev->pollCalls, g_appActive, (void *)dev->hWnd,
            g_focusSerial);
    }
    return hr;
}

static HRESULT __stdcall DIDev_SendDeviceData(DInputDeviceProxy *dev,
    DWORD cbObjectData, void *objectData, DWORD *inOut, DWORD flags)
{
    typedef HRESULT (__stdcall *Fn)(void *, DWORD, void *, DWORD *, DWORD);
    return ((Fn)(*(void ***)dev->real)[26])(dev->real, cbObjectData, objectData,
                                          inOut, flags);
}

static HRESULT __stdcall DIDev_EnumEffectsInFile(DInputDeviceProxy *dev,
    const char *fileName, void *callback, void *ref, DWORD flags)
{
    typedef HRESULT (__stdcall *Fn)(void *, const char *, void *, void *, DWORD);
    return ((Fn)(*(void ***)dev->real)[27])(dev->real, fileName, callback, ref,
                                          flags);
}

static HRESULT __stdcall DIDev_WriteEffectToFile(DInputDeviceProxy *dev,
    const char *fileName, DWORD entries, void *fileEffects, DWORD flags)
{
    typedef HRESULT (__stdcall *Fn)(void *, const char *, DWORD, void *, DWORD);
    return ((Fn)(*(void ***)dev->real)[28])(dev->real, fileName, entries,
                                          fileEffects, flags);
}

static HRESULT __stdcall DIDev_BuildActionMap(DInputDeviceProxy *dev,
    void *actionFormat, const char *userName, DWORD flags)
{
    typedef HRESULT (__stdcall *Fn)(void *, void *, const char *, DWORD);
    return ((Fn)(*(void ***)dev->real)[29])(dev->real, actionFormat, userName,
                                          flags);
}

static HRESULT __stdcall DIDev_SetActionMap(DInputDeviceProxy *dev,
    void *actionFormat, const char *userName, DWORD flags)
{
    typedef HRESULT (__stdcall *Fn)(void *, void *, const char *, DWORD);
    return ((Fn)(*(void ***)dev->real)[30])(dev->real, actionFormat, userName,
                                          flags);
}

static HRESULT __stdcall DIDev_GetImageInfo(DInputDeviceProxy *dev,
    void *imageInfo)
{
    typedef HRESULT (__stdcall *Fn)(void *, void *);
    return ((Fn)(*(void ***)dev->real)[31])(dev->real, imageInfo);
}

static void init_dinput_device_vtbl(void)
{
    DInputDeviceProxy *dummy = NULL;
    void **vtbl;

    dummy = wrap_dinput_device((void *)1);
    if (!dummy) return;
    vtbl = (void **)dummy->lpVtbl;
    vtbl[0]  = DIDev_QueryInterface;
    vtbl[1]  = DIDev_AddRef;
    vtbl[2]  = DIDev_Release;
    vtbl[3]  = DIDev_GetCapabilities;
    vtbl[4]  = DIDev_EnumObjects;
    vtbl[5]  = DIDev_GetProperty;
    vtbl[6]  = DIDev_SetProperty;
    vtbl[7]  = DIDev_Acquire;
    vtbl[8]  = DIDev_Unacquire;
    vtbl[9]  = DIDev_GetDeviceState;
    vtbl[10] = DIDev_GetDeviceData;
    vtbl[11] = DIDev_SetDataFormat;
    vtbl[12] = DIDev_SetEventNotification;
    vtbl[13] = DIDev_SetCooperativeLevel;
    vtbl[14] = DIDev_GetObjectInfo;
    vtbl[15] = DIDev_GetDeviceInfo;
    vtbl[16] = DIDev_RunControlPanel;
    vtbl[17] = DIDev_Initialize;
    vtbl[18] = DIDev_CreateEffect;
    vtbl[19] = DIDev_EnumEffects;
    vtbl[20] = DIDev_GetEffectInfo;
    vtbl[21] = DIDev_GetForceFeedbackState;
    vtbl[22] = DIDev_SendForceFeedbackCommand;
    vtbl[23] = DIDev_EnumCreatedEffectObjects;
    vtbl[24] = DIDev_Escape;
    vtbl[25] = DIDev_Poll;
    vtbl[26] = DIDev_SendDeviceData;
    vtbl[27] = DIDev_EnumEffectsInFile;
    vtbl[28] = DIDev_WriteEffectToFile;
    vtbl[29] = DIDev_BuildActionMap;
    vtbl[30] = DIDev_SetActionMap;
    vtbl[31] = DIDev_GetImageInfo;
    free(dummy);
}

static void *wrap_dinput_device_out(void *real)
{
    static BOOL initialized = FALSE;
    DInputDeviceProxy *dev;

    if (!initialized) {
        init_dinput_device_vtbl();
        initialized = TRUE;
    }

    dev = wrap_dinput_device(real);
    if (!dev) return real;
    LOG("DirectInput device wrapped real=%p proxy=%p", real, (void *)dev);
    return dev;
}

static HRESULT __stdcall DI_QueryInterface(DInputProxy *di,
    REFIID riid, void **ppv)
{
    if (!ppv) return DDERR_INVALIDPARAMS;
    *ppv = di;
    InterlockedIncrement(&di->refCount);
    return DD_OK;
}

static ULONG __stdcall DI_AddRef(DInputProxy *di)
{
    return (ULONG)InterlockedIncrement(&di->refCount);
}

static ULONG __stdcall DI_Release(DInputProxy *di)
{
    LONG ref = InterlockedDecrement(&di->refCount);
    if (ref == 0) {
        call_real_release(di->real);
        free(di);
    }
    return (ULONG)ref;
}

static HRESULT __stdcall DI_CreateDevice(DInputProxy *di,
    REFGUID guid, void **deviceOut, void *outer)
{
    typedef HRESULT (__stdcall *Fn)(void *, REFGUID, void **, void *);
    HRESULT hr = ((Fn)(*(void ***)di->real)[3])(di->real, guid, deviceOut, outer);
    if (!hr_failed(hr) && deviceOut && *deviceOut)
        *deviceOut = wrap_dinput_device_out(*deviceOut);
    return hr;
}

static HRESULT __stdcall DI_EnumDevices(DInputProxy *di,
    DWORD devType, void *callback, void *ref, DWORD flags)
{
    typedef HRESULT (__stdcall *Fn)(void *, DWORD, void *, void *, DWORD);
    return ((Fn)(*(void ***)di->real)[4])(di->real, devType, callback, ref, flags);
}

static HRESULT __stdcall DI_GetDeviceStatus(DInputProxy *di, REFGUID guid)
{
    typedef HRESULT (__stdcall *Fn)(void *, REFGUID);
    return ((Fn)(*(void ***)di->real)[5])(di->real, guid);
}

static HRESULT __stdcall DI_RunControlPanel(DInputProxy *di,
    HWND owner, DWORD flags)
{
    typedef HRESULT (__stdcall *Fn)(void *, HWND, DWORD);
    return ((Fn)(*(void ***)di->real)[6])(di->real, owner, flags);
}

static HRESULT __stdcall DI_Initialize(DInputProxy *di,
    HINSTANCE hinst, DWORD version)
{
    typedef HRESULT (__stdcall *Fn)(void *, HINSTANCE, DWORD);
    return ((Fn)(*(void ***)di->real)[7])(di->real, hinst, version);
}

static HRESULT __stdcall DI_FindDevice(DInputProxy *di,
    REFGUID guid, const char *name, GUID *outGuid)
{
    typedef HRESULT (__stdcall *Fn)(void *, REFGUID, const char *, GUID *);
    return ((Fn)(*(void ***)di->real)[8])(di->real, guid, name, outGuid);
}

static HRESULT __stdcall DI_CreateDeviceEx(DInputProxy *di,
    REFGUID guid, REFIID riid, void **deviceOut, void *outer)
{
    typedef HRESULT (__stdcall *Fn)(void *, REFGUID, REFIID, void **, void *);
    HRESULT hr = ((Fn)(*(void ***)di->real)[9])(di->real, guid, riid,
                                              deviceOut, outer);
    if (!hr_failed(hr) && deviceOut && *deviceOut)
        *deviceOut = wrap_dinput_device_out(*deviceOut);
    return hr;
}

static void *wrap_dinput(void *real)
{
    static void *diVtbl[10] = {
        DI_QueryInterface,
        DI_AddRef,
        DI_Release,
        DI_CreateDevice,
        DI_EnumDevices,
        DI_GetDeviceStatus,
        DI_RunControlPanel,
        DI_Initialize,
        DI_FindDevice,
        DI_CreateDeviceEx
    };
    DInputProxy *di;

    if (!real) return NULL;
    di = (DInputProxy *)calloc(1, sizeof(*di));
    if (!di) return real;

    di->lpVtbl = diVtbl;
    di->refCount = 1;
    di->real = real;
    LOG("DirectInput wrapped real=%p proxy=%p", real, (void *)di);
    return di;
}

static HRESULT WINAPI Hook_DirectInputCreateEx(
    HINSTANCE hinst, DWORD dwVersion, REFIID riidltf, void **ppvOut,
    void *punkOuter)
{
    HRESULT hr;

    if (!g_realDirectInputCreateEx)
        return DDERR_GENERIC;

    hr = g_realDirectInputCreateEx(hinst, dwVersion, riidltf, ppvOut, punkOuter);
    if (!hr_failed(hr) && ppvOut && *ppvOut)
        *ppvOut = wrap_dinput(*ppvOut);
    return hr;
}

static HRESULT WINAPI Hook_DirectInputCreateA(
    HINSTANCE hinst, DWORD dwVersion, void **ppvOut, void *punkOuter)
{
    HRESULT hr;

    if (!g_realDirectInputCreateA)
        return DDERR_GENERIC;

    hr = g_realDirectInputCreateA(hinst, dwVersion, ppvOut, punkOuter);
    LOG("DirectInputCreateA hook version=%08X hr=%08X out=%p",
        dwVersion, (DWORD)hr, ppvOut ? *ppvOut : NULL);
    if (!hr_failed(hr) && ppvOut && *ppvOut)
        *ppvOut = wrap_dinput(*ppvOut);
    return hr;
}

static void hook_dinput_iat(void)
{
    HMODULE module;
    HMODULE dinput;
    FARPROC dinputCreateEx;
    FARPROC dinputCreateA;
    BYTE *base;
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS32 *nt;
    IMAGE_IMPORT_DESCRIPTOR *imp;
    DWORD importRva;
    DWORD importSize;
    BOOL sawDInput = FALSE;
    DWORD hookCount = 0;

    if (g_dinputHooked)
        return;

    dinput = LoadLibraryA("DINPUT.dll");
    dinputCreateEx = dinput ? GetProcAddress(dinput, "DirectInputCreateEx") : NULL;
    dinputCreateA = dinput ? GetProcAddress(dinput, "DirectInputCreateA") : NULL;

    module = GetModuleHandleA(NULL);
    if (!module) {
        LOG("DirectInput hook: GetModuleHandle(NULL) failed");
        return;
    }

    base = (BYTE *)module;
    dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        LOG("DirectInput hook: bad DOS signature base=%p", (void *)base);
        return;
    }

    nt = (IMAGE_NT_HEADERS32 *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        LOG("DirectInput hook: bad NT signature base=%p e_lfanew=%ld",
            (void *)base, dos->e_lfanew);
        return;
    }

    if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        LOG("DirectInput hook: unexpected optional header magic=%04X",
            nt->OptionalHeader.Magic);
        return;
    }

    importRva = nt->OptionalHeader
        .DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    importSize = nt->OptionalHeader
        .DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size;

    LOG("DirectInput hook: module=%p importRva=%08X importSize=%08X dinput=%p procEx=%p procA=%p",
        (void *)module, importRva, importSize, (void *)dinput,
        (void *)dinputCreateEx, (void *)dinputCreateA);

    if (!importRva) {
        LOG("DirectInput hook: no import directory");
        return;
    }

    for (imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + importRva); imp->Name; ++imp) {
        const char *dllName = (const char *)(base + imp->Name);
        IMAGE_THUNK_DATA *nameThunk;
        IMAGE_THUNK_DATA *iatThunk;
        BOOL isDInputDll;

        isDInputDll =
            lstrcmpiA(dllName, "dinput.dll") == 0 ||
            lstrcmpiA(dllName, "dinput8.dll") == 0;
        LOG("DirectInput hook: import dll=%s oft=%08X ft=%08X%s",
            dllName, imp->OriginalFirstThunk, imp->FirstThunk,
            isDInputDll ? " target" : "");

        if (!isDInputDll)
            continue;
        sawDInput = TRUE;

        nameThunk = (IMAGE_THUNK_DATA *)(base +
            (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        iatThunk = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);

        for (; nameThunk->u1.AddressOfData; ++nameThunk, ++iatThunk) {
            IMAGE_IMPORT_BY_NAME *importName;
            DWORD oldProtect;
            const char *funcName = NULL;

            if (!IMAGE_SNAP_BY_ORDINAL(nameThunk->u1.Ordinal)) {
                importName = (IMAGE_IMPORT_BY_NAME *)(base + nameThunk->u1.AddressOfData);
                funcName = (const char *)importName->Name;
                LOG("DirectInput hook: candidate name=%s iat=%p value=%p",
                    funcName, (void *)&iatThunk->u1.Function,
                    (void *)(ULONG_PTR)iatThunk->u1.Function);
            } else {
                LOG("DirectInput hook: candidate ordinal=%lu iat=%p value=%p",
                    (DWORD)(nameThunk->u1.Ordinal & 0xFFFF),
                    (void *)&iatThunk->u1.Function,
                    (void *)(ULONG_PTR)iatThunk->u1.Function);
            }

            if (((funcName && lstrcmpA(funcName, "DirectInputCreateEx") == 0) ||
                 (dinputCreateEx &&
                  (FARPROC)(ULONG_PTR)iatThunk->u1.Function == dinputCreateEx)) &&
                (FARPROC)(ULONG_PTR)iatThunk->u1.Function !=
                    (FARPROC)Hook_DirectInputCreateEx) {
                g_realDirectInputCreateEx =
                    (PFN_DirectInputCreateEx)(ULONG_PTR)iatThunk->u1.Function;
                if (!VirtualProtect(&iatThunk->u1.Function,
                                    sizeof(iatThunk->u1.Function),
                                    PAGE_READWRITE, &oldProtect)) {
                    LOG("DirectInput hook: VirtualProtect failed iat=%p lastError=%lu",
                        (void *)&iatThunk->u1.Function, GetLastError());
                    return;
                }
                iatThunk->u1.Function = (ULONG_PTR)Hook_DirectInputCreateEx;
                VirtualProtect(&iatThunk->u1.Function,
                               sizeof(iatThunk->u1.Function), oldProtect,
                               &oldProtect);
                FlushInstructionCache(GetCurrentProcess(),
                                      &iatThunk->u1.Function,
                                      sizeof(iatThunk->u1.Function));
                hookCount++;
                LOG("DirectInputCreateEx IAT hook installed from %s", dllName);
                continue;
            }

            if (((funcName && lstrcmpA(funcName, "DirectInputCreateA") == 0) ||
                 (dinputCreateA &&
                  (FARPROC)(ULONG_PTR)iatThunk->u1.Function == dinputCreateA)) &&
                (FARPROC)(ULONG_PTR)iatThunk->u1.Function !=
                    (FARPROC)Hook_DirectInputCreateA) {
                g_realDirectInputCreateA =
                    (PFN_DirectInputCreateA)(ULONG_PTR)iatThunk->u1.Function;
                if (!VirtualProtect(&iatThunk->u1.Function,
                                    sizeof(iatThunk->u1.Function),
                                    PAGE_READWRITE, &oldProtect)) {
                    LOG("DirectInput hook: VirtualProtect failed iat=%p lastError=%lu",
                        (void *)&iatThunk->u1.Function, GetLastError());
                    return;
                }
                iatThunk->u1.Function = (ULONG_PTR)Hook_DirectInputCreateA;
                VirtualProtect(&iatThunk->u1.Function,
                               sizeof(iatThunk->u1.Function), oldProtect,
                               &oldProtect);
                FlushInstructionCache(GetCurrentProcess(),
                                      &iatThunk->u1.Function,
                                      sizeof(iatThunk->u1.Function));
                hookCount++;
                LOG("DirectInputCreateA IAT hook installed from %s", dllName);
            }
        }
    }

    if (hookCount > 0) {
        g_dinputHooked = TRUE;
        return;
    }

    LOG("DirectInput hook: not installed sawDInput=%d realProcEx=%p realProcA=%p",
        sawDInput, (void *)dinputCreateEx, (void *)dinputCreateA);
}

static void fps_throttle(void)
{
    const LONG fpsLimit = InterlockedCompareExchange(&g_runtimeFpsLimit, 0, 0);
    const LONG generation = InterlockedCompareExchange(&g_fpsLimitGeneration, 0, 0);

    if (!g_qpcInit) {
        QueryPerformanceFrequency(&g_qpcFreq);
        QueryPerformanceCounter(&g_nextFrameQpc);
        g_qpcInit = TRUE;
    }

    if (generation != g_appliedFpsLimitGeneration) {
        QueryPerformanceCounter(&g_nextFrameQpc);
        g_appliedFpsLimitGeneration = generation;
    }

    if (fpsLimit <= 0)
        return;

    LARGE_INTEGER now;
    LONGLONG frameTicks = g_qpcFreq.QuadPart / fpsLimit;

    QueryPerformanceCounter(&now);
    LONGLONG waitMs = 0;
    if (now.QuadPart < g_nextFrameQpc.QuadPart) {
        waitMs = (g_nextFrameQpc.QuadPart - now.QuadPart)
                 * 1000 / g_qpcFreq.QuadPart;
        if (waitMs > 1)
            Sleep((DWORD)(waitMs - 1));
        do { QueryPerformanceCounter(&now); }
        while (now.QuadPart < g_nextFrameQpc.QuadPart);
    }

    g_nextFrameQpc.QuadPart += frameTicks;

    QueryPerformanceCounter(&now);
    if (g_nextFrameQpc.QuadPart < now.QuadPart - frameTicks)
        g_nextFrameQpc = now;

    static DWORD throttleCallCount = 0;
    if ((++throttleCallCount % 120) == 1)
        LOG("fps_throttle call=%u limit=%d waitMs=%lld active=%d",
            throttleCallCount, (int)fpsLimit, (long long)waitMs, g_appActive);
}

static void reset_fps_deadline(void)
{
    if (!g_qpcInit)
        return;

    QueryPerformanceCounter(&g_nextFrameQpc);
    g_fpsPacedThisFlip = FALSE;
}

static void log_init(void)
{
#if !DDRAW_WRAPPER_ENABLE_LOGGING
    return;
#else
    char enabled[8] = {0};
    const DWORD enabledLength = GetEnvironmentVariableA(
        "EFZ_DDRAW_LOG",
        enabled,
        (DWORD)sizeof(enabled));
    if (enabledLength != 1 || enabled[0] != '1') {
        return;
    }

    if (!g_logCSInit) {
        InitializeCriticalSection(&g_logCS);
        g_logCSInit = TRUE;
    }
    g_log = fopen("ddraw_wrapper.log", "w");
    if (!g_log) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_log,
        "============================================================\n"
        "  EFZ DirectDraw Wrapper  –  built " __DATE__ "\n"
        "  F11  =  toggle borderless fullscreen\n"
        "  Supports: EFZ 1.11 / BSE 2.13 / BME 3.03 / Memorial 4.00\n"
        "============================================================\n"
        "  Started %04d-%02d-%02d %02d:%02d:%02d\n\n",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond);
    fflush(g_log);
#endif
}

#if !DDRAW_WRAPPER_ENABLE_LOGGING
#else
static void LOG(const char *fmt, ...)
{
    if (!g_log) return;
    EnterCriticalSection(&g_logCS);
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_logCS);
}
#endif

static BYTE *alloc_pixels(int w, int h, int *pitchOut)
{
    int pitch = (w + 3) & ~3;
    *pitchOut = pitch;
    return (BYTE *)calloc((size_t)(pitch * h), 1);
}

static void compute_letterbox(int screenW, int screenH, int gameW, int gameH,
                               int *outX, int *outY, int *outW, int *outH)
{
    int w = screenW;
    int h = (gameW > 0) ? screenW * gameH / gameW : screenH;
    if (h > screenH) {
        h = screenH;
        w = (gameH > 0) ? screenH * gameW / gameH : screenW;
    }
    *outX = (screenW - w) / 2;
    *outY = (screenH - h) / 2;
    *outW = w;
    *outH = h;
}

static int max_integer_scale_for_workarea(int gameW, int gameH)
{
    RECT wa;
    int sx, sy, s;
    if (gameW <= 0 || gameH <= 0) return 1;
    if (!SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0)) {
        wa.left = 0; wa.top = 0;
        wa.right = GetSystemMetrics(SM_CXSCREEN);
        wa.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    sx = (wa.right - wa.left) / gameW;
    sy = (wa.bottom - wa.top) / gameH;
    s = (sx < sy) ? sx : sy;
    if (s < 1) s = 1;
    return s;
}

static void get_primary_work_area(RECT *wa)
{
    if (wa == NULL)
        return;
    if (!SystemParametersInfoA(SPI_GETWORKAREA, 0, wa, 0)) {
        wa->left = 0;
        wa->top = 0;
        wa->right = GetSystemMetrics(SM_CXSCREEN);
        wa->bottom = GetSystemMetrics(SM_CYSCREEN);
    }
}

static int rounded_mul_div_int(int value, int mul, int div)
{
    if (div == 0)
        return value;
    return (int)(((long long)value * (long long)mul + (long long)div / 2) / (long long)div);
}

static void clamp_aspect_client_to_workarea(int *clientW, int *clientH,
                                            int gameW, int gameH,
                                            int borderW, int borderH)
{
    RECT wa;
    int maxClientW;
    int maxClientH;

    if (clientW == NULL || clientH == NULL || gameW <= 0 || gameH <= 0)
        return;

    if (*clientW < gameW)
        *clientW = gameW;
    if (*clientH < gameH)
        *clientH = gameH;

    get_primary_work_area(&wa);
    maxClientW = (wa.right - wa.left) - borderW;
    maxClientH = (wa.bottom - wa.top) - borderH;
    if (maxClientW < gameW)
        maxClientW = gameW;
    if (maxClientH < gameH)
        maxClientH = gameH;

    if (*clientW > maxClientW) {
        *clientW = maxClientW;
        *clientH = rounded_mul_div_int(*clientW, gameH, gameW);
    }
    if (*clientH > maxClientH) {
        *clientH = maxClientH;
        *clientW = rounded_mul_div_int(*clientH, gameW, gameH);
    }

    if (*clientW < gameW) {
        *clientW = gameW;
        *clientH = rounded_mul_div_int(*clientW, gameH, gameW);
    }
    if (*clientH < gameH) {
        *clientH = gameH;
        *clientW = rounded_mul_div_int(*clientH, gameW, gameH);
    }
}

static void aspect_client_from_drag(WPARAM sizingEdge,
                                    int proposedClientW,
                                    int proposedClientH,
                                    int gameW,
                                    int gameH,
                                    int borderW,
                                    int borderH,
                                    int *outClientW,
                                    int *outClientH)
{
    int clientW = proposedClientW;
    int clientH = proposedClientH;

    if (clientW < gameW)
        clientW = gameW;
    if (clientH < gameH)
        clientH = gameH;

    switch (sizingEdge) {
        case WMSZ_LEFT:
        case WMSZ_RIGHT:
            clientH = rounded_mul_div_int(clientW, gameH, gameW);
            break;
        case WMSZ_TOP:
        case WMSZ_BOTTOM:
            clientW = rounded_mul_div_int(clientH, gameW, gameH);
            break;
        default:
            if ((long long)clientW * (long long)gameH > (long long)clientH * (long long)gameW)
                clientW = rounded_mul_div_int(clientH, gameW, gameH);
            else
                clientH = rounded_mul_div_int(clientW, gameH, gameW);
            break;
    }

    clamp_aspect_client_to_workarea(&clientW, &clientH, gameW, gameH, borderW, borderH);
    *outClientW = clientW;
    *outClientH = clientH;
}

/* SetCursor alone cannot undo the game's ShowCursor(FALSE). */
static void apply_cursor_visibility(BOOL show)
{
    static int s_state = -1;
    int want = show ? 1 : 0;
    if (s_state == want)
        return;
    s_state = want;
    if (show) { while (ShowCursor(TRUE)  <  0) {} }
    else      { while (ShowCursor(FALSE) >= 0) {} }
    g_cursorHidden = !show;
}

static void update_cursor_idle(void)
{
    if (EFZDX9_Presenter_ShouldSuppressGameInput()) {
        apply_cursor_visibility(TRUE);
        SetCursor(LoadCursor(NULL, IDC_ARROW));
        return;
    }
    if (!g_cursorHidden && (GetTickCount() - g_lastMouseMoveTick) >= CURSOR_IDLE_HIDE_MS)
        apply_cursor_visibility(FALSE);
}

static DWORD sample_surface_hash(MyDDSurface *src)
{
    DWORD hash = 2166136261u;
    int total;
    int step;

    if (!src || !src->pixels || src->width <= 0 || src->height <= 0)
        return 0;

    total = src->pitch * src->height;
    step = total / 1024;
    if (step <= 0) step = 1;

    for (int i = 0; i < total; i += step) {
        hash ^= src->pixels[i];
        hash *= 16777619u;
    }
    return hash;
}

static void present_frame(MyDDraw *dd)
{
    DWORD presentNo = ++g_presentSerial;
    BOOL traceThisPresent = FALSE;

    if (g_renderTraceAfterFocus && g_renderTraceBudget > 0) {
        traceThisPresent = TRUE;
        g_renderTraceBudget--;
        if (g_renderTraceBudget == 0)
            g_renderTraceAfterFocus = FALSE;
    } else if (presentNo <= 8 || (presentNo % 300) == 0) {
        traceThisPresent = TRUE;
    }

    if (!dd || !dd->hWnd) {
        LOG("Render present skipped frame=%u reason=no-dd-or-hwnd dd=%p",
            presentNo, (void *)dd);
        return;
    }

    MyDDSurface *src = (dd->primarySurface && dd->primarySurface->attachedBackBuffer)
                     ? dd->primarySurface->attachedBackBuffer
                     : dd->primarySurface;
    if (!src || !src->pixels) {
        LOG("Render present skipped frame=%u reason=no-source primary=%p back=%p active=%d focusSerial=%u",
            presentNo,
            dd->primarySurface ? (void *)dd->primarySurface : NULL,
            (dd->primarySurface && dd->primarySurface->attachedBackBuffer)
                ? (void *)dd->primarySurface->attachedBackBuffer : NULL,
            g_appActive, g_focusSerial);
        return;
    }

    struct {
        BITMAPINFOHEADER bmiHeader;
        RGBQUAD          bmiColors[256];
    } bmi;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = src->width;
    bmi.bmiHeader.biHeight      = -(src->height);  /* negative = top-down */
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 8;
    bmi.bmiHeader.biCompression = BI_RGB;
    bmi.bmiHeader.biClrUsed     = 256;

    MyDDPalette *pal = src->palette
                     ? src->palette
                     : (dd->primarySurface ? dd->primarySurface->palette : NULL);
    if (pal)
        memcpy(bmi.bmiColors, pal->entries, 256 * sizeof(RGBQUAD));

    if (g_overlayCallback) {
        EfzDdrawOverlayFrame frame;
        frame.structSize = sizeof(frame);
        frame.pixels = src->pixels;
        frame.width = src->width;
        frame.height = src->height;
        frame.pitch = src->pitch;
        frame.hWnd = dd->hWnd;
        frame.palette = pal ? pal->entries : NULL;
        frame.presentSerial = presentNo;

        __try {
            g_overlayCallback(&frame, g_overlayCallbackUserData);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            LOG("Overlay callback faulted during present frame=%u; disabling callback",
                presentNo);
            g_overlayCallback = NULL;
            g_overlayCallbackUserData = NULL;
        }
    }

    RECT cr;
    GetClientRect(dd->hWnd, &cr);
    int fullW = cr.right  - cr.left;
    int fullH = cr.bottom - cr.top;
    if (fullW <= 0 || fullH <= 0) {
        LOG("Render present skipped frame=%u reason=empty-client client=%dx%d hwnd=%p active=%d focusSerial=%u",
            presentNo, fullW, fullH, (void *)dd->hWnd, g_appActive,
            g_focusSerial);
        return;
    }

    update_cursor_idle();

    int dstX, dstY, dstW, dstH;
    if (src->width > 0 && src->height > 0) {
        compute_letterbox(fullW, fullH, src->width, src->height,
                          &dstX, &dstY, &dstW, &dstH);
    } else {
        dstX = 0; dstY = 0; dstW = fullW; dstH = fullH;
    }

    {
        EfzDx9PresentParams dx9Params;
        dx9Params.window = dd->hWnd;
        dx9Params.pixels = src->pixels;
        dx9Params.width = src->width;
        dx9Params.height = src->height;
        dx9Params.pitch = src->pitch;
        dx9Params.palette = pal ? pal->entries : NULL;
        dx9Params.clientWidth = fullW;
        dx9Params.clientHeight = fullH;
        dx9Params.dstX = dstX;
        dx9Params.dstY = dstY;
        dx9Params.dstW = dstW;
        dx9Params.dstH = dstH;
        dx9Params.borderless = dd->borderless;
        dx9Params.appActive = g_appActive;
        dx9Params.linearScale = &g_cfg.linear_scale;
        dx9Params.presentSerial = presentNo;
        dx9Params.focusSerial = g_focusSerial;
        dx9Params.toggleBorderless = ToggleWindowModeFromPresenter;
        dx9Params.toggleBorderlessUserData = dd;

        if (EFZDX9_Presenter_Present(&dx9Params)) {
            return;
        }
    }

    HDC hDC = GetDC(dd->hWnd);
    if (!hDC) {
        LOG("Render present skipped frame=%u reason=GetDC-failed hwnd=%p active=%d focusSerial=%u lastError=%lu",
            presentNo, (void *)dd->hWnd, g_appActive, g_focusSerial,
            GetLastError());
        return;
    }

    if (src->width > 0 && src->height > 0
        && (dstX > 0 || dstY > 0 || dstX + dstW < fullW || dstY + dstH < fullH)) {
        /* Bars only: a full-screen fill flashes black through DWM. */
        HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
        RECT bar;
        if (dstY > 0) {
            bar.left = 0; bar.top = 0; bar.right = fullW; bar.bottom = dstY;
            FillRect(hDC, &bar, black);
        }
        if (dstY + dstH < fullH) {
            bar.left = 0; bar.top = dstY + dstH; bar.right = fullW; bar.bottom = fullH;
            FillRect(hDC, &bar, black);
        }
        if (dstX > 0) {
            bar.left = 0; bar.top = dstY; bar.right = dstX; bar.bottom = dstY + dstH;
            FillRect(hDC, &bar, black);
        }
        if (dstX + dstW < fullW) {
            bar.left = dstX + dstW; bar.top = dstY; bar.right = fullW; bar.bottom = dstY + dstH;
            FillRect(hDC, &bar, black);
        }
    }

    if (g_cfg.linear_scale) {
        SetStretchBltMode(hDC, HALFTONE);
        SetBrushOrgEx(hDC, 0, 0, NULL);  /* required after HALFTONE */
    } else {
        SetStretchBltMode(hDC, COLORONCOLOR);
    }
    int stretchResult = StretchDIBits(hDC,
                                      dstX, dstY, dstW, dstH,
                                      0, 0, src->width, src->height,
                                      src->pixels,
                                      (BITMAPINFO *)&bmi,
                                      DIB_RGB_COLORS,
                                      SRCCOPY);
    if (traceThisPresent || stretchResult == GDI_ERROR || stretchResult == 0) {
        LOG("Render present frame=%u flip=%u active=%d focusSerial=%u focusAgeMs=%lu hwnd=%p client=%dx%d src=%dx%d pitch=%d dst=%d,%d %dx%d borderless=%d pal=%d hash=%08X StretchDIBits=%d lastError=%lu",
            presentNo, g_flipSerial, g_appActive, g_focusSerial,
            GetTickCount() - g_lastFocusTick, (void *)dd->hWnd,
            fullW, fullH, src->width, src->height, src->pitch,
            dstX, dstY, dstW, dstH, dd->borderless, pal != NULL,
            sample_surface_hash(src), stretchResult, GetLastError());
    }
    ReleaseDC(dd->hWnd, hDC);
}

static void set_windowed_mode(MyDDraw *dd)
{
    HWND hWnd = dd->hWnd;
    if (!hWnd || dd->gameWidth <= 0) return;

    LONG style   = WS_OVERLAPPEDWINDOW;
    LONG exStyle = WS_EX_APPWINDOW;

    SetWindowLongA(hWnd, GWL_STYLE,   style);
    SetWindowLongA(hWnd, GWL_EXSTYLE, exStyle);

    int maxScale = max_integer_scale_for_workarea(dd->gameWidth, dd->gameHeight);
    int scale = (g_cfg.window_scale > 0) ? g_cfg.window_scale : 1;
    if (scale > maxScale) scale = maxScale;
    RECT wr = { 0, 0, dd->gameWidth * scale, dd->gameHeight * scale };
    AdjustWindowRectEx(&wr, style, FALSE, exStyle);
    int winW = wr.right  - wr.left;
    int winH = wr.bottom - wr.top;

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int posX    = (screenW - winW) / 2;
    int posY    = (screenH - winH) / 2;
    if (posX < 0) posX = 0;
    if (posY < 0) posY = 0;

    SetWindowPos(hWnd, HWND_TOP,
                 posX, posY, winW, winH,
                 SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);

    dd->borderless = FALSE;
    LOG("Window mode: windowed %dx%d (game %dx%d) at (%d,%d)",
        winW, winH, dd->gameWidth, dd->gameHeight, posX, posY);
}

static void set_borderless_mode(MyDDraw *dd)
{
    HWND hWnd = dd->hWnd;
    if (!hWnd) return;

    if (!dd->borderless) {
        GetWindowRect(hWnd, &dd->savedWinRect);
        dd->savedStyle   = GetWindowLongA(hWnd, GWL_STYLE);
        dd->savedExStyle = GetWindowLongA(hWnd, GWL_EXSTYLE);
    }

    SetWindowLongA(hWnd, GWL_STYLE,   WS_POPUP | WS_VISIBLE);
    SetWindowLongA(hWnd, GWL_EXSTYLE, WS_EX_APPWINDOW);

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    SetWindowPos(hWnd, HWND_TOP,
                 0, 0, screenW, screenH,
                 SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);

    dd->borderless = TRUE;
    LOG("Window mode: borderless %dx%d", screenW, screenH);
}

static void toggle_window_mode(MyDDraw *dd)
{
    if (dd->borderless)
        set_windowed_mode(dd);
    else
        set_borderless_mode(dd);
}

static void __stdcall ToggleWindowModeFromPresenter(void *userData)
{
    MyDDraw *dd = (MyDDraw *)userData;
    if (dd != NULL)
        toggle_window_mode(dd);
}

static LRESULT CALLBACK LLKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
        KBDLLHOOKSTRUCT *kb = (KBDLLHOOKSTRUCT *)lParam;
        if (kb->vkCode == VK_F11 && g_dd && g_appActive &&
            GetForegroundWindow() == g_dd->hWnd) {
            toggle_window_mode(g_dd);
            return 1;
        }
    }
    return CallNextHookEx(g_kbHook, nCode, wParam, lParam);
}

static LRESULT CALLBACK WrapperWndProc(HWND hWnd, UINT msg,
                                        WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_ACTIVATE) {
        BOOL newActive = (LOWORD(wParam) != WA_INACTIVE);
        if (newActive != g_appActive || LOWORD(wParam) == WA_INACTIVE) {
            g_focusSerial++;
            g_lastFocusTick = GetTickCount();
            g_renderTraceAfterFocus = TRUE;
            g_renderTraceBudget = 20;
            g_wndTraceBudget = 80;
            LOG("Focus WM_ACTIVATE serial=%u active %d->%d state=%u minimized=%u hwnd=%p other=%p fg=%p",
                g_focusSerial, g_appActive, newActive, LOWORD(wParam),
                HIWORD(wParam), (void *)hWnd, (void *)lParam,
                (void *)GetForegroundWindow());
            reset_fps_deadline();
        }
        g_appActive = newActive;
        EFZDX9_Presenter_SetActive(newActive);
        if (g_dd && g_dd->origWndProc)
            return CallWindowProcA(g_dd->origWndProc, hWnd, msg, wParam, lParam);
        return DefWindowProcA(hWnd, msg, wParam, lParam);
    } else if (msg == WM_ACTIVATEAPP) {
        BOOL newActive = (wParam != FALSE);
        if (newActive != g_appActive || !newActive) {
            g_focusSerial++;
            g_lastFocusTick = GetTickCount();
            g_renderTraceAfterFocus = TRUE;
            g_renderTraceBudget = 20;
            g_wndTraceBudget = 80;
            LOG("Focus WM_ACTIVATEAPP serial=%u active %d->%d thread=%lu hwnd=%p fg=%p",
                g_focusSerial, g_appActive, newActive, (DWORD)lParam,
                (void *)hWnd, (void *)GetForegroundWindow());
            reset_fps_deadline();
        }
        g_appActive = newActive;
        EFZDX9_Presenter_SetActive(newActive);
        if (g_dd && g_dd->origWndProc)
            return CallWindowProcA(g_dd->origWndProc, hWnd, msg, wParam, lParam);
        return DefWindowProcA(hWnd, msg, wParam, lParam);
    } else if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS) {
        LOG("Focus %s active=%d hwnd=%p other=%p fg=%p focusSerial=%u",
            msg == WM_SETFOCUS ? "WM_SETFOCUS" : "WM_KILLFOCUS",
            g_appActive, (void *)hWnd, (void *)wParam,
            (void *)GetForegroundWindow(), g_focusSerial);
        if (g_dd && g_dd->origWndProc)
            return CallWindowProcA(g_dd->origWndProc, hWnd, msg, wParam, lParam);
        return DefWindowProcA(hWnd, msg, wParam, lParam);
    } else if (msg == WM_PAINT && g_renderTraceAfterFocus) {
        LOG("Render WM_PAINT after-focus active=%d hwnd=%p primary=%p focusSerial=%u",
            g_appActive, (void *)hWnd,
            g_dd ? (void *)g_dd->primarySurface : NULL, g_focusSerial);
    } else if ((msg == WM_SIZE || msg == WM_WINDOWPOSCHANGED) && g_renderTraceAfterFocus) {
        LOG("Window msg=%s active=%d hwnd=%p wParam=%08X lParam=%08X focusSerial=%u",
            msg == WM_SIZE ? "WM_SIZE" : "WM_WINDOWPOSCHANGED",
            g_appActive, (void *)hWnd, (DWORD)wParam, (DWORD)lParam,
            g_focusSerial);
    } else if (g_wndTraceBudget > 0) {
        LOG("Window msg after-focus msg=%04X active=%d hwnd=%p wParam=%08X lParam=%08X focusSerial=%u",
            msg, g_appActive, (void *)hWnd, (DWORD)wParam, (DWORD)lParam,
            g_focusSerial);
        g_wndTraceBudget--;
    }

    if (msg == WM_MOUSEMOVE) {
        g_lastMouseMoveTick = GetTickCount();
        apply_cursor_visibility(TRUE);
    }

    if (msg == WM_SETCURSOR && LOWORD(lParam) == HTCLIENT && g_dd
        && EFZDX9_Presenter_ShouldSuppressGameInput()) {
        apply_cursor_visibility(TRUE);
        SetCursor(LoadCursor(NULL, IDC_ARROW));
        return TRUE;
    }

    if (msg == WM_SETCURSOR && LOWORD(lParam) == HTCLIENT && g_dd
        && !EFZDX9_Presenter_ShouldSuppressGameInput()) {
        /* TRUE stops the game's WndProc re-applying its NULL class cursor. */
        SetCursor(LoadCursor(NULL, IDC_ARROW));
        if ((GetTickCount() - g_lastMouseMoveTick) < CURSOR_IDLE_HIDE_MS)
            apply_cursor_visibility(TRUE);
        return TRUE;
    }

    if (msg == WM_SIZING && g_dd && !g_dd->borderless) {
        RECT *wr = (RECT *)lParam;
        int gameW = g_dd->gameWidth  > 0 ? g_dd->gameWidth  : 640;
        int gameH = g_dd->gameHeight > 0 ? g_dd->gameHeight : 480;
        LONG style   = GetWindowLongA(hWnd, GWL_STYLE);
        LONG exStyle = GetWindowLongA(hWnd, GWL_EXSTYLE);
        RECT br = { 0, 0, 0, 0 };
        AdjustWindowRectEx(&br, style, FALSE, exStyle);
        int borderW = br.right - br.left;
        int borderH = br.bottom - br.top;
        int propW = (wr->right - wr->left) - borderW;
        int propH = (wr->bottom - wr->top) - borderH;
        int clientW = gameW;
        int clientH = gameH;
        aspect_client_from_drag(wParam, propW, propH, gameW, gameH, borderW, borderH,
                                &clientW, &clientH);
        {
            int newW = clientW + borderW;
            int newH = clientH + borderH;
            switch (wParam) {
                case WMSZ_LEFT: case WMSZ_TOPLEFT: case WMSZ_BOTTOMLEFT:
                    wr->left = wr->right - newW; break;
                default:
                    wr->right = wr->left + newW; break;
            }
            switch (wParam) {
                case WMSZ_TOP: case WMSZ_TOPLEFT: case WMSZ_TOPRIGHT:
                    wr->top = wr->bottom - newH; break;
                default:
                    wr->bottom = wr->top + newH; break;
            }
        }
        return TRUE;
    }

    if (msg == WM_GETMINMAXINFO && g_dd && !g_dd->borderless) {
        MINMAXINFO *mmi = (MINMAXINFO *)lParam;
        int gameW = g_dd->gameWidth  > 0 ? g_dd->gameWidth  : 640;
        int gameH = g_dd->gameHeight > 0 ? g_dd->gameHeight : 480;
        RECT wr = { 0, 0, gameW, gameH };
        AdjustWindowRectEx(&wr, GetWindowLongA(hWnd, GWL_STYLE), FALSE,
                           GetWindowLongA(hWnd, GWL_EXSTYLE));
        mmi->ptMinTrackSize.x = wr.right - wr.left;
        mmi->ptMinTrackSize.y = wr.bottom - wr.top;
        return 0;
    }

    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && wParam == VK_F1 && g_dd && g_appActive && GetForegroundWindow() == g_dd->hWnd) {
        if ((lParam & (1UL << 30)) == 0) {
            EFZDX9_Presenter_ToggleMenu();
            apply_cursor_visibility(TRUE);
            SetCursor(LoadCursor(NULL, IDC_ARROW));
        }
        return 0;
    }

    {
        LRESULT imguiResult = 0;
        if (EFZDX9_Presenter_HandleWindowMessage(hWnd, msg, wParam, lParam, &imguiResult))
            return imguiResult;
    }

    if (msg == WM_SYSCOMMAND) {
        WPARAM cmd = wParam & 0xFFF0;
        if (cmd == SC_SCREENSAVE || cmd == SC_MONITORPOWER)
            return 0;
        if (cmd == SC_KEYMENU)
            return DefWindowProcA(hWnd, msg, wParam, lParam);
    }

    if (msg == WM_ERASEBKGND) return 1;

    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);

        if (!g_hasEverFlipped && g_dd && g_dd->primarySurface)
            present_frame(g_dd);
        (void)hdc;
        EndPaint(hWnd, &ps);
        return 0;
    }
    if (g_dd && g_dd->origWndProc)
        return CallWindowProcA(g_dd->origWndProc, hWnd, msg, wParam, lParam);
    return DefWindowProcA(hWnd, msg, wParam, lParam);
}

static ULONG   __stdcall DDPal_AddRef(MyDDPalette *p)
    { return (ULONG)InterlockedIncrement(&p->refCount); }

static ULONG   __stdcall DDPal_Release(MyDDPalette *p)
{
    LONG ref = InterlockedDecrement(&p->refCount);
    if (ref == 0) { LOG("IDirectDrawPalette freed"); free(p); }
    return (ULONG)ref;
}

static HRESULT __stdcall DDPal_QueryInterface(MyDDPalette *p, REFIID r, void **ppv)
    { *ppv = p; DDPal_AddRef(p); return DD_OK; }

static HRESULT __stdcall DDPal_GetCaps(MyDDPalette *p, DWORD *pdwCaps)
    { if (pdwCaps) *pdwCaps = DDPCAPS_8BIT | DDPCAPS_ALLOW256; return DD_OK; }

static HRESULT __stdcall DDPal_GetEntries(MyDDPalette *p,
    DWORD dwFlags, DWORD dwBase, DWORD dwNumEntries, PALETTEENTRY *pEntries)
{
    for (DWORD i = 0; i < dwNumEntries && (dwBase + i) < 256; i++) {
        pEntries[i].peRed   = p->entries[dwBase + i].rgbRed;
        pEntries[i].peGreen = p->entries[dwBase + i].rgbGreen;
        pEntries[i].peBlue  = p->entries[dwBase + i].rgbBlue;
        pEntries[i].peFlags = 0;
    }
    return DD_OK;
}

static HRESULT __stdcall DDPal_Initialize(MyDDPalette *p, void *pDD,
    DWORD dwFlags, PALETTEENTRY *pEntries) { return DDERR_UNSUPPORTED; }

static HRESULT __stdcall DDPal_SetEntries(MyDDPalette *p,
    DWORD dwFlags, DWORD dwBase, DWORD dwNumEntries, PALETTEENTRY *pEntries)
{
    for (DWORD i = 0; i < dwNumEntries && (dwBase + i) < 256; i++) {
        p->entries[dwBase + i].rgbRed      = pEntries[i].peRed;
        p->entries[dwBase + i].rgbGreen    = pEntries[i].peGreen;
        p->entries[dwBase + i].rgbBlue     = pEntries[i].peBlue;
        p->entries[dwBase + i].rgbReserved = 0;
    }
    return DD_OK;
}

/* IDirectDrawPalette vtable  (7 entries, SDK order) */
static void *g_DDPalVtbl[] = {
    (void *)DDPal_QueryInterface,
    (void *)DDPal_AddRef,
    (void *)DDPal_Release,
    (void *)DDPal_GetCaps,
    (void *)DDPal_GetEntries,
    (void *)DDPal_Initialize,
    (void *)DDPal_SetEntries,
};

static HRESULT __stdcall DDSurf_QueryInterface(MyDDSurface *, REFIID, void **);
static ULONG   __stdcall DDSurf_AddRef(MyDDSurface *);
static ULONG   __stdcall DDSurf_Release(MyDDSurface *);
static HRESULT __stdcall DDSurf_AddAttachedSurface(MyDDSurface *, MyDDSurface *);
static HRESULT __stdcall DDSurf_AddOverlayDirtyRect(MyDDSurface *, RECT *);
static HRESULT __stdcall DDSurf_Blt(MyDDSurface *, RECT *, MyDDSurface *, RECT *, DWORD, void *);
static HRESULT __stdcall DDSurf_BltBatch(MyDDSurface *, void *, DWORD, DWORD);
static HRESULT __stdcall DDSurf_BltFast(MyDDSurface *, DWORD, DWORD, MyDDSurface *, RECT *, DWORD);
static HRESULT __stdcall DDSurf_DeleteAttachedSurface(MyDDSurface *, DWORD, MyDDSurface *);
static HRESULT __stdcall DDSurf_EnumAttachedSurfaces(MyDDSurface *, void *, void *);
static HRESULT __stdcall DDSurf_EnumOverlayZOrders(MyDDSurface *, DWORD, void *, void *);
static HRESULT __stdcall DDSurf_Flip(MyDDSurface *, MyDDSurface *, DWORD);
static HRESULT __stdcall DDSurf_GetAttachedSurface(MyDDSurface *, MY_DDSCAPS *, MyDDSurface **);
static HRESULT __stdcall DDSurf_GetBltStatus(MyDDSurface *, DWORD);
static HRESULT __stdcall DDSurf_GetCaps(MyDDSurface *, MY_DDSCAPS *);
static HRESULT __stdcall DDSurf_GetClipper(MyDDSurface *, void **);
static HRESULT __stdcall DDSurf_GetColorKey(MyDDSurface *, DWORD, MY_DDCOLORKEY *);
static HRESULT __stdcall DDSurf_GetDC(MyDDSurface *, HDC *);
static HRESULT __stdcall DDSurf_GetFlipStatus(MyDDSurface *, DWORD);
static HRESULT __stdcall DDSurf_GetOverlayPosition(MyDDSurface *, LONG *, LONG *);
static HRESULT __stdcall DDSurf_GetPalette(MyDDSurface *, MyDDPalette **);
static HRESULT __stdcall DDSurf_GetPixelFormat(MyDDSurface *, MY_DDPIXELFORMAT *);
static HRESULT __stdcall DDSurf_GetSurfaceDesc(MyDDSurface *, MY_DDSURFACEDESC *);
static HRESULT __stdcall DDSurf_Initialize(MyDDSurface *, void *, MY_DDSURFACEDESC *);
static HRESULT __stdcall DDSurf_IsLost(MyDDSurface *);
static HRESULT __stdcall DDSurf_Lock(MyDDSurface *, RECT *, MY_DDSURFACEDESC *, DWORD, HANDLE);
static HRESULT __stdcall DDSurf_ReleaseDC(MyDDSurface *, HDC);
static HRESULT __stdcall DDSurf_Restore(MyDDSurface *);
static HRESULT __stdcall DDSurf_SetClipper(MyDDSurface *, void *);
static HRESULT __stdcall DDSurf_SetColorKey(MyDDSurface *, DWORD, MY_DDCOLORKEY *);
static HRESULT __stdcall DDSurf_SetOverlayPosition(MyDDSurface *, LONG, LONG);
static HRESULT __stdcall DDSurf_SetPalette(MyDDSurface *, MyDDPalette *);
static HRESULT __stdcall DDSurf_Unlock(MyDDSurface *, void *);
static HRESULT __stdcall DDSurf_UpdateOverlay(MyDDSurface *, RECT *, MyDDSurface *, RECT *, DWORD, void *);
static HRESULT __stdcall DDSurf_UpdateOverlayDisplay(MyDDSurface *, DWORD);
static HRESULT __stdcall DDSurf_UpdateOverlayZOrder(MyDDSurface *, DWORD, MyDDSurface *);

/* 36 entries, exact SDK order. */
static void *g_DDSurfVtbl[] = {
    (void *)DDSurf_QueryInterface,
    (void *)DDSurf_AddRef,
    (void *)DDSurf_Release,
    (void *)DDSurf_AddAttachedSurface,
    (void *)DDSurf_AddOverlayDirtyRect,
    (void *)DDSurf_Blt,
    (void *)DDSurf_BltBatch,
    (void *)DDSurf_BltFast,
    (void *)DDSurf_DeleteAttachedSurface,
    (void *)DDSurf_EnumAttachedSurfaces,
    (void *)DDSurf_EnumOverlayZOrders,
    (void *)DDSurf_Flip,
    (void *)DDSurf_GetAttachedSurface,
    (void *)DDSurf_GetBltStatus,
    (void *)DDSurf_GetCaps,
    (void *)DDSurf_GetClipper,
    (void *)DDSurf_GetColorKey,
    (void *)DDSurf_GetDC,
    (void *)DDSurf_GetFlipStatus,
    (void *)DDSurf_GetOverlayPosition,
    (void *)DDSurf_GetPalette,
    (void *)DDSurf_GetPixelFormat,
    (void *)DDSurf_GetSurfaceDesc,
    (void *)DDSurf_Initialize,
    (void *)DDSurf_IsLost,
    (void *)DDSurf_Lock,
    (void *)DDSurf_ReleaseDC,
    (void *)DDSurf_Restore,
    (void *)DDSurf_SetClipper,
    (void *)DDSurf_SetColorKey,
    (void *)DDSurf_SetOverlayPosition,
    (void *)DDSurf_SetPalette,
    (void *)DDSurf_Unlock,
    (void *)DDSurf_UpdateOverlay,
    (void *)DDSurf_UpdateOverlayDisplay,
    (void *)DDSurf_UpdateOverlayZOrder,
};

static MyDDSurface *surf_create(MyDDraw *dd, int w, int h,
                                 BOOL isPrimary, BOOL isBackBuffer)
{
    MyDDSurface *s = (MyDDSurface *)calloc(1, sizeof(MyDDSurface));
    if (!s) return NULL;
    s->lpVtbl      = g_DDSurfVtbl;
    s->refCount    = 1;
    s->width       = w;
    s->height      = h;
    s->pixels      = alloc_pixels(w, h, &s->pitch);
    s->isPrimary   = isPrimary;
    s->isBackBuffer = isBackBuffer;
    s->owner       = dd;
    return s;
}

static ULONG __stdcall DDSurf_AddRef(MyDDSurface *s)
    { return (ULONG)InterlockedIncrement(&s->refCount); }

static ULONG __stdcall DDSurf_Release(MyDDSurface *s)
{
    LONG ref = InterlockedDecrement(&s->refCount);
    if (ref == 0) {
        LOG("IDirectDrawSurface freed (%s %dx%d)",
            s->isPrimary ? "primary" : s->isBackBuffer ? "backbuf" : "offscreen",
            s->width, s->height);
        /* Release attached back-buffer (holds one implicit ref) */
        if (s->attachedBackBuffer) {
            DDSurf_Release(s->attachedBackBuffer);
            s->attachedBackBuffer = NULL;
        }
        if (s->palette) {
            DDPal_Release(s->palette);
            s->palette = NULL;
        }
        if (s->isPrimary && s->owner && s->owner->primarySurface == s)
            s->owner->primarySurface = NULL;
        free(s->pixels);
        free(s);
    }
    return (ULONG)ref;
}

static HRESULT __stdcall DDSurf_QueryInterface(MyDDSurface *s, REFIID r, void **ppv)
    { *ppv = s; DDSurf_AddRef(s); return DD_OK; }

static HRESULT __stdcall DDSurf_AddAttachedSurface(MyDDSurface *s, MyDDSurface *surf)
    { return DD_OK; }
static HRESULT __stdcall DDSurf_AddOverlayDirtyRect(MyDDSurface *s, RECT *r)
    { return DD_OK; }

static HRESULT __stdcall DDSurf_Blt(MyDDSurface *dst, RECT *destRect,
                                     MyDDSurface *src, RECT *srcRect,
                                     DWORD flags, void *pFX)
{
    /* Color fill (pFX is DDBLTFX; dwFillColor sits at byte offset 76) */
    if (flags & DDBLT_COLORFILL) {
        BYTE fill = pFX ? (BYTE)(*(DWORD *)((BYTE *)pFX + 76)) : 0;
        RECT dr = { 0, 0, dst->width, dst->height };
        if (destRect) dr = *destRect;
        for (int y = dr.top; y < dr.bottom && y < dst->height; y++) {
            int left = dr.left  < 0 ? 0 : dr.left;
            int right= dr.right > dst->width ? dst->width : dr.right;
            if (right > left)
                memset(dst->pixels + y * dst->pitch + left, fill, right - left);
        }
        return DD_OK;
    }
    if (!src) return DDERR_INVALIDPARAMS;

    RECT sr = { 0, 0, src->width, src->height };
    RECT dr = { 0, 0, dst->width, dst->height };
    if (srcRect) sr = *srcRect;
    if (destRect) dr = *destRect;

    int sw = sr.right - sr.left;
    int sh = sr.bottom - sr.top;
    int dw = dr.right  - dr.left;
    int dh = dr.bottom - dr.top;
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return DD_OK;

    BOOL ck = (flags & DDBLT_KEYSRC) && src->hasColorKeySrc;
    DWORD ckLo = src->colorKeySrcLow, ckHi = src->colorKeySrcHigh;

    for (int y = 0; y < dh; y++) {
        int sy = sr.top + y * sh / dh;
        if (sy < 0 || sy >= src->height) continue;
        for (int x = 0; x < dw; x++) {
            int sx = sr.left + x * sw / dw;
            if (sx < 0 || sx >= src->width) continue;
            BYTE px = src->pixels[sy * src->pitch + sx];
            if (ck && (DWORD)px >= ckLo && (DWORD)px <= ckHi) continue;
            int dy = dr.top + y, dx = dr.left + x;
            if (dy >= 0 && dy < dst->height && dx >= 0 && dx < dst->width)
                dst->pixels[dy * dst->pitch + dx] = px;
        }
    }
    return DD_OK;
}

static HRESULT __stdcall DDSurf_BltBatch(MyDDSurface *s, void *pBatch,
                                          DWORD dwCount, DWORD dwFlags)
    { return DD_OK; }

static HRESULT __stdcall DDSurf_BltFast(MyDDSurface *dst, DWORD destX, DWORD destY,
                                         MyDDSurface *src, RECT *srcRect, DWORD flags)
{
    if (!src) return DDERR_INVALIDPARAMS;

    RECT sr = { 0, 0, src->width, src->height };
    if (srcRect) sr = *srcRect;
    int sw = sr.right - sr.left;
    int sh = sr.bottom - sr.top;
    if (sw <= 0 || sh <= 0) return DD_OK;

    BOOL ck  = (flags & DDBLTFAST_SRCCOLORKEY) && src->hasColorKeySrc;
    DWORD lo = src->colorKeySrcLow, hi = src->colorKeySrcHigh;

    for (int y = 0; y < sh; y++) {
        int dy = (int)destY + y;
        if (dy < 0 || dy >= dst->height) continue;
        const BYTE *sRow = src->pixels + (sr.top + y) * src->pitch + sr.left;
        BYTE       *dRow = dst->pixels + dy * dst->pitch + destX;

        if (!ck) {
            int copyW = sw;
            if ((int)destX + copyW > dst->width)
                copyW = dst->width - (int)destX;
            if (copyW > 0)
                memcpy(dRow, sRow, (size_t)copyW);
        } else {
            for (int x = 0; x < sw; x++) {
                if ((int)destX + x >= dst->width) break;
                BYTE px = sRow[x];
                if ((DWORD)px >= lo && (DWORD)px <= hi) continue;
                dRow[x] = px;
            }
        }
    }
    return DD_OK;
}

static HRESULT __stdcall DDSurf_DeleteAttachedSurface(MyDDSurface *s,
    DWORD dwFlags, MyDDSurface *surf) { return DD_OK; }
static HRESULT __stdcall DDSurf_EnumAttachedSurfaces(MyDDSurface *s,
    void *pCtx, void *pfn) { return DD_OK; }
static HRESULT __stdcall DDSurf_EnumOverlayZOrders(MyDDSurface *s,
    DWORD dwFlags, void *pCtx, void *pfn) { return DD_OK; }

static HRESULT __stdcall DDSurf_Flip(MyDDSurface *s, MyDDSurface *target, DWORD flags)
{
    DWORD flipNo = ++g_flipSerial;

    if (flipNo <= 8 || g_renderTraceAfterFocus || (flipNo % 300) == 0) {
        LOG("DDSurf_Flip flip=%u surf=%p primary=%d owner=%p target=%p flags=%08X active=%d focusSerial=%u",
            flipNo, (void *)s, s ? s->isPrimary : 0,
            s ? (void *)s->owner : NULL, (void *)target, flags,
            g_appActive, g_focusSerial);
    }

    if (s && s->isPrimary && s->owner) {
        if (!g_fpsPacedThisFlip)
            fps_throttle();
        g_fpsPacedThisFlip = FALSE;

        g_hasEverFlipped = TRUE;
        present_frame(s->owner);
    }
    else if (flipNo <= 8 || g_renderTraceAfterFocus) {
        LOG("DDSurf_Flip skipped-present flip=%u reason=%s",
            flipNo, !s ? "null-surface" :
            !s->isPrimary ? "not-primary" : "no-owner");
    }
    return DD_OK;
}

static HRESULT __stdcall DDSurf_GetAttachedSurface(MyDDSurface *s,
    MY_DDSCAPS *pCaps, MyDDSurface **ppSurf)
{
    if (!ppSurf) return DDERR_INVALIDPARAMS;
    if (s->attachedBackBuffer) {
        DDSurf_AddRef(s->attachedBackBuffer);
        *ppSurf = s->attachedBackBuffer;
        LOG("GetAttachedSurface → back buffer %dx%d",
            s->attachedBackBuffer->width, s->attachedBackBuffer->height);
        return DD_OK;
    }
    *ppSurf = NULL;
    return DDERR_GENERIC;
}

static HRESULT __stdcall DDSurf_GetBltStatus(MyDDSurface *s, DWORD f)
    { return DD_OK; }

static HRESULT __stdcall DDSurf_GetCaps(MyDDSurface *s, MY_DDSCAPS *pCaps)
{
    if (pCaps)
        pCaps->dwCaps = s->isPrimary
                      ? (DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP |
                         DDSCAPS_COMPLEX | DDSCAPS_SYSTEMMEMORY)
                      : s->isBackBuffer
                      ? (DDSCAPS_BACKBUFFER | DDSCAPS_FLIP | DDSCAPS_SYSTEMMEMORY)
                      : (DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY);
    return DD_OK;
}

static HRESULT __stdcall DDSurf_GetClipper(MyDDSurface *s, void **pp)
    { return DDERR_UNSUPPORTED; }

static HRESULT __stdcall DDSurf_GetColorKey(MyDDSurface *s,
    DWORD dwFlags, MY_DDCOLORKEY *pCK)
{
    if (pCK && (dwFlags & DDCKEY_SRCBLT) && s->hasColorKeySrc) {
        pCK->dwLow  = s->colorKeySrcLow;
        pCK->dwHigh = s->colorKeySrcHigh;
    }
    return DD_OK;
}

static HRESULT __stdcall DDSurf_GetDC(MyDDSurface *s, HDC *phDC)
    { if (phDC) *phDC = NULL; return DDERR_UNSUPPORTED; }

static HRESULT __stdcall DDSurf_GetFlipStatus(MyDDSurface *s, DWORD f)
    { return DD_OK; }
static HRESULT __stdcall DDSurf_GetOverlayPosition(MyDDSurface *s, LONG *px, LONG *py)
    { return DDERR_UNSUPPORTED; }

static HRESULT __stdcall DDSurf_GetPalette(MyDDSurface *s, MyDDPalette **pp)
{
    if (!pp) return DDERR_INVALIDPARAMS;
    *pp = s->palette;
    if (s->palette) DDPal_AddRef(s->palette);
    return DD_OK;
}

static HRESULT __stdcall DDSurf_GetPixelFormat(MyDDSurface *s, MY_DDPIXELFORMAT *pFmt)
{
    if (!pFmt) return DDERR_INVALIDPARAMS;
    ZeroMemory(pFmt, sizeof(*pFmt));
    pFmt->dwSize        = sizeof(MY_DDPIXELFORMAT);
    pFmt->dwFlags       = 0x00000200;   /* DDPF_PALETTEINDEXED8 */
    pFmt->dwRGBBitCount = 8;
    return DD_OK;
}

static HRESULT __stdcall DDSurf_GetSurfaceDesc(MyDDSurface *s, MY_DDSURFACEDESC *pDesc)
{
    if (!pDesc || pDesc->dwSize < sizeof(MY_DDSURFACEDESC)) return DDERR_INVALIDPARAMS;
    pDesc->dwFlags  = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_CAPS;
    pDesc->dwWidth  = (DWORD)s->width;
    pDesc->dwHeight = (DWORD)s->height;
    pDesc->lPitch   = s->pitch;
    pDesc->ddsCaps.dwCaps = s->isPrimary ? DDSCAPS_PRIMARYSURFACE
                          : s->isBackBuffer ? DDSCAPS_BACKBUFFER
                          : DDSCAPS_OFFSCREENPLAIN;
    if (s->locked) {
        pDesc->dwFlags   |= DDSD_LPSURFACE;
        pDesc->lpSurface  = (DWORD)(ULONG_PTR)s->pixels;
    }
    return DD_OK;
}

static HRESULT __stdcall DDSurf_Initialize(MyDDSurface *s, void *pDD,
    MY_DDSURFACEDESC *pDesc) { return DDERR_UNSUPPORTED; }

static HRESULT __stdcall DDSurf_IsLost(MyDDSurface *s) { return DD_OK; }

static HRESULT __stdcall DDSurf_Lock(MyDDSurface *s, RECT *pRect,
                                      MY_DDSURFACEDESC *pDesc,
                                      DWORD dwFlags, HANDLE hEvent)
{
    if (!pDesc) return DDERR_INVALIDPARAMS;
    if (s->locked) return DDERR_ALREADYLOCKED;

    s->locked = TRUE;
    pDesc->dwFlags  |= DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_LPSURFACE;
    pDesc->dwWidth   = (DWORD)s->width;
    pDesc->dwHeight  = (DWORD)s->height;
    pDesc->lPitch    = s->pitch;
    pDesc->lpSurface = (DWORD)(ULONG_PTR)s->pixels;

    if (pRect) {
        pDesc->dwWidth   = (DWORD)(pRect->right  - pRect->left);
        pDesc->dwHeight  = (DWORD)(pRect->bottom - pRect->top);
        pDesc->lpSurface = (DWORD)(ULONG_PTR)(
            s->pixels + (size_t)pRect->top * s->pitch + pRect->left);
    }
    return DD_OK;
}

static HRESULT __stdcall DDSurf_ReleaseDC(MyDDSurface *s, HDC hDC)
    { return DD_OK; }
static HRESULT __stdcall DDSurf_Restore(MyDDSurface *s)
    { return DD_OK; }
static HRESULT __stdcall DDSurf_SetClipper(MyDDSurface *s, void *pClipper)
    { return DD_OK; }

static HRESULT __stdcall DDSurf_SetColorKey(MyDDSurface *s,
    DWORD dwFlags, MY_DDCOLORKEY *pCK)
{
    if (!pCK) return DDERR_INVALIDPARAMS;
    if (dwFlags & DDCKEY_SRCBLT) {
        s->hasColorKeySrc  = TRUE;
        s->colorKeySrcLow  = pCK->dwLow;
        s->colorKeySrcHigh = pCK->dwHigh;
        LOG("SetColorKey SRCBLT [%u, %u] on %s surface",
            pCK->dwLow, pCK->dwHigh,
            s->isPrimary ? "primary" : s->isBackBuffer ? "backbuf" : "offscreen");
    }
    return DD_OK;
}

static HRESULT __stdcall DDSurf_SetOverlayPosition(MyDDSurface *s, LONG x, LONG y)
    { return DDERR_UNSUPPORTED; }

static HRESULT __stdcall DDSurf_SetPalette(MyDDSurface *s, MyDDPalette *pPal)
{
    if (s->palette)  DDPal_Release(s->palette);
    s->palette = pPal;
    if (pPal) DDPal_AddRef(pPal);
    LOG("SetPalette on %s surface",
        s->isPrimary ? "primary" : s->isBackBuffer ? "backbuf" : "offscreen");
    return DD_OK;
}

static HRESULT __stdcall DDSurf_Unlock(MyDDSurface *s, void *pSurfData)
{
    s->locked = FALSE;
    return DD_OK;
}

static HRESULT __stdcall DDSurf_UpdateOverlay(MyDDSurface *s, RECT *sr,
    MyDDSurface *dst, RECT *dr, DWORD f, void *pFX) { return DDERR_UNSUPPORTED; }
static HRESULT __stdcall DDSurf_UpdateOverlayDisplay(MyDDSurface *s, DWORD f)
    { return DDERR_UNSUPPORTED; }
static HRESULT __stdcall DDSurf_UpdateOverlayZOrder(MyDDSurface *s,
    DWORD f, MyDDSurface *ref) { return DDERR_UNSUPPORTED; }

static ULONG __stdcall DD_AddRef(MyDDraw *dd)
    { return (ULONG)InterlockedIncrement(&dd->refCount); }

static ULONG __stdcall DD_Release(MyDDraw *dd)
{
    LONG ref = InterlockedDecrement(&dd->refCount);
    if (ref == 0) {
        LOG("IDirectDraw freed");
        if (dd->primarySurface) {
            DDSurf_Release(dd->primarySurface);
            dd->primarySurface = NULL;
        }
        if (dd == g_dd) g_dd = NULL;
        free(dd);
    }
    return (ULONG)ref;
}

static HRESULT __stdcall DD_QueryInterface(MyDDraw *dd, REFIID r, void **ppv)
    { *ppv = dd; DD_AddRef(dd); return DD_OK; }

static HRESULT __stdcall DD_Compact(MyDDraw *dd) { return DD_OK; }

static HRESULT __stdcall DD_CreateClipper(MyDDraw *dd, DWORD dwFlags,
    void **ppClipper, void *pUnk)
    { if (ppClipper) *ppClipper = NULL; return DDERR_UNSUPPORTED; }

static HRESULT __stdcall DD_CreatePalette(MyDDraw *dd, DWORD dwFlags,
    PALETTEENTRY *pEntries, MyDDPalette **ppPal, void *pUnk)
{
    if (!ppPal) return DDERR_INVALIDPARAMS;
    MyDDPalette *pal = (MyDDPalette *)calloc(1, sizeof(MyDDPalette));
    if (!pal) return DDERR_OUTOFMEMORY;
    pal->lpVtbl   = g_DDPalVtbl;
    pal->refCount = 1;
    if (pEntries) {
        for (int i = 0; i < 256; i++) {
            pal->entries[i].rgbRed      = pEntries[i].peRed;
            pal->entries[i].rgbGreen    = pEntries[i].peGreen;
            pal->entries[i].rgbBlue     = pEntries[i].peBlue;
            pal->entries[i].rgbReserved = 0;
        }
    }
    *ppPal = pal;
    LOG("CreatePalette flags=%08X", dwFlags);
    return DD_OK;
}

static HRESULT __stdcall DD_CreateSurface(MyDDraw *dd, MY_DDSURFACEDESC *pDesc,
    MyDDSurface **ppSurf, void *pUnk)
{
    if (!pDesc || !ppSurf) return DDERR_INVALIDPARAMS;

    DWORD caps = pDesc->ddsCaps.dwCaps;

    int w = (int)pDesc->dwWidth;
    int h = (int)pDesc->dwHeight;

    if (caps & DDSCAPS_PRIMARYSURFACE) {
        if (dd->gameWidth  > 0) w = dd->gameWidth;
        if (dd->gameHeight > 0) h = dd->gameHeight;
        if (w <= 0) w = 640;
        if (h <= 0) h = 480;

        MyDDSurface *primary = surf_create(dd, w, h, TRUE, FALSE);
        if (!primary) return DDERR_OUTOFMEMORY;

        DWORD backCount = pDesc->dwBackBufferCount ? pDesc->dwBackBufferCount : 1;
        if ((caps & DDSCAPS_FLIP) && backCount > 0) {
            MyDDSurface *back = surf_create(dd, w, h, FALSE, TRUE);
            if (!back) { free(primary->pixels); free(primary); return DDERR_OUTOFMEMORY; }
            primary->attachedBackBuffer = back;
        }
        dd->primarySurface = primary;
        *ppSurf = primary;
        LOG("CreateSurface primary %dx%d caps=%08X backCount=%u", w, h, caps, backCount);
        return DD_OK;
    }

    if (w <= 0 || h <= 0) { w = 1; h = 1; }
    MyDDSurface *surf = surf_create(dd, w, h, FALSE, FALSE);
    if (!surf) return DDERR_OUTOFMEMORY;
    *ppSurf = surf;
    LOG("CreateSurface offscreen %dx%d caps=%08X", w, h, caps);
    return DD_OK;
}

static HRESULT __stdcall DD_DuplicateSurface(MyDDraw *dd,
    MyDDSurface *pSrc, MyDDSurface **ppDup)
{
    if (!pSrc || !ppDup) return DDERR_INVALIDPARAMS;
    DDSurf_AddRef(pSrc);
    *ppDup = pSrc;
    return DD_OK;
}

static HRESULT __stdcall DD_EnumDisplayModes(MyDDraw *dd, DWORD dwFlags,
    MY_DDSURFACEDESC *pDesc, void *pCtx, void *pfn) { return DD_OK; }
static HRESULT __stdcall DD_EnumSurfaces(MyDDraw *dd, DWORD dwFlags,
    MY_DDSURFACEDESC *pDesc, void *pCtx, void *pfn) { return DD_OK; }
static HRESULT __stdcall DD_FlipToGDISurface(MyDDraw *dd) { return DD_OK; }
static HRESULT __stdcall DD_GetCaps(MyDDraw *dd, void *pDDCaps, void *pDDCaps2)
    { return DD_OK; }

static HRESULT __stdcall DD_GetDisplayMode(MyDDraw *dd, MY_DDSURFACEDESC *pDesc)
{
    if (!pDesc) return DDERR_INVALIDPARAMS;
    pDesc->dwFlags  = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT;
    pDesc->dwWidth  = (DWORD)dd->gameWidth;
    pDesc->dwHeight = (DWORD)dd->gameHeight;
    pDesc->lPitch   = dd->gameWidth;
    pDesc->ddpfPixelFormat.dwSize        = sizeof(MY_DDPIXELFORMAT);
    pDesc->ddpfPixelFormat.dwFlags       = 0x00000200; /* DDPF_PALETTEINDEXED8 */
    pDesc->ddpfPixelFormat.dwRGBBitCount = 8;
    return DD_OK;
}

static HRESULT __stdcall DD_GetFourCCCodes(MyDDraw *dd, DWORD *pNum, DWORD *pCodes)
    { return DD_OK; }
static HRESULT __stdcall DD_GetGDISurface(MyDDraw *dd, MyDDSurface **ppSurf)
    { return DDERR_UNSUPPORTED; }
static HRESULT __stdcall DD_GetMonitorFrequency(MyDDraw *dd, DWORD *pFreq)
    { if (pFreq) *pFreq = 60; return DD_OK; }
static HRESULT __stdcall DD_GetScanLine(MyDDraw *dd, DWORD *pSL)
    { if (pSL) *pSL = 0; return DD_OK; }
static HRESULT __stdcall DD_GetVerticalBlankStatus(MyDDraw *dd, BOOL *pbInVB)
    { if (pbInVB) *pbInVB = FALSE; return DD_OK; }
static HRESULT __stdcall DD_Initialize(MyDDraw *dd, GUID *pGUID)
    { return DDERR_UNSUPPORTED; }
static HRESULT __stdcall DD_RestoreDisplayMode(MyDDraw *dd)
    { LOG("RestoreDisplayMode"); return DD_OK; }

static HRESULT __stdcall DD_SetCooperativeLevel(MyDDraw *dd, HWND hWnd, DWORD dwFlags)
{
    LOG("SetCooperativeLevel hWnd=%p flags=%08X", (void *)hWnd, dwFlags);
    dd->hWnd = hWnd;
    g_dd     = dd;

    if (hWnd && !dd->origWndProc) {
        dd->origWndProc = (WNDPROC)SetWindowLongPtrA(
            hWnd, GWLP_WNDPROC, (LONG_PTR)WrapperWndProc);
        LOG("WndProc hooked (orig=%p)", (void *)dd->origWndProc);
    }

    if (!g_kbHook) {
        g_kbHook = SetWindowsHookExA(WH_KEYBOARD_LL, LLKeyboardProc, g_hInst, 0);
        LOG("LL keyboard hook installed (%p)", (void *)g_kbHook);
    }
    /* No set_windowed_mode here: SetWindowPos during focus changes causes flicker. */

    return DD_OK;
}

static HRESULT __stdcall DD_SetDisplayMode(MyDDraw *dd,
    DWORD dwWidth, DWORD dwHeight, DWORD dwBPP)
{
    dd->gameWidth  = (int)dwWidth;
    dd->gameHeight = (int)dwHeight;
    LOG("SetDisplayMode %ux%ux%u", dwWidth, dwHeight, dwBPP);

    if (dd->hWnd) {
        set_windowed_mode(dd);
        if (g_cfg.borderless)
            set_borderless_mode(dd);
    }

    return DD_OK;
}

static HRESULT __stdcall DD_WaitForVerticalBlank(MyDDraw *dd,
    DWORD dwFlags, HANDLE hEvent)
{
    if (dwFlags == 1) {  /* DDWAITVB_BLOCKBEGIN */
        fps_throttle();
        g_fpsPacedThisFlip = TRUE;  /* tell DDSurf_Flip not to double-wait */
    }
    return DD_OK;
}

/* 23 entries, exact SDK order. */
static void *g_DDVtbl[] = {
    (void *)DD_QueryInterface,
    (void *)DD_AddRef,
    (void *)DD_Release,
    (void *)DD_Compact,
    (void *)DD_CreateClipper,
    (void *)DD_CreatePalette,
    (void *)DD_CreateSurface,
    (void *)DD_DuplicateSurface,
    (void *)DD_EnumDisplayModes,
    (void *)DD_EnumSurfaces,
    (void *)DD_FlipToGDISurface,
    (void *)DD_GetCaps,
    (void *)DD_GetDisplayMode,
    (void *)DD_GetFourCCCodes,
    (void *)DD_GetGDISurface,
    (void *)DD_GetMonitorFrequency,
    (void *)DD_GetScanLine,
    (void *)DD_GetVerticalBlankStatus,
    (void *)DD_Initialize,
    (void *)DD_RestoreDisplayMode,
    (void *)DD_SetCooperativeLevel,
    (void *)DD_SetDisplayMode,
    (void *)DD_WaitForVerticalBlank,
};

/* Call from DirectDrawCreate, not DllMain (loader lock). */
static void enable_dpi_awareness(void)
{
    static LONG s_done = 0;
    HMODULE user32;
    if (InterlockedExchange(&s_done, 1))
        return;
    user32 = GetModuleHandleA("user32.dll");
    if (user32) {
        typedef BOOL (WINAPI *SetCtxFn)(HANDLE);
        SetCtxFn setCtx = (SetCtxFn)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (setCtx) {
            /* PER_MONITOR_AWARE_V2 = (HANDLE)-4, then PER_MONITOR_AWARE = (HANDLE)-3 */
            if (setCtx((HANDLE)-4) || setCtx((HANDLE)-3)) {
                LOG("DPI: per-monitor awareness enabled (SetProcessDpiAwarenessContext)");
                return;
            }
        }
        {
            typedef BOOL (WINAPI *SetDpiFn)(void);
            SetDpiFn setDpi = (SetDpiFn)GetProcAddress(user32, "SetProcessDPIAware");
            if (setDpi && setDpi()) {
                LOG("DPI: system awareness enabled (SetProcessDPIAware)");
                return;
            }
        }
    }
    LOG("DPI: awareness APIs unavailable; running DPI-unaware");
}

/* fps==0 disables this throttle; the caller then owns the frame deadline. */
__declspec(dllexport)
BOOL __stdcall EFZDDRAW_SetFrameLimit(UINT fps)
{
    if (fps != 0u && (fps < 30u || fps > 240u)) {
        LOG("SetFrameLimit rejected fps=%u", fps);
        return FALSE;
    }

    InterlockedExchange(&g_runtimeFpsLimit, (LONG)fps);
    InterlockedIncrement(&g_fpsLimitGeneration);
    LOG("SetFrameLimit fps=%u generation=%ld", fps,
        InterlockedCompareExchange(&g_fpsLimitGeneration, 0, 0));
    return TRUE;
}

__declspec(dllexport)
HRESULT __stdcall DirectDrawCreate(GUID *lpGUID, void **lplpDD, void *pUnkOuter)
{
    enable_dpi_awareness();
    LOG("DirectDrawCreate called (pid=%u)", GetCurrentProcessId());
    if (!lplpDD) return DDERR_INVALIDPARAMS;

    hook_dinput_iat();

    MyDDraw *dd = (MyDDraw *)calloc(1, sizeof(MyDDraw));
    if (!dd) return DDERR_OUTOFMEMORY;
    dd->lpVtbl   = g_DDVtbl;
    dd->refCount = 1;
    g_dd  = dd;
    *lplpDD = dd;

    LOG("DirectDrawCreate OK → %p", (void *)dd);
    return DD_OK;
}

__declspec(dllexport)
HRESULT __stdcall DirectDrawCreateEx(GUID *lpGUID, void **lplpDD,
    REFIID iid, void *pUnkOuter)
    { return DirectDrawCreate(lpGUID, lplpDD, pUnkOuter); }

__declspec(dllexport)
HRESULT __stdcall DirectDrawCreateClipper(DWORD dwFlags,
    void **ppClipper, void *pUnkOuter)
    { if (ppClipper) *ppClipper = NULL; return DDERR_UNSUPPORTED; }

__declspec(dllexport)
HRESULT __stdcall DirectDrawEnumerateA(void *pfn, void *pCtx)  { return DD_OK; }
__declspec(dllexport)
HRESULT __stdcall DirectDrawEnumerateW(void *pfn, void *pCtx)  { return DD_OK; }
__declspec(dllexport)
HRESULT __stdcall DirectDrawEnumerateExA(void *pfn, void *pCtx, DWORD f) { return DD_OK; }
__declspec(dllexport)
HRESULT __stdcall DirectDrawEnumerateExW(void *pfn, void *pCtx, DWORD f) { return DD_OK; }

__declspec(dllexport) HRESULT __stdcall DllCanUnloadNow(void) { return 1 /*S_FALSE*/; }
__declspec(dllexport)
HRESULT __stdcall DllGetClassObject(REFIID rclsid, REFIID riid, void **ppv)
    { if (ppv) *ppv = NULL; return 0x80040111L /*CLASS_E_CLASSNOTAVAILABLE*/; }

BOOL WINAPI DllMain(HINSTANCE hInstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH:
            g_hInst = hInstDLL;
            DisableThreadLibraryCalls(hInstDLL);
            log_init();
            LOG("DLL attached (pid=%u)", GetCurrentProcessId());
            break;
        case DLL_PROCESS_DETACH:
            EFZDX9_Presenter_Shutdown();
            if (g_kbHook) {
                UnhookWindowsHookEx(g_kbHook);
                g_kbHook = NULL;
            }
            if (g_log) {
                LOG("DLL detached");
                fclose(g_log);
                g_log = NULL;
            }
            if (g_logCSInit) {
                DeleteCriticalSection(&g_logCS);
                g_logCSInit = FALSE;
            }
            break;
    }
    return TRUE;
}
