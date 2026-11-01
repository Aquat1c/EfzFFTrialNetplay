#pragma once

#include <guiddef.h>

namespace thv
{

// Call on the thread owning the new native device.
void OnDinputDeviceCreated(const GUID* deviceGuid, void* device);

bool InitializeDinputRelay();
void ShutdownDinputRelay();
void MaintainDinputRelay();

}
