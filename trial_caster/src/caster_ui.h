#pragma once

namespace trial_caster
{

class CasterApp;

namespace ui
{

bool Initialize(float userScale = 1.0f);
void Shutdown();
bool RunFrame(CasterApp& app);

}
}
