#pragma once

#include <cstdint>
#include <cstdio>

namespace thv::menu
{
enum class NavScreen : std::uint8_t;
}

namespace thv::lifecycle
{

void Step(const char* eventClass, const char* subsystem, const char* fmt, ...);

void Phase(const char* subsystem, const char* from, const char* to, const char* detail = nullptr);
void Exit(const char* subsystem, const char* reason, const char* detail = nullptr);
void Rollback(const char* event, const char* fmt, ...);
void Sync(const char* subsystem, const char* fmt, ...);
void Input(const char* subsystem, const char* fmt, ...);
void Desync(const char* subsystem, const char* fmt, ...);
void InputPath(const char* fmt, ...);
void Netplay(const char* fmt, ...);
void Stall(const char* gate, const char* reason);

void NavScreenChange(thv::menu::NavScreen prev, thv::menu::NavScreen next);
void DiOverlay(
    const char* path,
    std::uint8_t localMask,
    std::uint8_t remoteMask,
    bool hasRemote,
    int memorialOrScreen);

void Init(const char* subsystem, const char* fmt, ...);

void Checkpoint(const char* reason);

void Fault(const char* subsystem, const char* fmt, ...);

void Heartbeat();

void WriteModStateSnapshot(FILE* file);

}
