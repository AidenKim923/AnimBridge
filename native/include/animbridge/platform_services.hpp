#pragma once
#include "core.hpp"
#include <nlohmann/json.hpp>
#include <functional>
#include <mutex>
#include <set>

namespace animbridge {
std::string nativeCapability();
// UI callbacks are scheduled by the shell after returning from its WebView callback.
struct DesktopPlatformServices {
    using Json = nlohmann::json;
    using PickCompletion = std::function<void(std::vector<fs::path>, std::string)>;
    using SaveCompletion = std::function<void(fs::path, std::string)>;
    std::function<void(const Json&, bool, PickCompletion)> pick;
    std::function<void(const Json&, SaveCompletion)> save;
    std::function<void(const std::string&)> copyText;
    std::function<Json(const fs::path&)> reveal;
    std::function<void(const std::string&)> openExternal;
    // HTTPS GET returning the response body; throws std::runtime_error on failure or non-200.
    // Unset on shells that do not support the release update check.
    std::function<std::string(const std::string& url)> httpGet;
    // Root of the Unity Hub editor install folder (root/<version>/Editor/Unity.exe).
    // Unset disables automatic Unity Editor discovery (tests and shells without a trusted root).
    std::function<fs::path()> unityHubEditorRoot;
};
// Windows: Program Files/Unity/Hub/Editor via SHGetKnownFolderPath (admin-writable only). Other platforms: empty.
fs::path defaultUnityHubEditorRoot();

// Only the shell supplies sender provenance. In particular the GTK signal's
// payload cannot supply a trustworthy origin; its no-frame lifecycle is required.
class WebViewSession {
public:
    using Json = nlohmann::json;
    void invalidate();
    Json commit(const std::string& origin, const std::string& platform, bool customFrame);
    bool accept(const Json&, const std::string& engineOrigin, bool mainFrame, bool frameCreationBlocked);
    Json envelope(const Json& value) const;
    bool ready() const;
private:
    mutable std::mutex mutex_;
    std::string epoch_, capability_, origin_;
    bool committed_ = false;
    std::set<std::string> seenIds_;
};
}
