#include "animbridge/core.hpp"
#include "animbridge/file_commit.hpp"
#include "animbridge/path_identity.hpp"
#include <nlohmann/json.hpp>
#include <array>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/evp.h>
#endif
#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace animbridge {
using json = nlohmann::json;
namespace {
#ifdef _WIN32
class Hash {
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
    std::vector<unsigned char> object_;
public:
    Hash() {
        if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) throw std::runtime_error("SHA-256 provider unavailable");
        DWORD size = 0, received = 0;
        if (BCryptGetProperty(algorithm_, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&size), sizeof(size), &received, 0) < 0) { BCryptCloseAlgorithmProvider(algorithm_, 0); algorithm_ = nullptr; throw std::runtime_error("SHA-256 object unavailable"); }
        object_.resize(size);
        if (BCryptCreateHash(algorithm_, &hash_, object_.data(), size, nullptr, 0, 0) < 0) { BCryptCloseAlgorithmProvider(algorithm_, 0); algorithm_ = nullptr; throw std::runtime_error("SHA-256 initialization failed"); }
    }
    ~Hash() { if (hash_) BCryptDestroyHash(hash_); if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0); }
    void update(const char* data, std::size_t size) {
        while (size) { auto chunk = static_cast<ULONG>(std::min<std::size_t>(size, 1u << 20)); if (BCryptHashData(hash_, reinterpret_cast<PUCHAR>(const_cast<char*>(data)), chunk, 0) < 0) throw std::runtime_error("SHA-256 update failed"); data += chunk; size -= chunk; }
    }
    std::string finish() {
        std::array<unsigned char, 32> bytes{}; if (BCryptFinishHash(hash_, bytes.data(), static_cast<ULONG>(bytes.size()), 0) < 0) throw std::runtime_error("SHA-256 finalization failed");
        std::ostringstream out; out << std::hex << std::setfill('0'); for (auto byte : bytes) out << std::setw(2) << static_cast<int>(byte); return out.str();
    }
};
#elif defined(__APPLE__)
class Hash {
    CC_SHA256_CTX context_{};
public:
    Hash() { if (CC_SHA256_Init(&context_) != 1) throw std::runtime_error("SHA-256 initialization failed"); }
    void update(const char* data, std::size_t size) {
        while (size) { auto count = static_cast<CC_LONG>(std::min<std::size_t>(size, 1u << 20)); if (CC_SHA256_Update(&context_, data, count) != 1) throw std::runtime_error("SHA-256 update failed"); data += count; size -= count; }
    }
    std::string finish() {
        std::array<unsigned char, CC_SHA256_DIGEST_LENGTH> bytes{};
        if (CC_SHA256_Final(bytes.data(), &context_) != 1) throw std::runtime_error("SHA-256 finalization failed");
        std::ostringstream out; out << std::hex << std::setfill('0'); for (auto byte : bytes) out << std::setw(2) << static_cast<int>(byte); return out.str();
    }
};
#else
class Hash {
    EVP_MD_CTX* context_ = nullptr;
public:
    Hash() {
        context_ = EVP_MD_CTX_new();
        if (!context_ || EVP_DigestInit_ex(context_, EVP_sha256(), nullptr) != 1) { if (context_) EVP_MD_CTX_free(context_); throw std::runtime_error("SHA-256 initialization failed"); }
    }
    ~Hash() { EVP_MD_CTX_free(context_); }
    void update(const char* data, std::size_t size) { if (EVP_DigestUpdate(context_, data, size) != 1) throw std::runtime_error("SHA-256 update failed"); }
    std::string finish() {
        std::array<unsigned char, 32> bytes{}; unsigned count = 0;
        if (EVP_DigestFinal_ex(context_, bytes.data(), &count) != 1 || count != bytes.size()) throw std::runtime_error("SHA-256 finalization failed");
        std::ostringstream out; out << std::hex << std::setfill('0'); for (auto byte : bytes) out << std::setw(2) << static_cast<int>(byte); return out.str();
    }
};
#endif
void noLinks(const fs::path& path) {
    auto current = fs::absolute(path);
    while (!current.empty()) {
#ifdef _WIN32
        auto attr = GetFileAttributesW(current.c_str()); if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT)) throw std::runtime_error("Linked/reparse output paths are not permitted");
#else
        if (fs::is_symlink(fs::symlink_status(current))) throw std::runtime_error("Linked output paths are not permitted");
#endif
        auto parent = current.parent_path(); if (parent == current) break; current = parent;
    }
}
void writeAtomic(const fs::path& path, const json& value, bool overwrite = false) {
    FileCommitter::writeAtomic(path, value.dump(2) + '\n', overwrite);
}
json readJson(const fs::path& path) { if (fs::file_size(path) > 64 * 1024 * 1024) throw std::runtime_error("JSON report size limit exceeded"); std::ifstream f(path, std::ios::binary); if (!f) throw std::runtime_error("Missing converter manifest"); return json::parse(f); }
void publishReport(const fs::path& requested, json value, const std::string& attemptId) {
    auto target = requested; bool owned = false;
    if (fs::exists(target)) {
        try {
            auto previous = readJson(target); auto previousAttempt = previous.value("attemptId", "");
            owned = previous.value("program", "") == "AnimBridge" && !previousAttempt.empty() && previousAttempt.find_first_not_of("0123456789abcdef") == std::string::npos &&
                    fs::is_regular_file(target.parent_path() / ".animbridge-history" / previousAttempt / "output-manifest.json");
        } catch (const std::exception&) {}
        if (!owned) target = target.parent_path() / fromUtf8(utf8(target.stem()) + "-" + attemptId + utf8(target.extension()));
    }
    value["program"] = "AnimBridge"; value["attemptId"] = attemptId;
    writeAtomic(target, value, owned);
}
// Exported animation FBX files carry bones but no mesh, and Unreal can only
// import them onto an existing Skeleton with the same bone names. Copy the
// character model the bake used into <output>/Character so the user can
// import it first and get that Skeleton. Returns the copy, or empty when no
// bake used the model. An existing file is never replaced: identical content
// is reused, different content gets a "-<attemptId>" name.
fs::path publishCharacterModel(const Plan& plan, const std::vector<Result>& results, const std::string& attemptId) {
    const auto& model = plan.options.skeleton;
    if (plan.options.direction != Direction::UnityToUnreal || model.empty() || !fs::is_regular_file(model)) return {};
    std::string expectedHash;
    for (const auto& result : results) {
        if (result.status != "ok" && result.status != "warning") continue;
        auto job = std::find_if(plan.jobs.begin(), plan.jobs.end(), [&](const Job& item) { return item.clip.id == result.id; });
        if (job != plan.jobs.end() && (job->action == "unity-helper-bake" || job->action == "python-convert") && !job->skeletonHash.empty()) { expectedHash = job->skeletonHash; break; }
    }
    if (expectedHash.empty()) return {};
    // The model must still be the exact file the animations were baked against.
    if (fileSha256(model) != expectedHash) throw std::runtime_error("The character model changed during export; it was not copied");
    // A clip may itself be named "Character"; never mix the model into a clip's folder.
    auto folded = [](std::string text) { for (auto& c : text) c = char(std::tolower(static_cast<unsigned char>(c))); return text; };
    auto clipFolder = [&](const fs::path& candidate) {
        for (const auto& job : plan.jobs) if (folded(utf8(job.output.filename())) == folded(utf8(candidate.filename()))) return true;
        return fs::exists(candidate / "manifest.json");
    };
    auto folder = plan.options.output / "Character";
    if (clipFolder(folder)) folder = plan.options.output / "Character_Model";
    if (clipFolder(folder)) folder = plan.options.output / fromUtf8("Character_Model-" + attemptId);
    fs::create_directories(folder);
    auto target = folder / model.filename();
    if (fs::exists(target)) {
        if (fs::is_regular_file(target) && fileSha256(target) == expectedHash) return target;
        target = folder / fromUtf8(utf8(model.stem()) + "-" + attemptId + utf8(model.extension()));
    }
    const auto staged = folder / fromUtf8(".animbridge-tmp-model-" + attemptId);
    fs::copy_file(model, staged, fs::copy_options::overwrite_existing);
    if (FileCommitter::noReplace(staged, target) != PublishResult::Committed) {
        std::error_code ignored; fs::remove(staged, ignored);
        throw std::runtime_error("The character model copy target already exists: " + utf8(target));
    }
    return target;
}
void rewritePaths(json& value, const fs::path& before, const fs::path& after) {
    if (value.is_string()) {
        auto text = value.get<std::string>(); auto prefix = utf8(before);
        if (text == prefix || text.starts_with(prefix + "\\") || text.starts_with(prefix + "/")) value = utf8(after) + text.substr(prefix.size());
    } else if (value.is_array() || value.is_object()) for (auto& child : value) rewritePaths(child, before, after);
}
bool within(const fs::path& root, const fs::path& path) {
    return pathContains(root, path) && fs::weakly_canonical(root) != fs::weakly_canonical(path);
}
bool headerFbx(const fs::path& path) {
    std::ifstream file(path, std::ios::binary); std::array<char, 4096> data{}; file.read(data.data(), data.size());
    std::string prefix(data.data(), static_cast<std::size_t>(file.gcount()));
    return prefix.starts_with("Kaydara FBX Binary") || prefix.find("FBXHeaderExtension") != std::string::npos || prefix.starts_with("; FBX");
}
fs::path renamed(const fs::path& path, std::set<fs::path>& reserved) {
    auto candidate = path;
    for (int suffix = 2; fs::exists(candidate) || reserved.contains(candidate); ++suffix) {
        if (suffix > 129) throw std::runtime_error("OUTPUT_CONFLICT: rename exhausted 128 candidate suffixes; verified staging is retained");
        candidate = path.parent_path() / fromUtf8(utf8(path.stem()) + "_" + std::to_string(suffix) + utf8(path.extension()));
    }
    reserved.insert(candidate); return candidate;
}
void commitDirectory(const fs::path& staged, const fs::path& target) {
    if (FileCommitter::noReplace(staged, target) == PublishResult::Exists) throw std::runtime_error("OUTPUT_CONFLICT: existing bake directories cannot be replaced safely; choose rename or skip");
}
std::string recoveryFor(const std::string& code) {
    if (code == "SOURCE_CHANGED") return "Re-open and analyse the changed source, then confirm a new plan";
    if (code == "DEPENDENCY_MISSING") return "Connect the required model or Avatar and validate it before retrying";
    if (code == "HELPER_UNAVAILABLE") return "Choose an installed Unity Editor and run the isolated Helper connection test";
    if (code == "UNSUPPORTED_DATA") return "Preserve the original or use a supported source-engine export/evaluation path";
    if (code == "OUTPUT_CONFLICT") return "Choose rename, skip, or a new output location; existing bake directories cannot be overwritten";
    if (code == "OUTPUT_WRITE_FAILED") return "Check the reported output operation, permissions, free space and file locks";
    if (code == "OUTPUT_VERIFICATION_FAILED") return "Inspect the failed attempt evidence and verify the source and converter output before retrying";
    if (code == "CONVERSION_FAILED" || code == "PROCESS_TIMEOUT" || code == "EVALUATION_FAILED") return "Open the converter/Helper log and resolve the reported processing failure before retrying";
    return "Open the attempt log and report to identify the cause; no unverified diagnosis or automatic repair was applied";
}
std::mutex activeMutex;
std::set<std::string> activeAttempts;
struct AttemptGuard {
    std::string key;
    explicit AttemptGuard(std::string value) : key(std::move(value)) { std::lock_guard lock(activeMutex); if (!activeAttempts.insert(key).second) throw std::runtime_error("This attempt is already running"); }
    ~AttemptGuard() { std::lock_guard lock(activeMutex); activeAttempts.erase(key); }
};
}
std::string fileSha256(const fs::path& path) {
    auto canonical = verifiedCanonicalPath(path);
    const auto expectedIdentity = fileIdentity(path);
    Hash hash; std::array<char, 65536> buffer{}; std::string openedIdentity;
    std::ostringstream identity; identity << std::hex;
#ifdef _WIN32
    struct File { HANDLE handle; ~File() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); } } file{CreateFileW(canonical.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
    if (file.handle == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot hash source: " + utf8(path));
    BY_HANDLE_FILE_INFORMATION before{}, after{};
    if (!GetFileInformationByHandle(file.handle, &before) || (before.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) throw std::runtime_error("SOURCE_CHANGED: source hash requires an unlinked regular file");
    identity << before.dwVolumeSerialNumber << ':' << before.nFileIndexHigh << ':' << before.nFileIndexLow;
    openedIdentity = identity.str();
    if (openedIdentity != expectedIdentity) throw std::runtime_error("SOURCE_CHANGED: input identity changed before hashing");
    for (;;) { DWORD count = 0; if (!ReadFile(file.handle, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr)) throw std::runtime_error("Source hash read failed: " + utf8(path)); if (!count) break; hash.update(buffer.data(), count); }
    if (!GetFileInformationByHandle(file.handle, &after) || before.nFileSizeHigh != after.nFileSizeHigh || before.nFileSizeLow != after.nFileSizeLow || CompareFileTime(&before.ftLastWriteTime, &after.ftLastWriteTime)) throw std::runtime_error("SOURCE_CHANGED: input changed while hashing");
#else
    struct File { int descriptor; ~File() { if (descriptor >= 0) close(descriptor); } } file{open(canonical.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
    if (file.descriptor < 0) throw std::runtime_error("Cannot hash source: " + utf8(path));
    struct stat before{}, after{};
    if (fstat(file.descriptor, &before) || !S_ISREG(before.st_mode)) throw std::runtime_error("SOURCE_CHANGED: source hash requires an unlinked regular file");
    identity << static_cast<std::uint64_t>(before.st_dev) << ':' << static_cast<std::uint64_t>(before.st_ino); openedIdentity = identity.str();
    if (openedIdentity != expectedIdentity) throw std::runtime_error("SOURCE_CHANGED: input identity changed before hashing");
    for (;;) { auto count = read(file.descriptor, buffer.data(), buffer.size()); if (count < 0 && errno == EINTR) continue; if (count < 0) throw std::runtime_error("Source hash read failed: " + utf8(path)); if (!count) break; hash.update(buffer.data(), static_cast<std::size_t>(count)); }
    if (fstat(file.descriptor, &after) || before.st_size != after.st_size || before.st_mtime != after.st_mtime
#ifdef __APPLE__
        || before.st_mtimespec.tv_nsec != after.st_mtimespec.tv_nsec
#else
        || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec
#endif
        ) throw std::runtime_error("SOURCE_CHANGED: input changed while hashing");
#endif
    if (fileIdentity(path) != openedIdentity || fileIdentity(canonical) != openedIdentity) throw std::runtime_error("SOURCE_CHANGED: input identity changed while hashing");
    return hash.finish();
}
std::string textSha256(const std::string& text) { Hash hash; hash.update(text.data(), text.size()); return hash.finish(); }
std::string newIdentity() { std::random_device random; std::ostringstream out; out << std::hex << std::setfill('0'); for (int i = 0; i < 4; ++i) out << std::setw(8) << random(); return out.str(); }
std::string timestampUtc() {
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()); std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    std::ostringstream out; out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ"); return out.str();
}
Plan retryFailed(const Plan& plan, const std::vector<Result>& results) {
    Plan retry = plan; retry.jobs.clear(); retry.parentAttemptId = plan.attemptId; retry.attemptId = newIdentity(); retry.createdAt = timestampUtc();
    for (auto& job : plan.jobs) {
        auto result = std::find_if(results.begin(), results.end(), [&](auto& r) { return r.id == job.clip.id && (r.status == "error" || r.status == "blocked"); });
        if (result != results.end()) retry.jobs.push_back(job);
    }
    if (retry.jobs.empty()) throw std::runtime_error("There are no failed items to retry");
    retry.planSnapshot.clear(); retry.planSnapshot = planJson(retry); return retry;
}
std::vector<Result> execute(const Plan& plan, Progress progress, std::shared_ptr<std::atomic_bool> cancel) {
    if (plan.jobs.empty()) throw std::runtime_error("Select at least one animation");
    if (!plan.planSnapshot.empty() && plan.planSnapshot != planJson(plan)) throw std::runtime_error("Plan changed after confirmation; create a new plan");
    if (plan.options.collisionPolicy == CollisionPolicy::Overwrite && !plan.options.explicitOverwrite) throw std::runtime_error("Overwrite requires explicit confirmation");
    auto attemptId = plan.attemptId.empty() ? newIdentity() : plan.attemptId;
    if (attemptId.size() != 32 || attemptId.find_first_not_of("0123456789abcdef") != std::string::npos) throw std::runtime_error("Invalid attempt identity");
    AttemptGuard guard(attemptId);
    noLinks(plan.options.output); fs::create_directories(plan.options.output);
    OutputCommitLock outputLock(plan.options.output);
    auto capabilities = FileCommitter::probe(plan.options.output);
    if (!capabilities.localVolume || !capabilities.flush || !capabilities.fileNoReplace || (plan.options.mode == ExportMode::Bake && !capabilities.directoryNoReplace)) throw std::runtime_error("OUTPUT_WRITE_FAILED: output volume no longer supports required atomic publication");
    auto plannedCapabilities = json::parse(plan.outputCapabilitiesJson, nullptr, false);
    if (plannedCapabilities.is_object() && plannedCapabilities.contains("volumeIdentity") && plannedCapabilities["volumeIdentity"] != capabilities.volumeIdentity) throw std::runtime_error("OUTPUT_WRITE_FAILED: output directory identity changed after confirmation");
    auto journalRoot = (plan.options.journalRoot.empty() ? plan.options.runtimeContext.stateRoot / "attempts" : plan.options.journalRoot) / attemptId;
    noLinks(journalRoot); fs::create_directories(journalRoot.parent_path());
    if (!fs::create_directory(journalRoot)) throw std::runtime_error("Attempt journal already exists; interrupted work requires review or a new retry attempt");
    noLinks(journalRoot);
    auto ledgerPath = plan.options.output / ".animbridge-export-ledger.json";
    json exportLedger{{"program", "AnimBridge"}, {"schemaVersion", 1}, {"identities", json::object()}};
    if (fs::exists(ledgerPath)) {
        exportLedger = readJson(ledgerPath);
        if (exportLedger.value("program", "") != "AnimBridge" || exportLedger.value("schemaVersion", 0) != 1 || !exportLedger.contains("identities") || !exportLedger["identities"].is_object()) throw std::runtime_error("OUTPUT_CONFLICT: existing export ledger is foreign or uses an unsupported schema");
    }
    for (const auto& job : plan.jobs) if (!job.clip.exportId.empty() && exportLedger["identities"].contains(job.clip.exportId) && exportLedger["identities"][job.clip.exportId].value("digest", "") != job.clip.exportIdentityDigest) throw std::runtime_error("EXPORT_ID_COLLISION: recorded external ID maps to a different identity");
    auto history = plan.options.output / ".animbridge-history" / attemptId;
    if (fs::exists(history)) throw std::runtime_error("Attempt already exists; retry with a new attempt ID");
    if (plan.options.collisionPolicy == CollisionPolicy::Fail)
        for (auto& job : plan.jobs) if (job.blockedReason.empty() && fs::exists(job.output)) throw std::runtime_error("Output already exists; choose skip, rename or explicit overwrite: " + utf8(job.output));
    noLinks(history); fs::create_directories(history.parent_path());
    if (!fs::create_directory(history)) throw std::runtime_error("This attempt is already running or has completed; create a new retry attempt");
    writeAtomic(history / "plan.json", json::parse(plan.planSnapshot.empty() ? planJson(plan) : plan.planSnapshot));
    std::vector<Result> results; std::set<fs::path> reserved;
    auto cancelled = [&] { return cancel && cancel->load(); };
    bool progressUnavailable = false;
    auto emit = [&](Result& result, const std::string& step) {
        result.step = step;
        if (progress && !progressUnavailable) try { progress(result); }
        catch (const std::exception&) { progressUnavailable = true; result.warnings.push_back("Progress delivery failed; the attempt continues with its durable journal and report"); }
        catch (...) { progressUnavailable = true; result.warnings.push_back("Progress delivery failed; the attempt continues with its durable journal and report"); }
    };
    for (auto& job : plan.jobs) {
        const auto absoluteDeadline = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() + static_cast<std::int64_t>(plan.options.jobTimeoutSeconds) * 1000;
        auto requireDeadline = [&] { if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() >= absoluteDeadline) throw std::runtime_error("PROCESS_TIMEOUT: item absolute deadline elapsed"); };
        Result result; result.id = job.clip.id; result.name = job.clip.name; result.clipIds = job.clipIds; result.output = job.output;
        result.jobId = plan.jobId; result.attemptId = attemptId; result.startedAt = timestampUtc(); result.sourceHash = job.sourceHash;
        result.status = "running"; json checks{{"sourceHash", "notRun"}, {"exists", "notRun"}, {"structure", "notRun"}, {"byteHash", "notRun"}, {"finalCommit", "notRun"}, {"enginePlayback", "notRun"}, {"retargeting", "notRun"}, {"engineImport", "notRun"}};
        checks["absoluteDeadlineMilliseconds"] = absoluteDeadline;
        fs::path scratch = plan.options.output / (".animbridge-tmp-" + attemptId + "-" + std::to_string(results.size()));
        fs::path replacementBackup;
        auto journal = journalRoot / (std::to_string(results.size()) + ".json");
        bool committed = false;
        bool preserveScratch = false;
        bool retainProcessWork = false;
        std::string successStatus = "ok";
        try {
            emit(result, "validating");
            if (cancelled()) { result.status = "cancelled"; result.message = "Cancelled before the item started"; }
            else if (!job.blockedReason.empty()) { result.status = "blocked"; result.exitCode = 2; result.errorCode = job.blockedCode.empty() ? "UNKNOWN_ERROR" : job.blockedCode; result.message = job.blockedReason; result.recovery = job.recovery.empty() ? recoveryFor(result.errorCode) : job.recovery; }
            else if (fs::exists(job.output) && plan.options.collisionPolicy == CollisionPolicy::Skip) { result.status = "skipped"; result.message = "Existing output preserved by the skip policy"; }
            else {
                if (plan.options.collisionPolicy == CollisionPolicy::Rename) result.output = renamed(job.output, reserved);
                checks["sourceHash"] = "failed";
                auto actualHash = fileSha256(job.clip.source);
                if (!job.clip.fileIdentity.empty() && fileIdentity(job.clip.packageSourcePath.empty() ? job.clip.source : job.clip.packageSourcePath) != job.clip.fileIdentity) throw std::runtime_error("SOURCE_CHANGED: source file identity changed after analysis");
                if (!job.sourceHash.empty() && actualHash != job.sourceHash) throw std::runtime_error("SOURCE_CHANGED: source bytes changed after plan confirmation; analyse again");
                result.sourceHash = actualHash; checks["sourceHash"] = "passed";
                if (!job.clip.packageSourceHash.empty() && fileSha256(job.clip.packageSourcePath) != job.clip.packageSourceHash) throw std::runtime_error("SOURCE_CHANGED: original package changed after analysis");
                if (plan.options.mode == ExportMode::Bake) {
                    if (!job.metadataHash.empty() && fileSha256(fromUtf8(utf8(job.clip.source) + ".meta")) != job.metadataHash) throw std::runtime_error("SOURCE_CHANGED: source metadata changed after plan confirmation");
                    if (!job.skeletonHash.empty() && fileSha256(plan.options.skeleton) != job.skeletonHash) throw std::runtime_error("SOURCE_CHANGED: skeleton changed after plan confirmation");
                    if (!job.skeletonMetadataHash.empty() && fileSha256(fromUtf8(utf8(plan.options.skeleton) + ".meta")) != job.skeletonMetadataHash) throw std::runtime_error("SOURCE_CHANGED: skeleton import metadata changed after plan confirmation");
                }
                noLinks(scratch); if (!fs::create_directory(scratch)) throw std::runtime_error("OUTPUT_WRITE_FAILED: cannot allocate an isolated temporary output");
                writeAtomic(scratch / ".animbridge-owner.json", {{"program", "AnimBridge"}, {"attemptId", attemptId}, {"outputRoot", utf8(plan.options.output)}});
                fs::path staged;
                if (plan.options.mode == ExportMode::ExtractOriginal) {
                    emit(result, "writingTemporary"); staged = scratch / "original";
                    fs::copy_file(job.clip.source, staged, fs::copy_options::none);
#ifdef _WIN32
                    // A source's read-only attribute must not prevent flushing
                    // our new staging file. Original file attributes are intact.
                    fs::permissions(staged, fs::perms::owner_write, fs::perm_options::add);
#endif
                    emit(result, "verifying"); result.outputHash = fileSha256(staged);
                    if (result.outputHash != actualHash || fileSha256(job.clip.source) != actualHash) throw std::runtime_error("OUTPUT_VERIFICATION_FAILED: original extraction hash mismatch");
                    checks["temporaryExists"] = "passed"; checks["byteHash"] = "passed";
                    checks["structure"] = "notRun"; checks["structureReason"] = "Original preservation does not claim successful parsing or evaluation";
                    result.outputFiles.push_back(result.output); checks["outputHashes"][utf8(result.output)] = result.outputHash;
                } else {
                    emit(result, "converting"); staged = scratch / "item";
                    Plan temporary = plan; temporary.planSnapshot.clear(); temporary.options.output = scratch; temporary.options.targetEditor.clear(); temporary.options.targetProject.clear();
                    temporary.jobs = {job}; temporary.jobs[0].output = staged;
                    auto& arguments = temporary.jobs[0].arguments;
                    for (std::size_t index = 0; index + 1 < arguments.size(); ++index) if (arguments[index] == "-o" || arguments[index] == "--output") arguments[index + 1] = utf8(staged);
                    auto converted = executeLegacy(temporary, {}, cancel, absoluteDeadline);
                    if (!converted.empty()) {
                        result.warnings.insert(result.warnings.end(), converted[0].warnings.begin(), converted[0].warnings.end());
                        auto adapterChecks = json::parse(converted[0].checksJson);
                        if (adapterChecks.contains("helperCacheCleanup")) checks["helperCacheCleanup"] = adapterChecks["helperCacheCleanup"];
                        if (adapterChecks.contains("processCleanup")) {
                            checks["processCleanup"] = adapterChecks["processCleanup"];
                            if (adapterChecks["processCleanup"].value("status", "") != "passed") { preserveScratch = true; retainProcessWork = true; }
                        }
                    }
                    if (converted.size() != 1 || (converted[0].status != "ok" && converted[0].status != "warning" && converted[0].status != "ready-for-unity-import")) {
                        if (!converted.empty()) { result.log = converted[0].log; result.exitCode = converted[0].exitCode; result.errorCode = converted[0].errorCode.empty() ? "CONVERSION_FAILED" : converted[0].errorCode; if (converted[0].status == "blocked") result.status = "blocked"; }
                        throw std::runtime_error(converted.empty() ? "Converter produced no result" : converted[0].message);
                    }
                    successStatus = converted[0].status; result.message = converted[0].message; result.log = converted[0].log;
                    emit(result, "verifying");
                    if (job.action == "python-convert" || job.action == "unity-helper-bake") {
                        auto manifest = readJson(staged / "manifest.json"); if (!manifest.contains("clips") || manifest["clips"].empty()) throw std::runtime_error("No converted clips to verify");
                        manifest["exportId"] = job.clip.exportId; manifest["exportIdentityVersion"] = job.clip.exportIdentityVersion; manifest["exportIdentityDigest"] = job.clip.exportIdentityDigest;
                        manifest["sourceIdentityVersion"] = 2; manifest["identityDigest"] = job.clip.identityDigest;
                        checks["converterVersion"] = manifest.contains("tool_version") ? manifest["tool_version"] : json(nullptr);
                        if (manifest.contains("warnings") && manifest["warnings"].is_array()) for (auto& warning : manifest["warnings"]) if (warning.is_string()) result.warnings.push_back(warning.get<std::string>());
                        for (auto& clip : manifest["clips"]) {
                            auto file = fromUtf8(clip.at("fbx").get<std::string>());
                            if (!within(staged, file) || !fs::is_regular_file(file) || !headerFbx(file)) throw std::runtime_error("Converter FBX output is missing, unsafe, or has an invalid FBX header");
                            result.outputFiles.push_back(result.output / file.lexically_relative(staged));
                            if (clip.contains("sidecar")) {
                                auto sidecar = fromUtf8(clip["sidecar"].get<std::string>());
                                if (!within(staged, sidecar) || !fs::is_regular_file(sidecar)) throw std::runtime_error("Converter sidecar is missing or outside its temporary output");
                                auto information = readJson(sidecar);
                                if (information.contains("warnings") && information["warnings"].is_array()) for (auto& warning : information["warnings"]) if (warning.is_string()) result.warnings.push_back(warning.get<std::string>());
                                result.outputFiles.push_back(result.output / sidecar.lexically_relative(staged));
                            }
                        }
                        result.outputFiles.push_back(result.output / "manifest.json");
                        writeAtomic(staged / "manifest.json", manifest, true);
                        if (job.action == "unity-helper-bake") {
                            checks["helperEvaluation"] = "passed";
                            for (auto name : {"response.json", "baked.json"}) if (fs::is_regular_file(staged / ".helper" / name)) result.outputFiles.push_back(result.output / ".helper" / name);
                        }
                    } else {
                        auto file = staged / fromUtf8(safeName(job.clip.name) + ".fbx");
                        if (!fs::is_regular_file(file) || !headerFbx(file)) throw std::runtime_error("Exported FBX has an invalid FBX header");
                        result.outputFiles.push_back(result.output / file.filename());
                    }
                    for (auto& entry : fs::directory_iterator(staged)) if (entry.is_regular_file() && entry.path().extension() == ".json") {
                        auto data = readJson(entry.path()); rewritePaths(data, staged, result.output); writeAtomic(entry.path(), data, true);
                    }
                    for (auto& file : result.outputFiles) checks["outputHashes"][utf8(file)] = fileSha256(staged / file.lexically_relative(result.output));
                    if (!result.warnings.empty() && successStatus == "ok") successStatus = "warning";
                    checks["temporaryExists"] = "passed"; checks["structure"] = "fbxHeaderPassed"; checks["byteHash"] = "notApplicableToBake";
                    if (fileSha256(job.clip.source) != actualHash) throw std::runtime_error("SOURCE_CHANGED: source changed while the converter was running");
                }
                if (!job.clip.fileIdentity.empty() && fileIdentity(job.clip.packageSourcePath.empty() ? job.clip.source : job.clip.packageSourcePath) != job.clip.fileIdentity) throw std::runtime_error("SOURCE_CHANGED: source file identity changed before commit");
                if (cancelled()) { result.status = "cancelled"; result.message = "Cancelled at the verified temporary-output boundary; final output was not published"; result.outputFiles.clear(); }
                else {
                    emit(result, "committing");
                    requireDeadline();
                    const bool overwrite = plan.options.collisionPolicy == CollisionPolicy::Overwrite && plan.options.explicitOverwrite;
                    for (unsigned retry = 0;; ++retry) {
                        for (const auto& original : plan.jobs) {
                            if (sameFile(result.output, original.clip.source) || (!original.clip.packageSourcePath.empty() && sameFile(result.output, original.clip.packageSourcePath))) throw std::runtime_error("OUTPUT_CONFLICT: final target aliases an original input");
                        }
                        auto publication = json{{"program", "AnimBridge"}, {"schemaVersion", 1}, {"operation", "publish"}, {"state", "staged"}, {"attemptId", attemptId},
                            {"target", utf8(result.output)}, {"staged", utf8(staged)}, {"scratch", utf8(scratch)}, {"stagedIdentity", fileIdentity(staged)}, {"sourceHash", actualHash}, {"updatedAt", timestampUtc()}};
                        FileCommitter::recordJournal(journal, publication.dump(2) + '\n');
                        preserveScratch = true;
                        if (plan.options.mode == ExportMode::ExtractOriginal && overwrite && fs::exists(result.output)) {
                            replacementBackup = history / "replaced" / fromUtf8(newIdentity() + "-" + utf8(result.output.filename()));
                            FileCommitter::replaceFile(staged, result.output, replacementBackup, journal);
                            committed = true; preserveScratch = retainProcessWork; break;
                        }
                        publication["state"] = "publish-pending"; FileCommitter::recordJournal(journal, publication.dump(2) + '\n');
                        if (FileCommitter::noReplace(staged, result.output) == PublishResult::Committed) {
                            committed = true; preserveScratch = retainProcessWork;
                            publication["state"] = "committed"; FileCommitter::recordJournal(journal, publication.dump(2) + '\n'); break;
                        }
                        if (plan.options.collisionPolicy == CollisionPolicy::Skip) {
                            result.status = "skipped"; result.message = "Output appeared before atomic commit and was preserved by the skip policy"; result.outputFiles.clear();
                            publication["state"] = "skipped"; FileCommitter::recordJournal(journal, publication.dump(2) + '\n'); preserveScratch = retainProcessWork; break;
                        }
                        if (plan.options.collisionPolicy != CollisionPolicy::Rename || retry >= 127) throw std::runtime_error("OUTPUT_CONFLICT: atomic publication found an existing target; verified staging is retained");
                        auto previousOutput = result.output;
                        result.output = renamed(job.output, reserved);
                        for (auto& file : result.outputFiles) file = result.output / file.lexically_relative(previousOutput);
                        checks.erase("outputHashes");
                        if (fs::is_directory(staged)) {
                            for (auto& entry : fs::directory_iterator(staged)) if (entry.is_regular_file() && entry.path().extension() == ".json") { auto data = readJson(entry.path()); rewritePaths(data, previousOutput, result.output); writeAtomic(entry.path(), data, true); }
                            for (auto& file : result.outputFiles) checks["outputHashes"][utf8(file)] = fileSha256(staged / file.lexically_relative(result.output));
                        } else checks["outputHashes"][utf8(result.output)] = result.outputHash;
                    }
                    if (committed) {
                        for (auto& file : result.outputFiles) {
                            if (!checks.contains("outputHashes") || !checks["outputHashes"].contains(utf8(file)) || fileSha256(file) != checks["outputHashes"][utf8(file)].get<std::string>()) throw std::runtime_error("OUTPUT_VERIFICATION_FAILED: published output no longer matches the verified temporary bytes");
                        }
                        if (!job.clip.exportId.empty()) {
                            exportLedger["identities"][job.clip.exportId] = {{"digest", job.clip.exportIdentityDigest}, {"internalDigest", job.clip.identityDigest}, {"exportIdentityVersion", job.clip.exportIdentityVersion}, {"sourceIdentityVersion", 2}, {"output", utf8(result.output)}};
                            writeAtomic(ledgerPath, exportLedger, fs::exists(ledgerPath));
                        }
                        if (!result.log.empty() && within(staged, result.log)) result.log = result.output / result.log.lexically_relative(staged);
                        result.status = successStatus;
                        if (plan.options.mode == ExportMode::ExtractOriginal) result.message = "Original file extracted; SHA-256 matches the source bytes";
                        checks["exists"] = "passed"; checks["finalCommit"] = "passed"; checks["commitMethod"] = "atomicNoReplace";
                    }
                }
            }
        } catch (const std::exception& error) {
            const bool ownedProcessesStopped = !checks.contains("processCleanup") || checks["processCleanup"].value("ownedCleanupComplete", true);
            if (cancelled() && ownedProcessesStopped) { result.status = "cancelled"; result.errorCode.clear(); }
            else if (result.status != "blocked") result.status = "error";
            if (!result.exitCode) result.exitCode = 2; result.message = error.what();
            if (result.errorCode.empty() && result.status != "cancelled") {
                result.errorCode = "UNKNOWN_ERROR";
                for (auto code : {"SOURCE_CHANGED", "OUTPUT_CONFLICT", "OUTPUT_WRITE_FAILED", "OUTPUT_VERIFICATION_FAILED", "PROCESS_TIMEOUT"}) if (result.message.starts_with(std::string(code) + ":")) result.errorCode = code;
            }
            result.recovery = recoveryFor(result.errorCode);
            if (!committed) result.outputFiles.clear();
        }
        if (!replacementBackup.empty() && fs::is_regular_file(replacementBackup)) {
            if (committed) checks["commitMethod"] = "atomicReplaceFileWithBackup";
            checks["replacedFileBackup"] = {{"path", utf8(replacementBackup)}, {"originalTarget", utf8(result.output)}, {"status", "preserved"}};
            try { checks["replacedFileBackup"]["sha256"] = fileSha256(replacementBackup); }
            catch (const std::exception& error) {
                checks["replacedFileBackup"]["sha256"] = nullptr; checks["replacedFileBackup"]["hashStatus"] = "failed";
                result.warnings.push_back(std::string("Previous bytes were preserved, but backup hashing failed: ") + error.what());
                if (result.status == "ok") result.status = "warning";
            }
        }
        if (fs::exists(scratch)) {
            // Keep failed-attempt evidence outside final outputs, never overwrite an earlier attempt.
            if (preserveScratch) {
                checks["recovery"] = {{"status", "manual-review"}, {"staging", utf8(scratch)}, {"journal", utf8(journal)}};
                result.warnings.push_back("Verified or exchanged staging was preserved at " + utf8(scratch) + "; inspect the durable commit journal before recovery");
            } else if (result.status == "error" || result.status == "blocked") {
                auto evidence = history / ("failed-" + std::to_string(results.size()));
                std::error_code error;
                try { if (FileCommitter::noReplace(scratch, evidence) != PublishResult::Committed) error = std::make_error_code(std::errc::file_exists); }
                catch (const std::exception&) { error = std::make_error_code(std::errc::io_error); }
                if (!error && !result.log.empty() && within(scratch, result.log)) result.log = evidence / result.log.lexically_relative(scratch);
                if (error) {
                    checks["temporaryCleanup"] = {{"status", "failed"}, {"remainingPath", utf8(scratch)}, {"error", error.message()}};
                    result.warnings.push_back("Failed-attempt evidence could not be moved; retained at " + utf8(scratch) + ": " + error.message());
                }
            } else {
                std::error_code error;
                if (result.status == "cancelled" && !result.log.empty() && fs::is_regular_file(result.log)) {
                    auto retainedLog = history / ("cancelled-" + std::to_string(results.size()) + ".log");
                    fs::copy_file(result.log, retainedLog, fs::copy_options::none, error); if (!error) result.log = retainedLog;
                }
                removeOwnedTree(scratch, plan.options.output, error);
                if (error) {
                    checks["temporaryCleanup"] = {{"status", "failed"}, {"remainingPath", utf8(scratch)}, {"error", error.message()}};
                    result.warnings.push_back("Temporary output cleanup failed; retained at " + utf8(scratch) + ": " + error.message());
                    if (result.status == "ok") result.status = "warning";
                }
            }
        }
        result.checksJson = checks.dump(); result.finishedAt = timestampUtc(); emit(result, result.status == "ok" || result.status == "warning" || result.status == "ready-for-unity-import" ? "completed" : result.status);
        results.push_back(result);
    }
    fs::path characterModel; std::string characterModelWarning;
    if (plan.options.mode == ExportMode::Bake) {
        // A failed model copy never fails the animations that were already published.
        try { characterModel = publishCharacterModel(plan, results, attemptId); }
        catch (const std::exception& error) { characterModelWarning = std::string("Character model was not copied: ") + error.what(); }
        json reverse = json::array();
        for (auto& result : results) if (result.status == "ready-for-unity-import") {
            auto job = std::find_if(plan.jobs.begin(), plan.jobs.end(), [&](auto& item) { return item.clip.id == result.id; });
            auto metadata = json::parse(job->clip.metadataJson); metadata.update({{"id", result.id}, {"exportId", job->clip.exportId}, {"exportIdentityVersion", job->clip.exportIdentityVersion}, {"exportIdentityDigest", job->clip.exportIdentityDigest}, {"identityDigest", job->clip.identityDigest}, {"name", result.name}, {"fbx", utf8(result.outputFiles.front())}, {"source_asset", job->clip.virtualPath}, {"fps", job->clip.fps}, {"duration", job->clip.duration}}); reverse.push_back(metadata);
        }
        if (!reverse.empty()) publishReport(plan.options.output / "animbridge-manifest.json", {{"animbridge_version", 1}, {"source_engine", "unreal"}, {"clips", reverse}}, attemptId);
        if (!plan.options.targetEditor.empty() && !cancelled()) applyEngineImports(plan, results, [&](const Result& result) { if (progress && !progressUnavailable) try { progress(result); } catch (...) { progressUnavailable = true; } }, cancel);
    }
    for (auto& result : results) result.manifestPath = history / "output-manifest.json";
    json manifest = json::parse(resultsJson(results)); manifest["jobId"] = plan.jobId; manifest["attemptId"] = attemptId; manifest["parentAttemptId"] = plan.parentAttemptId;
    manifest["planSnapshot"] = json::parse(plan.planSnapshot.empty() ? planJson(plan) : plan.planSnapshot);
    manifest["versions"] = {{"parser", ParserVersion}}; manifest["checks"] = {{"enginePlayback", "notRun"}, {"retargeting", "notRun"}};
    manifest["warnings"] = json::array(); for (auto& result : results) for (auto& warning : result.warnings) manifest["warnings"].push_back({{"id", result.id}, {"message", warning}});
    if (!characterModel.empty()) manifest["characterModel"] = utf8(characterModel);
    if (!characterModelWarning.empty()) manifest["warnings"].push_back({{"id", ""}, {"message", characterModelWarning}});
    manifest["completedAt"] = timestampUtc();
    writeAtomic(history / "output-manifest.json", manifest);
    publishReport(plan.options.output / "animbridge-results.json", manifest, attemptId);
    return results;
}
}
