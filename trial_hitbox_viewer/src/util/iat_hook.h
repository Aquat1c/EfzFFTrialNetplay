#pragma once

#include "win32_compat.h"

namespace thv::det::detail
{

inline bool PatchIatEntry(
    HMODULE targetModule,
    const char* dllName,
    const char* functionName,
    void* replacement,
    void** outOriginal)
{
    return win32::PatchImportAddressSlot(
        targetModule, dllName, functionName, replacement, outOriginal);
}

inline void UnpatchIatEntry(
    HMODULE targetModule,
    const char* dllName,
    const char* functionName,
    void* expectedReplacement,
    void* original)
{
    (void)win32::RestoreImportAddressSlot(
        targetModule, dllName, functionName, expectedReplacement, original);
}

}
