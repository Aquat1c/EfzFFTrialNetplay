#pragma once

namespace thv::watchdog
{

bool Initialize();
void Shutdown();

void MarkGameThread();

void EnterPhase(const char* name);
void ExitPhase();

struct ScopedPhase
{
    explicit ScopedPhase(const char* name) { EnterPhase(name); }
    ~ScopedPhase() { ExitPhase(); }
    ScopedPhase(const ScopedPhase&) = delete;
    ScopedPhase& operator=(const ScopedPhase&) = delete;
};

}
