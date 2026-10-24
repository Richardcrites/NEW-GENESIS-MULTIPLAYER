#pragma once

// Co-op combat-AI taming (Beta polish for shared aggro).
//
// In co-op each machine runs combat NPC AI independently (host fights the host,
// client fights the client) → the NPCs reposition differently on each side →
// position divergence. Rather than suppress the client AI (= rubber-banding),
// we TAME the movement patterns so NPCs reposition far less (hold position + aim
// + fire instead of circling/strafing/flanking/dodging). Less movement → far
// less per-machine divergence → the drift bias keeps them near-coincident.
//
// Mechanism: zero a focused set of combat GMSTs (the circle/dodge/avoid/
// adjust-range/flank CHANCE settings) while co-op is connected, restore the
// originals on disconnect. Pure runtime GameSettingCollection writes - no ESP,
// no per-NPC RE. Global (affects host + client + all NPCs) but only while in a
// co-op session. Gated by Config::coopTameCombatAI.
namespace Pulsar::Coop::CombatTaming
{
    // Game-thread tick: applies the taming on the co-op connect edge and restores
    // it on the disconnect edge. Call every frame from the main game-thread tick.
    void Tick();
}
