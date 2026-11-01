#pragma once

#include <cstddef>
#include <cstdint>

namespace thv::memlog
{

void LogGameHeadline(const char* tag);

void LogScalarWrite(
    const char* context,
    std::uintptr_t va,
    const char* fieldName,
    std::uint64_t before,
    std::uint64_t after,
    int widthBytes);

void LogRegionWrite(
    const char* context,
    const char* label,
    void* dst,
    const void* src,
    std::size_t size);

void LogMemsetWrite(
    const char* context,
    const char* label,
    void* dst,
    int fillByte,
    std::size_t size);

void LogBufferFingerprint(const char* context, const char* label, const void* live, const void* payload, std::size_t size);

void LogBlobHash(const char* context, const char* label, const void* data, std::size_t size);

void LogPostWriteVerify(
    const char* context,
    const char* label,
    const void* dst,
    const void* expected,
    std::size_t size);

}
