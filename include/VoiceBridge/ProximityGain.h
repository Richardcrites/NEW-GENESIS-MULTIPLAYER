#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Pulsar::Voice
{
    // Returns 0..1. alwaysAudible short-circuits to 1. Otherwise linear falloff
    // by distance: full <= dNear, silent >= dFar. Both positions are world units
    // (~70 units per metre).
    //
    // v1 is DISTANCE-ONLY. The cell ids are accepted for future use but NOT used
    // to hard-mute: in this co-op each machine has a DIFFERENT procedural-cell
    // instance FormID for the same room, so a cross-cell mute silenced players
    // standing right next to each other. Distance handles "different area = far
    // away = quiet" on its own, and the always-audible toggle covers the rest.
    inline float ComputeProximityGain(const float local[3], const float peer[3],
                                      uint32_t localCell, uint32_t peerCell,
                                      bool alwaysAudible, float dNear, float dFar)
    {
        (void)localCell;
        (void)peerCell;
        if (alwaysAudible) return 1.0f;
        const float dx = local[0] - peer[0];
        const float dy = local[1] - peer[1];
        const float dz = local[2] - peer[2];
        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (dFar <= dNear) return dist <= dNear ? 1.0f : 0.0f;
        const float g = (dFar - dist) / (dFar - dNear);
        return std::clamp(g, 0.0f, 1.0f);
    }
}
