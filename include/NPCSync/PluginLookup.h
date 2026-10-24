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
}
