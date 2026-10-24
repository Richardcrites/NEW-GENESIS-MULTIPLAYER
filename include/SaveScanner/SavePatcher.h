#pragma once

// L.7 Path B - Save-format patcher.
//
// Goal: strip the host's "I am piloting ship X" state from a .sfs save
// buffer before the host transmits it to the client via the L.7 save-
// bridge. The engine's load-from-save flow restores the full pilot
// binding (HUD + ship-control input layer) which our post-load actor-
// state clear can't undo. By patching it OUT of the save bytes, the
// engine never restores it on the client side in the first place.
//
// This file declares the buffer-level API. Implementation lives in
// SavePatcher.cpp; round-trip helpers in SaveScanner.cpp are reused.

#include <cstdint>
#include <string>
#include <vector>

namespace Pulsar::SaveScanner
{
    // Decoded BCPS container metadata, kept around so we can recompress
    // back to the same layout (same chunk size, padding, version).
    struct BcpsMeta
    {
        std::int32_t version0          = 0;
        std::int64_t unknown0          = 0;
        float        version1          = 0.0f;
        std::int32_t unknown1          = 0;
        std::int64_t sizeUncompressedChunks = 0;  // max bytes per decompressed chunk
        std::int64_t paddingSize       = 0;       // alignment boundary (16 typical)
        std::int32_t unknown2          = 0;
        char         compressionType[4]{};         // "ZIP "
        std::int64_t totalUncompressedSize = 0;   // sum of all decompressed chunks
        std::int32_t chunkCount         = 0;
    };

    // Read the entire file at a_path, parse BCPS header, decompress
    // every chunk, and return the concatenated decompressed bytes plus
    // the BCPS metadata needed to round-trip back.
    //
    // Returns false on any decompression error or magic-byte mismatch.
    bool LoadAndDecompressAll(
        const std::string& a_path,
        std::vector<std::uint8_t>& a_outDecompressed,
        BcpsMeta& a_outMeta);

    // Same but operating on an in-memory file buffer (avoids re-reading
    // from disk when the caller already has the bytes).
    bool DecompressBcpsBuffer(
        const std::vector<std::uint8_t>& a_fileBytes,
        std::vector<std::uint8_t>& a_outDecompressed,
        BcpsMeta& a_outMeta);

    // Repack decompressed bytes into a fresh BCPS container using the
    // same chunk size and padding as the original. Resulting buffer is
    // a valid .sfs file the engine can load.
    //
    // chunkCount is recomputed from a_decompressed.size() and
    // a_meta.sizeUncompressedChunks; a_meta.totalUncompressedSize is
    // updated. Other meta fields are kept verbatim.
    bool RecompressToBcps(
        const std::vector<std::uint8_t>& a_decompressed,
        BcpsMeta& a_meta,
        std::vector<std::uint8_t>& a_outBcpsBytes);

    // Round-trip helper for verification: load a save, decompress, then
    // recompress, and compare with the original file byte-by-byte.
    // Logs the result. Returns true if recompressed buffer loads back
    // (i.e., DecompressBcpsBuffer succeeds on it and yields the same
    // decompressed payload). Byte-identical match is NOT required -
    // zlib output is non-deterministic across versions.
    bool RoundTripTest(const std::string& a_path);

    // ─── Path B Phase 3 - Patch pilot state ──────────────────────────
    // (To be filled in after Phase 2 diff identifies the exact fields.
    //  Stub returns false = "no patch applied" so caller falls back to
    //  unpatched bytes.)
    //
    // Operates in-place on the decompressed buffer. Returns true if
    // any pilot-state bytes were modified.
    bool PatchPilotState(std::vector<std::uint8_t>& a_decompressed);

    // ─── Path B Phase 2 - Byte diff between two saves ────────────────
    // Decompresses both .sfs files, computes byte-by-byte diff over the
    // inner SFS_SAVEGAME data. Writes a human-readable summary to
    // a_outDiffFile (overwritten if exists).
    //
    // Filters trivially noisy regions:
    // file-time stamps in the header (always changes)
    // the leading header block before plugin info
    //
    // Logs candidate "interesting" diff ranges (in change records area)
    // to PULSAR.log so we can see them in real time too.
    bool DiffTwoSaves(
        const std::string& a_pathA,
        const std::string& a_pathB,
        const std::string& a_outDiffFile);
}
