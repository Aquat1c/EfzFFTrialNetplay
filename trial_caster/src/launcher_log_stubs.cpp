#include "lifecycle_log.h"

#include <cstdarg>
#include <cstdio>

namespace thv::lifecycle
{

void Init(const char* subsystem, const char* fmt, ...)
{
    (void)subsystem;
    (void)fmt;
}

void Checkpoint(const char* reason)
{
    (void)reason;
}

void Fault(const char* subsystem, const char* fmt, ...)
{
    (void)subsystem;
    (void)fmt;
}

void WriteModStateSnapshot(FILE* file)
{
    if (file != nullptr)
    {
        std::fprintf(file, "(launcher — no mod state snapshot)\n");
    }
}

}
