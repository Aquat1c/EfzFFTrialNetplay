#pragma once

namespace thv::gamespeed
{

// Trial global 0x47B658 — subtick count per logical frame.
struct SavedState
{
    int substeps = 3;
};

SavedState PushResimSpeed(int speed = 3);
void PopResimSpeed(const SavedState& saved);

}