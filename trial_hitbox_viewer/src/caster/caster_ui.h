#pragma once

struct ImGuiContext;

namespace thv::ui
{

ImGuiContext* GetImGuiContext();

bool Initialize();
void Shutdown();

void PollEvents();

void Render();

bool IsVisible();
void SetVisible(bool visible);

}
