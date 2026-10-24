#include "SaveScanner/SaveScanner.h"

#include <Windows.h>
#include <ShlObj.h>
#include <zlib.h>

#include <REL/REL.h>
#include <SFSE/SFSE.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace std::string_view_literals;

namespace Pulsar::SaveScanner
{
    // ─── Binary Reader helper ───

    class BinaryReader
    {
    public:
        BinaryReader(const uint8_t* data, size_t size) : m_data(data), m_size(size), m_pos(0) {}

        bool CanRead(size_t n) const { return m_pos + n <= m_size; }

        uint8_t  ReadU8()    { uint8_t  v; Read(&v, 1); return v; }
        uint16_t ReadU16()   { uint16_t v; Read(&v, 2); return v; }
        uint32_t ReadU32()   { uint32_t v; Read(&v, 4); return v; }
        int32_t  ReadI32()   { int32_t  v; Read(&v, 4); return v; }
        int64_t  ReadI64()   { int64_t  v; Read(&v, 8); return v; }
        uint64_t ReadU64()   { uint64_t v; Read(&v, 8); return v; }
        float    ReadFloat() { float    v; Read(&v, 4); return v; }

        std::string ReadString()
        {
            uint16_t len = ReadU16();
            if (!CanRead(len)) return "";
            std::string s(reinterpret_cast<const char*>(m_data + m_pos), len);
            m_pos += len;
            return s;
        }

        void Skip(size_t n) { m_pos += n; }
        size_t Pos() const { return m_pos; }

    private:
        void Read(void* dst, size_t n)
        {
            if (CanRead(n)) {
                std::memcpy(dst, m_data + m_pos, n);
                m_pos += n;
            }
        }

        const uint8_t* m_data;
        size_t m_size;
        size_t m_pos;
    };

    // ─── BCPS Container (compressed .sfs) ───
    // Reference: StarfieldSaveTool SfsFile.cs

    struct BcpsHeader
    {
        int32_t  version0;
        int64_t  chunkSizesOffset;
        int64_t  unknown0;
        int64_t  compressedDataOffset;
        int64_t  uncompressedDataSize;
        float    version1;
        int32_t  unknown1;
        int64_t  sizeUncompressedChunks;
        int64_t  paddingSize;
        int32_t  unknown2;
        char     compressionType[4];
    };

    static int PadToNearest(int paddingSize, int size)
    {
        int mask = paddingSize - 1;
        return (size + mask) & ~mask;
    }

    // Decompress only the first chunk (contains the save header)
    static bool DecompressFirstChunk(const std::vector<uint8_t>& a_fileData, std::vector<uint8_t>& a_out)
    {
        if (a_fileData.size() < 72)
            return false;

        // Check magic "BCPS"
        if (std::memcmp(a_fileData.data(), "BCPS", 4) != 0)
            return false;

        // Read BCPS header field-by-field (no struct padding issues)
        BinaryReader br(a_fileData.data() + 4, a_fileData.size() - 4);

        BcpsHeader hdr{};
        hdr.version0              = br.ReadI32();
        hdr.chunkSizesOffset      = br.ReadI64();
        hdr.unknown0              = br.ReadI64();
        hdr.compressedDataOffset  = br.ReadI64();
        hdr.uncompressedDataSize  = br.ReadI64();
        hdr.version1              = br.ReadFloat();
        hdr.unknown1              = br.ReadI32();
        hdr.sizeUncompressedChunks = br.ReadI64();
        hdr.paddingSize           = br.ReadI64();
        hdr.unknown2              = br.ReadI32();
        std::memcpy(hdr.compressionType, a_fileData.data() + 4 + br.Pos(), 4);

        PLOG_INFO("[PULSAR] BCPS: chunkOff={} compOff={} uncompSize={} chunkSize={} pad={} comp='{}{}{}{}'"sv,
            hdr.chunkSizesOffset, hdr.compressedDataOffset, hdr.uncompressedDataSize,
            hdr.sizeUncompressedChunks, hdr.paddingSize,
            hdr.compressionType[0], hdr.compressionType[1], hdr.compressionType[2], hdr.compressionType[3]);

        if (std::memcmp(hdr.compressionType, "ZIP ", 4) != 0)
            return false;

        // Calculate chunk count
        int chunkCount = static_cast<int>(
            (hdr.uncompressedDataSize + hdr.sizeUncompressedChunks - 1) / hdr.sizeUncompressedChunks);
        if (chunkCount < 1)
            return false;

        // Read first chunk size from the chunk sizes array
        if (hdr.chunkSizesOffset + 4 > static_cast<int64_t>(a_fileData.size()))
            return false;

        int32_t firstChunkSize = 0;
        std::memcpy(&firstChunkSize, a_fileData.data() + hdr.chunkSizesOffset, 4);

        // Padded size for reading
        int paddedSize = PadToNearest(static_cast<int>(hdr.paddingSize), firstChunkSize);

        PLOG_INFO("[PULSAR] BCPS: chunkCount={} firstChunk={} padded={}"sv,
            chunkCount, firstChunkSize, paddedSize);

        // Read compressed data from compressedDataOffset
        if (hdr.compressedDataOffset + paddedSize > static_cast<int64_t>(a_fileData.size()))
            return false;

        const uint8_t* compData = a_fileData.data() + hdr.compressedDataOffset;

        // Decompress with zlib
        uLongf decompSize = static_cast<uLongf>(hdr.sizeUncompressedChunks);
        a_out.resize(decompSize);

        // zlib uncompress expects raw deflate but Starfield uses zlib-wrapped deflate
        // Try uncompress first (zlib format), then inflate if that fails
        int ret = uncompress(a_out.data(), &decompSize, compData, static_cast<uLong>(firstChunkSize));
        if (ret != Z_OK) {
            // Try with padded size
            decompSize = static_cast<uLongf>(hdr.sizeUncompressedChunks);
            ret = uncompress(a_out.data(), &decompSize, compData, static_cast<uLong>(paddedSize));
        }
        if (ret != Z_OK) {
            // Try raw inflate (no zlib header)
            z_stream strm{};
            strm.next_in = const_cast<Bytef*>(compData);
            strm.avail_in = static_cast<uInt>(firstChunkSize);
            strm.next_out = a_out.data();
            strm.avail_out = static_cast<uInt>(hdr.sizeUncompressedChunks);

            if (inflateInit2(&strm, -15) == Z_OK) {  // -15 = raw deflate
                ret = inflate(&strm, Z_FINISH);
                decompSize = strm.total_out;
                inflateEnd(&strm);
                if (ret != Z_STREAM_END) ret = Z_DATA_ERROR;
                else ret = Z_OK;
            }
        }
        if (ret != Z_OK) {
            PLOG_WARN("[PULSAR] SaveScanner: zlib error {} (chunkSize={}, padded={}, decompTarget={})"sv,
                ret, firstChunkSize, paddedSize, (int64_t)hdr.sizeUncompressedChunks);
            return false;
        }

        a_out.resize(decompSize);
        return true;
    }

    // ─── SFS_SAVEGAME Header Parser ───
    // Reference: StarfieldSaveTool DatFile.cs ReadHeader()

    static bool ParseDecompressedHeader(const std::vector<uint8_t>& a_data, SaveInfo& a_out)
    {
        if (a_data.size() < 16)
            return false;

        // Check magic "SFS_SAVEGAME"
        if (std::memcmp(a_data.data(), "SFS_SAVEGAME", 12) != 0)
            return false;

        BinaryReader br(a_data.data() + 12, a_data.size() - 12);

        uint32_t headerSize = br.ReadU32();
        (void)headerSize;

        // FileHeader fields (DatFile.cs ReadHeader)
        uint32_t engineVersion = br.ReadU32();  (void)engineVersion;
        uint8_t  saveVersion   = br.ReadU8();   (void)saveVersion;
        uint32_t saveNumber    = br.ReadU32();   (void)saveNumber;

        a_out.playerName     = br.ReadString();
        a_out.playerLevel    = br.ReadU32();
        a_out.playerLocation = br.ReadString();
        a_out.playtime       = br.ReadString();
        a_out.raceName       = br.ReadString();
        a_out.gender         = br.ReadU16();
        a_out.experience     = br.ReadFloat();
        a_out.experienceRequired = br.ReadFloat();

        uint64_t fileTime = br.ReadU64();  (void)fileTime;
        uint32_t unknown0 = br.ReadU32();  (void)unknown0;
        br.Skip(8); // padding

        // After header: saveVersion byte, then game version strings
        uint8_t sv2 = br.ReadU8(); (void)sv2;
        a_out.gameVersion = br.ReadString();

        return true;
    }

    // ─── Public API ───

    bool ParseSaveFile(const std::string& a_path, SaveInfo& a_out)
    {
        // Read entire file
        std::ifstream file(a_path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            PLOG_WARN("[PULSAR] SaveScanner: can't open file"sv);
            return false;
        }

        auto fileSize = file.tellg();
        if (fileSize < 64) {
            PLOG_WARN("[PULSAR] SaveScanner: file too small ({})"sv, (int64_t)fileSize);
            return false;
        }

        std::vector<uint8_t> fileData(static_cast<size_t>(fileSize));
        file.seekg(0);
        file.read(reinterpret_cast<char*>(fileData.data()), fileSize);
        file.close();

        // Decompress first chunk
        std::vector<uint8_t> decompressed;
        if (!DecompressFirstChunk(fileData, decompressed)) {
            PLOG_WARN("[PULSAR] SaveScanner: decompress failed"sv);
            return false;
        }

        PLOG_INFO("[PULSAR] SaveScanner: decompressed {} bytes"sv, decompressed.size());

        // Parse header
        if (!ParseDecompressedHeader(decompressed, a_out)) {
            PLOG_WARN("[PULSAR] SaveScanner: header parse failed"sv);
            return false;
        }

        // Store filename
        a_out.filename = std::filesystem::path(a_path).filename().string();

        return true;
    }

    std::vector<SaveInfo> ScanSaves()
    {
        std::vector<SaveInfo> saves;

        // Find Starfield saves directory
        // Try multiple known paths (OneDrive redirect can break CSIDL_PERSONAL)
        std::filesystem::path savesDir;

        // Method 1: SHGetKnownFolderPath (modern, handles redirects)
        PWSTR wDocPath = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &wDocPath))) {
            savesDir = std::filesystem::path(wDocPath) / "My Games" / "Starfield" / "Saves";
            CoTaskMemFree(wDocPath);
            PLOG_INFO("[PULSAR] SaveScanner: trying {}"sv, savesDir.string());
        }

        // Method 2: Fallback to USERPROFILE
        if (savesDir.empty() || !std::filesystem::exists(savesDir)) {
            const char* userProfile = std::getenv("USERPROFILE");
            if (userProfile) {
                savesDir = std::filesystem::path(userProfile) / "Documents" / "My Games" / "Starfield" / "Saves";
                PLOG_INFO("[PULSAR] SaveScanner: fallback {}"sv, savesDir.string());
            }
        }

        if (savesDir.empty() || !std::filesystem::exists(savesDir)) {
            PLOG_WARN("[PULSAR] SaveScanner: saves directory not found"sv);
            return saves;
        }

        PLOG_INFO("[PULSAR] SaveScanner: scanning {}"sv, savesDir.string());

        // Collect all .sfs files with their last-write-time for sorting
        struct SaveEntry { std::filesystem::path path; std::filesystem::file_time_type time; };
        std::vector<SaveEntry> entries;

        for (auto& entry : std::filesystem::directory_iterator(savesDir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".sfs") {
                entries.push_back({ entry.path(), entry.last_write_time() });
            }
        }

        // Sort newest first
        std::sort(entries.begin(), entries.end(),
            [](const SaveEntry& a, const SaveEntry& b) { return a.time > b.time; });

        PLOG_INFO("[PULSAR] SaveScanner: {} .sfs files found"sv, entries.size());

        // Parse each save
        for (auto& e : entries) {
            SaveInfo info;
            if (ParseSaveFile(e.path.string(), info)) {
                saves.push_back(std::move(info));
            } else {
                PLOG_WARN("[PULSAR] SaveScanner: FAILED to parse {}"sv,
                    e.path.filename().string());
            }
        }

        return saves;
    }

    std::string GetSavesDirectory()
    {
        std::filesystem::path savesDir;
        PWSTR wDocPath = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &wDocPath))) {
            savesDir = std::filesystem::path(wDocPath) / "My Games" / "Starfield" / "Saves";
            CoTaskMemFree(wDocPath);
        }
        if (savesDir.empty() || !std::filesystem::exists(savesDir)) {
            const char* userProfile = std::getenv("USERPROFILE");
            if (userProfile) {
                savesDir = std::filesystem::path(userProfile) / "Documents" / "My Games" / "Starfield" / "Saves";
            }
        }
        if (!std::filesystem::exists(savesDir)) return {};
        return savesDir.string();
    }

    std::string GetMostRecentSavePath()
    {
        std::string dir = GetSavesDirectory();
        if (dir.empty()) return {};

        struct SaveEntry { std::filesystem::path path; std::filesystem::file_time_type time; };
        std::vector<SaveEntry> entries;
        for (auto& entry : std::filesystem::directory_iterator(dir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".sfs") {
                entries.push_back({ entry.path(), entry.last_write_time() });
            }
        }
        if (entries.empty()) return {};
        auto it = std::max_element(entries.begin(), entries.end(),
            [](const SaveEntry& a, const SaveEntry& b) { return a.time < b.time; });
        return it->path.string();
    }
}
