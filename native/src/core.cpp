#include "animbridge/core.hpp"
#include "animbridge/file_commit.hpp"
#include "animbridge/path_identity.hpp"
#include "animbridge/process.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <cmath>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif

namespace animbridge {
using json = nlohmann::json;
std::string utf8(const fs::path& path) {
    try { auto s = path.u8string(); std::string value(s.begin(), s.end()); requireUtf8(value); return value; }
    catch (const std::exception&) { throw std::runtime_error("INVALID_ENCODING: local path cannot be represented as valid UTF-8 (entry omitted)"); }
}
fs::path fromUtf8(const std::string& value) { requireUtf8(value); return fs::path(std::u8string(value.begin(), value.end())); }
std::string engineName(Engine engine) { return engine == Engine::Unity ? "unity" : engine == Engine::Unreal ? "unreal" : "unknown"; }
namespace {
struct PackageRollback {
    fs::path root, owner;
    bool committed = false;
    ~PackageRollback() { if (!committed && !root.empty()) { std::error_code ignored; removeOwnedTree(root, owner, ignored); } }
};
bool selectableFor(const Clip& clip, ExportMode mode, bool allowUnityHelper) {
    return mode == ExportMode::ExtractOriginal ? clip.extractable : clip.selectable ||
        (allowUnityHelper && clip.extractable && clip.kind == "anim" && clip.route == "requires-unity-bake");
}
std::string lower(std::string s) { std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); }); return s; }
std::string trim(std::string s) {
    auto first = s.find_first_not_of(" \t\r\n"); if (first == std::string::npos) return {};
    return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}
std::string unquote(std::string s) {
    s = trim(s);
    if (s.size() > 1 && s.front() == '"' && s.back() == '"') { try { return json::parse(s).get<std::string>(); } catch (...) {} }
    if (s.size() > 1 && s.front() == '\'' && s.back() == '\'') return s.substr(1, s.size() - 2);
    return s;
}
bool linked(const fs::path& p) {
#ifdef _WIN32
    auto a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
#else
    return fs::is_symlink(fs::symlink_status(p));
#endif
}
void requireCompanionWithin(const fs::path& root, const fs::path& path) {
    if (!pathContains(root, path)) throw std::runtime_error("Input companion leaves its approved directory");
    for (auto current = fs::absolute(path); !current.empty();) {
        if (linked(current)) throw std::runtime_error("Linked input companions are not permitted");
        if (sameFile(current, root)) return;
        auto parent = current.parent_path(); if (parent == current) break; current = parent;
    }
    throw std::runtime_error("Input companion directory identity could not be verified");
}
std::string readText(const fs::path& path, std::uintmax_t limit = 16 * 1024 * 1024) {
    if (fs::file_size(path) > limit) throw std::runtime_error("Metadata size limit exceeded: " + utf8(path));
    std::ifstream f(path, std::ios::binary); if (!f) throw std::runtime_error("Cannot read " + utf8(path));
    std::string value(std::istreambuf_iterator<char>(f), {}); requireUtf8(value); return value;
}
void writeJson(const fs::path& path, const json& value) {
    std::ofstream f(path, std::ios::binary); if (!f) throw std::runtime_error("Cannot write " + utf8(path));
    f << value.dump(2) << '\n'; if (!f) throw std::runtime_error("Write failed: " + utf8(path));
}
std::string stableId(const fs::path& source, const std::string& name) {
    return "s2_" + sourceIdentityDigest(source, projectNamespaceFor(source), "", name);
}
void addClip(Session& session, Clip clip) {
    requireUtf8(clip.name); requireUtf8(clip.virtualPath); utf8(clip.source);
    if (clip.id.empty()) clip.id = stableId(clip.source, clip.name);
    auto found = std::find_if(session.clips.begin(), session.clips.end(), [&](auto& other) { return other.id == clip.id; });
    if (found == session.clips.end()) session.clips.push_back(std::move(clip));
    else if ((!clip.sourceHash.empty() && clip.sourceHash != found->sourceHash) || clip.metadataJson != found->metadataJson) {
        clip.selected = found->selected; *found = std::move(clip);
    }
}
std::vector<std::string> metaClips(const fs::path& path) {
    std::vector<std::string> names;
    std::istringstream stream(readText(path)); std::string line; int listIndent = -1;
    while (std::getline(stream, line)) {
        auto t = trim(line); auto indent = static_cast<int>(line.find_first_not_of(" \t"));
        if (listIndent < 0) { if (t == "clipAnimations:") listIndent = indent; continue; }
        if (t.empty()) continue;
        if (indent < listIndent || (indent == listIndent && !t.starts_with('-'))) break;
        if ((indent == listIndent + 2 && t.starts_with("name:")) || (indent == listIndent && t.starts_with("- name:"))) {
            auto name = unquote(t.substr(t.find(':') + 1));
            if (!name.empty()) names.push_back(name);
        }
    }
    return names;
}
bool humanoidModelMeta(const fs::path& path) {
    std::istringstream stream(readText(path)); std::string line;
    while (std::getline(stream, line)) {
        // animationType belongs directly to ModelImporter, not clip settings.
        if (line.find_first_not_of(' ') != 2) continue;
        auto value = trim(line);
        if (!value.starts_with("animationType:")) continue;
        value = trim(value.substr(14)); auto comment = value.find('#');
        if (comment != std::string::npos) value = trim(value.substr(0, comment));
        return value == "3";
    }
    return false;
}
json clipJson(const Clip& c) {
    json caps = json::parse(c.capabilitiesJson, nullptr, false); if (caps.is_discarded() || caps.empty()) caps = {{"preview", false}, {"extract", true}, {"bake", c.selectable ? json(nullptr) : json(false)}};
    json dependencies = json::parse(c.dependenciesJson, nullptr, false); if (dependencies.is_discarded()) dependencies = json::array();
    json metadata = json::parse(c.metadataJson, nullptr, false); if (metadata.is_discarded()) metadata = json::object();
    return {{"id", c.id}, {"clipId", c.id}, {"name", c.name}, {"source", utf8(c.source)}, {"virtual_path", c.virtualPath},
            {"kind", c.kind}, {"route", c.route}, {"engine", engineName(c.engine)}, {"note", c.note},
            {"selected", c.selected}, {"selectable", c.selectable}, {"fps", c.fpsKnown || c.fps > 0 ? json(c.fps) : json(nullptr)}, {"duration", c.durationKnown ? json(c.duration) : json(nullptr)},
            {"sourceHash", c.sourceHash}, {"sourceId", c.sourceId}, {"packageId", c.packageId}, {"packageSourcePath", c.packageSourcePath.empty() ? json(nullptr) : json(utf8(c.packageSourcePath))}, {"packageSourceHash", c.packageSourceHash.empty() ? json(nullptr) : json(c.packageSourceHash)}, {"parserVersion", c.parserVersion},
            {"sourceIdentityVersion", c.sourceIdentityVersion}, {"identityDigest", c.identityDigest}, {"fileIdentity", c.fileIdentity}, {"projectNamespace", c.projectNamespace}, {"legacyId", c.legacyId}, {"exportId", c.exportId}, {"exportIdentityVersion", c.exportIdentityVersion}, {"exportIdentityDigest", c.exportIdentityDigest},
            {"type", c.type.empty() ? json(nullptr) : json(c.type)}, {"rigType", c.rigType.empty() ? json(nullptr) : json(c.rigType)},
            {"capabilities", caps}, {"capabilityReasons", json::parse(c.capabilityReasonsJson)}, {"dependencies", dependencies}, {"metadata", metadata}};
}
void scanManifest(Session& session, const fs::path& path) {
    auto data = json::parse(readText(path));
    if (data.value("animbridge_version", 0) != 1 || data.value("source_engine", "") != "unreal")
        throw std::runtime_error("Unsupported AnimBridge manifest: " + utf8(path));
    if (!data.contains("clips") || !data["clips"].is_array()) throw std::runtime_error("Manifest clips must be an array");
    for (auto& entry : data["clips"]) {
        auto fbx = fromUtf8(entry.at("fbx").get<std::string>());
        if (fbx.is_relative()) {
            auto normalized = fbx.lexically_normal();
            if (normalized.empty() || *normalized.begin() == "..") throw std::runtime_error("Manifest relative FBX leaves its directory");
            fbx = path.parent_path() / normalized;
        }
        if (!fs::is_regular_file(fbx) || linked(fbx) || lower(utf8(fbx.extension())) != ".fbx") throw std::runtime_error("Manifest FBX missing or invalid: " + utf8(fbx));
        requireCompanionWithin(path.parent_path(), fbx);
        Clip clip; clip.name = entry.value("name", utf8(fbx.stem()));
        auto exportedId = entry.value("id", "");
        // Unreal object paths are only unique inside one project. Qualify with the exported FBX identity.
        clip.id = stableId(fbx, exportedId.empty() ? clip.name : exportedId + "#" + clip.name);
        if (!exportedId.empty()) clip.identityDigest = "engine-object=" + exportedId;
        clip.legacyId = exportedId.empty() ? legacyClipId(fbx, clip.name) : exportedId + "#" + legacyClipId(fbx, exportedId);
        clip.projectNamespace = "unreal-export:" + utf8(verifiedCanonicalPath(path.parent_path()));
        if (entry.contains("exportId")) {
            clip.exportId = entry.at("exportId").get<std::string>(); requireUtf8(clip.exportId);
            clip.exportIdentityVersion = entry.value("exportIdentityVersion", 1u);
            clip.exportIdentityDigest = entry.value("exportIdentityDigest", entry.value("identityDigest", ""));
            if (clip.exportIdentityVersion != 1 && clip.exportIdentityVersion != 2) throw std::runtime_error("Unsupported external export identity version");
            if (!clip.exportIdentityDigest.empty()) (void)exportIdForDigest(clip.exportIdentityDigest);
        }
        clip.source = fs::absolute(fbx); clip.engine = Engine::Unreal; clip.kind = "fbx"; clip.route = "unreal-export";
        clip.virtualPath = entry.value("source_asset", utf8(fbx)); clip.fps = entry.value("fps", 0.0); clip.duration = entry.value("duration", 0.0);
        clip.metadataJson = entry.dump();
        clip.fpsKnown = entry.contains("fps") && entry["fps"].is_number(); clip.durationKnown = entry.contains("duration") && entry["duration"].is_number();
        clip.type = "unrealExport"; clip.rigType = "skeletal";
        clip.note = "Unreal engine export; Unity importer must validate the resulting Generic clip";
        addClip(session, std::move(clip));
    }
}
// Returns the only regular file with this extension directly inside folder.
// Zero or several matches are both "unknown": guessing a project is unsafe.
std::optional<fs::path> soleFileWithExtension(const fs::path& folder, const std::string& extension) {
    std::error_code error;
    std::optional<fs::path> found;
    for (fs::directory_iterator entry(folder, fs::directory_options::skip_permission_denied, error), end; !error && entry != end; entry.increment(error)) {
        const auto& candidate = entry->path();
        std::error_code typeError;
        if (!entry->is_regular_file(typeError) || typeError || linked(candidate)) continue;
        std::string name;
        try { name = lower(utf8(candidate.extension())); } catch (const std::exception&) { continue; }
        if (name != extension) continue;
        if (found) return std::nullopt;
        found = candidate;
    }
    return error ? std::nullopt : found;
}
void describeUnrealAsset(Clip& clip) {
    clip.engine = Engine::Unreal; clip.route = "requires-engine-export"; clip.selectable = false;
    auto location = locateUnrealAsset(clip.source);
    if (!location) {
        clip.note = "This .uasset is not inside an Unreal project (no .uproject found); open it from its project's Content folder to convert it";
        clip.metadataJson = json{{"unrealConvertible", false}}.dump();
        return;
    }
    clip.note = "Raw Unreal asset: convert it to FBX in AnimBridge (runs Unreal Editor headless on its project) before export";
    clip.metadataJson = json{{"unrealConvertible", true}, {"unrealProject", utf8(location->project)}, {"unrealObjectPath", location->objectPath}}.dump();
}
void scanFile(Session& session, const fs::path& path, const fs::path& root, bool allowUnrealManifests) {
    auto ext = lower(utf8(path.extension()));
    if (utf8(path.filename()) == "animbridge-manifest.json") { if (allowUnrealManifests) scanManifest(session, path); return; }
    if (ext != ".anim" && ext != ".fbx" && ext != ".uasset" && ext != ".json" && ext != ".controller" && ext != ".overridecontroller" && ext != ".cs" && ext != ".dll") return;
    Clip clip; clip.source = fs::absolute(path); clip.name = utf8(path.stem());
    clip.virtualPath = utf8(path.lexically_relative(root)); clip.kind = ext.substr(1);
    std::replace(clip.virtualPath.begin(), clip.virtualPath.end(), '\\', '/');
    if (ext == ".controller" || ext == ".overridecontroller" || ext == ".cs" || ext == ".dll") {
        clip.engine = Engine::Unity; clip.route = "preserve-only"; clip.selectable = false;
        clip.type = ext == ".cs" || ext == ".dll" ? "code" : "controller";
        clip.note = "Preserve original file only; package scripts, plugins, and state-machine logic are never executed";
    } else if (ext == ".uasset") {
        describeUnrealAsset(clip);
    } else if (ext == ".json") {
        try {
            auto data = json::parse(readText(path)); if (data.value("uab_baked", 0) != 1) return;
            for (auto& entry : data.at("clips")) {
                Clip baked = clip; baked.name = entry.at("name").get<std::string>(); baked.engine = Engine::Unity;
                baked.kind = "baked"; baked.route = "B"; baked.namedClip = true; baked.fps = entry.value("fps", data.value("fps", 0.0));
                baked.duration = entry.value("duration", 0.0); baked.durationKnown = entry.contains("duration") && entry["duration"].is_number(); baked.fpsKnown = baked.fps > 0;
                baked.type = "baked"; baked.rigType = "generic"; addClip(session, std::move(baked));
            }
        } catch (const json::exception&) { /* Other JSON files are not inputs. */ }
        return;
    } else if (ext == ".anim") {
        clip.engine = Engine::Unity; clip.route = "A";
        std::ifstream file(path, std::ios::binary); char prefix[5]{}; file.read(prefix, 5);
        if (std::string(prefix, 5) != "%YAML") {
            clip.route = "requires-unity-bake"; clip.selectable = false; clip.note = "Binary .anim: bake with UnityAnimBridgeBaker and drop baked.json";
        } else {
            std::istringstream text(readText(path, 64 * 1024 * 1024)); std::string line;
            while (std::getline(text, line)) {
                auto t = trim(line);
                if (t.starts_with("m_Name:")) { auto name = unquote(t.substr(7)); if (!name.empty()) clip.name = name; }
                if (t.starts_with("m_SampleRate:")) { try { clip.fps = std::stod(t.substr(13)); } catch (...) {} }
            }
            clip.namedClip = true; clip.note = "Transform clip; converter validates Humanoid/unsupported tracks before export";
        }
    } else {
        clip.engine = Engine::Unity; clip.route = "C";
        auto meta = fromUtf8(utf8(path) + ".meta");
        if (linked(meta)) throw std::runtime_error("Linked source metadata is not permitted");
        if (fs::is_regular_file(meta)) {
            requireCompanionWithin(path.parent_path(), meta);
            if (humanoidModelMeta(meta)) {
                clip.type = "humanoid"; clip.rigType = "humanoid"; clip.route = "requires-unity-bake";
                clip.note = "Humanoid FBX: evaluate the selected clip with a validated Avatar and isolated Unity Helper; original extraction remains available";
            }
            auto names = metaClips(meta);
            if (!names.empty()) {
                std::map<std::string, int> counts; for (auto& name : names) ++counts[name];
                for (std::size_t index = 0; index < names.size(); ++index) {
                    Clip split = clip; split.name = names[index]; split.namedClip = true;
                    if (counts[split.name] > 1) {
                        split.id = stableId(path, split.name + "#meta-index=" + std::to_string(index));
                        split.identityDigest = "meta-index=" + std::to_string(index);
                        split.selectable = false;
                        split.note = "Ambiguous duplicate clip name in .meta; give clips unique names in Unity before conversion";
                    }
                    addClip(session, std::move(split));
                }
                return;
            }
        }
        if (clip.note.empty()) clip.note = "FBX container: no named Unity clip metadata; conversion exports all embedded takes. Use engine-export manifest for Unreal input";
    }
    addClip(session, std::move(clip));
}
void scanPath(Session& session, const fs::path& path, bool allowUnrealManifests = true) {
    auto scan = [&](const fs::path& file, const fs::path& root) {
        try { utf8(file); }
        catch (const std::exception&) { session.warnings.push_back("INVALID_ENCODING: skipped input entry (diagnostic " + std::to_string(session.warnings.size() + 1) + ")"); return; }
        try { scanFile(session, file, root, allowUnrealManifests); }
        catch (const std::exception& error) {
            if (utf8(file.filename()) == "animbridge-manifest.json") throw;
            if (std::string(error.what()).starts_with("INVALID_ENCODING:")) { session.warnings.push_back("INVALID_ENCODING: skipped metadata entry (diagnostic " + std::to_string(session.warnings.size() + 1) + ")"); return; }
            Clip unknown; unknown.source = fs::absolute(file); unknown.name = utf8(file.stem()); unknown.kind = utf8(file.extension());
            if (unknown.kind.starts_with('.')) unknown.kind.erase(0, 1);
            unknown.virtualPath = utf8(file.lexically_relative(root)); unknown.route = "analysis-unknown"; unknown.selectable = false;
            unknown.note = std::string("Metadata could not be analysed: ") + error.what() + "; original preservation remains available if the file is readable";
            session.warnings.push_back(unknown.note); addClip(session, std::move(unknown));
        }
    };
    if (linked(path)) { session.warnings.push_back("Skipped linked/reparse input: " + utf8(path)); return; }
    if (fs::is_regular_file(path)) { scan(path, path.parent_path()); return; }
    if (!fs::is_directory(path)) throw std::runtime_error("Input not found: " + utf8(path));
    auto manifest = path / "animbridge-manifest.json";
    if (allowUnrealManifests && fs::is_regular_file(manifest)) { scanManifest(session, manifest); return; }
    for (fs::recursive_directory_iterator i(path, fs::directory_options::skip_permission_denied), end; i != end; ++i) {
        try { utf8(i->path()); }
        catch (const std::exception&) { if (i->is_directory()) i.disable_recursion_pending(); session.warnings.push_back("INVALID_ENCODING: skipped directory entry (diagnostic " + std::to_string(session.warnings.size() + 1) + ")"); continue; }
        if (linked(i->path())) { if (i->is_directory()) i.disable_recursion_pending(); continue; }
        if (allowUnrealManifests && i->is_directory() && fs::is_regular_file(i->path() / "animbridge-manifest.json")) {
            scanManifest(session, i->path() / "animbridge-manifest.json"); i.disable_recursion_pending(); continue;
        }
        if (i->is_regular_file()) scan(i->path(), path);
    }
}
bool contains(const fs::path& directory, const fs::path& path) {
    return pathContains(directory, path);
}
ProcessRunOptions processPolicy(const Options& options, std::int64_t deadline = 0, bool leases = false) {
    ProcessRunOptions policy; policy.jobTimeoutSeconds = options.jobTimeoutSeconds;
    policy.absoluteDeadlineMilliseconds = deadline; policy.stageLeases = leases;
    policy.journalRoot = options.journalRoot.empty() ? options.runtimeContext.stateRoot / "attempts" : options.journalRoot;
    for (const auto& path : {options.python, options.blender, options.unityEditor, options.targetEditor})
        if (!path.empty() && fs::is_regular_file(path)) policy.privateExecutables.push_back(verifiedCanonicalPath(path));
    return policy;
}
void requireItemTime(std::int64_t deadline) {
    if (deadline > 0 && deadline <= std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()) throw std::runtime_error("PROCESS_TIMEOUT: item deadline elapsed before engine import");
}
void verifyImportReport(const fs::path& path, const std::string& key, std::size_t expected) {
    auto report = json::parse(readText(path));
    if (!report.contains(key) || !report[key].is_array() || report[key].size() != expected)
        throw std::runtime_error("Engine import report does not contain the expected clip count: " + utf8(path));
    for (auto& entry : report[key]) {
        auto status = entry.value("status", "error");
        if (status != "ok" && status != "warning") throw std::runtime_error("Engine import failed; see " + utf8(path));
    }
}
void importUnreal(const Plan& plan, const Job& job, Result& result, std::int64_t deadline = 0, std::shared_ptr<std::atomic_bool> cancel = {}) {
    requireItemTime(deadline);
    auto bootstrap = job.output / "animbridge-unreal-import.py";
    auto engineName = utf8(job.output.filename());
    for (auto& c : engineName) if (!std::isalnum(static_cast<unsigned char>(c)) || static_cast<unsigned char>(c) >= 128) c = '_';
    engineName += "_" + (job.clip.exportId.empty() ? legacyClipId(job.clip.source, job.clip.name) : job.clip.exportId);
    // Separate destinations keep equal clip names from different source files from overwriting each other.
    auto destination = (plan.options.targetFolder.empty() ? "/Game/AnimBridge" : plan.options.targetFolder) + "/" + engineName;
    std::vector<std::string> argv{utf8(plan.options.repository / "unreal/uab_import.py"), "--manifest", utf8(job.output / "manifest.json"),
        "--skeleton", plan.options.targetSkeleton, "--dest", destination};
    std::ofstream file(bootstrap, std::ios::binary);
    file << "import sys, runpy\nsys.argv = " << json(argv).dump() << "\nrunpy.run_path(sys.argv[0], run_name='__main__')\n";
    file.close(); if (!file) throw std::runtime_error("Cannot write Unreal import bootstrap");
    auto log = job.output / "animbridge-unreal-import.log";
    auto process = runSupervisedProcess(plan.options.targetEditor, {utf8(fs::absolute(plan.options.targetProject)), "-unattended", "-nop4", "-nosplash", "-nullrhi", "-run=pythonscript", "-script=" + utf8(bootstrap)}, plan.options.repository, log, plan.options.timeoutSeconds, {}, cancel, processPolicy(plan.options, deadline));
    result.log = log;
    result.warnings.insert(result.warnings.end(), process.cleanupWarnings.begin(), process.cleanupWarnings.end());
    auto checks = json::parse(result.checksJson); checks["engineProcessCleanup"] = {{"complete", process.cleanupComplete}, {"ownedCleanupComplete", process.ownedCleanupComplete}, {"timeoutLayer", process.timeoutLayer}, {"stage", process.stage}}; result.checksJson = checks.dump();
    if (!process.ownedCleanupComplete) throw std::runtime_error("PROCESS_CLEANUP_INCOMPLETE: Unreal import cleanup is unverified; retain output and logs");
    if (process.timedOut) throw std::runtime_error("PROCESS_TIMEOUT: Unreal import reached its remaining item deadline or stage limit");
    if (process.cancelled) throw std::runtime_error("CANCELLED: Unreal engine import was cancelled");
    if (process.exitCode) throw std::runtime_error("Unreal engine import process failed; see log");
    auto converted = json::parse(readText(job.output / "manifest.json"));
    verifyImportReport(job.output / "unreal_import_report.json", "clips", converted.at("clips").size());
    result.status = "imported"; result.message = "Unreal import verified by engine report";
}
json importUnity(const Plan& plan, const fs::path& manifest, std::size_t count, std::int64_t deadline = 0, std::shared_ptr<std::atomic_bool> cancel = {}) {
    requireItemTime(deadline);
    auto helper = plan.options.repository / "unity/Editor/AnimBridgeImporter.cs";
    auto target = plan.options.targetProject / "Assets/Editor/AnimBridgeImporter.cs";
    if (fs::exists(target)) {
        if (readText(target) != readText(helper)) throw std::runtime_error("Existing Unity AnimBridgeImporter.cs differs; update it explicitly before import");
    } else { fs::create_directories(target.parent_path()); fs::copy_file(helper, target); }
    auto report = plan.options.output / "animbridge-unity-import-report.json";
    auto process = runSupervisedProcess(plan.options.targetEditor, {"-batchmode", "-quit", "-projectPath", utf8(fs::absolute(plan.options.targetProject)), "-executeMethod", "AnimBridgeImporter.ImportFromCommandLine", "-animBridgeManifest", utf8(manifest), "-animBridgeDest", plan.options.targetFolder.empty() ? "Assets/AnimBridge" : plan.options.targetFolder, "-animBridgeReport", utf8(report), "-logFile", utf8(plan.options.output / "animbridge-unity-editor.log")}, plan.options.repository, plan.options.output / "animbridge-unity-import.log", plan.options.timeoutSeconds, {}, cancel, processPolicy(plan.options, deadline));
    if (!process.ownedCleanupComplete) throw std::runtime_error("PROCESS_CLEANUP_INCOMPLETE: Unity import cleanup is unverified; retain output and logs");
    if (process.timedOut) throw std::runtime_error("PROCESS_TIMEOUT: Unity import reached its remaining item deadline or stage limit");
    if (process.cancelled) throw std::runtime_error("CANCELLED: Unity engine import was cancelled");
    if (!fs::is_regular_file(report)) throw std::runtime_error("Unity engine import produced no report (exit " + std::to_string(process.exitCode) + "); see editor log");
    auto payload = json::parse(readText(report));
    if (!payload.contains("clips") || !payload["clips"].is_array() || payload["clips"].size() != count)
        throw std::runtime_error("Unity import report does not contain the expected clip count");
    payload["process_exit_code"] = process.exitCode;
    payload["processCleanup"] = {{"complete", process.cleanupComplete}, {"ownedCleanupComplete", process.ownedCleanupComplete}, {"warnings", process.cleanupWarnings}};
    return payload;
}
}
std::string safeName(const std::string& value) {
    std::string name = value;
    for (auto& c : name) if (static_cast<unsigned char>(c) < 32 || std::string("\\/:*?\"<>|").find(c) != std::string::npos) c = '_';
    while (!name.empty() && (name.back() == '.' || name.back() == ' ')) name.pop_back();
    if (name.empty() || name == "." || name == "..") name = "Clip";
    auto stem = lower(name.substr(0, name.find('.')));
    if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul" ||
        (stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) && std::isdigit(static_cast<unsigned char>(stem[3])))) name = "_" + name;
    if (name.size() > 120) { name.resize(120); while (!name.empty() && (static_cast<unsigned char>(name.back()) & 0xc0) == 0x80) name.pop_back(); if (!name.empty() && static_cast<unsigned char>(name.back()) >= 0xc0) name.pop_back(); }
    return name;
}
std::string unrealObjectPath(const fs::path& contentRoot, const std::string& mountPoint, const fs::path& asset) {
    if (lower(utf8(asset.extension())) != ".uasset") return {};
    auto relative = asset.lexically_normal().lexically_relative(contentRoot.lexically_normal());
    if (relative.empty() || relative.is_absolute() || !relative.has_stem()) return {};
    std::string objectPath = mountPoint;
    for (const auto& folder : relative.parent_path()) {
        auto name = utf8(folder);
        if (name.empty() || name == "." || name == "..") return {};
        objectPath += "/" + name;
    }
    // Unreal names an asset's main object after its package: Name.uasset -> Name.Name.
    auto name = utf8(relative.stem());
    objectPath += "/" + name + "." + name;
    // The export script separates assets with ';'. Control characters are never valid package names.
    for (unsigned char c : objectPath) if (c < 32 || c == 127 || c == ';') return {};
    return objectPath;
}
std::optional<UnrealAssetLocation> locateUnrealAsset(const fs::path& asset) {
    auto file = fs::absolute(asset).lexically_normal();
    // A plugin's Content mounts at /<PluginName>; its project lies further up.
    std::string pluginObjectPath;
    for (auto folder = file.parent_path(); !folder.empty() && folder != folder.parent_path(); folder = folder.parent_path()) {
        std::string folderName;
        try { folderName = lower(utf8(folder.filename())); } catch (const std::exception&) { return std::nullopt; }
        if (folderName == "content" && pluginObjectPath.empty()) {
            auto owner = folder.parent_path();
            if (auto project = soleFileWithExtension(owner, ".uproject")) {
                auto objectPath = unrealObjectPath(folder, "/Game", file);
                if (objectPath.empty()) return std::nullopt;
                return UnrealAssetLocation{*project, objectPath};
            }
            if (auto plugin = soleFileWithExtension(owner, ".uplugin")) {
                pluginObjectPath = unrealObjectPath(folder, "/" + utf8(plugin->stem()), file);
                if (pluginObjectPath.empty()) return std::nullopt;
            }
        } else if (!pluginObjectPath.empty()) {
            // Engine plugins have no .uproject above them and stay unconvertible.
            if (auto project = soleFileWithExtension(folder, ".uproject")) return UnrealAssetLocation{*project, pluginObjectPath};
        }
    }
    return std::nullopt;
}
namespace {
// True when every asset listed by the extraction is still a regular file inside
// its own assets folder (no deletions, no links escaping the cache).
bool completeExtraction(const fs::path& root) {
    const auto assets = fs::canonical(root / "assets");
    const auto listing = json::parse(readText(root / ".animbridge-virtual-paths.json"));
    if (listing.value("portableNamePolicyVersion", 0u) != PortableNamePolicyVersion) return false;
    for (const auto& [key, spelling] : listing.at("paths").items()) {
        const auto file = fs::canonical(assets / fromUtf8(spelling.get<std::string>()));
        const auto relative = file.lexically_relative(assets);
        if (!fs::is_regular_file(file) || relative.empty() || *relative.begin() == "..") return false;
    }
    return true;
}
// Reopening a package from Recent must not extract it again. A fresh directory
// gives every clip a new path, which misses the path-keyed preview cache and
// makes Unity re-bake every clip. Reuse the newest finished extraction of the
// same package bytes and parser version instead.
std::optional<fs::path> reusableExtraction(const fs::path& owner, const std::string& sourceHash) {
    std::error_code error;
    if (sourceHash.empty() || !fs::is_directory(owner, error)) return std::nullopt;
    std::optional<fs::path> newest;
    fs::file_time_type newestTime{};
    for (const auto& entry : fs::directory_iterator(owner, error)) {
        const auto root = entry.path();
        if (!entry.is_directory(error) || !root.filename().wstring().starts_with(L"package-")) continue;
        try {
            const auto identityFile = root / ".animbridge-package.json";
            const auto identity = json::parse(readText(identityFile));
            if (identity.value("sourceHash", "") != sourceHash || identity.value("parserVersion", "") != ParserVersion ||
                identity.value("portableNamePolicyVersion", 0u) != PortableNamePolicyVersion) continue;
            if (!completeExtraction(root)) continue;
            const auto time = fs::last_write_time(identityFile);
            if (!newest || time > newestTime) { newest = root / "assets"; newestTime = time; }
        } catch (const std::exception&) {
            continue; // Damaged or foreign folder: a fresh extraction replaces it.
        }
    }
    return newest;
}
}
void Session::ingest(const std::vector<fs::path>& paths, const fs::path& cache, Limits limits, const RuntimeContext& context) {
    for (auto& input : paths) {
        try { utf8(input); }
        catch (const std::exception&) { warnings.push_back("INVALID_ENCODING: rejected input (diagnostic " + std::to_string(warnings.size() + 1) + ")"); continue; }
        try {
        auto path = fs::absolute(input);
        std::map<std::string, bool> selection; for (auto& clip : clips) selection[clip.id] = clip.selected;
        const bool packaged = lower(utf8(path.extension())) == ".unitypackage";
        if (packaged && fs::file_size(path) > limits.maxCompressedBytes) throw std::runtime_error("Compressed package size limit exceeded");
        PackageRecord record; record.sourcePath = path;
        if (fs::is_regular_file(path)) record.sourceHash = fileSha256(path);
        record.packageId = "p2_" + sourceIdentityDigest(path, projectNamespaceFor(path), "", record.sourceHash + "#" + ParserVersion);
        // Make each drop transactional so a corrupt package cannot leave a partial list.
        Session addition;
        PackageRollback rollback;
        bool reusedExtraction = false;
        if (packaged) {
            // Do not repeat the 64-character content hash and parser version in
            // every physical asset path. Ordinary commercial asset names then
            // exceed MAX_PATH on Windows systems with long paths disabled.
            // Each extraction owns an atomically allocated package directory;
            // retain the complete identity outside the asset tree.
            fs::path root;
            if (auto existing = reusableExtraction(cache / "packages", record.sourceHash)) {
                root = *existing;
                reusedExtraction = true; // Other sessions may use it, so a failure here must not delete it.
            } else {
                root = extractUnityPackage(path, cache / "packages", limits);
                rollback.root = root.parent_path();
                rollback.owner = cache / "packages";
                writeJson(root.parent_path() / ".animbridge-package.json", {{"sourcePath", utf8(path)}, {"sourceHash", record.sourceHash}, {"parserVersion", ParserVersion}, {"portableNamePolicyVersion", PortableNamePolicyVersion}, {"unicodeVersion", "16.0.0"}, {"normalizer", "utf8proc-2.10.0"}});
            }
            addition.extractedRoots.push_back(root);
            scanPath(addition, root, false);
            auto virtualPaths = json::parse(readText(root.parent_path() / ".animbridge-virtual-paths.json"));
            std::map<std::string, int> occurrences;
            for (auto& clip : addition.clips) {
                auto spelling = virtualPaths.at("paths").find(portableNameKey(clip.virtualPath));
                if (spelling == virtualPaths.at("paths").end()) throw std::runtime_error("Package virtual pathname provenance is missing");
                clip.virtualPath = spelling->get<std::string>();
                auto identity = clip.virtualPath + "#" + clip.name;
                auto index = occurrences[identity]++;
                clip.legacyId = legacyClipId(path, record.sourceHash + "#" + identity + (index ? "#occurrence=" + std::to_string(index) : ""));
                if (index) clip.identityDigest = "occurrence=" + std::to_string(index);
            }
        } else scanPath(addition, path);
        // Only a new extraction read the package bytes, so only it needs re-checking.
        if (packaged && !reusedExtraction && fileSha256(path) != record.sourceHash) throw std::runtime_error("Package bytes changed during extraction; analyse again");
        std::map<std::string, std::string> hashes;
        std::vector<Clip*> processed;
        for (auto& clip : addition.clips) {
            clip.packageId = record.packageId;
            if (packaged) { clip.packageSourcePath = path; clip.packageSourceHash = record.sourceHash; }
            const auto& identityPath = packaged ? path : clip.source;
            if (clip.projectNamespace.empty()) clip.projectNamespace = projectNamespaceFor(identityPath);
            clip.fileIdentity = fileIdentity(identityPath);
            auto token = clip.name + (clip.identityDigest.empty() ? "" : "#" + clip.identityDigest);
            if (clip.legacyId.empty()) clip.legacyId = legacyClipId(clip.source, token);
            clip.identityDigest = sourceIdentityDigest(identityPath, clip.projectNamespace, packaged ? clip.virtualPath : "", token);
            clip.id = "s2_" + clip.identityDigest;
            clip.sourceId = "source2_" + sourceIdentityDigest(identityPath, clip.projectNamespace, packaged ? clip.virtualPath : "", "source");
            // OS identities merge aliases only inside the same project. Preserve
            // the first verified spelling and ID instead of case-folding paths.
            auto isAlias = [&](const Clip& previous) {
                return previous.projectNamespace == clip.projectNamespace && previous.fileIdentity == clip.fileIdentity &&
                    (packaged ? previous.virtualPath == clip.virtualPath : previous.packageSourcePath.empty()) &&
                    previous.kind == clip.kind && ((!clip.namedClip && !previous.namedClip) || previous.name == clip.name) &&
                    (clip.engine != Engine::Unreal || json::parse(previous.metadataJson).value("id", "") == json::parse(clip.metadataJson).value("id", "")) &&
                    // Deliberately ambiguous duplicate metadata rows must remain
                    // separate so exact selection stays blocked.
                    previous.note.find("Ambiguous duplicate") == std::string::npos && clip.note.find("Ambiguous duplicate") == std::string::npos;
            };
            const Clip* firstAlias = nullptr;
            auto alias = std::find_if(clips.begin(), clips.end(), isAlias);
            if (alias != clips.end()) firstAlias = &*alias;
            else for (auto* previous : processed) if (isAlias(*previous)) { firstAlias = previous; break; }
            if (firstAlias) {
                clip.source = firstAlias->source; clip.virtualPath = firstAlias->virtualPath; clip.id = firstAlias->id;
                clip.identityDigest = firstAlias->identityDigest; clip.sourceId = firstAlias->sourceId;
                clip.legacyId = firstAlias->legacyId; clip.exportId = firstAlias->exportId; clip.exportIdentityVersion = firstAlias->exportIdentityVersion;
                clip.exportIdentityDigest = firstAlias->exportIdentityDigest;
            }
            if (clip.exportIdentityDigest.empty()) clip.exportIdentityDigest = clip.identityDigest;
            if (clip.exportId.empty()) {
#ifdef _WIN32
                clip.exportId = clip.legacyId; clip.exportIdentityVersion = 1;
#else
                clip.exportId = exportIdForDigest(clip.exportIdentityDigest); clip.exportIdentityVersion = 2;
#endif
            }
            processed.push_back(&clip);
            auto key = utf8(clip.source);
            try { if (!hashes.contains(key)) hashes[key] = fileSha256(clip.source); }
            catch (const std::exception& error) {
                clip.selectable = false; clip.extractable = false; clip.note = error.what();
                clip.capabilitiesJson = json{{"preview", false}, {"extract", false}, {"bake", false}}.dump();
                clip.capabilityReasonsJson = json{{"preview", clip.note}, {"extract", clip.note}, {"bake", clip.note}}.dump();
                continue;
            }
            clip.sourceHash = hashes[key];
            if (clip.kind == "anim" && clip.route == "A") {
                auto log = fs::absolute(cache) / "analysis" / ParserVersion / (clip.sourceHash + "-" + textSha256(utf8(clip.source.filename())).substr(0, 16) + ".json");
                try {
                    if (!fs::is_regular_file(log)) {
                        auto process = runProcess(runtimePaths().python, pythonArguments({"-m", "unityanimbridge.cli", "inspect", utf8(clip.source)}), runtimePaths().backend, log, 30,
                            pythonEnvironment(runtimePaths().backend, fs::absolute(cache) / "jobs", context));
                        if (process.exitCode || process.timedOut) throw std::runtime_error("Animation parser could not validate the clip");
                    }
                    auto info = json::parse(readText(log));
                    info["file"] = utf8(clip.source);
                    clip.metadataJson = info.dump(); clip.fps = info.value("sample_rate", 0.0); clip.fpsKnown = clip.fps > 0;
                    if (info.contains("start") && info["start"].is_number() && info.contains("stop") && info["stop"].is_number() && info["stop"].get<double>() >= info["start"].get<double>()) {
                        clip.duration = info["stop"].get<double>() - info["start"].get<double>(); clip.durationKnown = true;
                    }
                    if (info.value("route", "") == "B") {
                        clip.type = "humanoid"; clip.rigType = "humanoid"; clip.route = "requires-unity-bake"; clip.selectable = false;
                        clip.note = "Humanoid clip: connect a validated Avatar and isolated Unity Helper; original extraction is available";
                    } else { clip.type = "generic"; clip.rigType = "generic"; }
                } catch (const std::exception& error) {
                    clip.type.clear(); clip.rigType.clear(); clip.selectable = false;
                    clip.fps = 0; clip.duration = 0; clip.fpsKnown = false; clip.durationKnown = false;
                    clip.note = std::string("Metadata unknown: ") + error.what() + "; preserve the original or revalidate the parser";
                }
            }
            if (clip.kind == "fbx" && clip.type.empty()) clip.type = "fbx";
            if (clip.kind == "anim" || clip.kind == "baked" || clip.route == "requires-unity-bake") clip.dependenciesJson = json::array({{{"kind", "skeleton"}, {"required", true}, {"resolved", false}}}).dump();
            clip.capabilitiesJson = json{{"preview", false}, {"extract", true}, {"bake", clip.selectable ? json(nullptr) : json(false)}}.dump();
            clip.capabilityReasonsJson = json{{"preview", "Requires a separately validated preview renderer and matching model"}, {"extract", "Original file bytes can be preserved without evaluation"}, {"bake", clip.selectable ? "Requires selected skeleton/converter preflight" : clip.note}}.dump();
        }
        for (auto previous = packages.begin(); previous != packages.end();) {
            if (fs::weakly_canonical(previous->sourcePath) == fs::weakly_canonical(path) && (previous->sourceHash != record.sourceHash || fs::is_directory(path))) {
                auto previousId = previous->packageId;
                clips.erase(std::remove_if(clips.begin(), clips.end(), [&](auto& clip) { return clip.packageId == previousId; }), clips.end());
                previous = packages.erase(previous);
            } else ++previous;
        }
        if (std::none_of(packages.begin(), packages.end(), [&](auto& p) { return p.packageId == record.packageId; })) packages.push_back(record);
        for (auto& clip : addition.clips) { if (selection.contains(clip.id)) clip.selected = selection[clip.id]; addClip(*this, std::move(clip)); }
        // External short IDs have an explicit full-digest ledger. A truncated
        // collision never silently deduplicates distinct internal sources.
        std::map<std::string, std::string> exportLedger;
        std::map<std::string, std::set<std::string>> legacyMappings;
        for (auto& clip : clips) legacyMappings[clip.legacyId].insert(clip.id);
        for (auto& clip : clips) {
            auto [entry, inserted] = exportLedger.emplace(clip.exportId, clip.exportIdentityDigest);
            if ((!inserted && entry->second != clip.exportIdentityDigest) || (clip.exportIdentityVersion == 1 && legacyMappings[clip.legacyId].size() > 1)) {
                for (auto& collision : clips) if (collision.exportId == clip.exportId) {
                    collision.selected = false; collision.selectable = false;
                    collision.note = "EXPORT_ID_COLLISION: multiple identities share an external export ID; confirm new v2 identities";
                    collision.exportIdentityDigest = collision.identityDigest; collision.exportId = exportIdForDigest(collision.exportIdentityDigest); collision.exportIdentityVersion = 2;
                }
                warnings.push_back("EXPORT_ID_COLLISION: automatic legacy correspondence was rejected");
            }
        }
        warnings.insert(warnings.end(), addition.warnings.begin(), addition.warnings.end());
        extractedRoots.insert(extractedRoots.end(), addition.extractedRoots.begin(), addition.extractedRoots.end());
        rollback.committed = true;
        } catch (const std::exception& error) {
            if (paths.size() == 1) throw;
            std::string detail(error.what());
            if (!validUtf8(detail)) detail = "Input validation failed (unrepresentable diagnostic omitted)";
            warnings.push_back("INPUT_REJECTED: " + detail);
        }
    }
}
void Session::selectAll(bool value, ExportMode mode, bool allowUnityHelper) { for (auto& clip : clips) clip.selected = value && selectableFor(clip, mode, allowUnityHelper); }
bool Session::select(const std::string& id, bool value, ExportMode mode, bool allowUnityHelper) {
    for (auto& clip : clips) if (clip.id == id) { if (value && !selectableFor(clip, mode, allowUnityHelper)) return false; clip.selected = value; return true; }
    return false;
}
Plan makePlan(const Session& session, const Options& options) {
    if (options.output.empty()) throw std::runtime_error("Choose an output directory");
    if (!options.timeoutSeconds || !options.jobTimeoutSeconds) throw std::runtime_error("Timeout must be positive");
    if (!options.targetProfile.empty() && options.targetProfile != "unreal" && options.targetProfile != "unity" && options.targetProfile != "original") throw std::runtime_error("Unknown target profile");
    auto expectedProfile = options.direction == Direction::UnityToUnreal ? "unreal" : "unity";
    if (options.mode == ExportMode::Bake && !options.targetProfile.empty() && options.targetProfile != expectedProfile) throw std::runtime_error("Target profile does not match the transfer direction");
    if (options.collisionPolicy == CollisionPolicy::Overwrite && !options.explicitOverwrite) throw std::runtime_error("Overwrite requires explicit confirmation");
    if (options.sampleRate && (!std::isfinite(*options.sampleRate) || *options.sampleRate < 1 || *options.sampleRate > 1000)) throw std::runtime_error("Sample rate must be 1..1000");
    if (options.mode == ExportMode::ExtractOriginal && (options.sampleRate || options.rootMotion != "preserve")) throw std::runtime_error("Original extraction does not apply FPS or root-motion changes");
    if (options.mode == ExportMode::ExtractOriginal && !options.targetEditor.empty()) throw std::runtime_error("Original extraction cannot mutate a target engine project");
    if (options.rootMotion != "preserve") throw std::runtime_error("This root-motion policy has not been implemented; preserve the source or choose a supported adapter option");
    if (options.mode == ExportMode::Bake && options.direction == Direction::UnrealToUnity && options.sampleRate) throw std::runtime_error("Unreal FBX staging preserves source timing; FPS resampling must run in an evaluation adapter");
    if (options.targetEditor.empty() != options.targetProject.empty()) throw std::runtime_error("Engine import requires both target editor and target project");
    if (!options.targetEditor.empty()) {
        if (!fs::is_regular_file(options.targetEditor)) throw std::runtime_error("Target editor executable not found");
        if (options.direction == Direction::UnityToUnreal) {
            if (!fs::is_regular_file(options.targetProject) || lower(utf8(options.targetProject.extension())) != ".uproject") throw std::runtime_error("Choose an Unreal .uproject");
            if (!options.targetSkeleton.starts_with("/Game/")) throw std::runtime_error("Unreal import requires an existing /Game/... Skeleton object path");
        } else if (!fs::is_directory(options.targetProject / "Assets")) throw std::runtime_error("Unity target project must contain Assets");
        if (!options.targetFolder.empty()) {
            requireUtf8(options.targetFolder);
            auto folder = options.targetFolder;
            if (folder.find('\\') != std::string::npos || folder.find(':') != std::string::npos || folder.find_first_of("\r\n") != std::string::npos) throw std::runtime_error("Invalid target asset folder");
            auto required = options.direction == Direction::UnityToUnreal ? "/Game/" : "Assets/";
            auto rootName = options.direction == Direction::UnityToUnreal ? "/Game" : "Assets";
            if (folder != rootName && !folder.starts_with(required)) throw std::runtime_error("Target asset folder is outside the approved engine content root");
            auto relative = folder == rootName ? fs::path{} : fromUtf8(folder.substr(std::char_traits<char>::length(required)));
            for (const auto& component : relative) if (component == ".." || component == "." || component.empty()) throw std::runtime_error("Target asset folder contains unsafe components");
        }
    }
    Plan plan{options, {}}; plan.options.output = fs::absolute(options.output);
    if (plan.options.journalRoot.empty()) plan.options.journalRoot = plan.options.runtimeContext.stateRoot / "attempts";
    OutputCommitLock probeLock(plan.options.output);
    auto capabilities = FileCommitter::probe(plan.options.output);
    plan.outputCapabilitiesJson = capabilities.json();
    plan.recoveryEntriesJson = FileCommitter::recoveryEntries(plan.options.journalRoot, plan.options.output);
    json exportLedger = json::object();
    auto ledgerPath = plan.options.output / ".animbridge-export-ledger.json";
    if (fs::exists(ledgerPath)) {
        auto ledger = json::parse(readText(ledgerPath));
        if (ledger.value("program", "") != "AnimBridge" || ledger.value("schemaVersion", 0) != 1 || !ledger.contains("identities") || !ledger["identities"].is_object()) throw std::runtime_error("OUTPUT_CONFLICT: existing export ledger is foreign or uses an unsupported schema");
        exportLedger = ledger["identities"];
    }
    if (plan.options.targetProfile.empty()) plan.options.targetProfile = options.mode == ExportMode::ExtractOriginal ? "original" : expectedProfile;
    std::set<std::string> names; std::map<std::string, std::size_t> originalSources;
    for (auto& clip : session.clips) if (clip.selected) {
        if (options.mode == ExportMode::ExtractOriginal && originalSources.contains(clip.sourceId.empty() ? utf8(clip.source) : clip.sourceId)) {
            plan.jobs[originalSources[clip.sourceId.empty() ? utf8(clip.source) : clip.sourceId]].clipIds.push_back(clip.id); continue;
        }
        Job job; job.clip = clip; job.clipIds = {clip.id}; job.sourceHash = fileSha256(clip.source);
        if (job.clip.identityDigest.empty()) job.clip.identityDigest = sourceIdentityDigest(clip.source, projectNamespaceFor(clip.source), "", clip.name);
        if (job.clip.exportIdentityDigest.empty()) job.clip.exportIdentityDigest = job.clip.identityDigest;
        if (job.clip.exportId.empty()) { job.clip.exportId = exportIdForDigest(job.clip.exportIdentityDigest); job.clip.exportIdentityVersion = 2; }
        if (options.mode == ExportMode::Bake) {
            auto meta = fromUtf8(utf8(clip.source) + ".meta");
            if (linked(meta)) throw std::runtime_error("Linked source metadata is not permitted");
            if (fs::is_regular_file(meta)) { requireCompanionWithin(clip.source.parent_path(), meta); job.metadataHash = fileSha256(meta); }
            if (!options.skeleton.empty() && fs::is_regular_file(options.skeleton)) job.skeletonHash = fileSha256(options.skeleton);
            auto skeletonMeta = fromUtf8(utf8(options.skeleton) + ".meta");
            if (!options.skeleton.empty() && linked(skeletonMeta)) throw std::runtime_error("Linked skeleton metadata is not permitted");
            if (!options.skeleton.empty() && fs::is_regular_file(skeletonMeta)) { requireCompanionWithin(options.skeleton.parent_path(), skeletonMeta); job.skeletonMetadataHash = fileSha256(skeletonMeta); }
        }
        auto base = safeName(options.mode == ExportMode::ExtractOriginal ? utf8(clip.source.filename()) : clip.name); auto name = base;
        for (int suffix = 2; !names.insert(portableNameKey(name)).second; ++suffix) name = base + "_" + std::to_string(suffix);
        job.output = plan.options.output / fromUtf8(name);
        if (!clip.sourceHash.empty() && job.sourceHash != clip.sourceHash) { job.blockedReason = "Source bytes changed since analysis; re-open and analyse this input before export"; job.blockedCode = "SOURCE_CHANGED"; }
        else if (options.mode == ExportMode::ExtractOriginal) {
            job.action = "extract-original";
            originalSources[clip.sourceId.empty() ? utf8(clip.source) : clip.sourceId] = plan.jobs.size();
            if (sameFile(job.output, clip.source) || fs::weakly_canonical(job.output) == fs::weakly_canonical(clip.source)) { job.blockedReason = "Output would replace the original input"; job.blockedCode = "OUTPUT_CONFLICT"; }
        } else if ((clip.kind == "anim" || (clip.kind == "fbx" && clip.selectable)) && clip.route == "requires-unity-bake" && options.direction == Direction::UnityToUnreal) {
            job.action = "unity-helper-bake";
            if (options.unityEditor.empty() || !fs::is_regular_file(options.unityEditor)) { job.blockedReason = "Connect a valid Unity Editor executable for isolated Helper baking"; job.blockedCode = "HELPER_UNAVAILABLE"; }
            else if (options.skeleton.empty() || !fs::is_regular_file(options.skeleton) || lower(utf8(options.skeleton.extension())) != ".fbx") { job.blockedReason = "Connect the character/model FBX required by the Unity Helper"; job.blockedCode = "DEPENDENCY_MISSING"; }
            else {
                auto human = json::parse(options.humanMappingJson); auto bone = json::parse(options.boneMapJson);
                if (!human.is_object() || !bone.is_object()) throw std::runtime_error("Bone mappings must be JSON objects");
            }
        } else if (!clip.selectable) { job.blockedReason = clip.note; job.blockedCode = "UNSUPPORTED_DATA"; }
        else if (contains(job.output, clip.source)) { job.blockedReason = "Output directory contains the source asset"; job.blockedCode = "OUTPUT_CONFLICT"; }
        else if (options.direction == Direction::UnityToUnreal) {
            job.action = "python-convert";
            if (clip.engine != Engine::Unity) { job.blockedReason = "Unity to Unreal requires Unity clips"; job.blockedCode = "UNSUPPORTED_DATA"; }
            else if ((clip.kind == "anim" || clip.kind == "baked") && (!fs::is_regular_file(options.skeleton) || lower(utf8(options.skeleton.extension())) != ".fbx")) { job.blockedReason = "Choose a skeleton FBX for .anim/baked clips"; job.blockedCode = "DEPENDENCY_MISSING"; }
            job.arguments = {"-m", "unityanimbridge.cli", "convert"};
            if (clip.kind == "baked") job.arguments.push_back("--baked");
            job.arguments.push_back(utf8(clip.source));
            if (clip.namedClip) job.arguments.push_back("--clip=" + clip.name);
            job.arguments.insert(job.arguments.end(), {"-o", utf8(job.output)});
            if (!options.skeleton.empty()) job.arguments.insert(job.arguments.end(), {"--skeleton", utf8(fs::absolute(options.skeleton))});
            if (!options.blender.empty()) job.arguments.insert(job.arguments.end(), {"--blender", utf8(options.blender)});
            job.arguments.insert(job.arguments.end(), {"--timeout", std::to_string(options.timeoutSeconds)});
            if (options.sampleRate) job.arguments.insert(job.arguments.end(), {"--fps", std::to_string(*options.sampleRate)});
        } else {
            job.action = "stage-unity-fbx";
            if (clip.engine != Engine::Unreal || clip.kind != "fbx") { job.blockedReason = "Unreal to Unity requires animbridge-manifest.json from Unreal engine export"; job.blockedCode = "UNSUPPORTED_DATA"; }
        }
        if (options.mode == ExportMode::Bake && options.collisionPolicy == CollisionPolicy::Overwrite && fs::exists(job.output)) { job.blockedReason = "Existing bake directories cannot be overwritten safely; choose rename or skip"; job.blockedCode = "OUTPUT_CONFLICT"; }
        if (!capabilities.localVolume || !capabilities.flush || (options.mode == ExportMode::ExtractOriginal ? !capabilities.fileNoReplace : !capabilities.directoryNoReplace)) { job.blockedReason = "Output volume lacks verified atomic publication capabilities: " + capabilities.limitation; job.blockedCode = "OUTPUT_WRITE_FAILED"; }
#ifndef _WIN32
        if (options.collisionPolicy == CollisionPolicy::Overwrite && (!capabilities.hardLink || !capabilities.exchange)) { job.blockedReason = "Explicit overwrite requires same-volume hard links and atomic exchange"; job.blockedCode = "OUTPUT_WRITE_FAILED"; }
#endif
        if (!clip.packageSourcePath.empty() && (sameFile(job.output, clip.packageSourcePath) || contains(job.output, clip.packageSourcePath))) { job.blockedReason = "Output would replace or contain the original package"; job.blockedCode = "OUTPUT_CONFLICT"; }
        for (auto& original : session.clips) if (sameFile(job.output, original.source) || (options.mode == ExportMode::Bake && contains(job.output, original.source))) { job.blockedReason = "Output would replace an original input or contain source assets"; job.blockedCode = "OUTPUT_CONFLICT"; }
        if (!options.skeleton.empty() && (sameFile(job.output, options.skeleton) || (options.mode == ExportMode::Bake && contains(job.output, options.skeleton)))) { job.blockedReason = "Output would replace the connected skeleton"; job.blockedCode = "OUTPUT_CONFLICT"; }
        if (options.mode == ExportMode::Bake && options.direction == Direction::UnityToUnreal && !options.targetEditor.empty()) {
            auto destination = (options.targetFolder.empty() ? "/Game/AnimBridge" : options.targetFolder) + "/" + safeName(utf8(job.output.filename())) + "_" + job.clip.exportId;
            auto folder = options.targetFolder.empty() ? "/Game/AnimBridge" : options.targetFolder;
            auto relativeFolder = folder.starts_with("/Game/") ? folder.substr(6) : folder;
            auto physical = options.targetProject.parent_path() / "Content" / fromUtf8(relativeFolder) / fromUtf8(safeName(utf8(job.output.filename())) + "_" + job.clip.exportId) / fromUtf8(safeName(clip.name) + ".uasset");
            // UE's asset limit depends on editor long-path settings. Until a
            // native engine probe supplies that setting, use the conservative
            // 260-character default (UTF-8 bytes overcount non-ASCII safely).
            if (destination.size() + 2 + safeName(clip.name).size() * 2 >= 1024 || utf8(fs::absolute(physical)).size() >= 260) { job.blockedReason = "Unreal target exceeds the conservative asset path limit; choose a shorter target folder or project path"; job.blockedCode = "OUTPUT_CONFLICT"; }
        }
        for (auto& previous : plan.jobs) if (previous.clip.exportId == job.clip.exportId && previous.clip.exportIdentityDigest != job.clip.exportIdentityDigest) throw std::runtime_error("EXPORT_ID_COLLISION: one external ID maps to multiple full SHA-256 identities");
        if (job.clip.exportIdentityVersion == 2 && job.clip.exportId != exportIdForDigest(job.clip.exportIdentityDigest)) throw std::runtime_error("EXPORT_ID_COLLISION: external v2 ID does not match its full identity digest");
        if (exportLedger.contains(job.clip.exportId) && exportLedger[job.clip.exportId].value("digest", "") != job.clip.exportIdentityDigest) throw std::runtime_error("EXPORT_ID_COLLISION: external export ID conflicts with the recorded full identity digest");
        plan.jobs.push_back(std::move(job));
    }
    if (plan.jobs.empty()) throw std::runtime_error("Select at least one animation");
    plan.jobId = newIdentity(); plan.attemptId = newIdentity(); plan.createdAt = timestampUtc();
    plan.planSnapshot = planJson(plan);
    return plan;
}
std::vector<Result> executeLegacy(const Plan& plan, Progress progress, std::shared_ptr<std::atomic_bool> cancel, std::int64_t absoluteDeadlineMilliseconds) {
    fs::create_directories(plan.options.output);
    // Refuse existing job outputs to avoid replacing previous work or stale results.
    for (auto& job : plan.jobs) if (job.blockedReason.empty() && fs::exists(job.output)) throw std::runtime_error("Output already exists; choose a fresh output folder: " + utf8(job.output));
    for (auto filename : {"animbridge-results.json", "animbridge-manifest.json"}) if (fs::exists(plan.options.output / filename)) throw std::runtime_error("Output report already exists; choose a fresh output folder");
    std::vector<Result> results; json exported = json::array();
    for (auto& job : plan.jobs) {
        Result result; result.id = job.clip.id; result.name = job.clip.name; result.output = job.output;
        fs::path helperCache;
        json adapterChecks = json::object();
        bool retainProcessWork = false;
        result.status = "error";
        try {
            if (!job.blockedReason.empty()) { result.status = "blocked"; result.exitCode = 2; result.message = job.blockedReason; }
            else {
                fs::create_directories(job.output);
                if (job.action == "python-convert" || job.action == "unity-helper-bake") {
                    auto arguments = job.arguments;
                    bool helper = job.action == "unity-helper-bake";
                    auto response = job.output.parent_path() / "helper-response.json";
                    result.log = helper ? job.output.parent_path() / "animbridge-helper.log" : job.output / "animbridge-convert.log";
                    if (helper) {
                        helperCache = fs::absolute(plan.options.workRoot) / "helper" / newIdentity();
                        auto request = job.output.parent_path() / "helper-request.json";
                        json payload{{"source", utf8(job.clip.source)}, {"skeleton", utf8(fs::absolute(plan.options.skeleton))}, {"unity_editor", utf8(fs::absolute(plan.options.unityEditor))}, {"cache", utf8(helperCache)}, {"output", utf8(job.output)}, {"humanoid", job.clip.rigType == "humanoid"}, {"human_mapping", json::parse(plan.options.humanMappingJson)}, {"bone_map", json::parse(plan.options.boneMapJson)}};
                        if (job.clip.namedClip) payload["clip"] = job.clip.name;
                        if (plan.options.sampleRate) payload["fps"] = *plan.options.sampleRate;
                        if (!plan.options.blender.empty()) payload["blender"] = utf8(plan.options.blender);
                        writeJson(request, payload); arguments = {"-m", "unityanimbridge.desktop", "bake", "--request", utf8(request), "--out", utf8(response)};
                    } else {
                        auto mapping = json::parse(plan.options.boneMapJson);
                        if (!mapping.is_object()) throw std::runtime_error("Bone mapping must be a JSON object");
                        if (!mapping.empty()) {
                            for (auto& [path, bone] : mapping.items()) { (void)path; if (!bone.is_string() || bone.get<std::string>().empty()) throw std::runtime_error("Bone mapping values must be nonempty bone names"); }
                            auto mappingFile = job.output.parent_path() / "bone-map.json"; writeJson(mappingFile, mapping);
                            arguments.insert(arguments.end(), {"--bone-map", utf8(mappingFile)});
                        }
                    }
                    auto process = runSupervisedProcess(plan.options.python, pythonArguments(arguments, plan.options.repository), plan.options.repository, result.log, plan.options.timeoutSeconds,
                        pythonEnvironment(plan.options.repository, job.output.parent_path() / ".jobs", plan.options.runtimeContext), cancel, processPolicy(plan.options, absoluteDeadlineMilliseconds, true));
                    result.exitCode = process.exitCode;
                    retainProcessWork = !process.cleanupComplete;
                    adapterChecks["processCleanup"] = {{"status", process.cleanupComplete ? "passed" : "retained"}, {"ownedCleanupComplete", process.ownedCleanupComplete}, {"timeoutLayer", process.timeoutLayer}, {"stage", process.stage}};
                    result.warnings.insert(result.warnings.end(), process.cleanupWarnings.begin(), process.cleanupWarnings.end());
                    json helperResponse;
                    if (helper && fs::is_regular_file(response)) helperResponse = json::parse(readText(response));
                    if (!process.ownedCleanupComplete) { adapterChecks["processCleanup"]["status"] = "incomplete"; result.message = "Owned process cleanup remains unverified; retain private output and logs before retrying"; result.errorCode = "PROCESS_CLEANUP_INCOMPLETE"; }
                    else if (process.cancelled) { result.status = "cancelled"; result.message = "Converter cancelled at the process boundary"; }
                    else if (process.timedOut) { result.message = "Converter timed out; owned subprocess cleanup completed"; result.errorCode = "PROCESS_TIMEOUT"; }
                    else if (helper && helperResponse.is_object() && helperResponse.value("status", "error") == "blocked") {
                        result.status = "blocked"; result.message = helperResponse.value("reason", "Unity Helper requirements are not satisfied");
                        auto code = helperResponse.value("code", ""); result.errorCode = code == "needsAvatar" ? "DEPENDENCY_MISSING" : code == "helperUnavailable" ? "HELPER_UNAVAILABLE" : code == "evaluationFailed" ? "EVALUATION_FAILED" : code == "outputConflict" ? "OUTPUT_CONFLICT" : code == "unsupported" ? "UNSUPPORTED_DATA" : "UNKNOWN_ERROR";
                    }
                    else if (process.exitCode) { result.message = helper && helperResponse.is_object() ? helperResponse.value("reason", "Unity Helper failed; see log") : "Converter failed; see log"; result.errorCode = "CONVERSION_FAILED"; }
                    else {
                        if (helper) {
                            auto information = json::parse(readText(response));
                            if (information.value("status", "error") != "ok") throw std::runtime_error(information.value("reason", "Unity Helper did not validate the requested clip"));
                            auto evidence = job.output / ".helper"; fs::create_directory(evidence);
                            fs::copy_file(response, evidence / "response.json");
                            auto baked = information.value("baked", ""); if (!baked.empty() && fs::is_regular_file(fromUtf8(baked))) fs::copy_file(fromUtf8(baked), evidence / "baked.json");
                            fs::copy_file(result.log, job.output / "animbridge-convert.log");
                            result.log = job.output / "animbridge-convert.log";
                        }
                        auto manifestPath = job.output / "manifest.json";
                        auto manifest = json::parse(readText(manifestPath));
                        if (!manifest.contains("clips") || manifest["clips"].empty()) throw std::runtime_error("Converter returned no clips");
                        bool warning = false;
                        for (auto& clip : manifest["clips"]) {
                            auto state = clip.value("status", "error");
                            if (state != "ok" && state != "warning") throw std::runtime_error("Converter reported a failed clip");
                            if (!fs::is_regular_file(fromUtf8(clip.at("fbx").get<std::string>()))) throw std::runtime_error("Converter FBX output missing");
                            warning = warning || state == "warning";
                        }
                        result.status = warning ? "warning" : "ok";
                        result.message = "FBX exported; import the converter manifest with unreal/uab_import.py in Unreal Editor";
                        if (!plan.options.targetEditor.empty()) importUnreal(plan, job, result, absoluteDeadlineMilliseconds, cancel);
                    }
                } else if (job.action == "stage-unity-fbx") {
                    auto destination = job.output / fromUtf8(safeName(job.clip.name) + ".fbx");
                    fs::copy_file(job.clip.source, destination, fs::copy_options::none);
                    auto metadata = json::parse(job.clip.metadataJson);
                    metadata.update({{"id", job.clip.id}, {"exportId", job.clip.exportId}, {"exportIdentityVersion", job.clip.exportIdentityVersion}, {"exportIdentityDigest", job.clip.exportIdentityDigest}, {"identityDigest", job.clip.identityDigest}, {"name", job.clip.name}, {"fbx", utf8(destination)},
                                     {"source_asset", job.clip.virtualPath}, {"fps", job.clip.fps}, {"duration", job.clip.duration}});
                    exported.push_back(std::move(metadata));
                    result.status = "ready-for-unity-import";
                    result.message = "Exported FBX staged; run AnimBridgeImporter.ImportFromCommandLine in Unity Editor";
                } else throw std::runtime_error("Unknown job action");
            }
        } catch (const std::exception& e) { result.status = "error"; result.exitCode = 2; result.message = e.what(); if (result.errorCode.empty()) result.errorCode = "UNKNOWN_ERROR"; }
        if (!helperCache.empty() && !retainProcessWork && result.status != "error" && result.status != "blocked") {
            std::error_code error; removeOwnedTree(helperCache, plan.options.workRoot, error);
            if (error) {
                result.warnings.push_back("Temporary Unity Helper cache cleanup failed; retained at " + utf8(helperCache) + ": " + error.message());
                adapterChecks["helperCacheCleanup"] = {{"status", "failed"}, {"remainingPath", utf8(helperCache)}, {"error", error.message()}};
                if (result.status == "ok") result.status = "warning";
            } else adapterChecks["helperCacheCleanup"] = {{"status", "passed"}};
        } else if (!helperCache.empty() && fs::exists(helperCache)) {
            adapterChecks["helperCacheCleanup"] = {{"status", retainProcessWork ? "retainedForProcessHandles" : "retainedForFailure"}, {"remainingPath", utf8(helperCache)}};
        }
        result.checksJson = adapterChecks.dump();
        if (result.status == "ok" && !result.warnings.empty()) result.status = "warning";
        results.push_back(result); if (progress) progress(result);
    }
    if (!exported.empty()) {
        auto manifest = plan.options.output / "animbridge-manifest.json";
        writeJson(manifest, {{"animbridge_version", 1}, {"source_engine", "unreal"}, {"clips", exported}});
        if (!plan.options.targetEditor.empty()) {
            try {
                auto report = importUnity(plan, manifest, exported.size(), absoluteDeadlineMilliseconds, cancel);
                bool hasFailure = std::any_of(report["clips"].begin(), report["clips"].end(), [](auto& entry) { return entry.value("status", "error") == "error"; });
                for (auto& result : results) if (result.status == "ready-for-unity-import") {
                    auto entry = std::find_if(report["clips"].begin(), report["clips"].end(), [&](auto& value) { return value.value("id", "") == result.id; });
                    result.log = plan.options.output / "animbridge-unity-editor.log";
                    if (entry != report["clips"].end() && (entry->value("status", "error") == "ok" || entry->value("status", "error") == "warning") && (!report.value("process_exit_code", 0) || hasFailure)) {
                        result.status = "imported"; result.message = "Unity import verified by engine report";
                    } else {
                        result.status = "error"; result.exitCode = 2;
                        result.message = entry != report["clips"].end() ? entry->value("message", "Unity import failed; see engine report") : "Unity report is missing this clip identity";
                    }
                    if (progress) progress(result);
                }
            } catch (const std::exception& e) {
                for (auto& result : results) if (result.status == "ready-for-unity-import") { result.status = "error"; result.exitCode = 2; result.message = e.what(); }
            }
        }
    }
    writeJson(plan.options.output / "animbridge-results.json", json::parse(resultsJson(results)));
    return results;
}
void applyEngineImports(const Plan& plan, std::vector<Result>& results, Progress progress, std::shared_ptr<std::atomic_bool> cancel) {
    auto remainingDeadline = [&](const Result& result) {
        auto checks = json::parse(result.checksJson);
        auto deadline = checks.value("absoluteDeadlineMilliseconds", std::int64_t(0));
        if (deadline > 0 && deadline <= std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()) throw std::runtime_error("PROCESS_TIMEOUT: item deadline elapsed before engine import");
        if (cancel && cancel->load()) throw std::runtime_error("CANCELLED: engine import was cancelled before launch");
        return deadline;
    };
    auto importError = [&](Result& result, const std::exception& error) {
        result.status = cancel && cancel->load() ? "cancelled" : "error"; result.exitCode = 2; result.message = error.what();
        result.errorCode = result.message.starts_with("PROCESS_TIMEOUT:") ? "PROCESS_TIMEOUT" : result.message.starts_with("PROCESS_CLEANUP_INCOMPLETE:") ? "PROCESS_CLEANUP_INCOMPLETE" : "ENGINE_IMPORT_FAILED";
        auto checks = json::parse(result.checksJson); checks[result.step == "engineImport" ? "engineImport" : "preImportHash"] = "failed"; result.checksJson = checks.dump();
    };
    auto verifyHashes = [&](Result& result) {
        auto checks = json::parse(result.checksJson);
        if (checks.contains("outputHashes")) for (auto& [path, hash] : checks["outputHashes"].items())
            if (fileSha256(fromUtf8(path)) != hash.get<std::string>()) throw std::runtime_error("Output bytes changed before/during engine import; re-export and verify");
    };
    if (plan.options.direction == Direction::UnityToUnreal) {
        for (auto& result : results) if (result.status == "ok" || result.status == "warning") {
            try {
                auto deadline = remainingDeadline(result);
                verifyHashes(result);
                auto original = std::find_if(plan.jobs.begin(), plan.jobs.end(), [&](auto& job) { return job.clip.id == result.id; });
                Job job = *original; job.output = result.output; result.step = "engineImport"; if (progress) progress(result);
                importUnreal(plan, job, result, deadline, cancel); verifyHashes(result);
                auto checks = json::parse(result.checksJson); checks["engineImport"] = "passed"; result.checksJson = checks.dump();
            } catch (const std::exception& error) { importError(result, error); }
            result.step = result.status == "imported" ? "completed" : "error"; result.finishedAt = timestampUtc(); if (progress) progress(result);
        }
    } else {
        std::vector<Result*> pending;
        std::int64_t batchDeadline = 0;
        for (auto& result : results) if (result.status == "ready-for-unity-import") {
            try { auto deadline = remainingDeadline(result); verifyHashes(result); if (deadline > 0 && (!batchDeadline || deadline < batchDeadline)) batchDeadline = deadline; pending.push_back(&result); }
            catch (const std::exception& error) { importError(result, error); }
        }
        if (pending.empty()) return;
        try {
            for (auto* result : pending) { result->step = "engineImport"; if (progress) progress(*result); }
            json verifiedClips = json::array();
            for (auto* result : pending) {
                auto job = std::find_if(plan.jobs.begin(), plan.jobs.end(), [&](auto& item) { return item.clip.id == result->id; });
                auto metadata = json::parse(job->clip.metadataJson); metadata.update({{"id", result->id}, {"exportId", job->clip.exportId}, {"exportIdentityVersion", job->clip.exportIdentityVersion}, {"exportIdentityDigest", job->clip.exportIdentityDigest}, {"identityDigest", job->clip.identityDigest}, {"name", result->name}, {"fbx", utf8(result->outputFiles.front())}, {"source_asset", job->clip.virtualPath}, {"fps", job->clip.fps}, {"duration", job->clip.duration}});
                verifiedClips.push_back(metadata);
            }
            json sourceManifest{{"animbridge_version", 1}, {"source_engine", "unreal"}, {"clips", verifiedClips}};
            auto verifiedManifest = plan.options.output / ("animbridge-unity-import-" + plan.attemptId + ".json"); writeJson(verifiedManifest, sourceManifest);
            auto report = importUnity(plan, verifiedManifest, pending.size(), batchDeadline, cancel);
            bool hasFailure = std::any_of(report["clips"].begin(), report["clips"].end(), [](auto& entry) { return entry.value("status", "error") == "error"; });
            for (auto* result : pending) {
                if (report.contains("processCleanup")) {
                    auto checks = json::parse(result->checksJson); checks["engineProcessCleanup"] = report["processCleanup"]; result->checksJson = checks.dump();
                    auto warnings = report["processCleanup"].value("warnings", std::vector<std::string>{}); result->warnings.insert(result->warnings.end(), warnings.begin(), warnings.end());
                }
                auto entry = std::find_if(report["clips"].begin(), report["clips"].end(), [&](auto& item) { return item.value("id", "") == result->id; });
                result->log = plan.options.output / "animbridge-unity-editor.log";
                if (entry != report["clips"].end() && (entry->value("status", "error") == "ok" || entry->value("status", "error") == "warning") && (!report.value("process_exit_code", 0) || hasFailure)) {
                    verifyHashes(*result);
                    result->status = "imported"; result->message = "Unity import verified by engine report";
                    auto checks = json::parse(result->checksJson); checks["engineImport"] = "passed"; result->checksJson = checks.dump();
                } else { result->status = "error"; result->exitCode = 2; result->errorCode = "ENGINE_IMPORT_FAILED"; result->message = entry != report["clips"].end() ? entry->value("message", "Unity import failed") : "Unity report is missing this clip identity"; auto checks = json::parse(result->checksJson); checks["engineImport"] = "failed"; result->checksJson = checks.dump(); }
                result->step = result->status == "imported" ? "completed" : "error"; result->finishedAt = timestampUtc(); if (progress) progress(*result);
            }
        } catch (const std::exception& error) { for (auto* result : pending) { if (result->status == "imported") continue; importError(*result, error); } }
    }
}
std::string sessionJson(const Session& session) {
    json clips = json::array(); for (auto& clip : session.clips) clips.push_back(clipJson(clip));
    json packages = json::array(); for (auto& package : session.packages) packages.push_back({{"packageId", package.packageId}, {"sourceHash", package.sourceHash.empty() ? json(nullptr) : json(package.sourceHash)}, {"sourcePath", utf8(package.sourcePath)}, {"parserVersion", package.parserVersion}});
    return json{{"program", "AnimBridge"}, {"parserVersion", ParserVersion}, {"packages", packages}, {"clips", clips}, {"warnings", session.warnings}}.dump(2);
}
std::string planJson(const Plan& plan) {
    json jobs = json::array(); for (auto& job : plan.jobs) jobs.push_back({{"clip", clipJson(job.clip)}, {"clipIds", job.clipIds}, {"sourceHash", job.sourceHash}, {"metadataHash", job.metadataHash}, {"skeletonHash", job.skeletonHash}, {"skeletonMetadataHash", job.skeletonMetadataHash}, {"action", job.action}, {"output", utf8(job.output)}, {"arguments", job.arguments}, {"blocked_reason", job.blockedReason}, {"blockedCode", job.blockedCode}, {"recovery", job.recovery}});
    auto collision = plan.options.collisionPolicy == CollisionPolicy::Skip ? "skip" : plan.options.collisionPolicy == CollisionPolicy::Rename ? "rename" : plan.options.collisionPolicy == CollisionPolicy::Overwrite ? "overwrite" : "fail";
    json options{{"outputRoot", utf8(plan.options.output)}, {"skeleton", utf8(plan.options.skeleton)}, {"python", utf8(plan.options.python)}, {"blender", utf8(plan.options.blender)}, {"repository", utf8(plan.options.repository)}, {"targetEditor", utf8(plan.options.targetEditor)}, {"targetProject", utf8(plan.options.targetProject)}, {"targetSkeleton", plan.options.targetSkeleton}, {"targetFolder", plan.options.targetFolder}, {"timeoutSeconds", plan.options.timeoutSeconds}, {"collisionPolicy", collision}, {"explicitOverwrite", plan.options.explicitOverwrite}, {"sampleRate", plan.options.sampleRate ? json(*plan.options.sampleRate) : json(nullptr)}, {"rootMotion", plan.options.rootMotion}, {"targetProfile", plan.options.targetProfile}};
    options["workRoot"] = utf8(plan.options.workRoot); options["journalRoot"] = utf8(plan.options.journalRoot); options["jobTimeoutSeconds"] = plan.options.jobTimeoutSeconds;
    const auto& context = plan.options.runtimeContext;
    options["runtimeContext"] = {{"dataRoot", utf8(context.dataRoot)}, {"cacheRoot", utf8(context.cacheRoot)}, {"stateRoot", utf8(context.stateRoot)}, {"logRoot", utf8(context.logRoot)}, {"exportRoot", utf8(context.exportRoot)}, {"tempRoot", utf8(context.tempRoot)}};
    options["unityEditor"] = utf8(plan.options.unityEditor); options["humanMapping"] = json::parse(plan.options.humanMappingJson); options["boneMap"] = json::parse(plan.options.boneMapJson);
    return json{{"program", "AnimBridge"}, {"jobId", plan.jobId}, {"attemptId", plan.attemptId}, {"parentAttemptId", plan.parentAttemptId}, {"createdAt", plan.createdAt}, {"mode", plan.options.mode == ExportMode::ExtractOriginal ? "extractOriginal" : "bake"}, {"targetProfile", plan.options.targetProfile}, {"direction", plan.options.direction == Direction::UnityToUnreal ? "unity-to-unreal" : "unreal-to-unity"}, {"options", options}, {"jobs", jobs}, {"outputCapabilities", json::parse(plan.outputCapabilitiesJson)}, {"recoveryEntries", json::parse(plan.recoveryEntriesJson)}}.dump(2);
}
std::string resultsJson(const std::vector<Result>& results) {
    json rows = json::array(); for (auto& r : results) {
        json files = json::array(); for (auto& file : r.outputFiles) files.push_back(utf8(file));
        rows.push_back({{"id", r.id}, {"clipIds", r.clipIds}, {"name", r.name}, {"status", r.status}, {"message", r.message}, {"exit_code", r.exitCode}, {"output", utf8(r.output)}, {"log", utf8(r.log)},
            {"step", r.step}, {"jobId", r.jobId}, {"attemptId", r.attemptId}, {"errorCode", r.errorCode}, {"recovery", r.recovery}, {"sourceHash", r.sourceHash}, {"outputHash", r.outputHash.empty() ? json(nullptr) : json(r.outputHash)}, {"checks", json::parse(r.checksJson)}, {"outputFiles", files}, {"manifestPath", r.manifestPath.empty() ? json(nullptr) : json(utf8(r.manifestPath))}, {"warnings", r.warnings}, {"startedAt", r.startedAt}, {"finishedAt", r.finishedAt}});
    }
    return json{{"program", "AnimBridge"}, {"results", rows}}.dump(2);
}
}
