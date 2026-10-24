#pragma once

#include <cstdint>

namespace Pulsar::UI
{
    // Initialize the D3D12 Present hook for ImGui overlay
    // Call once at plugin load (kPostLoad)
    void InitOverlay();

    // Toggle overlay visibility (bound to F2)
    void ToggleOverlay();

    // Check if overlay is currently visible
    bool IsOverlayVisible();
}
