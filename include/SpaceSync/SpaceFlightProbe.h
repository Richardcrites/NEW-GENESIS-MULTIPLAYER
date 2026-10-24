#pragma once

// Phase L.3 V2 V0 - runtime probe that captures multiple flight-state
// candidates while player pilots in space. Profile-diff approach: dump
// every 250ms, look at which fields change between idle and piloting.
//
// READ-ONLY. No mutations, no network - pure diagnostic.

namespace Pulsar::SpaceFlightProbe {

void Init();
void Tick();   // call every game tick; internally rate-limits to 250ms.

}  // namespace Pulsar::SpaceFlightProbe
