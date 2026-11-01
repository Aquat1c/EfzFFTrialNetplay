# EFZ DirectDraw Wrapper

A small 32-bit `ddraw.dll` compatibility wrapper for older **EFZ** releases. It replaces the legacy DirectDraw path with a software 8-bit palette surface and presents through immediate D3D9 with a GDI fallback, allowing the game to run in a modern windowed or borderless fullscreen setup.

Derived from [Aquat1c/EfzDdrawWrapper](https://github.com/Aquat1c/EfzDdrawWrapper). This copy adds
the `EFZDDRAW_SetFrameLimit` export the netplay DLL requires, so the two ship and build together.

## Tested Versions

- EFZ 1.11
- EFZ BSE 2.13
- EFZ BME 3.03 Beta
- EFZ Memorial 4.00

## Features

- Windowed mode by default, centered at the game resolution
- Borderless fullscreen toggle with **F11**
- Software 8-bit palette surface emulation
- Immediate D3D9 presentation with GDI `StretchDIBits` fallback
- DirectInput focus-loss guard to prevent stale key states
- Optional frame pacing, defaulting to 60 FPS in this source
- Thread-safe `EFZDDRAW_SetFrameLimit` handoff so the netplay DLL can disable the
  wrapper throttle and become the single exact 60/64 FPS pacing owner
- Optional DirectDraw diagnostics for explicitly enabled troubleshooting builds

## Building

This wrapper must be built as a **32-bit DLL**, because the supported game executables are PE32/i386.

```bat
build.bat
```

The normal build compiles wrapper file logging out. To build diagnostic logging support:

```bat
build.bat Release Log
```

Logging remains inactive in that diagnostic binary unless `EFZ_DDRAW_LOG=1` is present in
the game's environment.

Or with CMake:

```bat
cmake -S . -B build -A Win32
cmake --build build --config Release
```

The output DLL should be named:

```text
ddraw.dll
```

## Installation

1. Build `ddraw.dll`.
2. Copy it next to the target EFZ executable.
3. Launch the game normally.
4. Press **F11** to toggle between windowed and borderless fullscreen mode.

## Diagnostic logs

Release builds do not create a wrapper log by default. To opt in, first build with
`DDRAW_WRAPPER_ENABLE_LOGGING=ON` (or use `build.bat Release Log`), then launch the game
with this environment variable:

```text
EFZ_DDRAW_LOG=1
```

That explicitly enabled configuration writes:

```text
ddraw_wrapper.log
```

The log is created in the game directory and includes DirectDraw calls, window-mode changes,
focus events, rendering diagnostics, and shutdown messages. Removing the environment variable
disables logging without requiring another build; a normal build omits the logging code entirely.

## Notes

- This is a focused DirectDraw 1.0 compatibility shim, not a full DirectDraw implementation.
- Unsupported API calls generally return `DDERR_UNSUPPORTED` or a safe no-op result.
- `EFZDDRAW_SetFrameLimit(0)` disables present throttling; non-zero standalone limits are
  accepted from 30 through 240 FPS. The deadline resets on the render thread, so a
  netplay/session thread never races the wrapper's 64-bit QPC state in the 32-bit process.
- The launcher validates this setter separately from the wrapper's identity aliases,
  and a managed 60 or 64 FPS session fails closed if resolving or invoking it fails. Managed
  sessions do not fall back to the wrapper's standalone limiter.
- The wrapper keeps CPU-side palette surfaces, prefers immediate D3D9 presentation, and
  falls back to GDI; it does not require DXGI or a modern graphics API.
