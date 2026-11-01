#pragma once

#include <string>

namespace trial_caster::ipinfo
{

void StartDetection();

std::string LanIp();
std::string PublicIp();
bool PublicIpPending();

}
