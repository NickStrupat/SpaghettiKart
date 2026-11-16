#pragma once

#include "Companion.h"
#include <filesystem>
#include <vector>
#include <cstdint>

class GameExtractor {
public:
    static bool GenAssetFile();
    std::optional<std::string> ValidateChecksum() const;
    bool SelectGameFromUI();
    void GetRoms(std::vector<std::string>& roms);
    bool GenerateOTR() const;
    /**
     * Loads the copy of the ROM that an earlier extraction kept (GenerateOTR), so that a newer version can make
     * mk64.o2r again without asking the player for it.
     * @returns false when there is no kept ROM, or it is not a supported one.
     */
    bool LoadSavedRom();
private:
    /** Keeps a copy of the ROM in the game's folder, for LoadSavedRom. */
    void SaveRom() const;

    fs::path mGamePath;
    std::vector<uint8_t> mGameData;
};
