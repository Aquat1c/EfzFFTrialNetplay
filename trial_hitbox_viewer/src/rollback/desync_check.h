#pragma once

#include "trial_protocol.h"

#include <cstdint>

namespace thv::rollback::desync
{

constexpr int kRegions = proto::kFrameHashRegions;

void Reset();

// game thread (engine): per-region CRC32 of the locally confirmed state for `frame`.
void RecordLocal(std::uint32_t frame, const std::uint32_t* regionHashes, int count);

void RecordRemote(std::uint32_t frame, const std::uint32_t* regionHashes, int count);

bool GetLatestLocalChain(std::uint32_t* frame, std::uint64_t* chain);
void RecordRemoteChain(std::uint32_t frame, std::uint64_t chain);

std::uint32_t Mismatches();
std::uint32_t LastMismatchFrame();

void NoteResimDivergence();
std::uint32_t ResimDivergences();

// Game thread only. regionMask bit r set => dump region r.
bool TakeDumpRequest(std::uint32_t* frame, std::uint32_t* regionMask);

const char* RegionName(int region);

void RequestDump(std::uint32_t frame, std::uint32_t regionMask);

}
