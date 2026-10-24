#include "SaveScanner/SavePatcher.h"

#include <Windows.h>
#include <zlib.h>

#include <REL/REL.h>
#include <SFSE/SFSE.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

using namespace std::string_view_literals;

namespace Pulsar::SaveScanner
{
    // ─── Helpers ─────────────────────────────────────────────────────

    static int PadToNearestImpl(int paddingSize, int size)
    {
        const int mask = paddingSize - 1;
        return (size + mask) & ~mask;
    }

    static bool ReadFileBytes(const std::string& a_path,
                              std::vector<std::uint8_t>& a_out)
    {
        std::ifstream f(a_path, std::ios::binary | std::ios::ate);
        if (!f.is_open()) return false;
        const auto sz = f.tellg();
        if (sz < 64) return false;
        a_out.resize(static_cast<std::size_t>(sz));
        f.seekg(0);
        f.read(reinterpret_cast<char*>(a_out.data()), sz);
        return f.good();
    }

    // Read a 32-bit LE little-endian integer from a buffer.
    static std::int32_t ReadI32LE(const std::uint8_t* p)
    {
        std::int32_t v;
        std::memcpy(&v, p, 4);
        return v;
    }
    static std::int64_t ReadI64LE(const std::uint8_t* p)
    {
        std::int64_t v;
        std::memcpy(&v, p, 8);
        return v;
    }
    static float ReadF32LE(const std::uint8_t* p)
    {
        float v;
        std::memcpy(&v, p, 4);
        return v;
    }

    static void WriteI32LE(std::vector<std::uint8_t>& a_buf, std::int32_t v)
    {
        const std::size_t pos = a_buf.size();
        a_buf.resize(pos + 4);
        std::memcpy(a_buf.data() + pos, &v, 4);
    }
    static void WriteI64LE(std::vector<std::uint8_t>& a_buf, std::int64_t v)
    {
        const std::size_t pos = a_buf.size();
        a_buf.resize(pos + 8);
        std::memcpy(a_buf.data() + pos, &v, 8);
    }
    static void WriteF32LE(std::vector<std::uint8_t>& a_buf, float v)
    {
        const std::size_t pos = a_buf.size();
        a_buf.resize(pos + 4);
        std::memcpy(a_buf.data() + pos, &v, 4);
    }

    // ─── BCPS Header parsing ─────────────────────────────────────────

    // Layout (after the 4-byte "BCPS" magic):
    //   i32  version0
    //   i64  chunkSizesOffset
    //   i64  unknown0
    //   i64  compressedDataOffset
    //   i64  uncompressedDataSize
    //   f32  version1
    //   i32  unknown1
    //   i64  sizeUncompressedChunks
    //   i64  paddingSize
    //   i32  unknown2
    //   char compressionType[4]   ("ZIP ")
    static constexpr std::size_t kBcpsHeaderSize = 4 + 4 + 8 + 8 + 8 + 8 + 4 + 4 + 8 + 8 + 4 + 4;

    static bool ParseBcpsHeader(
        const std::vector<std::uint8_t>& a_fileBytes,
        BcpsMeta& a_out,
        std::int64_t& a_outChunkSizesOffset,
        std::int64_t& a_outCompressedDataOffset)
    {
        if (a_fileBytes.size() < kBcpsHeaderSize) return false;
        if (std::memcmp(a_fileBytes.data(), "BCPS", 4) != 0) return false;

        const std::uint8_t* p = a_fileBytes.data() + 4;
        a_out.version0          = ReadI32LE(p); p += 4;
        a_outChunkSizesOffset   = ReadI64LE(p); p += 8;
        a_out.unknown0          = ReadI64LE(p); p += 8;
        a_outCompressedDataOffset = ReadI64LE(p); p += 8;
        a_out.totalUncompressedSize = ReadI64LE(p); p += 8;
        a_out.version1          = ReadF32LE(p); p += 4;
        a_out.unknown1          = ReadI32LE(p); p += 4;
        a_out.sizeUncompressedChunks = ReadI64LE(p); p += 8;
        a_out.paddingSize       = ReadI64LE(p); p += 8;
        a_out.unknown2          = ReadI32LE(p); p += 4;
        std::memcpy(a_out.compressionType, p, 4);

        if (std::memcmp(a_out.compressionType, "ZIP ", 4) != 0) return false;
        if (a_out.sizeUncompressedChunks <= 0) return false;
        if (a_out.paddingSize <= 0) return false;
        if (a_out.totalUncompressedSize <= 0) return false;

        a_out.chunkCount = static_cast<std::int32_t>(
            (a_out.totalUncompressedSize + a_out.sizeUncompressedChunks - 1)
            / a_out.sizeUncompressedChunks);
        if (a_out.chunkCount < 1) return false;

        return true;
    }

    // ─── Decompress a single chunk via zlib (with raw-deflate fallback)

    static bool InflateChunk(
        const std::uint8_t* a_compData,
        std::size_t        a_compSize,
        std::size_t        a_paddedSize,
        std::size_t        a_decompTarget,
        std::vector<std::uint8_t>& a_out)
    {
        a_out.assign(a_decompTarget, 0);
        uLongf decompSize = static_cast<uLongf>(a_decompTarget);

        // Try plain uncompress (zlib-wrapped deflate) first.
        int ret = uncompress(a_out.data(), &decompSize,
                             a_compData,
                             static_cast<uLong>(a_compSize));
        if (ret != Z_OK) {
            // Try with padded size - sometimes the chunk size doesn't
            // match exactly and the padding bytes are zeros that zlib
            // tolerates.
            decompSize = static_cast<uLongf>(a_decompTarget);
            ret = uncompress(a_out.data(), &decompSize,
                             a_compData,
                             static_cast<uLong>(a_paddedSize));
        }
        if (ret != Z_OK) {
            // Raw deflate (no zlib header).
            z_stream strm{};
            strm.next_in   = const_cast<Bytef*>(a_compData);
            strm.avail_in  = static_cast<uInt>(a_compSize);
            strm.next_out  = a_out.data();
            strm.avail_out = static_cast<uInt>(a_decompTarget);
            if (inflateInit2(&strm, -15) == Z_OK) {
                ret = inflate(&strm, Z_FINISH);
                decompSize = strm.total_out;
                inflateEnd(&strm);
                if (ret == Z_STREAM_END) ret = Z_OK;
                else                     ret = Z_DATA_ERROR;
            }
        }
        if (ret != Z_OK) return false;

        a_out.resize(decompSize);
        return true;
    }

    // ─── Public: decompress all chunks ───────────────────────────────

    bool DecompressBcpsBuffer(
        const std::vector<std::uint8_t>& a_fileBytes,
        std::vector<std::uint8_t>& a_outDecompressed,
        BcpsMeta& a_outMeta)
    {
        std::int64_t chunkSizesOffset    = 0;
        std::int64_t compressedDataOffset = 0;

        if (!ParseBcpsHeader(a_fileBytes, a_outMeta,
                             chunkSizesOffset, compressedDataOffset)) {
            PLOG_WARN("[PULSAR] [SavePatcher] BCPS header parse failed"sv);
            return false;
        }

        PLOG_INFO(
            "[PULSAR] [SavePatcher] BCPS: chunks={} chunkSize={} pad={} totalUncomp={} compOff={}"sv,
            a_outMeta.chunkCount,
            a_outMeta.sizeUncompressedChunks,
            a_outMeta.paddingSize,
            a_outMeta.totalUncompressedSize,
            compressedDataOffset);

        // Read chunk sizes table.
        if (chunkSizesOffset + a_outMeta.chunkCount * 4
                > static_cast<std::int64_t>(a_fileBytes.size())) {
            PLOG_WARN("[PULSAR] [SavePatcher] chunk-sizes table out of range"sv);
            return false;
        }

        std::vector<std::int32_t> chunkSizes(a_outMeta.chunkCount);
        std::memcpy(chunkSizes.data(),
                    a_fileBytes.data() + chunkSizesOffset,
                    static_cast<std::size_t>(a_outMeta.chunkCount) * 4);

        // Decompress each chunk and concatenate.
        a_outDecompressed.clear();
        a_outDecompressed.reserve(static_cast<std::size_t>(a_outMeta.totalUncompressedSize));

        std::int64_t readPos = compressedDataOffset;
        std::int64_t remaining = a_outMeta.totalUncompressedSize;

        for (std::int32_t i = 0; i < a_outMeta.chunkCount; ++i) {
            const int paddedSize = PadToNearestImpl(
                static_cast<int>(a_outMeta.paddingSize), chunkSizes[i]);
            if (readPos + paddedSize
                    > static_cast<std::int64_t>(a_fileBytes.size())) {
                PLOG_WARN("[PULSAR] [SavePatcher] chunk {} read out of range"sv, i);
                return false;
            }

            // The last chunk may be smaller than sizeUncompressedChunks.
            const std::size_t expectDecomp =
                (remaining < a_outMeta.sizeUncompressedChunks)
                    ? static_cast<std::size_t>(remaining)
                    : static_cast<std::size_t>(a_outMeta.sizeUncompressedChunks);

            std::vector<std::uint8_t> chunkOut;
            if (!InflateChunk(a_fileBytes.data() + readPos,
                              static_cast<std::size_t>(chunkSizes[i]),
                              static_cast<std::size_t>(paddedSize),
                              expectDecomp,
                              chunkOut)) {
                PLOG_WARN(
                    "[PULSAR] [SavePatcher] chunk {} inflate failed (compSize={}, padded={})"sv,
                    i, chunkSizes[i], paddedSize);
                return false;
            }

            a_outDecompressed.insert(a_outDecompressed.end(),
                                     chunkOut.begin(), chunkOut.end());

            readPos   += paddedSize;
            remaining -= chunkOut.size();
        }

        if (a_outDecompressed.size()
                != static_cast<std::size_t>(a_outMeta.totalUncompressedSize)) {
            PLOG_WARN(
                "[PULSAR] [SavePatcher] decompressed size mismatch: got {} expected {}"sv,
                a_outDecompressed.size(),
                a_outMeta.totalUncompressedSize);
            // Not fatal - engine may tolerate small mismatches. Continue.
        }

        PLOG_INFO(
            "[PULSAR] [SavePatcher] decompressed {} bytes from {} chunks"sv,
            a_outDecompressed.size(), a_outMeta.chunkCount);
        return true;
    }

    bool LoadAndDecompressAll(
        const std::string& a_path,
        std::vector<std::uint8_t>& a_outDecompressed,
        BcpsMeta& a_outMeta)
    {
        std::vector<std::uint8_t> fileBytes;
        if (!ReadFileBytes(a_path, fileBytes)) {
            PLOG_WARN("[PULSAR] [SavePatcher] can't read {}"sv, a_path);
            return false;
        }
        return DecompressBcpsBuffer(fileBytes, a_outDecompressed, a_outMeta);
    }

    // ─── Compress: zlib wrapped (default level) ──────────────────────

    static bool DeflateChunk(
        const std::uint8_t* a_in,
        std::size_t         a_inSize,
        std::vector<std::uint8_t>& a_out)
    {
        // worst-case compressed bound
        uLong compBound = ::compressBound(static_cast<uLong>(a_inSize));
        a_out.assign(static_cast<std::size_t>(compBound), 0);

        uLongf compSize = compBound;
        const int ret = compress2(a_out.data(), &compSize,
                                  a_in, static_cast<uLong>(a_inSize),
                                  Z_DEFAULT_COMPRESSION);
        if (ret != Z_OK) return false;
        a_out.resize(static_cast<std::size_t>(compSize));
        return true;
    }

    // Repack into a fresh BCPS file with the same chunk policy.
    //
    // Layout we produce:
    //   [BCPS header (kBcpsHeaderSize)]
    //   [chunk-sizes table (chunkCount * 4 bytes)]
    //   [pad to paddingSize boundary]
    //   [chunk 0 compressed, padded to paddingSize]
    //   [chunk 1 compressed, padded to paddingSize]
    //   ...
    bool RecompressToBcps(
        const std::vector<std::uint8_t>& a_decompressed,
        BcpsMeta& a_meta,
        std::vector<std::uint8_t>& a_out)
    {
        if (a_decompressed.empty()) return false;
        if (a_meta.sizeUncompressedChunks <= 0) return false;
        if (a_meta.paddingSize <= 0) return false;

        a_meta.totalUncompressedSize =
            static_cast<std::int64_t>(a_decompressed.size());
        a_meta.chunkCount = static_cast<std::int32_t>(
            (a_meta.totalUncompressedSize + a_meta.sizeUncompressedChunks - 1)
            / a_meta.sizeUncompressedChunks);
        if (a_meta.chunkCount < 1) return false;

        // Compress each chunk separately.
        std::vector<std::vector<std::uint8_t>> compressedChunks(a_meta.chunkCount);
        std::vector<std::int32_t> chunkSizes(a_meta.chunkCount);

        std::size_t inPos = 0;
        for (std::int32_t i = 0; i < a_meta.chunkCount; ++i) {
            const std::size_t take = std::min<std::size_t>(
                static_cast<std::size_t>(a_meta.sizeUncompressedChunks),
                a_decompressed.size() - inPos);
            if (!DeflateChunk(a_decompressed.data() + inPos, take,
                              compressedChunks[i])) {
                PLOG_WARN("[PULSAR] [SavePatcher] chunk {} deflate failed"sv, i);
                return false;
            }
            chunkSizes[i] = static_cast<std::int32_t>(compressedChunks[i].size());
            inPos += take;
        }

        // Compute layout offsets.
        // header + chunk-size-table → align to paddingSize → chunks
        const std::int64_t chunkSizesOffset =
            static_cast<std::int64_t>(kBcpsHeaderSize);
        const std::int64_t afterTable =
            chunkSizesOffset + static_cast<std::int64_t>(a_meta.chunkCount) * 4;
        const std::int64_t compressedDataOffset = static_cast<std::int64_t>(
            PadToNearestImpl(static_cast<int>(a_meta.paddingSize),
                             static_cast<int>(afterTable)));

        // Compute padded sizes per chunk + total file size.
        std::vector<int> paddedSizes(a_meta.chunkCount);
        std::int64_t totalChunkBytes = 0;
        for (std::int32_t i = 0; i < a_meta.chunkCount; ++i) {
            paddedSizes[i] = PadToNearestImpl(
                static_cast<int>(a_meta.paddingSize), chunkSizes[i]);
            totalChunkBytes += paddedSizes[i];
        }
        const std::int64_t fileSize = compressedDataOffset + totalChunkBytes;

        a_out.clear();
        a_out.reserve(static_cast<std::size_t>(fileSize));

        // BCPS magic + header.
        a_out.insert(a_out.end(), { 'B', 'C', 'P', 'S' });
        WriteI32LE(a_out, a_meta.version0);
        WriteI64LE(a_out, chunkSizesOffset);
        WriteI64LE(a_out, a_meta.unknown0);
        WriteI64LE(a_out, compressedDataOffset);
        WriteI64LE(a_out, a_meta.totalUncompressedSize);
        WriteF32LE(a_out, a_meta.version1);
        WriteI32LE(a_out, a_meta.unknown1);
        WriteI64LE(a_out, a_meta.sizeUncompressedChunks);
        WriteI64LE(a_out, a_meta.paddingSize);
        WriteI32LE(a_out, a_meta.unknown2);
        a_out.insert(a_out.end(),
                     a_meta.compressionType,
                     a_meta.compressionType + 4);

        // Chunk-sizes table.
        for (std::int32_t i = 0; i < a_meta.chunkCount; ++i) {
            WriteI32LE(a_out, chunkSizes[i]);
        }

        // Pad to compressedDataOffset with "nexus\0" pattern (matches
        // engine's own padding bytes - see RE notes).
        static constexpr char kPadPattern[6] = { 'n', 'e', 'x', 'u', 's', 0 };
        std::size_t padIdx = 0;
        while (static_cast<std::int64_t>(a_out.size()) < compressedDataOffset) {
            a_out.push_back(static_cast<std::uint8_t>(kPadPattern[padIdx % 6]));
            ++padIdx;
        }

        // Each chunk + per-chunk padding to paddingSize boundary.
        padIdx = 0;
        for (std::int32_t i = 0; i < a_meta.chunkCount; ++i) {
            a_out.insert(a_out.end(),
                         compressedChunks[i].begin(),
                         compressedChunks[i].end());
            const int padBytes = paddedSizes[i] - chunkSizes[i];
            for (int j = 0; j < padBytes; ++j) {
                a_out.push_back(static_cast<std::uint8_t>(kPadPattern[padIdx % 6]));
                ++padIdx;
            }
        }

        PLOG_INFO(
            "[PULSAR] [SavePatcher] recompressed: {} bytes (in={} chunks={} compOff={})"sv,
            a_out.size(),
            a_decompressed.size(),
            a_meta.chunkCount,
            compressedDataOffset);
        return true;
    }

    // ─── Round-trip test ─────────────────────────────────────────────

    bool RoundTripTest(const std::string& a_path)
    {
        std::vector<std::uint8_t> decompressed;
        BcpsMeta meta{};
        if (!LoadAndDecompressAll(a_path, decompressed, meta)) {
            PLOG_WARN("[PULSAR] [SavePatcher] RoundTrip: load+decompress failed"sv);
            return false;
        }

        std::vector<std::uint8_t> recompressed;
        if (!RecompressToBcps(decompressed, meta, recompressed)) {
            PLOG_WARN("[PULSAR] [SavePatcher] RoundTrip: recompress failed"sv);
            return false;
        }

        // Verify the recompressed buffer can be decompressed back to
        // identical bytes (== zlib round-trip works).
        std::vector<std::uint8_t> redecompressed;
        BcpsMeta meta2{};
        if (!DecompressBcpsBuffer(recompressed, redecompressed, meta2)) {
            PLOG_WARN("[PULSAR] [SavePatcher] RoundTrip: re-decompress failed"sv);
            return false;
        }

        const bool sizeMatch = (redecompressed.size() == decompressed.size());
        const bool byteMatch = sizeMatch &&
            (std::memcmp(redecompressed.data(),
                         decompressed.data(),
                         decompressed.size()) == 0);

        PLOG_INFO(
            "[PULSAR] [SavePatcher] RoundTrip: src={} bytes, recomp={} bytes, "
            "decomp-after-recomp={} bytes, sizeMatch={} byteMatch={}"sv,
            std::filesystem::file_size(a_path),
            recompressed.size(),
            redecompressed.size(),
            sizeMatch ? 1 : 0,
            byteMatch ? 1 : 0);

        return byteMatch;
    }

    // ─── Phase 3 - pilot patcher ─────────────────────────────────────

    bool PatchPilotState(std::vector<std::uint8_t>& a_decompressed)
    {
        // Phase 2 diff (2026-05-06) identified the pilot binding as an
        // active package with debug name "InitiateSitSleepPackage on
        // <formID>" preceded by a ~22-byte header (u32 listCount=1, two
        // FormID/index refs, a u16 string-length prefix).
        //
        // Patch: search for the ASCII marker. When found, zero a window
        // from (match - 22 bytes) through (match + string-length).
        // Engine reads listCount=0 → no packages activated → client
        // loads with no pilot binding bound to the chair.
        static constexpr char        kMarker[]      = "InitiateSitSleepPackage";
        static constexpr std::size_t kMarkerLen     = sizeof(kMarker) - 1;
        static constexpr std::size_t kPreambleBytes = 22;

        if (a_decompressed.size() < kMarkerLen + kPreambleBytes) {
            PLOG_INFO("[PULSAR] [SavePatcher] PatchPilot: buffer too small"sv);
            return false;
        }

        const std::size_t scanEnd = a_decompressed.size() - kMarkerLen;
        std::size_t patchedCount = 0;

        for (std::size_t i = 0; i < scanEnd; ++i) {
            if (a_decompressed[i] != static_cast<std::uint8_t>(kMarker[0])) continue;
            if (std::memcmp(a_decompressed.data() + i, kMarker, kMarkerLen) != 0) continue;

            const std::size_t winStart =
                (i >= kPreambleBytes) ? (i - kPreambleBytes) : 0;

            std::uint16_t strLen = 0;
            if (i >= 2) {
                std::memcpy(&strLen, a_decompressed.data() + (i - 2), 2);
            }
            if (strLen > 256 || strLen < kMarkerLen) {
                strLen = static_cast<std::uint16_t>(kMarkerLen + 32);
            }
            std::size_t winEnd = i + strLen;
            if (winEnd > a_decompressed.size()) winEnd = a_decompressed.size();

            PLOG_INFO(
                "[PULSAR] [SavePatcher] PatchPilot: marker @0x{:X} "
                "strLen={} → zeroing 0x{:X}..0x{:X} ({} bytes)"sv,
                i, strLen, winStart, winEnd, winEnd - winStart);

            std::memset(a_decompressed.data() + winStart, 0, winEnd - winStart);
            ++patchedCount;
            i = winEnd - 1;  // skip past zeroed region
        }

        if (patchedCount == 0) {
            PLOG_INFO(
                "[PULSAR] [SavePatcher] PatchPilot: no SitSleepPackage marker found "
                "(host may not have been piloting at save time - that's fine)"sv);
            return false;
        }
        PLOG_INFO(
            "[PULSAR] [SavePatcher] PatchPilot: zeroed {} marker(s)"sv,
            patchedCount);
        return true;
    }

    // ─── Phase 2 - Diff tool ─────────────────────────────────────────

    // Group consecutive byte differences into runs. Returns ranges
    // [start, end) where every byte in the range differs.
    struct DiffRange
    {
        std::size_t start;
        std::size_t end;   // exclusive
    };

    static std::vector<DiffRange> FindDiffRanges(
        const std::vector<std::uint8_t>& a,
        const std::vector<std::uint8_t>& b,
        std::size_t a_maxRanges = 16384)
    {
        std::vector<DiffRange> out;
        const std::size_t n = std::min(a.size(), b.size());
        std::size_t i = 0;
        while (i < n) {
            if (a[i] != b[i]) {
                std::size_t s = i;
                // Allow up to 8 matching bytes inside a run (small
                // matching gaps inside a structurally-related block
                // shouldn't split the diff).
                while (i < n) {
                    if (a[i] != b[i]) {
                        ++i;
                        continue;
                    }
                    // Look ahead: any diff in next 8 bytes?
                    bool nearby = false;
                    for (std::size_t j = i + 1;
                         j < std::min(i + 9, n); ++j) {
                        if (a[j] != b[j]) { nearby = true; break; }
                    }
                    if (!nearby) break;
                    ++i;
                }
                out.push_back({ s, i });
                if (out.size() >= a_maxRanges) {
                    PLOG_WARN(
                        "[PULSAR] [Diff] hit max ranges {} - truncating"sv,
                        a_maxRanges);
                    return out;
                }
            } else {
                ++i;
            }
        }
        if (a.size() != b.size()) {
            // Tail diff (one save has more bytes than the other).
            out.push_back({ n, std::max(a.size(), b.size()) });
        }
        return out;
    }

    // Heuristic: does this 4-byte aligned region look like a float?
    // IEEE 754 floats with magnitude in [1e-3 .. 1e6] tend to have the
    // high byte in a narrow range. Most "noise" floats (positions,
    // velocities, quaternions) hit this pattern.
    static bool LooksLikeFloat4(const std::uint8_t* p)
    {
        // Reject zero (always 00 00 00 00 - clearly not noise).
        if (p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 0) return false;
        // High byte (sign + upper exponent bits)
        const std::uint8_t hi = p[3];
        // Floats with magnitude ~1.0 → exponent 0x7F → high byte
        //   3F.. (positive) or BF.. (negative)
        // ~10..100 → exponent 0x82-0x85 → 41-42 / C1-C2
        // ~0.001..0.01 → exponent 0x76-0x79 → 3A-3C / BA-BC
        if ((hi >= 0x38 && hi <= 0x46) || (hi >= 0xB8 && hi <= 0xC6)) {
            return true;
        }
        return false;
    }
    // Looks like a region full of float patterns?
    static bool RangeLooksFloaty(
        const std::vector<std::uint8_t>& a,
        const std::vector<std::uint8_t>& b,
        std::size_t start, std::size_t len)
    {
        if (len < 8) return false;
        // Sample every 4 bytes. If most look like floats, treat as
        // floaty noise.
        int hits = 0, samples = 0;
        for (std::size_t i = start; i + 4 <= start + len; i += 4) {
            ++samples;
            if (LooksLikeFloat4(a.data() + i) ||
                LooksLikeFloat4(b.data() + i)) {
                ++hits;
            }
        }
        return samples > 0 && (hits * 100 / samples) >= 70;
    }

    // SEH-isolated core diff. C++ unwind objects (vectors, strings) live
    // in DiffTwoSaves below; this helper takes pointers + sizes so __try
    // doesn't trip C2712.
    static bool DiffCoreSafeImpl(
        const std::uint8_t* a, std::size_t aSize,
        const std::uint8_t* b, std::size_t bSize,
        FILE* fp,
        std::size_t kHeaderSkip,
        int& outRangeCount)
    {
        outRangeCount = 0;
        std::fprintf(fp,
            "PULSAR Save Diff Tool - Path B Phase 2 (simple mode)\n");
        std::fprintf(fp, "A size: %zu\n", aSize);
        std::fprintf(fp, "B size: %zu\n", bSize);
        std::fprintf(fp, "sizeDelta: %lld\n",
            static_cast<long long>(bSize)
            - static_cast<long long>(aSize));
        std::fprintf(fp, "Header skip: 0x%zx\n", kHeaderSkip);
        std::fprintf(fp, "Format: each range = consecutive diff bytes "
                         "(>=5 bytes, max 200 ranges shown).\n\n");

        const std::size_t n = (aSize < bSize) ? aSize : bSize;
        if (n <= kHeaderSkip) return false;

        constexpr std::size_t kMinRangeLen = 5;
        constexpr int         kMaxRanges   = 200;
        constexpr std::size_t kDumpBytes   = 48;

        std::size_t i = kHeaderSkip;
        std::size_t totalDiffBytes = 0;

        while (i < n && outRangeCount < kMaxRanges) {
            if (a[i] != b[i]) {
                std::size_t s = i;
                // Run length: include up to 4 matching bytes inside
                // (small gaps in the same field shouldn't split it).
                while (i < n) {
                    if (a[i] != b[i]) { ++i; continue; }
                    bool nearby = false;
                    const std::size_t lookahead =
                        (n - i < 5) ? (n - i) : 5;
                    for (std::size_t j = 1; j < lookahead; ++j) {
                        if (a[i + j] != b[i + j]) { nearby = true; break; }
                    }
                    if (!nearby) break;
                    ++i;
                }
                const std::size_t len = i - s;
                totalDiffBytes += len;
                if (len >= kMinRangeLen) {
                    const std::size_t dumpLen =
                        (len < kDumpBytes) ? len : kDumpBytes;
                    std::fprintf(fp, "@0x%zx len=%zu\n", s, len);
                    std::fprintf(fp, "  A hex:");
                    for (std::size_t k = 0; k < dumpLen; ++k)
                        std::fprintf(fp, " %02X", a[s + k]);
                    std::fprintf(fp, "\n  A asc: ");
                    for (std::size_t k = 0; k < dumpLen; ++k) {
                        const auto c = a[s + k];
                        std::fputc((c >= 0x20 && c < 0x7F) ? c : '.', fp);
                    }
                    std::fprintf(fp, "\n  B hex:");
                    for (std::size_t k = 0; k < dumpLen; ++k)
                        std::fprintf(fp, " %02X", b[s + k]);
                    std::fprintf(fp, "\n  B asc: ");
                    for (std::size_t k = 0; k < dumpLen; ++k) {
                        const auto c = b[s + k];
                        std::fputc((c >= 0x20 && c < 0x7F) ? c : '.', fp);
                    }
                    std::fputc('\n', fp);
                    std::fputc('\n', fp);
                    ++outRangeCount;
                }
            } else {
                ++i;
            }
        }

        std::fprintf(fp,
            "\nSCAN COMPLETE. shown=%d totalDiffBytes=%zu\n",
            outRangeCount, totalDiffBytes);
        return true;
    }
    static bool DiffCoreSafe(
        const std::uint8_t* a, std::size_t aSize,
        const std::uint8_t* b, std::size_t bSize,
        FILE* fp,
        std::size_t kHeaderSkip,
        int& outRangeCount)
    {
        __try {
            return DiffCoreSafeImpl(a, aSize, b, bSize, fp,
                                    kHeaderSkip, outRangeCount);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            outRangeCount = -1;
            return false;
        }
    }

    bool DiffTwoSaves(
        const std::string& a_pathA,
        const std::string& a_pathB,
        const std::string& a_outDiffFile)
    {
        std::vector<std::uint8_t> decompA, decompB;
        BcpsMeta metaA{}, metaB{};
        if (!LoadAndDecompressAll(a_pathA, decompA, metaA)) {
            PLOG_WARN("[PULSAR] [Diff] can't load A: {}"sv, a_pathA);
            return false;
        }
        if (!LoadAndDecompressAll(a_pathB, decompB, metaB)) {
            PLOG_WARN("[PULSAR] [Diff] can't load B: {}"sv, a_pathB);
            return false;
        }

        PLOG_INFO(
            "[PULSAR] [Diff] A={} bytes  B={} bytes (sizeDelta={})"sv,
            decompA.size(), decompB.size(),
            static_cast<std::int64_t>(decompB.size())
                - static_cast<std::int64_t>(decompA.size()));

        // Skip leading header (timestamps, save#) by starting the diff
        // at the first byte AFTER the file-time field. From RE notes,
        // the FILETIME lives at offset 0x86 within the inner data.
        // Conservative skip-to: 0x100 (after header block).
        constexpr std::size_t kHeaderSkipBytes = 0x100;

        if (decompA.size() < kHeaderSkipBytes ||
            decompB.size() < kHeaderSkipBytes) {
            PLOG_WARN("[PULSAR] [Diff] saves too small for header skip"sv);
            return false;
        }

        // Open output file via plain C stdio - saner threading semantics
        // than std::ofstream when invoked from a detached worker.
        FILE* fp = nullptr;
#ifdef _WIN32
        fopen_s(&fp, a_outDiffFile.c_str(), "w");
#else
        fp = std::fopen(a_outDiffFile.c_str(), "w");
#endif
        if (!fp) {
            PLOG_WARN("[PULSAR] [Diff] can't open output {}"sv, a_outDiffFile);
            return false;
        }

        std::fprintf(fp, "A path: %s\n", a_pathA.c_str());
        std::fprintf(fp, "B path: %s\n\n", a_pathB.c_str());

        int rangeCount = 0;
        const bool ok = DiffCoreSafe(
            decompA.data(), decompA.size(),
            decompB.data(), decompB.size(),
            fp, kHeaderSkipBytes, rangeCount);

        std::fclose(fp);

        if (!ok) {
            PLOG_WARN(
                "[PULSAR] [Diff] core failed (rangeCount={}, possible SEH)"sv,
                rangeCount);
            return false;
        }

        PLOG_INFO(
            "[PULSAR] [Diff] wrote {} ({} ranges shown, simple-mode)"sv,
            a_outDiffFile, rangeCount);
        return true;
    }
}
