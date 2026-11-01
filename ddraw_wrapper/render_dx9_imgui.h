#pragma once

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (__stdcall *EfzDx9ToggleBorderlessFn)(void *userData);

typedef struct EfzDx9PresentParams {
    HWND window;
    const BYTE *pixels;
    int width;
    int height;
    int pitch;
    const RGBQUAD *palette;
    int clientWidth;
    int clientHeight;
    int dstX;
    int dstY;
    int dstW;
    int dstH;
    BOOL borderless;
    BOOL appActive;
    BOOL *linearScale;
    DWORD presentSerial;
    DWORD focusSerial;
    EfzDx9ToggleBorderlessFn toggleBorderless;
    void *toggleBorderlessUserData;
} EfzDx9PresentParams;

#define EFZ_DX9_NETPLAY_HUD_MAX_LINES 4
#define EFZ_DX9_NETPLAY_HUD_TEXT_CAP 96

typedef struct EfzDx9NetplayHudState {
    DWORD structSize;
    BOOL  visible;
    char  matchHeader[EFZ_DX9_NETPLAY_HUD_TEXT_CAP];
    char  sessionDetail[EFZ_DX9_NETPLAY_HUD_TEXT_CAP];
    int   practiceLineCount;
    char  practiceLines[EFZ_DX9_NETPLAY_HUD_MAX_LINES][EFZ_DX9_NETPLAY_HUD_TEXT_CAP];
    int   debugLineCount;
    char  debugLines[EFZ_DX9_NETPLAY_HUD_MAX_LINES][EFZ_DX9_NETPLAY_HUD_TEXT_CAP];
    int   toastLineCount;
    char  toastLines[EFZ_DX9_NETPLAY_HUD_MAX_LINES][EFZ_DX9_NETPLAY_HUD_TEXT_CAP];
} EfzDx9NetplayHudState;

typedef BOOL (__stdcall *EfzDx9NetplayHudProvider)(
    EfzDx9NetplayHudState *outState,
    void *userData);

#define EFZ_DX9_OVERLAY_MAX_COMMANDS 192
#define EFZ_DX9_OVERLAY_TEXT_CAP 128

enum EfzDx9OverlayCommandType {
    EFZ_DX9_OVERLAY_COMMAND_TEXT = 1,
    EFZ_DX9_OVERLAY_COMMAND_RECT_FILLED = 2,
    EFZ_DX9_OVERLAY_COMMAND_RECT = 3
};

enum EfzDx9OverlayTextFlags {
    EFZ_DX9_OVERLAY_TEXT_SHADOW = 0x1,
    EFZ_DX9_OVERLAY_TEXT_ALIGN_CENTER = 0x2,
    EFZ_DX9_OVERLAY_TEXT_ALIGN_RIGHT = 0x4
};

typedef struct EfzDx9OverlayCommand {
    DWORD structSize;
    int type;
    float x;
    float y;
    float w;
    float h;
    float fontSize;
    float thickness;
    DWORD color;
    DWORD flags;
    char text[EFZ_DX9_OVERLAY_TEXT_CAP];
} EfzDx9OverlayCommand;

typedef struct EfzDx9OverlayFrame {
    DWORD structSize;
    BOOL visible;
    float virtualWidth;
    float virtualHeight;
    int commandCount;
    EfzDx9OverlayCommand commands[EFZ_DX9_OVERLAY_MAX_COMMANDS];
} EfzDx9OverlayFrame;

typedef BOOL (__stdcall *EfzDx9OverlayProvider)(
    EfzDx9OverlayFrame *outFrame,
    void *userData);

struct IDirect3DDevice9;

typedef BOOL (__stdcall *EfzDx9TrainingMenuVisibleFn)(void *userData);
typedef BOOL (__stdcall *EfzDx9TrainingMenuHandleMessageFn)(
    HWND hwnd,
    UINT msg,
    WPARAM wParam,
    LPARAM lParam,
    LRESULT *outResult,
    void *userData);
typedef void (__stdcall *EfzDx9TrainingMenuRendererFn)(
    struct IDirect3DDevice9 *device,
    HWND hwnd,
    const EfzDx9PresentParams *params,
    void *userData);
// wantsDraw gates drawing, isVisible gates input suppression.
typedef BOOL (__stdcall *EfzDx9TrainingMenuWantsDrawFn)(void *userData);
// Release D3DPOOL_DEFAULT objects on phase 0 (pre-reset), recreate on 1 (post-reset).
typedef void (__stdcall *EfzDx9TrainingMenuDeviceNotifyFn)(int phase, void *userData);

BOOL EFZDX9_Presenter_HandleWindowMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, LRESULT *outResult);
BOOL EFZDX9_Presenter_IsMenuVisible(void);
BOOL EFZDX9_Presenter_ShouldSuppressGameInput(void);
BOOL EFZDX9_Presenter_Present(const EfzDx9PresentParams *params);
void EFZDX9_Presenter_SetActive(BOOL active);
void EFZDX9_Presenter_Shutdown(void);
void EFZDX9_Presenter_ToggleMenu(void);
void __stdcall EFZDX9_RegisterNetplayHudProvider(
    EfzDx9NetplayHudProvider provider,
    void *userData);
void __stdcall EFZDX9_RegisterOverlayProvider(
    EfzDx9OverlayProvider provider,
    void *userData);
void __stdcall EFZDX9_RegisterTrainingMenuBridge(
    EfzDx9TrainingMenuRendererFn renderer,
    EfzDx9TrainingMenuVisibleFn isVisible,
    EfzDx9TrainingMenuHandleMessageFn handleMessage,
    EfzDx9TrainingMenuWantsDrawFn wantsDraw,
    EfzDx9TrainingMenuDeviceNotifyFn deviceNotify,
    void *userData);

#ifdef __cplusplus
}
#endif
