#pragma once

#include <windows.h>

namespace thv::crash
{

using FlushLogsFn = void (*)();
using SessionLogPathFn = const char* (*)();

struct Options
{
    const char* component = "netplay";
    FlushLogsFn flushLogs = nullptr;
    SessionLogPathFn sessionLogPathFn = nullptr;
};

bool Install(HMODULE module, const Options& options = {});
void Shutdown();

void WriteManualDump(const char* reason, unsigned int detailValue);

}
