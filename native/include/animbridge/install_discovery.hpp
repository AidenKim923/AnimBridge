#pragma once
// First-run defaults that come from the machine rather than from the user:
//   * which Unreal Engine installs the Epic Games Launcher knows about, and
//   * which UI language the installer was run in (or, failing that, the OS UI language).
// Everything here is pure (no file or registry access) so it can be unit tested; the Windows
// shell supplies the raw text / LANGID and the bridge validates the resulting paths.
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace animbridge::discovery {

struct UnrealInstall {
    int major = 0;
    int minor = 0;
    std::string location;  // UTF-8 engine root, e.g. C:\Program Files\Epic Games\UE_5.8
};

// "UE_5.8" -> {5, 8}. Launcher entries for plugins ("FabPlugin_5.8"), older engines
// ("UE_4.27") or custom builds ("UE_5.8_custom") are not engine installs we support.
inline std::optional<std::pair<int, int>> unrealEngineVersion(std::string_view name) {
    constexpr std::string_view prefix = "UE_5.";
    if (!name.starts_with(prefix)) return std::nullopt;
    const auto minor = name.substr(prefix.size());
    if (minor.empty() || minor.size() > 4 || !std::all_of(minor.begin(), minor.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) return std::nullopt;
    return std::make_pair(5, std::stoi(std::string(minor)));
}

// Newest version first. stable_sort keeps the launcher's order for equal versions.
inline void sortNewestFirst(std::vector<UnrealInstall>& installs) {
    std::stable_sort(installs.begin(), installs.end(), [](const UnrealInstall& a, const UnrealInstall& b) {
        return std::make_pair(a.major, a.minor) > std::make_pair(b.major, b.minor);
    });
}

// LauncherInstalled.dat is JSON: {"InstallationList": [{"AppName": "UE_5.8", "InstallLocation": "..."}]}.
// The launcher also lists plugins and Fab/Quixel items with the same InstallLocation, so only
// "UE_5.x" entries count. Malformed input yields an empty list: auto-detection is best effort.
inline std::vector<UnrealInstall> parseLauncherInstalled(std::string_view text) {
    std::vector<UnrealInstall> result;
    // The launcher writes a UTF-8 file, sometimes with a byte order mark.
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    const auto document = nlohmann::json::parse(text, nullptr, false);
    if (!document.is_object() || !document.contains("InstallationList") || !document["InstallationList"].is_array()) return result;
    for (const auto& entry : document["InstallationList"]) {
        if (!entry.is_object()) continue;
        const auto name = entry.find("AppName");
        const auto location = entry.find("InstallLocation");
        if (name == entry.end() || !name->is_string() || location == entry.end() || !location->is_string() || location->get_ref<const std::string&>().empty()) continue;
        const auto version = unrealEngineVersion(name->get_ref<const std::string&>());
        if (!version) continue;
        result.push_back({version->first, version->second, location->get<std::string>()});
    }
    sortNewestFirst(result);
    return result;
}

// ---- Interface language -------------------------------------------------------------

inline std::string lowercase(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

// Maps the installer's ActiveLanguage (the Name in packaging/AnimBridge.iss [Languages]) to one
// of the UI locale codes. A code that is already a UI locale ("pt-BR") is accepted as well.
// Returns empty for anything unknown, so the caller can fall back to the OS language.
inline std::string languageFromInstaller(std::string_view value) {
    if (value.starts_with("\xEF\xBB\xBF")) value.remove_prefix(3);  // tolerate a UTF-8 byte order mark
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.remove_suffix(1);
    struct Entry { std::string_view key; std::string_view code; };
    // Keys are lowercase. Inno ships no Chinese or Hindi file, but unofficial translations
    // use these names, so mapping them costs nothing.
    static constexpr Entry table[] = {
        {"english", "en"}, {"arabic", "ar"}, {"spanish", "es"}, {"french", "fr"}, {"japanese", "ja"},
        {"korean", "ko"}, {"polish", "pl"}, {"brazilianportuguese", "pt-BR"}, {"portuguese", "pt-PT"},
        {"russian", "ru"}, {"thai", "th"}, {"turkish", "tr"}, {"hindi", "hi"},
        {"chinesesimplified", "zh-CN"}, {"chinesetraditional", "zh-TW"},
        {"en", "en"}, {"ar", "ar"}, {"es", "es"}, {"fr", "fr"}, {"ja", "ja"}, {"ko", "ko"}, {"pl", "pl"},
        {"pt-br", "pt-BR"}, {"pt-pt", "pt-PT"}, {"ru", "ru"}, {"th", "th"}, {"tr", "tr"}, {"hi", "hi"},
        {"zh-cn", "zh-CN"}, {"zh-tw", "zh-TW"},
    };
    const auto key = lowercase(value);
    for (const auto& entry : table) if (entry.key == key) return std::string(entry.code);
    return {};
}

// Maps a Windows LANGID (GetUserDefaultUILanguage) to a UI locale code, or empty if unsupported.
// LANGID = (sublanguage << 10) | primary language.
inline std::string languageFromWindowsLangId(unsigned langId) {
    const unsigned primary = langId & 0x3FF, sub = (langId >> 10) & 0x3F;
    switch (primary) {
    case 0x01: return "ar";
    case 0x09: return "en";
    case 0x0A: return "es";
    case 0x0C: return "fr";
    case 0x11: return "ja";
    case 0x12: return "ko";
    case 0x15: return "pl";
    case 0x19: return "ru";
    case 0x1E: return "th";
    case 0x1F: return "tr";
    case 0x39: return "hi";
    case 0x16: return sub == 2 ? "pt-PT" : "pt-BR";  // 0x0816 is Portugal; Brazil and neutral use pt-BR
    case 0x04:
        // Traditional: Taiwan (1), Hong Kong (3), Macao (5) and the neutral zh-Hant (0x1F).
        // Simplified: China (2), Singapore (4) and the neutral zh-Hans (0).
        return sub == 1 || sub == 3 || sub == 5 || sub == 0x1F ? "zh-TW" : "zh-CN";
    default: return {};
    }
}

// installLanguageText: contents of {app}\install-language.txt (empty if the file is missing).
// Order of preference: what the user picked in the installer, the OS UI language, English.
inline std::string chooseDefaultLanguage(std::string_view installLanguageText, unsigned osLangId) {
    if (auto code = languageFromInstaller(installLanguageText); !code.empty()) return code;
    if (auto code = languageFromWindowsLangId(osLangId); !code.empty()) return code;
    return "en";
}

}  // namespace animbridge::discovery
