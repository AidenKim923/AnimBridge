#include "animbridge/path_grants.hpp"
#include "animbridge/platform_services.hpp"
#include "animbridge/file_commit.hpp"
#include "animbridge/path_identity.hpp"
#include "animbridge/ui_message.hpp"
#include <fstream>
#include <regex>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#endif

namespace animbridge {
namespace {
using Json = nlohmann::json;
std::string purposeName(GrantPurpose p) { switch (p) { case GrantPurpose::Read: return "read"; case GrantPurpose::ExportRoot: return "exportRoot"; case GrantPurpose::Editor: return "editor"; case GrantPurpose::EngineProject: return "engineProject"; } return {}; }
GrantPurpose purposeValue(const std::string& p) { if (p == "read") return GrantPurpose::Read; if (p == "exportRoot") return GrantPurpose::ExportRoot; if (p == "editor") return GrantPurpose::Editor; if (p == "engineProject") return GrantPurpose::EngineProject; throw std::runtime_error("Unknown path approval purpose"); }
std::string sourceName(GrantSource s) { switch (s) { case GrantSource::Picker: return "native-picker"; case GrantSource::NativeDrop: return "native-drop"; case GrantSource::NativeDocument: return "native-document"; case GrantSource::CommandLine: return "command-line"; case GrantSource::AutoDiscovered: return "auto-discovered"; case GrantSource::AppOutput: return "app-output"; } return {}; }
Json fingerprint(const fs::path& path) {
    const auto canonical = fs::canonical(path); const auto encoded = utf8(canonical);
    // dump validates boundary UTF-8 before any bytes enter state or UI messages.
    (void)Json(encoded).dump();
    Json result = {{"path", encoded}, {"directory", fs::is_directory(canonical)}};
#ifdef _WIN32
    auto handle = CreateFileW(canonical.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("Path is unavailable or access was denied");
    BY_HANDLE_FILE_INFORMATION info{}; auto ok = GetFileInformationByHandle(handle, &info); CloseHandle(handle);
    if (!ok) throw std::runtime_error("Could not inspect path identity");
    result["identity"] = std::to_string(info.dwVolumeSerialNumber) + ":" + std::to_string(info.nFileIndexHigh) + ":" + std::to_string(info.nFileIndexLow);
#else
    struct stat info{}; if (::stat(canonical.c_str(), &info)) throw std::runtime_error("Path is unavailable or access was denied");
    result["identity"] = std::to_string(info.st_dev) + ":" + std::to_string(info.st_ino);
#endif
    if (fs::is_regular_file(canonical)) { result["sha256"] = fileSha256(canonical); result["size"] = fs::file_size(canonical); }
    return result;
}
void checkWritable(const fs::path& root) {
    if (!fs::is_directory(root)) throw std::runtime_error("Choose an existing writable folder");
    auto probe = root / (".animbridge-access-" + newIdentity());
#ifdef _WIN32
    auto handle = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("Folder write access was denied"); CloseHandle(handle);
#else
    auto descriptor = ::open(probe.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor < 0) throw std::runtime_error("Folder write access was denied"); ::close(descriptor); ::unlink(probe.c_str());
#endif
}
Json probeEditor(const fs::path& path, const std::string& engine, const fs::path& probeRoot) {
    if (engine == "runtime") {
        if (!developmentMode()) throw std::runtime_error("External runtime approvals are disabled in distribution mode");
        return {{"kind", "development-runtime"}};
    }
    if (engine == "unreal") {
        auto cursor = path.parent_path();
        for (int i = 0; i < 9 && cursor.has_parent_path(); ++i, cursor = cursor.parent_path()) {
            auto versionFile = cursor / "Build/Build.version";
            if (!fs::is_regular_file(versionFile)) continue;
            auto version = Json::parse(std::ifstream(versionFile));
            if (version.value("MajorVersion", 0) != 5 || version.value("MinorVersion", 0) != 8 || version.value("PatchVersion", 0) != 2) throw std::runtime_error("This engine integration currently requires Unreal 5.8.2");
            return {{"kind", "unreal-build-version"}, {"version", "5.8.2"}, {"metadata", fingerprint(versionFile)}};
        }
        throw std::runtime_error("The selected Unreal Editor has no verifiable Engine/Build/Build.version");
    }
    if (engine != "unity") throw std::runtime_error("Choose the Editor engine before selecting its executable");
    // Unity documents -version as printing the version without opening Editor.
    auto work = (probeRoot.empty() ? resolveRuntimeContext().cacheRoot : probeRoot) / "editor-probes" / newIdentity(); fs::create_directories(work);
    auto log = work / "version.log";
    auto result = runProcess(path, {"-version"}, work, log, 30, processEnvironment({{"TMPDIR", utf8(work)}, {"TMP", utf8(work)}, {"TEMP", utf8(work)}}));
    std::ifstream input(log, std::ios::binary); std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (result.timedOut || result.exitCode || !std::regex_search(text, std::regex(R"(\b6000\.4\.7f1\b)"))) throw std::runtime_error("The selected Editor could not verify Unity 6000.4.7f1");
    return {{"kind", "unity-version-command"}, {"version", "6000.4.7f1"}};
}
void validate(const Json& record) {
    auto path = fromUtf8(record.at("path").get<std::string>());
    if (fingerprint(path) != record.at("fingerprint")) throw std::runtime_error("Path approval is stale; select the path again");
    auto purpose = purposeValue(record.at("purpose"));
    if (purpose == GrantPurpose::Read && fs::is_directory(path)) {
        std::error_code error; fs::directory_iterator accessProbe(path, error);
        if (error) throw std::runtime_error("Source folder access was denied; select it again in the native picker");
    }
    if (record.contains("boundaryFingerprint") && fingerprint(fromUtf8(record.at("boundary"))) != record.at("boundaryFingerprint")) throw std::runtime_error("Approved project directory was replaced");
    if (purpose == GrantPurpose::ExportRoot || purpose == GrantPurpose::EngineProject) checkWritable(record.at("boundary").get<std::string>());
    if (purpose == GrantPurpose::Editor) {
        if (!fs::is_regular_file(path)) throw std::runtime_error("Choose an Editor executable");
#ifndef _WIN32
        if (::access(path.c_str(), X_OK)) throw std::runtime_error("Editor is not executable");
#else
        if (path.extension() != L".exe" && path.extension() != L".EXE") throw std::runtime_error("Choose an Editor executable");
#endif
        if (record.contains("engineProbe")) {
            const auto& probe = record["engineProbe"];
            if (probe.contains("metadata") && fingerprint(fromUtf8(probe["metadata"].at("path"))) != probe["metadata"]) throw std::runtime_error("Editor version metadata changed; select the Editor again");
        }
    }
}
}
bool pathWithin(const fs::path& path, const fs::path& root) {
    // Compare actual component identities when spelling differs. Windows can
    // contain case-sensitive directories; macOS can contain either volume type.
    try { return pathContains(root, path); } catch (...) { return false; }
}
PathGrantRegistry::Json PathGrantRegistry::issue(const fs::path& chosen, GrantPurpose purpose, GrantSource source, const std::string& engine, const std::string& targetFolder) {
    auto path = fs::canonical(chosen);
#ifdef __APPLE__
    if (purpose == GrantPurpose::Editor && path.extension() == ".app") path /= "Contents/MacOS/" + (engine == "unreal" ? std::string("UnrealEditor") : std::string("Unity"));
#endif
    auto boundary = purpose == GrantPurpose::EngineProject && fs::is_regular_file(path) ? path.parent_path() : path;
    std::vector<fs::path> companions;
    if (purpose == GrantPurpose::Read && path.filename() == "animbridge-manifest.json") {
        if (fs::file_size(path) > 16 * 1024 * 1024) throw std::runtime_error("Engine manifest exceeds its size limit");
        auto manifest = Json::parse(std::ifstream(path));
        if (!manifest.contains("clips") || !manifest["clips"].is_array() || manifest["clips"].size() > 100000) throw std::runtime_error("Invalid engine manifest clips");
        for (const auto& clip : manifest["clips"]) {
            auto member = fromUtf8(clip.at("fbx").get<std::string>());
            if (member.is_relative()) member = path.parent_path() / member;
            if (!pathWithin(member, path.parent_path()) || !fs::is_regular_file(member) || member.extension() != ".fbx") throw std::runtime_error("Manifest FBX escapes its approved companion directory");
            auto relative = fs::absolute(member).lexically_relative(path.parent_path()); auto cursor = path.parent_path();
            for (const auto& component : relative) { cursor /= component; if (fs::is_symlink(fs::symlink_status(cursor))) throw std::runtime_error("Manifest companion path contains a symlink"); }
            companions.push_back(fs::canonical(member));
        }
    }
    auto chosenEngine = engine.empty() ? "unity" : engine;
    if (purpose == GrantPurpose::EngineProject) {
        if (chosenEngine == "unity") {
            if (!fs::is_directory(path) || !fs::is_directory(path / "Assets") || !fs::is_directory(path / "ProjectSettings")) throw std::runtime_error("Choose an existing Unity project with Assets and ProjectSettings");
        } else if (chosenEngine == "unreal") {
            if (!fs::is_regular_file(path) || path.extension() != ".uproject") throw std::runtime_error("Choose an Unreal .uproject file");
        } else throw std::runtime_error("Unknown engine project kind");
    }
    Json record = {{"path", utf8(path)}, {"purpose", purposeName(purpose)}, {"source", sourceName(source)}, {"boundary", utf8(boundary)}, {"fingerprint", fingerprint(path)}, {"engine", chosenEngine}, {"targetFolder", targetFolder}};
    if (purpose == GrantPurpose::EngineProject) record["boundaryFingerprint"] = fingerprint(boundary);
    validate(record);
    if (purpose == GrantPurpose::Editor) record["engineProbe"] = probeEditor(path, chosenEngine, probeRoot_);
    auto token = nativeCapability(); {
        std::lock_guard lock(mutex_);
        // A fresh native selection replaces the previous approval for this path
        // and purpose. Public state must not resurrect an older stale token.
        for (auto entry = grants_.begin(); entry != grants_.end();) if (entry->second.record["path"] == record["path"] && entry->second.record["purpose"] == record["purpose"]) entry = grants_.erase(entry); else ++entry;
        grants_.emplace(token, Grant{token, record});
    }
    for (const auto& companion : companions) issue(companion, GrantPurpose::Read, source);
    auto result = record; result.erase("fingerprint"); result["grant"] = token; return result;
}
fs::path PathGrantRegistry::resolve(const std::string& token, GrantPurpose purpose, const fs::path& child) const {
    std::lock_guard lock(mutex_); auto found = grants_.find(token);
    if (found == grants_.end() || found->second.record.at("purpose") != purposeName(purpose)) throw UiError("native.openPathNotAllowed", "Missing or incorrect native path approval. Select the path again.");
    validate(found->second.record); auto path = fromUtf8(found->second.record.at("path"));
    if (child.empty()) return path;
    if (!fs::is_directory(path) || !pathWithin(child, path)) throw std::runtime_error("Path is outside its approved root");
    // A newly introduced symlink may point inside the same root; reject that too.
    auto relative = fs::absolute(child).lexically_relative(path); auto cursor = path;
    for (const auto& component : relative) { cursor /= component; if (fs::is_symlink(fs::symlink_status(cursor))) throw std::runtime_error("A path component changed to a symlink"); }
    return fs::weakly_canonical(child);
}
bool PathGrantRegistry::permits(const fs::path& path, GrantPurpose purpose) const {
    std::vector<std::string> tokens; { std::lock_guard lock(mutex_); for (const auto& [token, grant] : grants_) if (grant.record["purpose"] == purposeName(purpose)) tokens.push_back(token); }
    for (const auto& token : tokens) try { auto root = resolve(token, purpose); if (root == fs::weakly_canonical(path) || (fs::is_directory(root) && resolve(token, purpose, path) == fs::weakly_canonical(path))) return true; } catch (...) {} return false;
}
PathGrantRegistry::Json PathGrantRegistry::records() const { std::lock_guard lock(mutex_); Json result = Json::array(); for (const auto& [_, grant] : grants_) result.push_back(grant.record); return result; }
PathGrantRegistry::Json PathGrantRegistry::publicRecords() const { std::lock_guard lock(mutex_); Json result = Json::array(); for (const auto& [token, grant] : grants_) { auto row = grant.record; row.erase("fingerprint"); row["grant"] = token; result.push_back(row); } return result; }
PathGrantRegistry::Json PathGrantRegistry::restore(const Json& records) {
    Json stale = Json::array(); for (const auto& row : records) try {
        validate(row);
        if (row.value("purpose", "") == "editor") {
            if (!row.contains("engineProbe")) throw std::runtime_error("Editor requires a new native version probe");
            if (probeEditor(fromUtf8(row.at("path")), row.value("engine", "unity"), probeRoot_) != row["engineProbe"]) throw std::runtime_error("Editor version changed");
        }
        std::lock_guard lock(mutex_);
        for (auto entry = grants_.begin(); entry != grants_.end();) if (entry->second.record["path"] == row["path"] && entry->second.record["purpose"] == row["purpose"]) entry = grants_.erase(entry); else ++entry;
        auto token = nativeCapability(); grants_.emplace(token, Grant{token, row});
    } catch (const std::exception& e) { auto failed = row; failed.erase("fingerprint"); failed["state"] = "stale"; failed["reason"] = e.what(); stale.push_back(failed); } return stale;
}
void PathGrantRegistry::approveTarget(const std::string& token, const std::string& engine, const std::string& target) const {
    (void)resolve(token, GrantPurpose::EngineProject); std::lock_guard lock(mutex_); const auto& row = grants_.at(token).record;
    if (row.value("engine", "") != engine || row.value("targetFolder", "") != target) throw std::runtime_error("Engine or target folder differs from the native project approval; select the project again");
    const std::string prefix = engine == "unity" ? "Assets/" : "/Game/";
    if (!target.starts_with(prefix) || target.find("..") != std::string::npos || target.find('\\') != std::string::npos || target.find(':') != std::string::npos) throw std::runtime_error("Choose a target folder inside the approved Assets or Content root");
    auto root = fromUtf8(row.at("boundary"));
    auto relative = fromUtf8(engine == "unity" ? target : "Content/" + target.substr(prefix.size()));
    auto destination = root / relative;
    if (!pathWithin(destination, root)) throw std::runtime_error("Engine output escapes the approved project");
    auto cursor = root;
    for (const auto& component : relative) { cursor /= component; std::error_code error; auto status = fs::symlink_status(cursor, error); if (!error && fs::is_symlink(status)) throw std::runtime_error("A project target folder is a symlink; select a local project folder"); }
}
void writeDesktopJson(const fs::path& path, const Json& value) {
    FileCommitter::writeAtomic(path, value.dump(2), true);
}
}
