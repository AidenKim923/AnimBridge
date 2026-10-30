#pragma once
// Release update check helpers. The host fetches the GitHub releases list; these
// functions only derive the fixed API URL and compare versions, so they stay testable.
#include <nlohmann/json.hpp>
#include <cctype>
#include <cstdint>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace animbridge::update {

// "https://github.com/Owner/Repo" -> releases API URL, or empty for any other URL.
inline std::string releasesApiUrl(std::string_view repository) {
    static const std::regex pattern(R"(^https://github\.com/([A-Za-z0-9_.-]+)/([A-Za-z0-9_.-]+?)(?:\.git)?/?$)");
    const std::string text(repository);
    std::smatch match;
    if (!std::regex_match(text, match, pattern)) return {};
    return "https://api.github.com/repos/" + match[1].str() + "/" + match[2].str() + "/releases?per_page=20";
}

// Major/minor/patch of "v1.2.3" or "1.2.3-beta"; empty when the tag is not a release version.
inline std::vector<int> versionParts(std::string_view tag) {
    static const std::regex pattern(R"(^v?(\d{1,6})\.(\d{1,6})\.(\d{1,6})(?:[-.][A-Za-z0-9]{1,32}){0,4}$)");
    const std::string text(tag);
    std::smatch match;
    if (!std::regex_match(text, match, pattern)) return {};
    return {std::stoi(match[1].str()), std::stoi(match[2].str()), std::stoi(match[3].str())};
}

inline bool isNewer(std::string_view latest, std::string_view current) {
    const auto a = versionParts(latest), b = versionParts(current);
    return !a.empty() && !b.empty() && a > b;
}

// --- In-app update download -------------------------------------------------------
// The installer and its checksum list come from the same GitHub release. The checksum
// therefore protects integrity (a truncated or corrupted download), not authenticity:
// whoever can publish a release can publish both. Code signing the installer is the
// long-term fix for authenticity.

inline constexpr std::string_view ChecksumsAssetName = "SHA256SUMS.txt";

// The exact installer file name the release pipeline publishes for a version.
inline std::string installerAssetName(std::string_view version) {
    return "AnimBridge-" + std::string(version) + "-Setup-x64.exe";
}

// Downloads (including every redirect hop) may only use HTTPS on GitHub's own release
// hosts. No user info, no explicit port, no fragment; hosts are matched exactly.
inline bool isAllowedDownloadUrl(std::string_view url) {
    static const std::regex pattern(R"(^https://([a-z0-9.-]+)(/[!$&'()*+,;=:@%A-Za-z0-9._~/?-]*)$)");
    const std::string text(url);
    std::smatch match;
    if (text.size() > 4096 || !std::regex_match(text, match, pattern)) return false;
    const auto host = match[1].str();
    return host == "github.com" || host == "objects.githubusercontent.com" || host == "release-assets.githubusercontent.com";
}

// SHA-256 (lowercase hex) for fileName in a sha256sum-style list. Lines look like
// "<hash> *<name>" (binary mode) or "<hash>  <name>" (text mode). Returns nothing when
// the file is not listed, or when it is listed twice with different hashes.
inline std::optional<std::string> checksumFor(std::string_view sums, std::string_view fileName) {
    static const std::regex line(R"(^([0-9A-Fa-f]{64})[ \t]+\*?(.+?)[ \t]*$)");
    std::optional<std::string> found;
    std::size_t start = 0;
    while (start <= sums.size()) {
        auto end = sums.find('\n', start);
        if (end == std::string_view::npos) end = sums.size();
        std::string text(sums.substr(start, end - start));
        if (!text.empty() && text.back() == '\r') text.pop_back();
        std::smatch match;
        if (std::regex_match(text, match, line) && match[2].str() == fileName) {
            std::string hash = match[1].str();
            for (auto& c : hash) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (found && *found != hash) return std::nullopt;  // conflicting entries: trust neither
            found = hash;
        }
        start = end + 1;
    }
    return found;
}

struct UpdateAssets {
    std::string installerUrl;
    std::string checksumsUrl;
    std::uint64_t installerSize = 0;  // 0 when the API did not report a size
};

// Picks the installer and SHA256SUMS.txt from one release entry of the releases API.
// Only exact names and allowed download URLs count; anything else means "not installable".
inline std::optional<UpdateAssets> selectAssets(const nlohmann::json& release, std::string_view version) {
    if (!release.is_object() || !release.contains("assets") || !release["assets"].is_array()) return std::nullopt;
    const auto installerName = installerAssetName(version);
    UpdateAssets result;
    for (const auto& asset : release["assets"]) {
        if (!asset.is_object() || !asset.contains("name") || !asset["name"].is_string()) continue;
        if (!asset.contains("browser_download_url") || !asset["browser_download_url"].is_string()) continue;
        const auto& name = asset["name"].get_ref<const std::string&>();
        const auto& url = asset["browser_download_url"].get_ref<const std::string&>();
        if (!isAllowedDownloadUrl(url)) continue;
        if (name == installerName) {
            result.installerUrl = url;
            if (asset.contains("size") && asset["size"].is_number_integer() && asset["size"].get<std::int64_t>() > 0) result.installerSize = asset["size"].get<std::uint64_t>();
        } else if (name == ChecksumsAssetName) {
            result.checksumsUrl = url;
        }
    }
    if (result.installerUrl.empty() || result.checksumsUrl.empty()) return std::nullopt;
    return result;
}

struct Release {
    std::string tag;      // validated by versionParts, safe to put in a URL path
    std::string version;  // tag without the leading "v"
    std::string name;
    bool prerelease = false;
    std::optional<UpdateAssets> assets;  // set when the release can be installed in-app
};

// Newest published release in a GitHub releases list. Drafts and unusual tags are ignored.
inline std::optional<Release> newestRelease(const nlohmann::json& list) {
    std::optional<Release> best;
    std::vector<int> bestParts;
    if (!list.is_array()) return best;
    for (const auto& item : list) {
        if (!item.is_object()) continue;
        if (item.contains("draft") && (!item["draft"].is_boolean() || item["draft"].get<bool>())) continue;
        if (!item.contains("tag_name") || !item["tag_name"].is_string()) continue;
        const auto tag = item["tag_name"].get<std::string>();
        const auto parts = versionParts(tag);
        if (parts.empty() || (best && parts <= bestParts)) continue;
        Release release;
        release.tag = tag;
        release.version = tag.front() == 'v' ? tag.substr(1) : tag;
        if (item.contains("name") && item["name"].is_string()) release.name = item["name"].get<std::string>().substr(0, 200);
        release.prerelease = item.contains("prerelease") && item["prerelease"].is_boolean() && item["prerelease"].get<bool>();
        release.assets = selectAssets(item, release.version);
        best = release;
        bestParts = parts;
    }
    return best;
}

}  // namespace animbridge::update
