#pragma once
// Release update check helpers. The host fetches the GitHub releases list; these
// functions only derive the fixed API URL and compare versions, so they stay testable.
#include <nlohmann/json.hpp>
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

struct Release {
    std::string tag;      // validated by versionParts, safe to put in a URL path
    std::string version;  // tag without the leading "v"
    std::string name;
    bool prerelease = false;
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
        best = release;
        bestParts = parts;
    }
    return best;
}

}  // namespace animbridge::update
