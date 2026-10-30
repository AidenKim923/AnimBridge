#pragma once
#include "core.hpp"
#include "install_discovery.hpp"
#include <nlohmann/json.hpp>
#include <cstdint>
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
    // Streams an HTTPS download into `destination`. Every redirect hop must pass
    // update::isAllowedDownloadUrl. `progress(received, total)` (total 0 = unknown) may
    // return false to cancel; the function then returns false and leaves a partial file.
    // Throws std::runtime_error on network errors, non-200 replies or more than maxBytes.
    using DownloadProgress = std::function<bool(std::uint64_t received, std::uint64_t total)>;
    std::function<bool(const std::string& url, const fs::path& destination, std::uint64_t maxBytes, const DownloadProgress& progress)> download;
    // Starts a verified installer as a normal visible process (so Windows can show UAC if
    // ever needed) and returns once it is running. Throws when it could not be started.
    std::function<void(const fs::path& installer, const std::vector<std::string>& arguments)> launchInstaller;
    // Asks the shell to close the app (like the window close button) so the installer can
    // replace its files. May be called from any thread.
    std::function<void()> requestExit;
    // Root of the Unity Hub editor install folder (root/<version>/Editor/Unity.exe).
    // Unset disables automatic Unity Editor discovery (tests and shells without a trusted root).
    std::function<fs::path()> unityHubEditorRoot;
    // Unreal Engine installs known to the Epic Games Launcher or found under Program Files, in any order.
    // The bridge validates every path itself. Unset disables automatic Unreal Editor discovery.
    std::function<std::vector<discovery::UnrealInstall>()> unrealInstalls;
    // UI locale code for a first run with no saved language (installer choice, else OS language).
    // Unset or empty means English.
    std::function<std::string()> defaultLanguage;
};
// Windows: Program Files/Unity/Hub/Editor via SHGetKnownFolderPath (admin-writable only). Other platforms: empty.
fs::path defaultUnityHubEditorRoot();
// Windows: UE_5.* entries of the Epic launcher's LauncherInstalled.dat plus the folders under
// Program Files/Epic Games. Candidates only; DesktopBridge verifies each one. Other platforms: empty.
std::vector<discovery::UnrealInstall> defaultUnrealInstalls();
// Windows: language from {app}\install-language.txt (written by the installer), else the OS UI language.
std::string defaultInterfaceLanguage();

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
