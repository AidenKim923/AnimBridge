#include "animbridge/desktop_bridge.hpp"
#include "animbridge/app_config.hpp"
#include "animbridge/ui_message.hpp"
#include "animbridge/update_check.hpp"
#include "animbridge/process.hpp"
#include "animbridge/file_commit.hpp"
#include "animbridge/path_identity.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif

namespace animbridge {
namespace {
using Json = nlohmann::json;
std::string now() {
    auto stamp = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm value{};
#ifdef _WIN32
    gmtime_s(&value, &stamp);
#else
    gmtime_r(&stamp, &value);
#endif
    std::ostringstream text;
    text << std::put_time(&value, "%Y-%m-%dT%H:%M:%SZ"); return text.str();
}
std::string identity() { return newIdentity(); }
void writeJson(const fs::path& path, const Json& value) { writeDesktopJson(path, value); }
Json readJson(const fs::path& path) { std::ifstream file(path, std::ios::binary); if (!file) throw UiError("native.reportReadFailed", "Missing local report: " + utf8(path), {{"path", utf8(path)}}); Json j; file >> j; return j; }
std::string valueString(const Json& j, const char* key, const std::string& fallback = "") {
    auto i = j.find(key); return i != j.end() && i->is_string() ? i->get<std::string>() : fallback;
}
bool supportedLanguage(const Json& value) {
    if (!value.is_string()) return false;
    constexpr std::string_view languages[] = {"en", "es", "ja", "ko", "zh-CN", "zh-TW", "pl", "hi", "ru", "ar", "pt-BR", "pt-PT", "fr", "th", "tr"};
    const auto& code = value.get_ref<const std::string&>();
    for (const auto language : languages) if (language == code) return true;
    return false;
}
bool inside(const fs::path& path, const fs::path& root) { return pathWithin(path, root); }
std::string fileHash(const fs::path& path) {
    return fs::is_regular_file(path) ? fileSha256(path) : "";
}
std::uintmax_t removeOwnedCache(const fs::path& path, const fs::path& cache, const std::vector<fs::path>& protectedPaths) {
    if (!inside(path, cache)) return 0;
    for (const auto& active : protectedPaths) if (inside(active, path) || inside(path, active)) return 0;
    std::error_code error; auto status = fs::symlink_status(path, error);
    if (error || fs::is_symlink(status)) return 0;
    std::uintmax_t removed = 0;
    if (fs::is_directory(path)) {
        for (const auto& child : fs::directory_iterator(path)) removed += removeOwnedCache(child.path(), cache, protectedPaths);
        if (fs::is_empty(path) && fs::remove(path)) ++removed;
    } else if (fs::remove(path)) ++removed;
    return removed;
}
Json resultRow(const Result& r) { return Json::parse(resultsJson({r}))["results"][0]; }
std::string attemptKey(const Json& row) {
    auto id = valueString(row, "attemptId");
    return id.empty() ? "legacy:" + row.dump() : "attempt:" + id;
}
Json uniqueAttempts(const Json& rows, std::set<std::string> seen = {}) {
    std::vector<Json> reversed;
    for (auto row = rows.rbegin(); row != rows.rend(); ++row) if (seen.insert(attemptKey(*row)).second) reversed.push_back(*row);
    Json result = Json::array(); for (auto row = reversed.rbegin(); row != reversed.rend(); ++row) result.push_back(*row);
    return result;
}
Json approvedEditorRecord(const PathGrantRegistry& grants, const fs::path& path, const std::string& engine) {
    if (path.empty() || !grants.permits(path, GrantPurpose::Editor)) throw std::runtime_error("Select and approve the " + engine + " Editor with the native picker first");
    const auto kind = engine == "unreal" ? "unreal-build-version" : "unity-version-command";
    const auto version = engine == "unreal" ? "5.8.2" : "6000.4.7f1";
    for (const auto& record : grants.records()) {
        if (record.value("purpose", "") != "editor" || record.value("engine", "") != engine || !sameFile(fromUtf8(record.value("path", "")), path)) continue;
        const auto probe = record.value("engineProbe", Json::object());
        if (probe.value("kind", "") == kind && probe.value("version", "") == version) return record;
    }
    throw std::runtime_error("The selected executable has no current " + engine + " version approval");
}
Json capabilityFile(const fs::path& path, const fs::path& boundary) {
    auto canonical = verifiedCanonicalPath(path);
    if (!inside(canonical, boundary) || !fs::is_regular_file(canonical)) throw std::runtime_error("An engine companion escaped its approved installation");
    auto identity = fileIdentity(canonical), hash = fileSha256(canonical);
    if (fileIdentity(path) != identity || fileIdentity(canonical) != identity) throw std::runtime_error("Engine companion changed while checking its identity");
    return {{"path", utf8(canonical)}, {"sha256", hash}};
}
Json approveUnrealCompanions(PathGrantRegistry& grants, const fs::path& selected, bool deriveFromPicker, GrantSource source = GrantSource::Picker) {
    const auto parentApproval = approvedEditorRecord(grants, selected, "unreal");
    auto engineDirectory = verifiedCanonicalPath(selected).parent_path();
    while (!engineDirectory.empty() && engineDirectory.filename() != "Engine") {
        auto parent = engineDirectory.parent_path(); if (parent == engineDirectory) break; engineDirectory = parent;
    }
    if (engineDirectory.filename() != "Engine" || !inside(selected, engineDirectory / "Binaries")) throw std::runtime_error("Choose an Unreal Editor inside its verified Engine/Binaries installation");
    auto engineRoot = engineDirectory.parent_path();
    auto versionPath = engineDirectory / "Build/Build.version";
    const auto metadata = parentApproval.at("engineProbe").at("metadata");
    if (!sameFile(fromUtf8(metadata.at("path")), versionPath)) throw std::runtime_error("Unreal version approval belongs to a different installation");
#ifdef _WIN32
    auto command = engineDirectory / "Binaries/Win64/UnrealEditor-Cmd.exe";
#elif defined(__APPLE__)
    auto command = engineDirectory / "Binaries/Mac/UnrealEditor-Cmd";
#else
    auto command = engineDirectory / "Binaries/Linux/UnrealEditor-Cmd";
#endif
    auto canonicalCommand = verifiedCanonicalPath(command);
    if (!inside(canonicalCommand, engineDirectory / "Binaries")) throw std::runtime_error("Unreal command companion escaped its selected installation");
    // Only a new native Manny selection (or the Epic-launcher auto-discovery, which passes
    // GrantSource::AutoDiscovered) can issue or renew these grants.
    // Reusing a saved Editor must never silently renew replaced companions.
    auto commandApproval = deriveFromPicker
        ? grants.issue(canonicalCommand, GrantPurpose::Editor, source, "unreal")
        : approvedEditorRecord(grants, canonicalCommand, "unreal");
    if (commandApproval.at("engineProbe").at("metadata") != metadata) throw std::runtime_error("Unreal command companion has a different version identity");
    auto commandCapability = capabilityFile(canonicalCommand, engineDirectory / "Binaries");
    auto templates = engineRoot / "Templates/TemplateResources/High/Characters/Content/Mannequins/Meshes";
    Json templateCapabilities = Json::array();
    for (const auto* name : {"SKM_Manny_Simple.uasset", "SK_Mannequin.uasset"}) {
        auto canonicalTemplate = verifiedCanonicalPath(templates / name);
        if (!inside(canonicalTemplate, engineRoot)) throw std::runtime_error("Unreal template escaped its selected installation");
        // The user's native Editor approval authorizes precisely these two
        // fixed data companions, never a general engine subtree read grant.
        if (deriveFromPicker) grants.issue(canonicalTemplate, GrantPurpose::Read, source, "unreal");
        else if (!grants.permits(canonicalTemplate, GrantPurpose::Read)) throw std::runtime_error("Unreal template approval expired; select the Manny Unreal Editor again or choose a model FBX");
        auto capability = capabilityFile(canonicalTemplate, engineRoot);
        templateCapabilities.push_back(capability);
    }
    return {{"engineRoot", utf8(engineRoot)}, {"editor", commandCapability}, {"versionFile", capabilityFile(versionPath, engineDirectory)},
        {"templates", templateCapabilities}, {"version", "5.8.2"}};
}
// The only Unity version the integration approves (see probeEditor); auto-discovery looks nowhere else.
constexpr const char* BaselineUnityVersion = "6000.4.7f1";
// Returns the canonical Unity executable for the baseline version inside the Hub root, or empty.
// A junction or symlink that resolves outside the resolved root is rejected.
fs::path discoverUnityEditor(const fs::path& hubRoot) {
    if (hubRoot.empty()) return {};
    std::error_code error;
    const auto root = fs::canonical(hubRoot, error);
    if (error || !fs::is_directory(root, error)) return {};
#ifdef _WIN32
    const auto candidate = root / BaselineUnityVersion / "Editor" / "Unity.exe";
#else
    return {};
#endif
    error.clear();
    const auto resolved = fs::canonical(candidate, error);
    if (error || !fs::is_regular_file(resolved, error) || !inside(resolved, root)) return {};
    return resolved;
}
// Canonical UnrealEditor.exe of every launcher-known 5.x install that has both editor
// executables, newest version first. The shell only supplies candidate folders; like the Unity
// code, a junction or symlink that resolves outside its install folder rejects the candidate.
// Whether the engine version is actually supported is decided later by the Editor grant probe.
std::vector<fs::path> discoverUnrealEditors(std::vector<discovery::UnrealInstall> installs) {
    std::vector<fs::path> editors;
#ifdef _WIN32
    discovery::sortNewestFirst(installs);
    std::set<fs::path> seen;
    for (const auto& install : installs) {
        std::error_code error;
        const auto root = fs::canonical(fromUtf8(install.location), error);
        if (error || !fs::is_directory(root, error) || !seen.insert(root).second) continue;
        const auto binaries = root / "Engine" / "Binaries" / "Win64";
        const auto editor = fs::canonical(binaries / "UnrealEditor.exe", error);
        if (error || !fs::is_regular_file(editor, error) || !inside(editor, root)) continue;
        // The conversion runs the -Cmd variant, so an install without it is unusable.
        const auto command = fs::canonical(binaries / "UnrealEditor-Cmd.exe", error);
        if (error || !fs::is_regular_file(command, error) || !inside(command, root)) continue;
        editors.push_back(editor);
    }
#else
    (void)installs;
#endif
    return editors;
}
}

fs::path defaultUnityHubEditorRoot() {
#ifdef _WIN32
    PWSTR known = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, KF_FLAG_DEFAULT, nullptr, &known)) || !known) { if (known) CoTaskMemFree(known); return {}; }
    fs::path result = fs::path(known) / L"Unity" / L"Hub" / L"Editor";
    CoTaskMemFree(known);
    return result;
#else
    return {};
#endif
}

std::vector<discovery::UnrealInstall> defaultUnrealInstalls() {
    std::vector<discovery::UnrealInstall> installs;
#ifdef _WIN32
    auto knownFolder = [](const KNOWNFOLDERID& id) {
        PWSTR known = nullptr;
        fs::path result;
        if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &known)) && known) result = known;
        if (known) CoTaskMemFree(known);
        return result;
    };
    // 1. The Epic launcher's own list also covers engines installed to a custom drive.
    //    The same file is read by src/unityanimbridge/default_humanoid.py.
    if (const auto programData = knownFolder(FOLDERID_ProgramData); !programData.empty()) {
        const auto list = programData / L"Epic" / L"UnrealEngineLauncher" / L"LauncherInstalled.dat";
        std::error_code error;
        // A launcher list is a few KB; refuse anything absurd rather than parse it.
        if (fs::is_regular_file(list, error) && fs::file_size(list, error) <= 4 * 1024 * 1024) {
            std::ifstream file(list, std::ios::binary);
            const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
            installs = discovery::parseLauncherInstalled(text);
        }
    }
    // 2. The default launcher location, for machines where the list is missing or outdated.
    if (const auto programFiles = knownFolder(FOLDERID_ProgramFiles); !programFiles.empty()) {
        std::error_code error;
        for (fs::directory_iterator it(programFiles / L"Epic Games", error), end; !error && it != end; it.increment(error)) {
            const auto version = discovery::unrealEngineVersion(utf8(it->path().filename()));
            if (version && it->is_directory(error)) installs.push_back({version->first, version->second, utf8(it->path())});
        }
    }
#endif
    return installs;
}

std::string defaultInterfaceLanguage() {
#ifdef _WIN32
    std::string installerChoice;
    try {
        // Written next to the executable by the installer ([Code] in packaging/AnimBridge.iss).
        std::ifstream file(runtimePaths().bundleRoot / "install-language.txt", std::ios::binary);
        char buffer[64] = {};
        file.read(buffer, sizeof(buffer) - 1);
        installerChoice.assign(buffer, static_cast<std::size_t>(file.gcount()));
    } catch (...) {}
    return discovery::chooseDefaultLanguage(installerChoice, GetUserDefaultUILanguage());
#else
    return {};
#endif
}

DesktopBridge::DesktopBridge(Sender send, Picker pick, SavePicker save, void*, const fs::path& dataRoot)
    : DesktopBridge(std::move(send), DesktopPlatformServices{
        [pick = std::move(pick)](const Json& payload, bool folder, DesktopPlatformServices::PickCompletion done) { try { done(pick(payload, folder), {}); } catch (const std::exception& e) { done({}, e.what()); } },
        [save = std::move(save)](const Json& payload, DesktopPlatformServices::SaveCompletion done) { try { done(save(payload), {}); } catch (const std::exception& e) { done({}, e.what()); } }, {}, {}, {}}, dataRoot) {}
DesktopBridge::DesktopBridge(Sender send, DesktopPlatformServices services, const fs::path& dataRoot)
    : send_(std::move(send)), services_(std::move(services)),
      context_(resolveRuntimeContext(dataRoot)), grants_(context_.cacheRoot), root_(context_.dataRoot), statePath_(root_ / "state-v2.json"), cache_(context_.cacheRoot) {
    auto sender = std::move(send_);
    send_ = [sender = std::move(sender)](const Json& value) { try { (void)value.dump(); sender(value); } catch (...) { try { sender({{"event", "error"}, {"data", {{"message", "NATIVE_COMPLETION_FAILED: invalid local data or disconnected UI"}}}}); } catch (...) {} } };
    // A first run (no saved settings) follows the installer's language; a saved choice always wins
    // because the settings loaded below overwrite this default.
    std::string initialLanguage = "en";
    if (services_.defaultLanguage) {
        try { if (const auto code = services_.defaultLanguage(); supportedLanguage(code)) initialLanguage = code; } catch (...) {}
    }
    settings_ = {{"language", initialLanguage}, {"theme", "dark"}, {"uiScale", 100}, {"python", utf8(runtimePaths().python)},
        {"blender", utf8(runtimePaths().blender)}, {"skeleton", ""}, {"unityEditor", ""}, {"unreal_editor", ""}, {"unityProject", ""},
        {"outputRoot", utf8(context_.exportRoot)}, {"cacheRoot", utf8(cache_)}};
    packages_ = jobs_ = history_ = Json::array(); scan_ = {{"state", "idle"}, {"current", 0}, {"total", 0}, {"path", ""}};
    unity_ = {{"state", "disconnected"}, {"connected", false}};
    unityAuto_ = {{"state", services_.unityHubEditorRoot ? "searching" : "disabled"}, {"path", ""}};
    unrealAuto_ = {{"state", services_.unrealInstalls ? "searching" : "disabled"}, {"path", ""}};
    skeleton_ = {{"state", "missing"}, {"valid", false}};
    auto defaultSettings = settings_;
    try {
        auto loadPath = statePath_;
        if (!fs::exists(loadPath) && fs::exists(root_ / "state.json")) {
            loadPath = root_ / "state.json";
            // Backup must complete before parsing or migrating v1. The v1 file is
            // never replaced, including after downgrade changes it later.
            fs::copy_file(loadPath, root_ / ("state.v1-backup-" + identity() + ".json"), fs::copy_options::none);
            migrated_ = true;
        }
        if (fs::exists(loadPath)) {
            auto saved = readJson(loadPath);
            if (!saved.is_object() || !saved.contains("schemaVersion") || !saved["schemaVersion"].is_number_integer() || (saved["schemaVersion"] != (migrated_ ? 1 : 2))) {
                persistAllowed_ = false;
                throw UiError("native.stateVersionUnsupported", "Unsupported state file version. Saving is disabled so this file cannot be overwritten.");
            }
            if (migrated_) { legacyState_ = saved; legacyStateHash_ = fileHash(loadPath); }
            else {
                staleGrants_ = grants_.restore(saved.value("pathApprovals", Json::array()));
                legacyState_ = saved.value("legacyState", Json());
                identityRecords_ = saved.value("identityRecords", Json::array());
                legacyStateHash_ = saved.value("legacyStateHash", "");
                if (!legacyStateHash_.empty() && fs::exists(root_ / "state.json") && fileHash(root_ / "state.json") != legacyStateHash_) {
                    auto warning = messageData(std::runtime_error("The older state.json changed after migration. Existing v2 state was retained without merging the older file."));
                    warningMessages_.push_back(warning); session_.warnings.push_back(warning["message"]);
                }
            }
            for (const auto& key : {"packages", "jobs", "history", "selectedIds"}) if (saved.contains(key) && !saved[key].is_array()) throw UiError("native.stateListInvalid", std::string("Invalid state list: ") + key, {{"field", key}});
            if (saved.contains("settings") && !saved["settings"].is_object()) throw UiError("native.settingsObjectInvalid", "Invalid settings object.");
            if (saved.contains("settings")) {
                const auto& storedSettings = saved["settings"];
                for (const auto& key : {"theme", "python", "blender", "skeleton", "unityEditor", "unreal_editor", "unityProject", "outputRoot", "cacheRoot"}) if (storedSettings.contains(key) && !storedSettings[key].is_string()) throw UiError("native.savedSettingInvalid", std::string("Invalid saved setting: ") + key, {{"field", key}});
                if (storedSettings.contains("uiScale") && (!storedSettings["uiScale"].is_number_integer() || storedSettings["uiScale"].get<int>() < 75 || storedSettings["uiScale"].get<int>() > 200)) throw UiError("native.savedScaleInvalid", "Invalid saved UI scale.");
            }
            for (const auto& key : {"packages", "jobs", "history"}) for (const auto& row : saved.value(key, Json::array())) if (!row.is_object()) throw UiError("native.stateItemInvalid", std::string("Invalid state item: ") + key, {{"field", key}});
            for (const auto& package : saved.value("packages", Json::array())) if (!package.contains("path") || !package["path"].is_string()) throw UiError("native.recentSourcePathInvalid", "Invalid recent source path.");
            for (const auto& key : {"jobs", "history"}) for (const auto& job : saved.value(key, Json::array())) {
                if (job.contains("state") && !job["state"].is_string()) throw UiError("native.savedJobStateInvalid", "Invalid saved job state.");
                if (job.contains("results") && !job["results"].is_array()) throw UiError("native.savedJobResultsInvalid", "Invalid saved job results.");
            }
            {
                settings_.update(saved.value("settings", Json::object()));
                if (!supportedLanguage(settings_["language"])) settings_["language"] = "en";
                // Defaults follow a relocated portable bundle; explicitly chosen
                // external executables retain their saved paths.
                auto previousDefaults = saved.value("runtimeDefaults", Json::object());
                if (previousDefaults.is_object()) for (const auto& key : {"python", "blender"}) {
                    if (previousDefaults.contains(key) && previousDefaults[key].is_string() && settings_[key] == previousDefaults[key]) settings_[key] = defaultSettings[key];
                }
                packages_ = saved.value("packages", Json::array());
                for (auto& package : packages_) { package["hashVerified"] = false; package["approvalState"] = grants_.permits(fromUtf8(package.value("path", "")), GrantPurpose::Read) ? "recheck-required" : "stale"; }
                // Selections are restored only after a unique source/hash/identity
                // mapping is available during a subsequent approved scan.
                if (!migrated_) for (const auto& id : saved.value("selectedIds", Json::array())) if (id.is_string()) savedSelection_.insert(id.get<std::string>());
                history_ = saved.value("history", Json::array()); jobs_ = saved.value("jobs", Json::array());
                compactAttemptsLocked();
                // Unfinished attempts cannot silently resume after a process restart.
                for (auto& job : jobs_) if (job.value("state", "") == "running" || job.value("state", "") == "queued") {
                    job["state"] = "interrupted"; job.update(messageData(UiError("native.jobInterrupted", "The previous session ended. Reopen and verify the source before retrying failed clips."), "error"));
                }
            }
        }
    } catch (const std::exception& e) {
        settings_ = defaultSettings; packages_ = jobs_ = history_ = Json::array(); savedSelection_.clear();
        auto backup = root_ / ("state.recovery-" + identity() + ".json");
        try {
            auto original = migrated_ ? root_ / "state.json" : statePath_;
            fs::copy_file(original, backup, fs::copy_options::none);
            auto warning = messageData(UiError("native.stateRecovered", std::string("Could not read saved state. The original was preserved: ") + e.what() + " · " + utf8(backup), {{"detail", e.what()}, {"backup", utf8(backup)}}));
            session_.warnings.push_back(warning["message"].get<std::string>()); warningMessages_.push_back(warning);
        } catch (const std::exception& backupError) {
            persistAllowed_ = false;
            auto warning = messageData(UiError("native.stateBackupFailed", std::string("State backup failed, so saving was disabled. The original file is unchanged: ") + backupError.what(), {{"detail", backupError.what()}}));
            session_.warnings.push_back(warning["message"].get<std::string>()); warningMessages_.push_back(warning);
        }
    }
    for (const auto& warning : processRecoveryWarnings(context_.stateRoot / "attempts")) {
        session_.warnings.push_back(warning); warningMessages_.push_back(messageData(std::runtime_error(warning)));
    }
    // Runtime executables are bundle facts in distribution mode. Previous custom
    // values remain in the migration backup, never become execution authority.
    for (const auto* key : {"python", "blender"}) {
        auto bundled = std::string(key) == "python" ? runtimePaths().python : runtimePaths().blender;
        if (!developmentMode() || (fromUtf8(valueString(settings_, key)) != bundled && !grants_.permits(fromUtf8(valueString(settings_, key)), GrantPurpose::Editor))) settings_[key] = utf8(bundled);
    }
    approveDefaultOutputRoot();
    for (const auto* key : {"outputRoot", "skeleton", "unityEditor", "unreal_editor", "unityProject"}) {
        auto purpose = std::string(key) == "outputRoot" ? GrantPurpose::ExportRoot : std::string(key) == "unityEditor" || std::string(key) == "unreal_editor" ? GrantPurpose::Editor : std::string(key) == "unityProject" ? GrantPurpose::EngineProject : GrantPurpose::Read;
        auto stored = valueString(settings_, key);
        if (!stored.empty() && !grants_.permits(fromUtf8(stored), purpose)) settings_[std::string(key) + "Approval"] = "stale";
        else settings_.erase(std::string(key) + "Approval");
    }
    // Cache ownership is fixed; a saved/custom path never expands deletion authority.
    settings_["cacheRoot"] = utf8(cache_);
    fs::create_directories(cache_); cacheIdentity_ = fileIdentity(cache_);
    auto marker = cache_ / ".animbridge-cache-owner.json";
    if (!fs::exists(marker)) FileCommitter::writeAtomic(marker, Json{{"owner", "AnimBridge"}, {"identity", cacheIdentity_}, {"entries", {"packages", "jobs", "ipc", "helper", "editor-probes"}}}.dump(), false);
    if (migrated_ && persistAllowed_) persist();
    try { for (auto& worker : readWorkers_) worker = std::thread([this] { readLoop(); }); }
    catch (...) {
        { std::lock_guard lock(workMutex_); stopping_ = true; }
        readReady_.notify_all();
        for (auto& worker : readWorkers_) if (worker.joinable()) worker.join();
        throw;
    }
    if (services_.unityHubEditorRoot) {
        try { unityDiscoveryWorker_ = std::thread([this] { autoConnectUnity(); }); }
        catch (...) { unityAuto_["state"] = "none"; }
    }
    if (services_.unrealInstalls) {
        try { unrealDiscoveryWorker_ = std::thread([this] { autoConnectUnreal(); }); }
        catch (...) { unrealAuto_["state"] = "none"; }
    }
}
DesktopBridge::~DesktopBridge() {
    alive_->store(false); cancel();
    if (unityDiscoveryWorker_.joinable()) unityDiscoveryWorker_.join();
    if (unrealDiscoveryWorker_.joinable()) unrealDiscoveryWorker_.join();
    updateCancel_.store(true);  // stop an update download so the join below is prompt
    if (updateWorker_.joinable()) updateWorker_.join();
    { std::lock_guard lock(workMutex_); stopping_ = true; }
    readReady_.notify_all();
    if (worker_.joinable()) worker_.join();
    for (auto& worker : readWorkers_) if (worker.joinable()) worker.join();
}
void DesktopBridge::cancel() { cancel_->store(true); }
DesktopBridge::Json DesktopBridge::stateLocked() const {
    auto session = Json::parse(sessionJson(session_)); Json ids = Json::array();
    session["warningMessages"] = warningMessages_;
    for (const auto& clip : session_.clips) if (clip.selected) ids.push_back(clip.id);
    auto packages = packages_;
    for (auto& package : packages) {
        std::error_code ec; auto status = fs::status(fromUtf8(package.value("path", "")), ec);
        package["exists"] = !ec && fs::exists(status); package["directory"] = !ec && fs::is_directory(status);
        package["sourceState"] = ec == std::errc::permission_denied ? "permissionDenied" : ec && ec != std::errc::no_such_file_or_directory ? "unavailable" : fs::exists(status) ? "present" : "missing";
    }
    return {{"schemaVersion", 2}, {"pathGrants", grants_.publicRecords()}, {"stalePathApprovals", staleGrants_}, {"runtimeReadOnly", !developmentMode()}, {"program", "AnimBridge"}, {"session", session}, {"clips", session["clips"]},
        {"selectedIds", ids}, {"packages", packages}, {"settings", settings_}, {"jobs", jobs_},
        {"history", history_}, {"busy", busy_.load()}, {"readBusy", working() && !busy_.load()},
        {"readActive", readOutstanding_.load()}, {"readConcurrency", ReadConcurrency},
        {"retryAttemptId", lastPlan_ && std::any_of(lastResults_.begin(), lastResults_.end(), [](const Result& r) { return r.status == "error" || r.status == "blocked"; }) ? lastPlan_->attemptId : ""},
        {"statePersistenceEnabled", persistAllowed_}, {"cacheLocationReadOnly", true}, {"scan", scan_}, {"unity", unity_}, {"unityAuto", unityAuto_}, {"unrealAuto", unrealAuto_}, {"skeleton", skeleton_},
        {"defaultHumanoidCapability", {{"state", valueString(settings_, "unreal_editor").empty() ? "needs-unreal-editor" : "recheck-on-use"}, {"engineVersion", "5.8.2"}, {"readScope", "two-fixed-template-files"}}}};
}
DesktopBridge::Json DesktopBridge::state() const { std::lock_guard lock(mutex_); return stateLocked(); }
void DesktopBridge::emitState() {
    // Keep snapshot creation and publication in one order. Without this lock a
    // slower read worker can publish readBusy=true after the final idle event.
    // Callers release mutex_/workMutex_ before entering; send_ only marshals UI data.
    std::lock_guard emitLock(stateEmitMutex_);
    try { send_({{"event", "state"}, {"data", state()}}); }
    catch (...) { try { send_({{"event", "error"}, {"data", {{"message", "STATE_ENCODING_FAILED: a local filename could not be encoded safely"}}}}); } catch (...) {} }
}
void DesktopBridge::compactAttemptsLocked() {
    // An older file may contain a running job plus its completed history snapshot.
    // Preserve terminal evidence when removing the duplicate storage row.
    auto terminal = [](const Json& row) { auto state = valueString(row, "state"); return state == "completed" || state == "completed-with-skips" || state == "failed" || state == "cancelled" || state == "interrupted"; };
    std::map<std::string, Json> snapshots;
    for (const auto& row : history_) snapshots[attemptKey(row)] = row;
    for (const auto& row : jobs_) {
        auto key = attemptKey(row); auto found = snapshots.find(key);
        if (found == snapshots.end() || terminal(row) || !terminal(found->second)) snapshots[key] = row;
    }
    jobs_ = uniqueAttempts(jobs_); std::set<std::string> present;
    for (auto& job : jobs_) { auto key = attemptKey(job); job = snapshots.at(key); present.insert(key); }
    history_ = uniqueAttempts(history_, std::move(present));
}
void DesktopBridge::persist() {
    std::lock_guard diskLock(persistMutex_);
    if (!persistAllowed_) throw UiError("native.stateSaveBlocked", "Saving was stopped because the original state could not be backed up. Check its permissions and locks.");
    if (fs::exists(statePath_)) {
        Json current;
        try { current = readJson(statePath_); }
        catch (...) {
            try { fs::copy_file(statePath_, root_ / ("state.recovery-" + identity() + ".json"), fs::copy_options::none); }
            catch (...) { persistAllowed_ = false; throw std::runtime_error("State changed and could not be backed up. Saving is disabled."); }
        }
        if (current.is_object() && current.contains("schemaVersion") && current["schemaVersion"].is_number_integer() && current["schemaVersion"] != 2) {
            persistAllowed_ = false; throw std::runtime_error("A newer state version appeared. Saving is disabled; the file is unchanged.");
        }
    }
    Json saved; { std::lock_guard lock(mutex_); compactAttemptsLocked(); saved = {{"schemaVersion", 2}, {"settings", settings_}, {"packages", packages_}, {"jobs", jobs_}, {"history", history_}, {"selectedIds", savedSelection_}}; }
    // Opaque session credentials must never survive a restart, including when
    // settings originally arrived with their fieldGrant transport companions.
    for (auto item = saved["settings"].begin(); item != saved["settings"].end();) if (item.key().ends_with("Grant")) item = saved["settings"].erase(item); else ++item;
    saved["identityRecords"] = identityRecords_;
    saved["pathApprovals"] = grants_.records(); saved["legacyState"] = legacyState_; saved["legacyStateHash"] = legacyStateHash_;
    saved["migration"] = {{"sourceIdentityVersion", 2}, {"source", migrated_ ? "state.json" : "state-v2.json"}};
    saved["runtimeDefaults"] = {{"python", utf8(runtimePaths().python)}, {"blender", utf8(runtimePaths().blender)}};
    writeJson(statePath_, saved);
}
// Connects the Hub-installed baseline Unity Editor once per session when none is set.
// Uses the same Editor grant and version probe as the native picker; never replaces a user choice.
void DesktopBridge::autoConnectUnity() {
    auto finish = [this](const char* state, const std::string& path = "") {
        { std::lock_guard lock(mutex_); unityAuto_ = {{"state", state}, {"path", path}}; }
        emitState();
    };
    try {
        std::string stored; bool declined;
        { std::lock_guard lock(mutex_); stored = valueString(settings_, "unityEditor"); declined = settings_.value("unityEditorAutoCleared", false); }
        if (declined) { finish("declined"); return; }
        const auto candidate = discoverUnityEditor(services_.unityHubEditorRoot());
        if (!stored.empty()) {
            const auto storedPath = fromUtf8(stored);
            if (grants_.permits(storedPath, GrantPurpose::Editor)) {
                // Keep showing the note for an editor this feature connected earlier.
                bool automatic = false;
                for (const auto& record : grants_.records()) if (record.value("purpose", "") == "editor" && record.value("source", "") == "auto-discovered" && sameFile(fromUtf8(record.value("path", "")), storedPath)) automatic = true;
                finish(automatic ? "connected" : "none", automatic ? stored : "");
                return;
            }
            // A stale approval is renewed only for the exact auto-discovered location.
            std::error_code same;
            if (candidate.empty() || !fs::exists(storedPath, same) || !sameFile(storedPath, candidate)) { finish("none"); return; }
        }
        if (candidate.empty()) { finish("none"); return; }
        auto approval = grants_.issue(candidate, GrantPurpose::Editor, GrantSource::AutoDiscovered, "unity");
        const auto approvedPath = approval.at("path").get<std::string>();
        bool connected = false;
        {
            std::lock_guard lock(mutex_);
            connected = valueString(settings_, "unityEditor") == stored;  // otherwise the user chose an editor meanwhile
            if (connected) { settings_["unityEditor"] = approvedPath; settings_.erase("unityEditorApproval"); }
            unityAuto_ = {{"state", connected ? "connected" : "none"}, {"path", connected ? approvedPath : ""}};
        }
        if (connected && persistAllowed_) { try { persist(); } catch (...) {} }
        emitState();
    } catch (const std::exception&) {
        // No usable Hub editor (missing, wrong version, probe failure): stay unconnected.
        try { finish("none"); } catch (...) {}
    }
}
// Connects the newest supported Unreal Editor known to the Epic launcher once per session when none
// is set. Goes through the same Editor grant, version probe and companion approval as the native
// picker, but marked auto-discovered. Never replaces a user choice.
void DesktopBridge::autoConnectUnreal() {
    auto finish = [this](const char* state, const std::string& path = "") {
        { std::lock_guard lock(mutex_); unrealAuto_ = {{"state", state}, {"path", path}}; }
        emitState();
    };
    try {
        std::string stored; bool declined;
        { std::lock_guard lock(mutex_); stored = valueString(settings_, "unreal_editor"); declined = settings_.value("unrealEditorAutoCleared", false); }
        if (declined) { finish("declined"); return; }
        auto candidates = discoverUnrealEditors(services_.unrealInstalls());
        if (!stored.empty()) {
            const auto storedPath = fromUtf8(stored);
            if (grants_.permits(storedPath, GrantPurpose::Editor)) {
                // Keep showing the note for an editor this feature connected earlier.
                bool automatic = false;
                for (const auto& record : grants_.records()) if (record.value("purpose", "") == "editor" && record.value("source", "") == "auto-discovered" && sameFile(fromUtf8(record.value("path", "")), storedPath)) automatic = true;
                finish(automatic ? "connected" : "none", automatic ? stored : "");
                return;
            }
            // A stale approval is renewed only for the exact auto-discovered location.
            std::error_code same;
            if (!fs::exists(storedPath, same)) { finish("none"); return; }
            std::erase_if(candidates, [&](const fs::path& candidate) { return !sameFile(storedPath, candidate); });
        }
        // Newest first; an install the integration does not support (the probe rejects it)
        // or one with missing companions is skipped in favor of the next one.
        for (const auto& candidate : candidates) {
            std::string approvedPath;
            try {
                approvedPath = grants_.issue(candidate, GrantPurpose::Editor, GrantSource::AutoDiscovered, "unreal").at("path").get<std::string>();
                approveUnrealCompanions(grants_, fromUtf8(approvedPath), true, GrantSource::AutoDiscovered);
            } catch (const std::exception&) { continue; }
            bool connected = false;
            {
                std::lock_guard lock(mutex_);
                connected = valueString(settings_, "unreal_editor") == stored;  // otherwise the user chose an editor meanwhile
                if (connected) { settings_["unreal_editor"] = approvedPath; settings_.erase("unreal_editorApproval"); }
                unrealAuto_ = {{"state", connected ? "connected" : "none"}, {"path", connected ? approvedPath : ""}};
            }
            if (connected && persistAllowed_) { try { persist(); } catch (...) {} }
            emitState();
            return;
        }
        finish("none");
    } catch (const std::exception&) {
        try { finish("none"); } catch (...) {}
    }
}
void DesktopBridge::reply(const Json& id, const Json& result) { send_({{"id", id}, {"ok", true}, {"result", result}}); }
void DesktopBridge::error(const Json& id, const std::string& code, const std::exception& message) { auto data = messageData(message); data["code"] = code; send_({{"id", id}, {"ok", false}, {"error", data}}); }
void DesktopBridge::emitUpdate(Json data) { send_({{"event", "updateProgress"}, {"data", std::move(data)}}); }
void DesktopBridge::installUpdate(const std::string& version, const std::string& installerUrl, const std::string& checksumsUrl, std::uint64_t installerSize) {
    constexpr std::uint64_t MaxInstallerBytes = 2ull * 1024 * 1024 * 1024;
    constexpr std::uint64_t MaxChecksumsBytes = 1024 * 1024;
    const auto installerName = update::installerAssetName(version);
    const auto folder = cache_ / "updates";
    const auto sumsPath = folder / "SHA256SUMS.txt";
    const auto partialPath = folder / (installerName + ".partial");
    const auto installerPath = folder / installerName;
    auto discard = [&] { std::error_code ignored; fs::remove(partialPath, ignored); fs::remove(installerPath, ignored); };
    try {
        // Re-check here as well: the URLs were validated when parsed, but this is the
        // last point before bytes are fetched.
        if (!update::isAllowedDownloadUrl(installerUrl) || !update::isAllowedDownloadUrl(checksumsUrl)) throw UiError("native.updateUrlRejected", "The update download address is not a trusted GitHub release address.");
        std::error_code ignored;
        fs::remove_all(folder, ignored);  // leftovers from an earlier attempt are never reused
        fs::create_directories(folder);

        // 1. The checksum list first, so a release without one fails before the large download.
        emitUpdate({{"state", "downloading"}, {"received", 0}, {"total", installerSize}});
        const auto keepGoing = [this](std::uint64_t, std::uint64_t) { return !updateCancel_.load(); };
        if (!services_.download(checksumsUrl, sumsPath, MaxChecksumsBytes, keepGoing) || updateCancel_.load()) { discard(); emitUpdate({{"state", "cancelled"}}); return; }
        std::ifstream sumsFile(sumsPath, std::ios::binary);
        const std::string sums{std::istreambuf_iterator<char>(sumsFile), std::istreambuf_iterator<char>()};
        sumsFile.close();
        const auto expected = update::checksumFor(sums, installerName);
        if (!expected) throw UiError("native.updateChecksumMissing", "The release has no checksum for the installer, so it was not downloaded.");

        // 2. The installer, with progress events throttled to whole-percent (or 1 MB) steps.
        std::uint64_t lastReported = 0; int lastPercent = -1;
        const auto progress = [&](std::uint64_t received, std::uint64_t total) {
            if (updateCancel_.load()) return false;
            if (total == 0) total = installerSize;
            const int percent = total ? static_cast<int>(std::min<std::uint64_t>(100, received * 100 / total)) : -1;
            if (percent != lastPercent || (total == 0 && received - lastReported >= 1024 * 1024)) {
                lastPercent = percent; lastReported = received;
                emitUpdate({{"state", "downloading"}, {"received", received}, {"total", total}});
            }
            return true;
        };
        if (!services_.download(installerUrl, partialPath, MaxInstallerBytes, progress) || updateCancel_.load()) { discard(); emitUpdate({{"state", "cancelled"}}); return; }

        // 3. Verify before the file even gets its runnable name.
        emitUpdate({{"state", "verifying"}});
        if (installerSize && fs::file_size(partialPath) != installerSize) throw UiError("native.updateChecksumMismatch", "The downloaded installer did not match the published checksum, so it was deleted.");
        if (fileSha256(partialPath) != *expected) throw UiError("native.updateChecksumMismatch", "The downloaded installer did not match the published checksum, so it was deleted.");
        fs::rename(partialPath, installerPath);

        // 4. Launch, then close so the installer can replace the app files. A job started
        // during the download must finish first; the installer would otherwise close it.
        if (updateCancel_.load()) { discard(); emitUpdate({{"state", "cancelled"}}); return; }
        if (busy()) throw UiError("native.updateBusy", "Finish or cancel the running job, then try updating again.");
        // /RELAUNCH=1 lets the installer start AnimBridge again after a silent update (see AnimBridge.iss).
        services_.launchInstaller(installerPath, {"/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/CLOSEAPPLICATIONS", "/RELAUNCH=1"});
        emitUpdate({{"state", "installing"}, {"version", version}});
        services_.requestExit();
    } catch (const std::exception& e) {
        discard();
        auto data = messageData(e); data["state"] = "failed";
        emitUpdate(data);
    }
}
void DesktopBridge::background(const Json& id, std::function<Json()> task, bool replyNow, Json accepted, bool blocksWorkspace) {
    {
        std::lock_guard lock(workMutex_);
        if (working()) { error(id, busy_ ? "BUSY" : "READ_IN_PROGRESS", busy_ ? UiError("native.busy", "Wait for the current job to finish before trying again.") : UiError("native.readInProgress", "Preview or connection validation is running. Wait for it to finish or cancel it.")); return; }
        working_.store(true); busy_.store(blocksWorkspace); cancel_->store(false);
    }
    if (worker_.joinable()) worker_.join();
    if (replyNow) { accepted["accepted"] = true; reply(id, accepted); } emitState();
    worker_ = std::thread([this, id, task = std::move(task), replyNow]() {
        try { auto result = task(); if (!replyNow) reply(id, result); }
        catch (const std::exception& e) {
            { std::lock_guard lock(mutex_); if (scan_.value("state", "") == "scanning") { scan_["state"] = "error"; scan_.update(messageData(e, "error")); }
              for (auto& job : jobs_) if (job.value("state", "") == "running") {
                  job["state"] = cancel_->load() ? "cancelled" : "failed"; job.update(messageData(e, "error")); job["finishedAt"] = now(); history_.push_back(job);
                  completedEvent_ = job;
              }
              if (skeleton_.value("state", "") == "checking") { skeleton_["state"] = "error"; skeleton_["valid"] = false; skeleton_.update(messageData(e)); }
              if (unity_.value("state", "") == "checking") { unity_["state"] = "error"; unity_["connected"] = false; unity_.update(messageData(e)); }
            }
            error(id, cancel_->load() ? "CANCELLED" : "OPERATION_FAILED", e);
            auto data = messageData(e); data["cancelled"] = cancel_->load(); send_({{"event", "error"}, {"data", data}});
        }
        busy_.store(false);
        try { persist(); } catch (const std::exception& e) { send_({{"event", "error"}, {"data", messageData(e)}}); }
        Json completed; { std::lock_guard lock(mutex_); completed = std::move(completedEvent_); completedEvent_ = Json(); }
        working_.store(false);
        emitState();
        if (!completed.is_null()) send_({{"event", "result"}, {"data", completed}});
    });
}
void DesktopBridge::readLoop() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock lock(workMutex_);
            readReady_.wait(lock, [this] { return stopping_ || !readTasks_.empty(); });
            if (readTasks_.empty()) { if (stopping_) return; continue; }
            task = std::move(readTasks_.front()); readTasks_.pop_front();
        }
        try { task(); } catch (...) { /* Never terminate the UI process on a completion/encoding failure. */ }
    }
}
void DesktopBridge::backgroundRead(const Json& id, const std::string& operation, Json payload) {
    {
        std::lock_guard lock(workMutex_);
        if (working_.load() || readOutstanding_.load() >= ReadConcurrency || stopping_) {
            error(id, busy_ ? "BUSY" : "READ_IN_PROGRESS", UiError("native.analysisBusy", "Wait for analysis, preview, or connection validation to finish or be cancelled before trying again.")); return;
        }
        // Never clear a cancellation flag while another read still owns it.
        if (!readOutstanding_.load()) cancel_->store(false);
        else if (cancel_->load()) { error(id, "CANCELLED", UiError("native.readCancelled", "Preview or connection validation was cancelled.")); return; }
        ++readOutstanding_;
        readTasks_.push_back([this, id, operation, payload = std::move(payload)] {
            Json response;
            try {
                if (cancel_->load()) throw UiError("native.readCancelled", "Preview or connection validation was cancelled.");
                response = {{"id", id}, {"ok", true}, {"result", helper(operation, payload)}};
            } catch (const std::exception& e) {
                auto data = messageData(e); data["code"] = cancel_->load() ? "CANCELLED" : "OPERATION_FAILED";
                response = {{"id", id}, {"ok", false}, {"error", data}};
            }
            // Read-only evaluations neither persist nor mutate connection/validation state.
            // Their IPC/cache directories and child process jobs are unique per request.
            --readOutstanding_;
            emitState();
            send_(response);
        });
    }
    emitState(); readReady_.notify_one();
}
DesktopBridge::Json DesktopBridge::scan(const std::vector<fs::path>& paths) {
    Session next; Json packages;
    { std::lock_guard lock(mutex_); next = session_; packages = packages_; scan_ = {{"state", "scanning"}, {"current", 0}, {"total", paths.size()}, {"path", ""}}; }
    emitState(); std::size_t current = 0;
    for (auto path : paths) {
        if (cancel_->load()) break;
        path = fs::absolute(path).lexically_normal();
        { std::lock_guard lock(mutex_); scan_["path"] = utf8(path); scan_["current"] = current; }
        emitState();
        if (!grants_.permits(path, GrantPurpose::Read)) throw std::runtime_error("Source approval expired. Select the source again.");
        auto sourceHash = fileHash(path); // Reopening always verifies actual bytes before ingesting.
        auto existing = std::find_if(packages.begin(), packages.end(), [&](const Json& p) { std::error_code ec; return fs::equivalent(fromUtf8(p.value("path", "")), path, ec); });
        next.ingest({path}, cache_, {}, context_);
        {
            std::lock_guard lock(mutex_);
            std::map<std::string, unsigned> legacyCounts;
            for (const auto& clip : next.clips) if (!clip.legacyId.empty()) ++legacyCounts[clip.legacyId];
            std::map<std::string, std::vector<Json>> records;
            std::function<void(const Json&)> collect = [&](const Json& value) {
                if (value.is_array()) for (const auto& row : value) collect(row);
                else if (value.is_object()) {
                    if (value.contains("source") && value["source"].is_string() && value.contains("sourceHash") && value["sourceHash"].is_string()) records[value["sourceHash"].get<std::string>()].push_back(value);
                    for (const auto* key : {"jobs", "history", "plan", "clip", "clips"}) if (value.contains(key)) collect(value[key]);
                }
            };
            collect(identityRecords_); collect(legacyState_);
            std::set<std::string> legacySelection;
            if (legacyState_.is_object()) for (const auto& id : legacyState_.value("selectedIds", Json::array())) if (id.is_string()) legacySelection.insert(id.get<std::string>());
            for (auto& clip : next.clips) {
                if (savedSelection_.contains(clip.id)) clip.selected = true;
                std::map<std::string, Json> matches;
                for (const auto& row : records[clip.sourceHash]) {
                    const bool direct = row.value("source", "") == utf8(clip.source);
                    const bool packaged = !clip.packageSourcePath.empty() && row.value("packageSourcePath", "") == utf8(clip.packageSourcePath) && row.value("packageSourceHash", "") == clip.packageSourceHash && row.value("virtualPath", "") == clip.virtualPath;
                    if (!direct && !packaged) continue;
                    auto recordedId = row.value("id", "");
                    if (recordedId != clip.id && recordedId != clip.legacyId && row.value("legacyId", "") != clip.legacyId) continue;
                    auto exportId = row.value("exportId", "");
                    if (!exportId.empty()) matches.emplace(exportId, row);
                }
                if (matches.size() == 1 && legacyCounts[clip.legacyId] == 1) {
                    const auto& record = matches.begin()->second;
                    clip.exportId = record["exportId"].get<std::string>(); clip.exportIdentityVersion = record.value("exportIdentityVersion", 1u);
                    clip.exportIdentityDigest = record.value("exportIdentityDigest", record.value("identityDigest", clip.identityDigest));
                } else if (matches.size() > 1) { clip.selected = false; next.warnings.push_back("AMBIGUOUS_LEGACY_ID: review source selection and engine destination"); }
                // v1 IDs may be restored only against the same verified original
                // package bytes and a unique legacy mapping.
                if (legacySelection.contains(clip.legacyId) && legacyCounts[clip.legacyId] == 1) {
                    bool verified = false;
                    if (legacyState_.is_object()) for (const auto& old : legacyState_.value("packages", Json::array())) if (old.value("path", "") == utf8(path) && old.value("sourceHash", "") == sourceHash && !sourceHash.empty()) verified = true;
                    if (verified) { clip.selected = true; savedSelection_.insert(clip.id); }
                }
            }
            identityRecords_ = Json::parse(sessionJson(next))["clips"];
        }
        Json row = {{"path", utf8(path)}, {"sourcePath", utf8(path)}, {"name", utf8(path.filename())}, {"sourceHash", sourceHash}, {"exists", true}, {"scannedAt", now()}, {"hashVerified", !sourceHash.empty()}};
        if (existing == packages.end()) packages.push_back(row); else *existing = row;
        ++current;
        { std::lock_guard lock(mutex_); session_ = next; packages_ = packages; scan_["current"] = current; }
        emitState();
    }
    { std::lock_guard lock(mutex_); scan_["state"] = cancel_->load() ? "cancelled" : "complete"; }
    return {{"accepted", true}, {"count", next.clips.size()}};
}
DesktopBridge::Json DesktopBridge::removeSource(const fs::path& path, bool releaseCache) {
    // A source that was moved or deleted can still be removed, so fall back
    // to comparing spellings when the file system cannot resolve it.
    auto samePath = [](const fs::path& a, const fs::path& b) {
        std::error_code error;
        if (fs::equivalent(a, b, error)) return true;
        return a.lexically_normal() == b.lexically_normal();
    };
    std::vector<fs::path> releasedRoots, protectedPaths;
    std::size_t removedClips = 0;
    {
        std::lock_guard lock(mutex_);
        auto removed = session_.removeSource(path);
        const auto before = packages_.size();
        Json kept = Json::array();
        for (const auto& row : packages_) if (!samePath(fromUtf8(row.value("path", "")), path)) kept.push_back(row);
        packages_ = kept;
        if (!removed.found && kept.size() == before) throw UiError("native.sourceRemoveMissing", "This source is no longer in the library.");
        for (const auto& clipId : removed.clipIds) savedSelection_.erase(clipId);
        // A reviewed plan may name removed clips; make the user plan again.
        if (!removed.clipIds.empty()) pendingPlan_.reset();
        removedClips = removed.clipIds.size();
        // Only whole package-* extraction folders inside the app cache qualify.
        for (const auto& root : removed.extractedRoots) {
            auto folder = root.parent_path();
            if (inside(folder, cache_ / "packages") && folder.filename().wstring().starts_with(L"package-")) releasedRoots.push_back(folder);
        }
        // Same protection as clearCache: never remove what is still in use.
        for (const auto& clip : session_.clips) if (inside(clip.source, cache_)) protectedPaths.push_back(clip.source);
        for (const auto& root : session_.extractedRoots) if (inside(root, cache_)) protectedPaths.push_back(root);
        if (lastPlan_) for (const auto& job : lastPlan_->jobs) if (inside(job.clip.source, cache_)) protectedPaths.push_back(job.clip.source);
    }
    std::uintmax_t removedFiles = 0;
    if (releaseCache && !releasedRoots.empty() && fileIdentity(cache_) == cacheIdentity_ && !fs::is_symlink(cache_)) {
        for (const auto& folder : releasedRoots) {
            try { removedFiles += removeOwnedCache(folder, cache_, protectedPaths); }
            catch (const std::exception&) { /* A locked cache file is cleaned later by Clear cache. */ }
        }
    }
    return {{"removed", true}, {"clips", removedClips}, {"cacheEntries", removedFiles}};
}
void DesktopBridge::ingest(const std::vector<fs::path>& paths, GrantSource source) {
    if (paths.empty()) return;
    send_({{"event", "nativeDrop"}, {"data", {{"count", paths.size()}}}});
    background("native-drop", [this, paths, source] { std::vector<fs::path> accepted; for (const auto& path : paths) {
        try { auto grant = grants_.issue(path, GrantPurpose::Read, source); accepted.push_back(fromUtf8(grant.at("path"))); }
        catch (const std::exception& e) { auto warning = messageData(e); { std::lock_guard lock(mutex_); warningMessages_.push_back(warning); } }
    } return scan(accepted); }, true);
}
Options DesktopBridge::options(const Json& value) const {
    Json merged; { std::lock_guard lock(mutex_); merged = settings_; } merged.update(value);
    Options result; result.runtimeContext = context_;
    result.workRoot = cache_ / "jobs"; result.journalRoot = context_.stateRoot / "attempts";
    result.output = fromUtf8(valueString(merged, "outputRoot", valueString(merged, "output")));
    result.skeleton = fromUtf8(valueString(merged, "skeleton")); result.python = fromUtf8(valueString(merged, "python", utf8(runtimePaths().python)));
    result.blender = fromUtf8(valueString(merged, "blender"));
    result.mode = valueString(merged, "mode", "extractOriginal") == "extractOriginal" ? ExportMode::ExtractOriginal : ExportMode::Bake;
    auto collision = valueString(merged, "collisionPolicy", "rename");
    result.collisionPolicy = collision == "skip" ? CollisionPolicy::Skip : collision == "overwrite" ? CollisionPolicy::Overwrite : collision == "rename" ? CollisionPolicy::Rename : CollisionPolicy::Fail;
    result.explicitOverwrite = result.collisionPolicy == CollisionPolicy::Overwrite && merged.value("explicitOverwrite", false);
    if (merged.contains("sampleRate") && merged["sampleRate"].is_number() && merged["sampleRate"].get<double>() > 0) result.sampleRate = merged["sampleRate"].get<double>();
    result.rootMotion = valueString(merged, "rootMotion", "preserve");
    result.targetProfile = valueString(merged, "targetProfile");
    result.unityEditor = fromUtf8(valueString(merged, "unityEditor", valueString(merged, "unity_editor")));
    auto humanMapping = merged.value("humanMapping", merged.value("human_mapping", Json::object()));
    auto boneMap = merged.value("boneMap", merged.value("bone_map", Json::object()));
    if (!humanMapping.is_object() || !boneMap.is_object()) throw UiError("native.boneMappingInvalid", "Bone mapping must be an object mapping names to paths.");
    result.humanMappingJson = humanMapping.dump(); result.boneMapJson = boneMap.dump();
    result.direction = valueString(merged, "direction") == "unreal-to-unity" ? Direction::UnrealToUnity : Direction::UnityToUnreal;
    if (result.targetProfile == "neutral") result.targetProfile = "original";
    if (result.targetProfile.empty()) result.targetProfile = result.mode == ExportMode::ExtractOriginal ? "original" : result.direction == Direction::UnityToUnreal ? "unreal" : "unity";
    // Engine project mutation requires explicit options, not a saved connectivity probe.
    result.targetEditor = fromUtf8(valueString(value, "targetEditor")); result.targetProject = fromUtf8(valueString(value, "targetProject"));
    result.targetSkeleton = valueString(value, "targetSkeleton"); result.targetFolder = valueString(value, "targetFolder");
    validateRuntimeOverrides(runtimePaths().backend, result.python, result.blender);
    if (!result.output.empty() && !grants_.permits(result.output, GrantPurpose::ExportRoot)) throw std::runtime_error("Select and approve an output folder first");
    if (!result.skeleton.empty() && !grants_.permits(result.skeleton, GrantPurpose::Read)) throw std::runtime_error("Select and approve the model first");
    if (!result.unityEditor.empty() && !grants_.permits(result.unityEditor, GrantPurpose::Editor)) throw std::runtime_error("Select and approve the Unity Editor first");
    if (!result.targetEditor.empty() && !grants_.permits(result.targetEditor, GrantPurpose::Editor)) throw std::runtime_error("Target Editor approval expired");
    if (!result.targetProject.empty()) grants_.approveTarget(valueString(merged, "targetProjectGrant"), result.targetProfile, result.targetFolder);
    return result;
}
Plan DesktopBridge::plan(const Json& payload, bool preflight) const {
    Session selected; { std::lock_guard lock(mutex_); selected = session_; }
    auto opts = options(payload.value("options", Json::object()));
    for (const auto& clip : selected.clips) if (clip.selected && !grants_.permits(clip.source, GrantPurpose::Read) && !inside(clip.source, cache_)) throw std::runtime_error("Source approval expired. Reopen the source.");
    if (payload.contains("clipIds")) {
        std::set<std::string> ids; for (const auto& id : payload["clipIds"]) ids.insert(id.get<std::string>());
        for (auto& clip : selected.clips) clip.selected = ids.contains(clip.id);
    }
    if (std::none_of(selected.clips.begin(), selected.clips.end(), [](const Clip& clip) { return clip.selected; })) throw UiError("native.clipsRequired", "Select clips to export.");
    if (opts.output.empty()) throw UiError("native.outputRequired", "Select an output folder.");
    bool awaitingConsent = preflight && opts.collisionPolicy == CollisionPolicy::Overwrite && !opts.explicitOverwrite;
    if (awaitingConsent) opts.explicitOverwrite = true; // Prepare the potential plan; start still requires consent.
    for (const auto& clip : selected.clips) if (clip.selected) {
        auto original = clip.packageSourcePath.empty() ? clip.source : clip.packageSourcePath;
        if (!grants_.permits(original, GrantPurpose::Read)) throw std::runtime_error("Source approval expired. Reopen the original source.");
    }
    auto result = makePlan(selected, opts);
    if (awaitingConsent) { result.options.explicitOverwrite = false; result.planSnapshot = planJson(result); }
    return result;
}
void DesktopBridge::verifyPlanGrants(const Plan& plan) const {
    if (!grants_.permits(plan.options.output, GrantPurpose::ExportRoot)) throw std::runtime_error("Output approval expired. Review a new plan.");
    for (const auto& job : plan.jobs) {
        auto original = job.clip.packageSourcePath.empty() ? job.clip.source : job.clip.packageSourcePath;
        if (!grants_.permits(original, GrantPurpose::Read)) throw std::runtime_error("Source approval expired. Review a new plan.");
    }
    if (!plan.options.skeleton.empty() && !grants_.permits(plan.options.skeleton, GrantPurpose::Read)) throw std::runtime_error("Model approval expired");
    if (!plan.options.targetEditor.empty() && !grants_.permits(plan.options.targetEditor, GrantPurpose::Editor)) throw std::runtime_error("Editor approval expired");
    if (!plan.options.targetProject.empty() && !grants_.permits(plan.options.targetProject, GrantPurpose::EngineProject)) throw std::runtime_error("Project approval expired");
}
DesktopBridge::Json DesktopBridge::run(Plan active, Json optionSnapshot) {
    verifyPlanGrants(active);
    auto pj = Json::parse(planJson(active)); std::string id = active.jobId, attempt = active.attemptId;
    Json job = {{"jobId", id}, {"attemptId", attempt}, {"parentAttemptId", active.parentAttemptId}, {"state", "running"}, {"createdAt", now()},
        {"plan", pj}, {"options", pj.value("options", optionSnapshot)}, {"results", Json::array()}, {"outputRoot", utf8(active.options.output)}};
    { std::lock_guard lock(mutex_); jobs_.push_back(job); lastPlan_ = std::make_unique<Plan>(active); lastOptionSnapshot_ = optionSnapshot; lastResults_.clear(); }
    persist(); emitState();
    auto results = execute(active, [this, attempt](const Result& result) {
        auto row = resultRow(result); Json updatedJob;
        { std::lock_guard lock(mutex_); for (auto& item : jobs_) if (item.value("attemptId", "") == attempt) {
            auto& rows = item["results"]; auto found = std::find_if(rows.begin(), rows.end(), [&](const Json& r) { return r.value("id", "") == result.id; });
            if (found == rows.end()) rows.push_back(row); else *found = row;
            item["current"] = row;
            updatedJob = item;
        }
          auto previous = std::find_if(lastResults_.begin(), lastResults_.end(), [&](const Result& r) { return r.id == result.id; });
          if (previous == lastResults_.end()) lastResults_.push_back(result); else *previous = result;
        }
        send_({{"event", "progress"}, {"data", {{"job", updatedJob}, {"result", row}}}});
    }, cancel_);
    Json rows = Json::parse(resultsJson(results))["results"];
    bool failure = false, cancelled = false, allSuccess = true;
    for (const auto& row : rows) { auto status = row.value("status", ""); failure |= status == "error" || status == "blocked" || status == "failed"; cancelled |= status == "cancelled"; allSuccess &= status == "ok" || status == "imported" || status == "warning" || status == "ready-for-unity-import"; }
    { std::lock_guard lock(mutex_); lastResults_ = results; for (auto& item : jobs_) if (item.value("attemptId", "") == attempt) {
        item["results"] = rows; item["finishedAt"] = now(); item["state"] = cancelled ? "cancelled" : failure ? "failed" : allSuccess ? "completed" : "completed-with-skips";
        history_.push_back(item); job = item; completedEvent_ = item;
    } }
    persist(); return job;
}
DesktopBridge::Json DesktopBridge::helper(const std::string& operation, Json payload) {
    auto requestId = identity(); fs::path ipc = cache_ / "ipc" / requestId;
    fs::create_directories(ipc); Json settings;
    { std::lock_guard lock(mutex_); settings = settings_; }
    if (payload.contains("clipId")) {
        std::lock_guard lock(mutex_); auto id = payload["clipId"].get<std::string>();
        auto found = std::find_if(session_.clips.begin(), session_.clips.end(), [&](const Clip& clip) { return clip.id == id; });
        if (found == session_.clips.end()) throw UiError("native.clipNotFound", "Clip not found. Reopen the package.");
        auto original = found->packageSourcePath.empty() ? found->source : found->packageSourcePath;
        if (!grants_.permits(original, GrantPurpose::Read)) throw std::runtime_error("Original source approval expired");
        payload["source"] = utf8(found->source); if (found->namedClip) payload["clip"] = found->name;
    }
    if (payload.contains("options")) { auto opts = payload["options"]; payload.update(opts); payload.erase("options"); }
    if (!payload.contains("skeleton")) payload["skeleton"] = valueString(settings, "skeleton");
    if (!payload.contains("blender")) payload["blender"] = valueString(settings, "blender");
    if (!payload.contains("unreal_editor")) payload["unreal_editor"] = valueString(settings, "unreal_editor");
    payload["cache"] = utf8(cache_ / "helper");
    if (payload.contains("unityEditor")) payload["unity_editor"] = payload["unityEditor"];
    if (payload.contains("unityProject")) payload["unity_project"] = payload["unityProject"];
    if (payload.contains("projectPath")) payload["unity_project"] = payload["projectPath"];
    if (payload.contains("editorPath")) payload["unity_editor"] = payload["editorPath"];
    else if (!payload.contains("unity_editor")) payload["unity_editor"] = valueString(settings, "unityEditor");
    if (operation == "validate" && payload.contains("path")) payload["model"] = payload["path"];
    if (operation == "validate") {
        auto mapping = payload.value("mapping", Json::object());
        if (mapping.is_null()) mapping = Json::object();
        if (!mapping.is_object()) throw UiError("native.validationMappingInvalid", "Validation bone mapping must be an object mapping names to paths.");
        payload["mapping"] = mapping;
    }
    auto input = ipc / "request.json", output = ipc / "result.json", log = ipc / "process.log";
    auto python = fromUtf8(valueString(settings, "python", utf8(runtimePaths().python)));
    validateRuntimeOverrides(runtimePaths().backend, python, fromUtf8(valueString(settings, "blender")));
    for (const auto* key : {"source", "skeleton", "model", "path"}) if (payload.contains(key) && payload[key].is_string() && !payload[key].get_ref<const std::string&>().empty()) {
        auto path = fromUtf8(payload[key]); if (!grants_.permits(path, GrantPurpose::Read) && !inside(path, cache_)) throw std::runtime_error("Read approval expired before evaluator launch");
    }
    auto unityEditor = valueString(payload, "unity_editor");
    if (operation == "helper-test" && unityEditor.empty()) throw UiError("native.editorApprovalRequired", "Select and approve a Unity Editor before connecting it.");
    if (!unityEditor.empty()) (void)approvedEditorRecord(grants_, fromUtf8(unityEditor), "unity");
    if (!valueString(payload, "targetEditor").empty() && !grants_.permits(fromUtf8(valueString(payload, "targetEditor")), GrantPurpose::Editor)) throw std::runtime_error("Editor approval expired");
    payload.erase("_native_unreal_capability"); payload.erase("_native_manny_capability");
    payload["_native_editor_policy"] = "explicit-only";
    if (!valueString(payload, "unreal_editor").empty()) {
        auto capability = approveUnrealCompanions(grants_, fromUtf8(valueString(payload, "unreal_editor")), false);
        payload["unreal_editor"] = capability.at("editor").at("path");
        payload["_native_unreal_capability"] = capability;
    }
    writeJson(input, payload);
    ProcessRunOptions processOptions; processOptions.stageLeases = true; processOptions.jobTimeoutSeconds = 3600; processOptions.journalRoot = context_.stateRoot / "attempts";
    processOptions.privateExecutables = {python, runtimePaths().blender};
    if (!unityEditor.empty()) processOptions.privateExecutables.push_back(fromUtf8(unityEditor));
    if (!valueString(payload, "unreal_editor").empty()) processOptions.privateExecutables.push_back(fromUtf8(valueString(payload, "unreal_editor")));
    auto result = runSupervisedProcess(python, pythonArguments({"-m", "unityanimbridge.desktop", operation, "--request", utf8(input), "--out", utf8(output)}),
        runtimePaths().backend, log, 600, pythonEnvironment(runtimePaths().backend, cache_ / "jobs", context_), cancel_, processOptions);
    if (result.cancelled) throw UiError("native.readCancelled", "Preview or connection validation was cancelled.");
    if (result.timedOut) throw std::runtime_error("Evaluator timeout at " + result.timeoutLayer + " / " + result.stage + "; preserved log: " + utf8(log));
    if (!result.ownedCleanupComplete) throw std::runtime_error("Evaluator private process cleanup remains unverified; retained result, cache and log: " + utf8(log));
    if (result.exitCode) throw std::runtime_error("Evaluator failed with exit " + std::to_string(result.exitCode) + "; retained result, cache and log: " + utf8(log));
    if (!fs::exists(output)) throw UiError("native.helperNoResult", "The local evaluator did not produce a result. Check Python/Blender and the log: " + utf8(log) + " (exit " + std::to_string(result.exitCode) + ")", {{"log", utf8(log)}, {"exitCode", result.exitCode}});
    auto response = readJson(output);
    response["cleanupComplete"] = result.cleanupComplete; response["cleanupWarnings"] = result.cleanupWarnings;
    if (!result.timeoutLayer.empty()) response["timeoutLayer"] = result.timeoutLayer;
    if (!result.stage.empty()) response["stage"] = result.stage;
    if (operation == "helper-test") { std::lock_guard lock(mutex_); unity_ = response; unity_["state"] = response.value("status", "error");
        unity_["connected"] = response.value("status", "error") == "connected" && response.value("connected", false);
        if (unity_["connected"].get<bool>()) {
            settings_["unityEditor"] = valueString(payload, "unity_editor");
            settings_["unityProject"] = valueString(response, "project");
        }
    }
    if (operation == "validate") {
        response["validationMapping"] = payload["mapping"];
        std::lock_guard lock(mutex_); skeleton_ = response; skeleton_["state"] = response.value("status", "invalid"); skeleton_["valid"] = response.value("status", "invalid") == "valid" && response.value("valid", false); skeleton_["path"] = valueString(payload, "model", valueString(payload, "skeleton")); skeleton_["clipId"] = valueString(payload, "clipId");
    }
    return response;
}
// Checks everything the UI can repair before Unreal starts: the clip, its
// project approval and the configured Editor. Throws UiError on refusal.
DesktopBridge::Json DesktopBridge::prepareUnrealConversion(const Json& payload) const {
    Clip clip;
    std::string editor;
    {
        std::lock_guard lock(mutex_);
        auto id = valueString(payload, "clipId");
        auto found = std::find_if(session_.clips.begin(), session_.clips.end(), [&](const Clip& candidate) { return candidate.id == id; });
        if (found == session_.clips.end()) throw UiError("native.clipNotFound", "Clip not found. Reopen the package.");
        clip = *found;
        editor = valueString(settings_, "unreal_editor");
    }
    if (clip.kind != "uasset") throw UiError("native.unrealAssetRequired", "Only Unreal .uasset files can be converted to FBX.");
    if (!grants_.permits(clip.source, GrantPurpose::Read)) throw std::runtime_error("Source approval expired. Reopen the source.");
    // Resolve again instead of trusting the scan: folders may have moved since.
    auto location = locateUnrealAsset(clip.source);
    if (!location) throw UiError("native.unrealProjectMissing", "This .uasset is not inside an Unreal project (no .uproject found).");
    const auto projectText = utf8(location->project);
    // authorizePayload already turned unrealProjectGrant into a verified path.
    auto chosen = valueString(payload, "unrealProject");
    if (!chosen.empty() && !sameFile(fromUtf8(chosen), location->project))
        throw UiError("native.unrealProjectMismatch", "The selected project does not own this asset. Choose " + projectText + ".", {{"project", projectText}});
    // Unreal reads the whole project, so a file-level read grant is not enough.
    if (!grants_.permits(location->project, GrantPurpose::EngineProject))
        throw UiError("native.unrealProjectApprovalRequired", "Approve the Unreal project that owns this asset: " + projectText, {{"project", projectText}});
    if (editor.empty()) throw UiError("native.unrealEditorRequired", "Choose Unreal Editor in Settings before converting .uasset files.");
    // Empty skeleton/Unity values keep helper() from attaching preview-only settings.
    return {{"clipId", clip.id}, {"skeleton", ""}, {"unity_editor", ""}, {"unreal_project", utf8(fs::canonical(location->project))}, {"unreal_object_path", location->objectPath}};
}
DesktopBridge::Json DesktopBridge::convertUnrealAsset(const Json& prepared) {
    // helper() re-checks the source grant and the Editor capability, then runs
    // the worker under the process supervisor exactly like a Manny preview.
    auto response = helper("unreal-export", prepared);
    if (response.value("status", "") != "ok") {
        const auto reason = valueString(response, "reason", "unknown error");
        throw UiError("native.unrealConvertFailed", "Unreal Editor could not convert this asset: " + reason,
            {{"reason", reason}, {"log", valueString(response, "log")}});
    }
    auto manifest = fromUtf8(valueString(response, "manifest"));
    // Only a manifest inside the worker's own cache may become a library source.
    if (manifest.filename() != "animbridge-manifest.json" || !inside(manifest, cache_ / "helper" / "unreal-export"))
        throw std::runtime_error("The Unreal conversion result is outside the app-owned cache");
    // The grant also covers the manifest's FBX companions, so the new clips can
    // be previewed, revealed and exported like any other approved FBX.
    auto grant = grants_.issue(manifest, GrantPurpose::Read, GrantSource::AppOutput);
    auto approved = fromUtf8(grant.at("path").get<std::string>());
    scan({approved});
    Json clipIds = Json::array();
    {
        std::lock_guard lock(mutex_);
        for (const auto& clip : session_.clips) if (inside(clip.source, approved.parent_path())) clipIds.push_back(clip.id);
    }
    return {{"status", "converted"}, {"manifest", utf8(approved)}, {"outputs", response.value("outputs", Json::array())},
        {"clipIds", clipIds}, {"objectPath", prepared.at("unreal_object_path")}, {"log", valueString(response, "log")}};
}
// The default output folder (Documents/AnimBridge) is chosen by the app, not in
// a native picker, so no picker ever approves it. Create and approve it here so
// a first export works without choosing a folder. A folder the user picked
// keeps its own approval and is never touched.
void DesktopBridge::approveDefaultOutputRoot() {
    const auto stored = valueString(settings_, "outputRoot");
    if (stored.empty() || fromUtf8(stored).lexically_normal() != context_.exportRoot.lexically_normal()) return;
    try {
        if (grants_.permits(context_.exportRoot, GrantPurpose::ExportRoot)) return;
        fs::create_directories(context_.exportRoot);
        auto grant = grants_.issue(context_.exportRoot, GrantPurpose::ExportRoot, GrantSource::AppOutput);
        // The UI matches approvals by exact path text, so use the approved spelling.
        settings_["outputRoot"] = grant.at("path");
    } catch (const std::exception&) {
        // Not writable (e.g. a locked Documents folder): the caller marks the
        // setting stale and the user picks another folder.
    }
}
bool DesktopBridge::allowedPath(const fs::path& path) const {
    if (path.empty() || !fs::exists(path)) return false;
    if (inside(path, root_) || inside(path, cache_)) return true;
    return grants_.permits(path, GrantPurpose::Read) || grants_.permits(path, GrantPurpose::ExportRoot) || grants_.permits(path, GrantPurpose::Editor) || grants_.permits(path, GrantPurpose::EngineProject);
}
DesktopBridge::Json DesktopBridge::authorizePayload(const Json& value, const std::string& action) const {
    if (!value.is_object()) throw std::runtime_error("Request payload must be an object");
    auto result = value;
    // Apply the same rule to options/settings before flattening helper payloads.
    for (const auto* nested : {"options", "settings"}) if (result.contains(nested)) result[nested] = authorizePayload(result[nested], action);
    const std::map<std::string, GrantPurpose> fields = {{"source", GrantPurpose::Read}, {"path", GrantPurpose::Read}, {"model", GrantPurpose::Read}, {"skeleton", GrantPurpose::Read},
        {"output", GrantPurpose::ExportRoot}, {"outputRoot", GrantPurpose::ExportRoot}, {"unityEditor", GrantPurpose::Editor}, {"unity_editor", GrantPurpose::Editor}, {"unreal_editor", GrantPurpose::Editor}, {"editorPath", GrantPurpose::Editor}, {"targetEditor", GrantPurpose::Editor},
        {"unityProject", GrantPurpose::EngineProject}, {"unity_project", GrantPurpose::EngineProject}, {"projectPath", GrantPurpose::EngineProject}, {"targetProject", GrantPurpose::EngineProject},
        {"unrealProject", GrantPurpose::EngineProject}};
    for (const auto& [field, purpose] : fields) {
        if (!result.contains(field) && !result.contains(field + "Grant")) continue;
        if (result.contains(field) && !result[field].is_string()) throw std::runtime_error("Path fields must be strings");
        if (valueString(result, field.c_str()).empty() && !result.contains(field + "Grant")) continue;
        auto token = valueString(result, (field + "Grant").c_str());
        if (action == "openPath" && field == "path") {
            if (!allowedPath(fromUtf8(valueString(result, "path")))) throw std::runtime_error("Path has no active native approval");
            // App-owned reports and previously observed outputs can be revealed;
            // this action never creates read, write, or executable authority.
            continue;
        }
        auto resolved = grants_.resolve(token, purpose);
        if (field == "unreal_editor") (void)approvedEditorRecord(grants_, resolved, "unreal");
        result[field] = utf8(resolved);
    }
    for (const auto* field : {"python", "blender", "repo", "backend"}) if (result.contains(field)) {
        if (!result[field].is_string()) throw std::runtime_error("Runtime fields must be strings");
        auto expected = std::string(field) == "python" ? runtimePaths().python : std::string(field) == "blender" ? runtimePaths().blender : runtimePaths().backend;
        if (!valueString(result, field).empty() && fromUtf8(valueString(result, field)) != expected) {
            if (!developmentMode() || std::string(field) == "repo" || std::string(field) == "backend") throw std::runtime_error("Runtime executable overrides are disabled in distribution mode");
            result[field] = utf8(grants_.resolve(valueString(result, (std::string(field) + "Grant").c_str()), GrantPurpose::Editor));
        }
    }
    // unreal_project/unreal_object_path are derived natively from an approved clip.
    for (const auto* forbidden : {"_preview_project", "_native_editor_policy", "_native_unreal_capability", "_native_manny_capability", "cache", "cache_root", "workRoot", "log", "request", "response", "unreal_project", "unreal_object_path"}) if (result.contains(forbidden)) throw std::runtime_error("App-owned worker paths cannot be supplied by UI requests");
    return result;
}
void DesktopBridge::handle(const Json& request, const std::vector<fs::path>& dropped) {
    Json id = request.is_object() ? request.value("id", Json()) : Json();
    try {
        if (!request.is_object() || !request.contains("action") || !request["action"].is_string()) throw UiError("native.requestInvalid", "Invalid app request.");
        auto action = request["action"].get<std::string>(); auto payload = request.value("payload", Json::object());
        if (!payload.is_object() || request.dump().size() > 1024 * 1024) throw std::runtime_error("Invalid or oversized request payload");
        if (action == "chooseFiles" || action == "chooseFolder" || action == "saveReport") for (const auto* field : {"title", "filterLabel", "allFilesLabel"}) if (payload.contains(field)) {
            if (!payload[field].is_string()) throw UiError("native.dialogLabelInvalid", "Dialog labels must be plain text of at most 200 characters.");
            const auto& text = payload[field].get_ref<const std::string&>(); requireUtf8(text);
            unsigned characters = 0; for (unsigned char c : text) { if (c < 32 || c == 127) throw UiError("native.dialogLabelInvalid", "Dialog labels must be plain text of at most 200 characters."); if ((c & 0xc0) != 0x80) ++characters; }
            if (characters > 200) throw UiError("native.dialogLabelInvalid", "Dialog labels must be plain text of at most 200 characters.");
        }
        if (action != "chooseFiles" && action != "chooseFolder") payload = authorizePayload(payload, action);
        if (action == "getState") reply(id, state());
        else if (action == "getAppInfo") {
            Json provenance = {{"runtimeMode", developmentMode() ? "development" : "distribution"}, {"sourceFallback", developmentMode()}, {"runtimeManifestSha256", runtimePaths().manifest.empty() ? "unavailable" : fileHash(runtimePaths().manifest)},
#ifdef ANIMBRIDGE_SOURCE_SNAPSHOT_SHA
                {"sourceSnapshot", ANIMBRIDGE_SOURCE_SNAPSHOT_SHA},
#else
                {"sourceSnapshot", "unavailable"},
#endif
#if defined(ANIMBRIDGE_ENABLE_UI_TEST_HOOKS) && ANIMBRIDGE_ENABLE_UI_TEST_HOOKS
                {"testHooks", true}, {"artifactKind", "G2-harness"}
#else
                {"testHooks", false}, {"artifactKind", "release"}
#endif
            };
            reply(id, {{"version", app::Version}, {"provenance", provenance}, {"links", {
                {"github", !app::GithubUrl.empty()}, {"fairy", !app::FairyUrl.empty()},
                {"githubSponsors", !app::GithubSponsorsUrl.empty()}
            }}});
        }
        else if (action == "checkForUpdate") {
            const std::string current(app::Version);
            const auto apiUrl = services_.httpGet ? update::releasesApiUrl(app::GithubUrl) : std::string();
            if (apiUrl.empty()) { reply(id, {{"supported", false}, {"available", false}, {"current", current}}); return; }
            if (updateRunning_.exchange(true)) { reply(id, {{"supported", true}, {"available", false}, {"current", current}, {"pending", true}}); return; }
            if (updateWorker_.joinable()) updateWorker_.join();
            try {
                updateWorker_ = std::thread([this, id, apiUrl, current] {
                    try {
                        const auto release = update::newestRelease(Json::parse(services_.httpGet(apiUrl)));
                        Json result = {{"supported", true}, {"available", false}, {"current", current}};
                        if (release) {
                            const bool newer = update::isNewer(release->version, current);
                            result["latest"] = release->version; result["name"] = release->name; result["prerelease"] = release->prerelease;
                            result["available"] = newer;
                            // In-app install needs the exact installer + checksum assets and a shell
                            // that can download and launch; otherwise the UI offers the release page.
                            const bool installable = newer && release->assets && services_.download && services_.launchInstaller && services_.requestExit;
                            result["installable"] = installable;
                            if (newer) {
                                std::lock_guard lock(mutex_);
                                latestReleaseTag_ = release->tag;
                                pendingUpdateVersion_ = installable ? release->version : std::string();
                                pendingInstallerUrl_ = installable ? release->assets->installerUrl : std::string();
                                pendingChecksumsUrl_ = installable ? release->assets->checksumsUrl : std::string();
                                pendingInstallerSize_ = installable ? release->assets->installerSize : 0;
                            }
                        }
                        updateRunning_.store(false);
                        reply(id, result);
                    } catch (const std::exception& e) {
                        updateRunning_.store(false);
                        error(id, "UPDATE_CHECK_FAILED", e);
                    }
                });
            } catch (...) { updateRunning_.store(false); throw; }
        }
        else if (action == "installUpdate") {
            // The UI only names the action; the version and URLs come from the last check.
            std::string version, installerUrl, checksumsUrl; std::uint64_t installerSize = 0;
            {
                std::lock_guard lock(mutex_);
                version = pendingUpdateVersion_; installerUrl = pendingInstallerUrl_;
                checksumsUrl = pendingChecksumsUrl_; installerSize = pendingInstallerSize_;
            }
            if (version.empty() || !update::isNewer(version, app::Version)) throw UiError("native.updateNotReady", "No installable update was found. Check for updates again.");
            if (busy()) throw UiError("native.updateBusy", "Finish or cancel the running job, then try updating again.");
            if (updateRunning_.exchange(true)) throw UiError("native.updateRunning", "An update is already in progress.");
            if (updateWorker_.joinable()) updateWorker_.join();
            updateCancel_.store(false);
            try {
                updateWorker_ = std::thread([this, version, installerUrl, checksumsUrl, installerSize] {
                    installUpdate(version, installerUrl, checksumsUrl, installerSize);
                    updateRunning_.store(false);
                });
            } catch (...) { updateRunning_.store(false); throw; }
            // Progress, failure and completion arrive as "updateProgress" events.
            reply(id, {{"started", true}, {"version", version}});
        }
        else if (action == "cancelUpdate") {
            updateCancel_.store(true);
            reply(id, {{"cancelling", updateRunning_.load()}});
        }
        else if (action == "openExternalLink") {
            const auto link = payload.value("link", "");
            if (link != "github" && link != "fairy" && link != "githubSponsors" && link != "latestRelease") {
                error(id, "UNKNOWN_LINK", UiError("native.linkUnsupported", "Unsupported external link."));
                return;
            }
            std::string releaseUrl;
            if (link == "latestRelease") {
                std::string tag; { std::lock_guard lock(mutex_); tag = latestReleaseTag_; }
                // The tag was validated by update::versionParts, so it is safe in a URL path.
                if (!tag.empty() && !app::GithubUrl.empty()) releaseUrl = std::string(app::GithubUrl) + "/releases/tag/" + tag;
            }
            const std::string configuredUrl = link == "latestRelease" ? releaseUrl : std::string(app::externalLinkUrl(link));
            if (configuredUrl.empty()) {
                error(id, "LINK_NOT_CONFIGURED", link == "github" || link == "latestRelease" ? UiError("native.githubPending", "The GitHub link is not configured yet.") : UiError("native.supportPending", "The support link is not configured yet."));
                return;
            }
            // The UI sends an identifier, never a navigable URL or shell command.
            if (!services_.openExternal) throw std::runtime_error("External browser service is unavailable");
            services_.openExternal(std::string(configuredUrl));
            reply(id, {{"opened", true}});
        }
        else if (action == "chooseFiles" || action == "chooseFolder") {
            if (!services_.pick) throw std::runtime_error("Native picker is unavailable");
            if (working()) throw UiError("native.analysisBusy", "Wait for the active operation before selecting a path.");
            const auto field = valueString(payload, "purpose");
            auto purpose = field.find("output") != std::string::npos ? GrantPurpose::ExportRoot : field.find("editor") != std::string::npos || field.find("python") != std::string::npos || field.find("blender") != std::string::npos ? GrantPurpose::Editor : field.find("project") != std::string::npos ? GrantPurpose::EngineProject : GrantPurpose::Read;
            if (!developmentMode() && (field.find("python") != std::string::npos || field.find("blender") != std::string::npos)) throw std::runtime_error("Bundled runtimes are read-only");
            if (pickerOutstanding_.exchange(true)) throw std::runtime_error("A native selection dialog is already open");
            auto lifetime = alive_;
            auto pickerPayload = payload;
            // A conversion source project is only read, so its title names no import target.
            if (purpose == GrantPurpose::EngineProject && field != "unreal-source-project") {
                auto target = valueString(payload, "targetFolder", valueString(payload, "engine", "unity") == "unity" ? "Assets/AnimBridge" : "/Game/AnimBridge");
                pickerPayload["title"] = valueString(payload, "title", "Select project") + " · " + target;
            }
            try { services_.pick(pickerPayload, action == "chooseFolder", [this, lifetime, id, purpose, payload](std::vector<fs::path> paths, std::string failure) {
                if (!lifetime->load()) return;
                pickerOutstanding_.store(false);
                if (!failure.empty()) { error(id, "PICK_FAILED", std::runtime_error(failure)); return; }
                if (paths.empty()) { reply(id, {{"paths", Json::array()}, {"grants", Json::array()}}); return; }
                background(id, [this, paths, purpose, payload] {
                    Json displayed = Json::array(), approvals = Json::array();
                    for (const auto& path : paths) {
                        auto field = valueString(payload, "purpose");
                        auto engine = field.find("python") != std::string::npos || field.find("blender") != std::string::npos ? "runtime" : valueString(payload, "engine", "unity");
                        auto approval = grants_.issue(path, purpose, GrantSource::Picker, engine, valueString(payload, "targetFolder", valueString(payload, "engine", "unity") == "unity" ? "Assets/AnimBridge" : "/Game/AnimBridge"));
                        if (purpose == GrantPurpose::Editor && engine == "unreal" && field == "unreal-editor") {
                            approval["defaultHumanoidCapability"] = approveUnrealCompanions(grants_, fromUtf8(approval.at("path")), true);
                            // Selecting Cmd itself can renew its token during
                            // fixed-companion approval; return the current one.
                            for (const auto& current : grants_.publicRecords()) if (current["path"] == approval["path"] && current["purpose"] == "editor") { approval["grant"] = current["grant"]; break; }
                        }
                        displayed.push_back(approval["path"]); approvals.push_back(approval);
                    }
                    return Json{{"paths", displayed}, {"grants", approvals}};
                }, false, Json::object(), false);
            }); } catch (...) { pickerOutstanding_.store(false); throw; }
        }
        else if (action == "scan" || action == "drop") {
            auto paths = dropped;
            if (!paths.empty()) { for (auto& path : paths) { auto grant = grants_.issue(path, GrantPurpose::Read, GrantSource::NativeDrop); path = fromUtf8(grant["path"]); } }
            else if (payload.contains("grants") && payload["grants"].is_array()) for (const auto& token : payload["grants"]) paths.push_back(grants_.resolve(token.get<std::string>(), GrantPurpose::Read));
            if (paths.empty()) { error(id, action == "drop" ? "DROP_PATH_UNAVAILABLE" : "INPUT_EMPTY", UiError("native.inputPathUnavailable", "Select files with the native picker or drop actual files.")); return; }
            background(id, [this, paths] { return scan(paths); }, true);
        }
        else if (action == "select") {
            if (busy_) { error(id, "BUSY", UiError("native.selectionBusy", "Wait for analysis or the current job to finish before changing the selection.")); return; }
            std::set<std::string> ids; for (const auto& item : payload.value("ids", Json::array())) ids.insert(item.get<std::string>());
            { std::lock_guard lock(mutex_); savedSelection_ = ids; for (auto& clip : session_.clips) clip.selected = ids.contains(clip.id); }
            persist();
            reply(id, {{"selected", ids.size()}}); emitState();
        }
        else if (action == "removeSource") {
            // Forget one SOURCE LIBRARY entry: its clips, its Recent row and the
            // app-owned extraction made for it. The user's original files stay.
            // "sourcePath" is not a grant-checked field: removing only forgets
            // state, so a source whose approval expired can still be removed.
            auto path = fromUtf8(valueString(payload, "sourcePath"));
            if (path.empty()) throw UiError("native.sourceRemoveMissing", "Choose a source to remove from the library.");
            if (busy_) { error(id, "BUSY", UiError("native.busy", "Wait for the current job to finish before trying again.")); return; }
            if (!working()) { background(id, [this, path] { return removeSource(path, true); }); return; }
            // Thumbnail previews run almost all the time. Removal must not wait
            // for them, so forget the source now and leave its extraction in
            // place (a running preview may still read it); Clear cache frees it later.
            auto result = removeSource(path, false);
            persist(); reply(id, result); emitState();
        }
        else if (action == "plan" || action == "start") {
            // Planning hashes sources too, so it belongs on the same background worker.
            Json snapshot; { std::lock_guard lock(mutex_); snapshot = settings_; }
            snapshot.update(payload.value("options", Json::object())); payload["options"] = snapshot;
            if (action == "plan") background(id, [this, payload] {
                auto prepared = plan(payload, true); auto report = Json::parse(planJson(prepared));
                { std::lock_guard lock(mutex_); pendingPlan_ = std::make_unique<Plan>(prepared); }
                return report;
            });
            else if (payload.contains("plan")) {
                Plan confirmed;
                { std::lock_guard lock(mutex_);
                  if (!pendingPlan_ || Json::parse(planJson(*pendingPlan_)) != payload["plan"]) throw UiError("native.planExpired", "The confirmed plan changed or expired. Review the export settings again.");
                  confirmed = *pendingPlan_;
                }
                auto requested = options(payload["options"]); requested.output = fs::absolute(requested.output);
                auto expectedOptions = confirmed.options;
                if (expectedOptions.collisionPolicy == CollisionPolicy::Overwrite && requested.explicitOverwrite) expectedOptions.explicitOverwrite = true;
                Plan expected; expected.options = expectedOptions; Plan supplied; supplied.options = requested;
                auto ej = Json::parse(planJson(expected)), sj = Json::parse(planJson(supplied));
                if (ej["options"] != sj["options"] || ej["mode"] != sj["mode"] || ej["direction"] != sj["direction"]) throw UiError("native.planSettingsChanged", "Settings changed after plan confirmation. Review a new plan.");
                if (expectedOptions.collisionPolicy == CollisionPolicy::Overwrite && !requested.explicitOverwrite) throw UiError("native.overwriteConsentRequired", "Explicitly confirm overwriting existing files before starting.");
                if (payload.contains("clipIds")) {
                    std::set<std::string> expectedIds, suppliedIds;
                    for (const auto& job : confirmed.jobs) for (const auto& clipId : job.clipIds) expectedIds.insert(clipId);
                    for (const auto& clipId : payload["clipIds"]) suppliedIds.insert(clipId.get<std::string>());
                    if (expectedIds != suppliedIds) throw UiError("native.planSelectionChanged", "The clip selection changed after plan confirmation. Review a new plan.");
                }
                confirmed.options = expectedOptions; confirmed.planSnapshot = planJson(confirmed);
                background(id, [this, confirmed, payload] {
                    { std::lock_guard lock(mutex_); pendingPlan_.reset(); }
                    return run(confirmed, payload["options"]);
                }, true, {{"jobId", confirmed.jobId}, {"attemptId", confirmed.attemptId}});
            } else background(id, [this, payload] { return run(plan(payload), payload["options"]); }, true);
        }
        else if (action == "cancel") { cancel(); auto response = messageData(UiError("native.cancelRequested", "Cancellation will stop at a safe boundary. Wait for the current process to finish.")); response["requested"] = true; reply(id, response); emitState(); }
        else if (action == "retryFailed") {
            Plan previous; std::vector<Result> results; Json approvedOptions;
            auto wanted = valueString(payload, "attemptId", valueString(payload, "jobId"));
            { std::lock_guard lock(mutex_);
              if (!lastPlan_) throw UiError("native.retrySourceRequired", "Reopen the source package, select failed clips, and start a new job.");
              auto count = std::count_if(jobs_.begin(), jobs_.end(), [&](const Json& job) { return job.value("jobId", "") == lastPlan_->jobId; });
              if (wanted.empty() || (wanted != lastPlan_->attemptId && !(wanted == lastPlan_->jobId && count == 1))) throw UiError("native.retryHistorical", "This older attempt cannot be retried directly. Reopen the source and review a new plan for its failed clips.");
              previous = *lastPlan_; results = lastResults_; approvedOptions = lastOptionSnapshot_;
            }
            auto metadata = Json::parse(planJson(previous)); auto opts = approvedOptions; opts.update(metadata["options"]);
            opts["mode"] = metadata["mode"]; opts["direction"] = metadata["direction"];
            opts["explicitOverwrite"] = false;
            opts.update(payload.value("options", Json::object()));
            opts["explicitOverwrite"] = opts.value("collisionPolicy", "rename") == "overwrite" && payload.value("explicitOverwrite", false);
            if (opts.value("collisionPolicy", "rename") == "overwrite" && !opts["explicitOverwrite"].get<bool>()) throw UiError("native.retryConsentRequired", "Give new consent to overwrite existing results for this retry.");
            auto nextAttempt = newIdentity();
            // Revalidate only failed clips against the original immutable settings plus explicit repairs.
            background(id, [this, previous, results, opts, nextAttempt] {
                auto failedPlan = animbridge::retryFailed(previous, results); Session failed;
                for (const auto& job : failedPlan.jobs) { auto clip = job.clip; clip.selected = true; failed.clips.push_back(clip); }
                auto retry = makePlan(failed, options(opts)); retry.jobId = previous.jobId; retry.parentAttemptId = previous.attemptId;
                retry.attemptId = nextAttempt;
                retry.planSnapshot = planJson(retry); return run(retry, opts);
            }, true, {{"jobId", previous.jobId}, {"attemptId", nextAttempt}});
        }
        else if (action == "inspect" || action == "preview" || action == "validateSkeleton" || action == "connectUnity") {
            auto operation = action == "validateSkeleton" ? "validate" : action == "connectUnity" ? "helper-test" : action;
            if (action == "inspect" || action == "preview") { backgroundRead(id, operation, payload); return; }
            if (working()) { error(id, busy_ ? "BUSY" : "READ_IN_PROGRESS", UiError("native.analysisBusy", "Wait for analysis, preview, or connection validation to finish or be cancelled before trying again.")); return; }
            auto check = payload; if (check.contains("options") && check["options"].is_object()) check.update(check["options"]);
            { std::lock_guard lock(mutex_);
              if (operation == "validate") skeleton_ = {{"state", "checking"}, {"valid", false}, {"path", valueString(check, "path", valueString(check, "model", valueString(check, "skeleton", valueString(settings_, "skeleton"))))}, {"clipId", valueString(check, "clipId")}, {"validationMapping", check.value("mapping", Json::object())}};
              if (operation == "helper-test") unity_ = {{"state", "checking"}, {"connected", false}, {"editorPath", valueString(check, "editorPath", valueString(check, "unityEditor", valueString(settings_, "unityEditor")))}};
            }
            background(id, [this, operation, payload] { return helper(operation, payload); }, false, Json::object(), false);
        }
        else if (action == "convertUnrealAsset") {
            if (working()) { error(id, busy_ ? "BUSY" : "READ_IN_PROGRESS", UiError("native.analysisBusy", "Wait for analysis, preview, or connection validation to finish or be cancelled before trying again.")); return; }
            Json prepared;
            try { prepared = prepareUnrealConversion(payload); }
            catch (const UiError& refused) {
                // Stable codes let the UI offer the matching picker instead of a dead end.
                static const std::map<std::string, std::string> codes = {
                    {"native.unrealProjectApprovalRequired", "UNREAL_PROJECT_APPROVAL_REQUIRED"},
                    {"native.unrealProjectMismatch", "UNREAL_PROJECT_MISMATCH"},
                    {"native.unrealProjectMissing", "UNREAL_PROJECT_MISSING"},
                    {"native.unrealEditorRequired", "UNREAL_EDITOR_REQUIRED"}};
                auto code = codes.find(refused.messageKey);
                error(id, code == codes.end() ? "REQUEST_FAILED" : code->second, refused);
                return;
            }
            background(id, [this, prepared] { return convertUnrealAsset(prepared); });
        }
        else if (action == "saveSettings") {
            if (working()) { error(id, busy_ ? "BUSY" : "READ_IN_PROGRESS", UiError("native.analysisBusy", "Wait for analysis, preview, or connection validation to finish or be cancelled before trying again.")); return; }
            auto incoming = payload.value("settings", payload);
            if (!incoming.is_object()) throw UiError("native.incomingSettingsInvalid", "Invalid settings format.");
            if (incoming.contains("language") && !supportedLanguage(incoming["language"])) throw UiError("native.unsupportedLanguage", "Unsupported interface language.");
            if (incoming.contains("cacheRoot") && incoming["cacheRoot"] != utf8(cache_)) throw UiError("native.cacheReadOnly", "The app cache location is read-only. Only the displayed app-owned location is used.");
            if (incoming.contains("uiScale")) { int scale = incoming["uiScale"].get<int>(); if (scale < 75 || scale > 200) throw UiError("native.uiScaleInvalid", "Set UI scale between 75% and 200%."); }
            if (incoming.contains("outputRoot") && !incoming["outputRoot"].get<std::string>().empty()) {
                // authorizePayload resolved and revalidated the native grant
                // before dispatch. Settings never create a directory from text.
                auto path = fromUtf8(incoming["outputRoot"].get<std::string>());
                if (fs::space(path).available < 1024 * 1024) throw UiError("native.outputSpaceLow", "The output folder has less than 1 MiB of free space.");
            }
            { std::lock_guard lock(mutex_);
              if (incoming.contains("skeleton") && incoming["skeleton"] != settings_["skeleton"] && incoming["skeleton"] != skeleton_.value("path", "")) skeleton_ = {{"state", "unvalidated"}, {"valid", false}, {"path", incoming["skeleton"]}};
              if (incoming.contains("unityEditor") && incoming["unityEditor"] != settings_["unityEditor"]) {
                  unity_ = {{"state", "disconnected"}, {"connected", false}};
                  // Clearing the editor is a decision: do not auto-connect it again.
                  if (incoming["unityEditor"] == "" && !valueString(settings_, "unityEditor").empty()) { settings_["unityEditorAutoCleared"] = true; unityAuto_ = {{"state", "declined"}, {"path", ""}}; }
              }
              // Same for Unreal Editor: clearing it must not bring the auto-detected one back.
              if (incoming.contains("unreal_editor") && incoming["unreal_editor"] == "" && !valueString(settings_, "unreal_editor").empty()) { settings_["unrealEditorAutoCleared"] = true; unrealAuto_ = {{"state", "declined"}, {"path", ""}}; }
              settings_.update(incoming); settings_["cacheRoot"] = utf8(cache_);
            }
            persist(); reply(id, state()["settings"]); emitState();
        }
        else if (action == "openPath") {
            auto path = fromUtf8(payload.value("path", ""));
            if (!allowedPath(path)) throw UiError("native.openPathNotAllowed", "Only verified local files and output folders can be opened.");
            if (!services_.reveal) throw std::runtime_error("Native reveal service is unavailable");
            reply(id, services_.reveal(path));
        }
        else if (action == "copyText") {
            if (!services_.copyText) throw std::runtime_error("Native clipboard is unavailable");
            services_.copyText(valueString(payload, "text")); reply(id, {{"copied", true}});
        }
        else if (action == "saveReport") {
            Json report; { std::lock_guard lock(mutex_); if (jobs_.empty()) throw UiError("native.reportMissing", "There is no job report to save."); auto wanted = valueString(payload, "attemptId", valueString(payload, "jobId")); report = jobs_.back(); if (!wanted.empty()) { auto found = std::find_if(jobs_.rbegin(), jobs_.rend(), [&](const Json& item) { return item.value("jobId", "") == wanted || item.value("attemptId", "") == wanted; }); if (found == jobs_.rend()) throw UiError("native.historyMissing", "Job history entry not found."); report = *found; } }
            if (!services_.save) throw std::runtime_error("Native save dialog is unavailable");
            if (working()) throw UiError("native.analysisBusy", "Wait for the active operation before saving a report.");
            if (pickerOutstanding_.exchange(true)) throw std::runtime_error("A native selection dialog is already open");
            auto lifetime = alive_; try { services_.save(payload, [this, lifetime, id, report](fs::path path, std::string failure) {
                if (!lifetime->load()) return;
                pickerOutstanding_.store(false);
                if (!failure.empty()) { error(id, "SAVE_FAILED", std::runtime_error(failure)); return; }
                if (path.empty()) { reply(id, {{"saved", false}}); return; }
                const auto existed = fs::exists(path); const auto approvedIdentity = existed ? fileIdentity(path) : std::string();
                background(id, [path, report, existed, approvedIdentity] {
                    if (existed && (!fs::exists(path) || fileIdentity(path) != approvedIdentity)) throw std::runtime_error("The report destination changed after native confirmation; choose it again");
                    FileCommitter::writeAtomic(path, report.dump(2), existed);
                    return Json{{"saved", true}, {"path", utf8(path)}};
                }, false, Json::object(), false);
            }); } catch (...) { pickerOutstanding_.store(false); throw; }
        }
        else if (action == "clearCache") {
            background(id, [this] {
                // Only known app cache content, never a user-supplied cacheRoot or source directory.
                if (fileIdentity(cache_) != cacheIdentity_ || fs::is_symlink(cache_)) throw UiError("native.cacheOwnershipInvalid", "Could not verify the app-owned cache path.");
                auto owner = readJson(cache_ / ".animbridge-cache-owner.json");
                if (owner.value("owner", "") != "AnimBridge" || owner.value("identity", "") != cacheIdentity_) throw std::runtime_error("Cache ownership record does not match its actual directory");
                std::vector<fs::path> protectedPaths;
                { std::lock_guard lock(mutex_);
                  for (const auto& clip : session_.clips) if (inside(clip.source, cache_)) protectedPaths.push_back(clip.source);
                  for (const auto& path : session_.extractedRoots) if (inside(path, cache_)) protectedPaths.push_back(path);
                  for (const auto* plan : {pendingPlan_.get(), lastPlan_.get()}) if (plan) for (const auto& job : plan->jobs) {
                      if (inside(job.clip.source, cache_)) protectedPaths.push_back(job.clip.source);
                      if (!plan->options.skeleton.empty() && inside(plan->options.skeleton, cache_)) protectedPaths.push_back(plan->options.skeleton);
                  }
                }
                std::uintmax_t removed = 0;
                for (const auto* entry : {"packages", "jobs", "ipc", "helper", "editor-probes"}) {
                    auto path = cache_ / entry; if (fs::exists(path)) removed += removeOwnedCache(path, cache_, protectedPaths);
                }
                auto response = messageData(UiError("native.cacheCleared", "Unused app-owned cache was cleared. Cache used by active packages and confirmed plans was preserved."));
                response.update({{"removed", removed}, {"protectedPaths", protectedPaths.size()}}); return response;
            });
        }
        else error(id, "UNKNOWN_ACTION", UiError("native.actionUnsupported", "Unsupported app request: " + action, {{"action", action}}));
    } catch (const std::exception& e) { error(id, "REQUEST_FAILED", e); }
}
}
