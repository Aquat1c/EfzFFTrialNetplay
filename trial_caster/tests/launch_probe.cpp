#include <windows.h>

#include <iterator>

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    if (reason != DLL_PROCESS_ATTACH)
    {
        return TRUE;
    }

    wchar_t marker[4096] = {};
    const DWORD length = GetEnvironmentVariableW(
        L"TRIAL_LAUNCH_PROBE_MARKER",
        marker,
        static_cast<DWORD>(std::size(marker)));
    if (length == 0u || length >= std::size(marker))
    {
        return TRUE;
    }

    HANDLE const file = CreateFileW(
        marker,
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file != INVALID_HANDLE_VALUE)
    {
        constexpr char text[] = "injected";
        DWORD written = 0u;
        WriteFile(file, text, static_cast<DWORD>(sizeof(text) - 1u), &written, nullptr);
        CloseHandle(file);
    }
    return TRUE;
}
