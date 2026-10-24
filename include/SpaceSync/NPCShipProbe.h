#pragma once

namespace Pulsar::NPCShipProbe {

    // M.1 V1 - Discovery probe for NPC ships in space cells.
    //
    // Goal: confirm we can enumerate non-player ships in the current space
    // cell and identify them reliably. Probes periodically while player is in
    // space, logs ref counts + base-form-type histogram + per-ship details
    // (FormID, base, distance to player). Auto-disables after a short window.
    //
    // After V1 we know:
    // whether NPC ships appear in cell.references at all
    // what base form types ships use → filter for V2
    // typical ship counts → packet sizing for V2/V3
    //
    // Reuses CellContentSync's verified raw-offset pattern (cell+0x80).

    void Init();
    void Tick();

}  // namespace Pulsar::NPCShipProbe
