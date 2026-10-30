#pragma once
#include "animbridge/runtime.hpp"
#include <cstdint>
#include <atomic>
#include <memory>
#include <optional>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace animbridge {
namespace fs = std::filesystem;
enum class Engine { Unity, Unreal, Unknown };
enum class Direction { UnityToUnreal, UnrealToUnity };
enum class ExportMode { Bake, ExtractOriginal };
enum class CollisionPolicy { Fail, Skip, Rename, Overwrite };
inline constexpr const char* ParserVersion = "animbridge-native-0.3.0";
struct Limits {
    std::uint64_t maxCompressedBytes = 2ull * 1024 * 1024 * 1024;
    std::uint64_t maxFileBytes = 512ull * 1024 * 1024;
    std::uint64_t maxExpandedBytes = 2ull * 1024 * 1024 * 1024;
    std::size_t maxEntries = 100000;
    unsigned maxSeconds = 120;
};
struct Clip {
    std::string id, name;
    fs::path source;
    std::string virtualPath, kind, route, note;
    std::string metadataJson = "{}"; // Preserve engine exporter events/frames and future metadata.
    std::string sourceHash, sourceId, packageId, parserVersion = ParserVersion;
    std::string projectNamespace, fileIdentity, identityDigest, exportIdentityDigest, legacyId, exportId;
    unsigned sourceIdentityVersion = 2, exportIdentityVersion = 2;
    fs::path packageSourcePath;
    std::string packageSourceHash;
    std::string type, rigType, dependenciesJson = "[]";
    std::string capabilitiesJson = "{}", capabilityReasonsJson = "{}";
    bool fpsKnown = false, durationKnown = false;
    Engine engine = Engine::Unknown;
    bool selected = false;
    bool selectable = true;
    bool extractable = true;
    bool namedClip = false;
    double fps = 0, duration = 0;
};
struct PackageRecord {
    std::string packageId, sourceHash, parserVersion = ParserVersion;
    fs::path sourcePath;
};
// What Session::removeSource dropped, so the caller can clean up selection
// state and the app-owned extraction folders that nothing uses any more.
struct RemovedSource {
    std::vector<std::string> clipIds;
    std::vector<fs::path> extractedRoots; // Only roots no remaining clip lives in.
    bool found = false;
};
struct Session {
    std::vector<Clip> clips;
    std::vector<std::string> warnings;
    std::vector<fs::path> extractedRoots;
    std::vector<PackageRecord> packages;
    void ingest(const std::vector<fs::path>& droppedPaths, const fs::path& cache, Limits limits = {},
        const RuntimeContext& context = resolveRuntimeContext());
    // Forgets one dropped source (package, folder or file) and its clips.
    // Never touches the files on disk; the caller decides about cache folders.
    RemovedSource removeSource(const fs::path& sourcePath);
    void selectAll(bool value, ExportMode mode = ExportMode::Bake, bool allowUnityHelper = false);
    bool select(const std::string& id, bool value, ExportMode mode = ExportMode::Bake, bool allowUnityHelper = false);
};
struct Options {
    RuntimeContext runtimeContext = resolveRuntimeContext();
    Direction direction = Direction::UnityToUnreal;
    fs::path output, skeleton, blender = runtimePaths().blender;
    fs::path repository = runtimePaths().backend;
    fs::path python = runtimePaths().python;
    fs::path workRoot = resolveRuntimeContext().cacheRoot / "jobs";
    fs::path journalRoot;
    fs::path targetEditor, targetProject;
    std::string targetSkeleton, targetFolder;
    unsigned timeoutSeconds = 600;
    unsigned jobTimeoutSeconds = 3600;
    ExportMode mode = ExportMode::Bake;
    CollisionPolicy collisionPolicy = CollisionPolicy::Fail;
    bool explicitOverwrite = false;
    std::optional<double> sampleRate;
    std::string rootMotion = "preserve";
    std::string targetProfile; // Empty infers the current direction, preserving the legacy API.
    fs::path unityEditor;
    std::string humanMappingJson = "{}", boneMapJson = "{}";
};
struct Job {
    Clip clip;
    fs::path output;
    std::vector<std::string> arguments;
    std::string action, blockedReason, blockedCode, recovery;
    std::string sourceHash, metadataHash, skeletonHash, skeletonMetadataHash;
    std::vector<std::string> clipIds;
};
struct Plan {
    Options options; std::vector<Job> jobs;
    std::string jobId, attemptId, parentAttemptId, createdAt, planSnapshot;
    std::string outputCapabilitiesJson = "{}", recoveryEntriesJson = "[]";
};
struct Result {
    std::string id, name, status, message;
    int exitCode = 0;
    fs::path output, log;
    fs::path manifestPath;
    std::string step, jobId, attemptId, errorCode, recovery, sourceHash, outputHash;
    std::string checksJson = "{}", startedAt, finishedAt;
    std::vector<fs::path> outputFiles;
    std::vector<std::string> clipIds;
    std::vector<std::string> warnings;
};
using Progress = std::function<void(const Result&)>;
// A raw .uasset can only be read by Unreal Editor with its owning project
// loaded, so converting one needs both the project file and the object path.
struct UnrealAssetLocation {
    fs::path project;       // <Project>/<Name>.uproject
    std::string objectPath; // /Game/A/B/Name.Name or /<Plugin>/A/Name.Name
};
// Maps <contentRoot>/A/B/Name.uasset to <mountPoint>/A/B/Name.Name. Returns an
// empty string when the file is not a .uasset below contentRoot.
std::string unrealObjectPath(const fs::path& contentRoot, const std::string& mountPoint, const fs::path& asset);
// Walks up from a .uasset to the .uproject whose Content folder (or one of its
// plugins' Content folders) holds it. Never reads the asset bytes.
std::optional<UnrealAssetLocation> locateUnrealAsset(const fs::path& asset);
Plan makePlan(const Session& session, const Options& options);
std::vector<Result> execute(const Plan& plan, Progress progress = {}, std::shared_ptr<std::atomic_bool> cancel = {});
Plan retryFailed(const Plan& plan, const std::vector<Result>& results);
std::string fileSha256(const fs::path& path);
std::string textSha256(const std::string& text);
std::string newIdentity();
std::string timestampUtc();
// Adapter entry points used by the transactional workflow; UI calls execute().
std::vector<Result> executeLegacy(const Plan& plan, Progress progress = {},
    std::shared_ptr<std::atomic_bool> cancel = {}, std::int64_t absoluteDeadlineMilliseconds = 0);
void applyEngineImports(const Plan& plan, std::vector<Result>& results, Progress progress = {},
    std::shared_ptr<std::atomic_bool> cancel = {});
std::string sessionJson(const Session& session);
std::string planJson(const Plan& plan);
std::string resultsJson(const std::vector<Result>& results);
std::string utf8(const fs::path& path);
fs::path fromUtf8(const std::string& value);
std::string engineName(Engine engine);
std::string safeName(const std::string& value);
fs::path extractUnityPackage(const fs::path& package, const fs::path& cache, const Limits& limits);
struct ProcessResult {
    int exitCode; bool timedOut; bool cancelled = false;
    std::string timeoutLayer, stage;
    bool cleanupComplete = true;
    bool ownedCleanupComplete = true;
    std::vector<std::string> cleanupWarnings;
};
ProcessResult runProcess(const fs::path& executable, const std::vector<std::string>& args,
                         const fs::path& workingDirectory, const fs::path& log, unsigned timeoutSeconds,
                         const std::vector<std::pair<std::string, std::string>>& environment = {},
                         std::shared_ptr<std::atomic_bool> cancel = {});
}
