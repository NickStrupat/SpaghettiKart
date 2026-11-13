// Makes mk64.o2r from a Mario Kart 64 ROM, in the browser (tools/web/extractor/CMakeLists.txt). The same steps as
// GameExtractor::GenerateOTR on the desktop (src/port/GameExtractor.cpp); src/port/web/ExtractWorker.js runs it.
//
//   extractor <rom.z64> <source folder> <destination folder>
//
// The source folder holds Torch's config.yml and asset descriptions; mk64.o2r is written to the destination.
#include "AssetsVersionDefine.h"
#include "Companion.h"

#include <atomic>
#include <cstdio>
#include <exception>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

int main(int argc, char* argv[]) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: extractor <rom.z64> <source folder> <destination folder>\n");
        return 2;
    }
    std::ifstream file(argv[1], std::ios::binary);
    if (!file.is_open()) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }
    std::vector<uint8_t> rom((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    try {
        // As strings: a char* would choose the constructor whose next parameter is the bool "modding".
        Companion::Instance = new Companion(rom, ArchiveType::O2R, false, std::string(argv[2]), std::string(argv[3]));
        Companion::Instance->SetAdditionalFiles({ "meta/mods.toml" });
        // Which version made it (src/port/AssetsVersion.h, which the game's side reads; this one is Torch's alone).
        const std::string version = SK_ASSETS_VERSION;
        Companion::Instance->RegisterArchiveFile("assetsVersion", std::vector<char>(version.begin(), version.end()));
        std::atomic<size_t> assetCount = 0;
        Companion::Instance->Init(ExportType::Binary, assetCount, true);
        // Companion::Init processes the ROM itself everywhere but under Emscripten.
        Companion::Instance->Process(assetCount);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Torch failed: %s\n", error.what());
        return 1;
    }
    return 0;
}
