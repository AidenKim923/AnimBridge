// Unit tests for the pure first-run helpers: Epic launcher list parsing, newest-version choice
// and installer / OS language mapping. No files or registry are touched.
#include "animbridge/install_discovery.hpp"
#include <iostream>
#include <stdexcept>

using namespace animbridge::discovery;

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

// Shape of a real LauncherInstalled.dat: engines, but also plugins that share the install folder.
const char* launcherList = R"({
  "InstallationList": [
    {"InstallLocation": "C:\\Program Files\\Epic Games\\UE_5.8", "AppName": "UE_5.8"},
    {"InstallLocation": "C:\\Program Files\\Epic Games\\UE_5.8", "AppName": "FabPlugin_5.8"},
    {"InstallLocation": "C:\\Program Files\\Epic Games\\UE_5.7", "AppName": "QuixelBridge_5.7"},
    {"InstallLocation": "D:\\Engines\\UE_5.10", "AppName": "UE_5.10"},
    {"InstallLocation": "D:\\Engines\\UE_5.7", "AppName": "UE_5.7"},
    {"InstallLocation": "D:\\Engines\\UE_4.27", "AppName": "UE_4.27"},
    {"InstallLocation": "D:\\Engines\\Custom", "AppName": "UE_5.8_Custom"},
    {"InstallLocation": "", "AppName": "UE_5.6"},
    {"AppName": "UE_5.5"},
    "not an object"
  ]
})";
}

int main() {
    try {
        int passed = 0;

        // Only UE_5.x engine entries are kept, newest first (5.10 is newer than 5.8: numeric, not textual).
        const auto installs = parseLauncherInstalled(launcherList);
        require(installs.size() == 3, "Plugins, UE 4, custom builds and incomplete entries must be ignored");
        require(installs[0].minor == 10 && installs[0].location == "D:\\Engines\\UE_5.10", "5.10 must sort before 5.8");
        require(installs[1].minor == 8 && installs[1].location == "C:\\Program Files\\Epic Games\\UE_5.8", "5.8 must be second");
        require(installs[2].minor == 7, "5.7 must be last"); ++passed;

        // Unusable input never throws; auto-detection is best effort.
        require(parseLauncherInstalled("").empty() && parseLauncherInstalled("{").empty() && parseLauncherInstalled("[]").empty()
                && parseLauncherInstalled(R"({"InstallationList": 3})").empty(), "Malformed launcher lists must yield no installs");
        require(parseLauncherInstalled(std::string("\xEF\xBB\xBF") + R"({"InstallationList":[{"AppName":"UE_5.8","InstallLocation":"E:\\UE"}]})").size() == 1, "A byte order mark must be accepted"); ++passed;

        // Folder-name scan (Program Files) uses the same version rule.
        require(unrealEngineVersion("UE_5.8") && unrealEngineVersion("UE_5.8")->second == 8, "UE_5.8 must parse");
        require(!unrealEngineVersion("UE_5.") && !unrealEngineVersion("UE_5.x") && !unrealEngineVersion("UE_5.12345") && !unrealEngineVersion("ue_5.8") && !unrealEngineVersion("Launcher"), "Non-engine folders must be ignored"); ++passed;

        // Equal versions keep the launcher's order; newer always wins.
        std::vector<UnrealInstall> mixed = {{5, 7, "a"}, {5, 8, "b"}, {5, 8, "c"}, {5, 9, "d"}};
        sortNewestFirst(mixed);
        require(mixed[0].location == "d" && mixed[1].location == "b" && mixed[2].location == "c" && mixed[3].location == "a", "Newest-first order is wrong"); ++passed;

        // Installer language names (packaging/AnimBridge.iss [Languages]) map to UI locales.
        require(languageFromInstaller("english") == "en" && languageFromInstaller("korean") == "ko" && languageFromInstaller("japanese") == "ja"
                && languageFromInstaller("arabic") == "ar" && languageFromInstaller("spanish") == "es" && languageFromInstaller("french") == "fr"
                && languageFromInstaller("polish") == "pl" && languageFromInstaller("russian") == "ru" && languageFromInstaller("thai") == "th"
                && languageFromInstaller("turkish") == "tr", "Installer language names must map to UI locales");
        require(languageFromInstaller("brazilianportuguese") == "pt-BR" && languageFromInstaller("portuguese") == "pt-PT", "Portuguese variants must stay distinct");
        require(languageFromInstaller("  Korean\r\n") == "ko" && languageFromInstaller("\xEF\xBB\xBFkorean") == "ko", "Whitespace, case and a byte order mark must be tolerated");
        require(languageFromInstaller("pt-br") == "pt-BR", "UI locale codes are matched case-insensitively");
        require(languageFromInstaller("zh-TW") == "zh-TW" && languageFromInstaller("chinesesimplified") == "zh-CN", "UI locale codes must pass through");
        require(languageFromInstaller("klingon").empty() && languageFromInstaller("").empty(), "Unknown names must map to nothing"); ++passed;

        // Windows LANGIDs: primary language in the low 10 bits, sublanguage above.
        require(languageFromWindowsLangId(0x0409) == "en" && languageFromWindowsLangId(0x0809) == "en", "English LANGIDs");
        require(languageFromWindowsLangId(0x0412) == "ko" && languageFromWindowsLangId(0x0411) == "ja" && languageFromWindowsLangId(0x0C0A) == "es"
                && languageFromWindowsLangId(0x040C) == "fr" && languageFromWindowsLangId(0x0419) == "ru" && languageFromWindowsLangId(0x0415) == "pl"
                && languageFromWindowsLangId(0x041E) == "th" && languageFromWindowsLangId(0x041F) == "tr" && languageFromWindowsLangId(0x0401) == "ar"
                && languageFromWindowsLangId(0x0439) == "hi", "Single-locale LANGIDs");
        require(languageFromWindowsLangId(0x0416) == "pt-BR" && languageFromWindowsLangId(0x0816) == "pt-PT", "Portuguese LANGIDs");
        require(languageFromWindowsLangId(0x0804) == "zh-CN" && languageFromWindowsLangId(0x1004) == "zh-CN" && languageFromWindowsLangId(0x0004) == "zh-CN", "Simplified Chinese LANGIDs");
        require(languageFromWindowsLangId(0x0404) == "zh-TW" && languageFromWindowsLangId(0x0C04) == "zh-TW" && languageFromWindowsLangId(0x7C04) == "zh-TW", "Traditional Chinese LANGIDs");
        require(languageFromWindowsLangId(0x0407).empty(), "German is not a UI language"); ++passed;

        // Preference order: installer choice, OS language, English.
        require(chooseDefaultLanguage("korean", 0x0409) == "ko", "The installer language must beat the OS language");
        require(chooseDefaultLanguage("", 0x0411) == "ja", "A missing install file must fall back to the OS language");
        require(chooseDefaultLanguage("klingon", 0x0412) == "ko", "An unknown installer name must fall back to the OS language");
        require(chooseDefaultLanguage("", 0x0407) == "en", "An unsupported OS language must fall back to English"); ++passed;

        std::cout << passed << " install discovery checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
