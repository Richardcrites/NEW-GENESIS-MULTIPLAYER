#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Pulsar::SaveScanner
{
    struct SaveInfo
    {
        std::string filename;       // just the .sfs filename
        std::string playerName;
        uint32_t    playerLevel = 0;
        std::string playerLocation;
        std::string playtime;
        std::string raceName;
        uint16_t    gender = 0;
        float       experience = 0.0f;
        float       experienceRequired = 0.0f;
        std::string gameVersion;
    };

    // Scan all .sfs files in the Starfield saves directory
    // Returns metadata for each save, sorted by newest first
    std::vector<SaveInfo> ScanSaves();

    // Parse a single .sfs file and extract header metadata
    // Returns true on success
    bool ParseSaveFile(const std::string& a_path, SaveInfo& a_out);

    // Returns full path to the Starfield saves directory
    std::string GetSavesDirectory();

    // Returns full path to the most recent .sfs save file
    std::string GetMostRecentSavePath();
}
