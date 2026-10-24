#pragma once

#include <cstdint>

// Ghidra-verified FormID ↔ plugin-name lookup for Starfield 1.16.236.
// All offsets come from T34 (`tools/RE_proc_cell/T34_findings.md`), NOT
// CommonLibSF - that lib's TESDataHandler/TESFile layout is stale for 1.16.236.
//
// TESDataHandler singleton is at RVA 0x5FD9B28 (absolute 0x145FD9B28).
// Loaded-mod linked-list starts at TESDataHandler+0x1570 (nodes {TESFile*, next*}).
// TESFile+0x38  = char* fileName
// TESFile+0x1B8 = u32 flags (bit8=light, bit9=overlay, bit10=medium)
// TESFile+0x1D8 = TESFile** overlaySource (only if bit9)
// TESFile+0x218 = u8 compileIndex (regular ESM; 0xFF = unloaded)
// TESFile+0x219 = u8 mediumSlot
// TESFile+0x21A = u16 lightSlot
namespace Pulsar::NPCSync::PluginLookup
{
    // Host-side: given a fully-qualified FormID, copy the owning plugin's
    // filename into `outBuf` (null-terminated, bounded by `outSize`).
    // Returns true iff a plugin was found AND the copy succeeded. The copy
    // itself is SEH-guarded - if TESFile+0x38's char* is garbage (half-
    // initialized TESFile during save-load, unmapped memory past a string's
    // end), we fail cleanly instead of taking down the process. On false,
    // outBuf[0] is set to '\0'.
    bool FormIDToPluginName(uint32_t formID, char* outBuf, size_t outSize) noexcept;

    // Flag-aware local-ID extract. For regular ESM the low 24 bits, for
    // medium the low 16, for light the low 12. Needs the file-flags word -
    // pass 0 to let this function re-walk the mod list to determine the
    // plugin from the FormID itself.
    uint32_t FormIDToLocalID(uint32_t formID) noexcept;

    // Client-side: given a plugin name + local-ID sent by the host, produce
    // the client-local full FormID. Returns 0 if the plugin isn't loaded
    // on this machine. The local ID's high bits (if any) are masked off
    // according to the file's encoding before reassembly.
    uint32_t ResolveCrossPlugin(const char* pluginName, uint32_t localID) noexcept;

    // The client-local FormID that PulsarRemoteControl's FindFormInAnyPlugin will land
    // on for `localID`: the first hit over Starfield.esm and its fixed list of official
    // masters, in the script's order. 0 if none of them has a form there.
    //
    // The script only receives a plugin-local ID, so for a form from any other plugin
    // (every Star Wars Genesis NPC and placed ref) it silently picks whatever vanilla
    // form shares that local ID. Callers compare this against the FormID they mean
    // and skip the Papyrus request when the two differ.
    uint32_t PapyrusFindFormInAnyPlugin(uint32_t localID) noexcept;

    // Resolve a {plugin, localID, rawFormID} triple sent by the peer.
    //
    // Unlike the "ResolveCrossPlugin, else the raw ID" pattern, the peer's raw FormID
    // is only trusted when it really belongs to the named plugin on this machine (or
    // the peer named no plugin at all: dynamic forms, older builds). With a large
    // modlist such as Star Wars Genesis, a plugin the peer has and we don't would
    // otherwise make its raw ID land on an unrelated form from whatever plugin holds
    // that load-order slot here. Returns 0 when nothing matches.
    uint32_t ResolveFromPeer(const char* pluginName, uint32_t localID, uint32_t rawFormID) noexcept;
}
