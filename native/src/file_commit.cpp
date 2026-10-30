#include "animbridge/file_commit.hpp"
#include "animbridge/core.hpp"
#include "animbridge/path_identity.hpp"
#include <nlohmann/json.hpp>
#include <array>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <system_error>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <stdio.h>
#include <sys/mount.h>
#else
#include <linux/fs.h>
#include <sys/syscall.h>
#include <sys/vfs.h>
#endif
#endif

namespace animbridge {
using json = nlohmann::json;
namespace {
[[noreturn]] void fail(const char* operation) {
#ifdef _WIN32
    throw std::runtime_error(std::string("OUTPUT_WRITE_FAILED: ") + operation + " (Windows error " + std::to_string(GetLastError()) + ")");
#else
    throw std::runtime_error(std::string("OUTPUT_WRITE_FAILED: ") + operation + " (" + std::strerror(errno) + ")");
#endif
}
void noLinks(const fs::path& path) {
    utf8(path);
    auto current = fs::absolute(path);
    for (;;) {
#ifdef _WIN32
        auto attrs = GetFileAttributesW(current.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) throw std::runtime_error("OUTPUT_WRITE_FAILED: linked output component");
#else
        std::error_code error;
        auto status = fs::symlink_status(current, error);
        if (!error && fs::is_symlink(status)) throw std::runtime_error("OUTPUT_WRITE_FAILED: linked output component");
        if (error && error != std::errc::no_such_file_or_directory && error != std::errc::not_a_directory) throw std::system_error(error);
#endif
        auto parent = current.parent_path(); if (parent.empty() || parent == current) break; current = parent;
    }
}
#ifdef _WIN32
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE handle) : value(handle) {}
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
};
struct RemoveTemporary {
    fs::path path;
    bool owned = false;
    ~RemoveTemporary() { if (owned) { std::error_code ignored; fs::remove(path, ignored); } }
};
void flushFile(const fs::path& path) {
    noLinks(path);
    Handle file(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (file.value == INVALID_HANDLE_VALUE || !FlushFileBuffers(file.value)) fail("cannot flush staged file");
}
void flushDirectory(const fs::path&) {
    // Windows MoveFileEx WRITE_THROUGH and ReplaceFile are used below. Directory
    // FlushFileBuffers is not supported by NTFS and is not claimed here.
}
#else
struct Descriptor {
    int value = -1;
    explicit Descriptor(int descriptor) : value(descriptor) {}
    ~Descriptor() { if (value >= 0) close(value); }
    Descriptor(const Descriptor&) = delete;
    Descriptor(Descriptor&& other) noexcept : value(other.value) { other.value = -1; }
};
// Resolve every component with O_NOFOLLOW. Directory descriptors anchor rename,
// link and unlink so a parent rename cannot redirect a commit to a symlink.
Descriptor directory(const fs::path& path) {
    auto absolute = fs::absolute(path).lexically_normal(); utf8(absolute);
    Descriptor current(open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (current.value < 0) fail("cannot open filesystem root");
    for (auto& component : absolute.relative_path()) {
        if (component == "." || component.empty()) continue;
        if (component == "..") throw std::runtime_error("OUTPUT_WRITE_FAILED: invalid output parent");
        int next = openat(current.value, component.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (next < 0) fail("cannot securely open output directory");
        close(current.value); current.value = next;
    }
    return current;
}
void flushDirectory(const fs::path& path) {
    auto parent = directory(path);
    if (fsync(parent.value)) fail("cannot flush output directory");
}
void flushFile(const fs::path& path) {
    auto parent = directory(path.parent_path());
    Descriptor file(openat(parent.value, path.filename().c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (file.value < 0 || fsync(file.value)) fail("cannot flush staged file");
#ifdef __APPLE__
    if (fcntl(file.value, F_FULLFSYNC) < 0) fail("cannot flush staged file to stable storage");
#endif
}
int renameFlags(int sourceDir, const fs::path& source, int targetDir, const fs::path& target, bool exchange) {
#ifdef __APPLE__
    return renameatx_np(sourceDir, source.c_str(), targetDir, target.c_str(), exchange ? RENAME_SWAP : RENAME_EXCL);
#elif defined(SYS_renameat2)
    return static_cast<int>(syscall(SYS_renameat2, sourceDir, source.c_str(), targetDir, target.c_str(), exchange ? RENAME_EXCHANGE : RENAME_NOREPLACE));
#else
    (void)sourceDir; (void)source; (void)targetDir; (void)target; (void)exchange;
    errno = ENOTSUP; return -1;
#endif
}
void sameVolume(int left, int right) {
    struct stat a{}, b{};
    if (fstat(left, &a) || fstat(right, &b)) fail("cannot identify output volume");
    if (a.st_dev != b.st_dev) throw std::runtime_error("OUTPUT_WRITE_FAILED: cross-device commit is not permitted");
}
#endif
json journalValue(const fs::path& staged, const fs::path& target, const fs::path& backup,
    const std::string& oldIdentity, const std::string& oldHash, const std::string& newIdentityValue, const std::string& newHash, const std::string& stage) {
    return {{"program", "AnimBridge"}, {"schemaVersion", 1}, {"operation", "replaceFile"}, {"state", stage},
        {"staged", utf8(staged)}, {"target", utf8(target)}, {"backup", utf8(backup)},
        {"oldIdentity", oldIdentity}, {"oldHash", oldHash}, {"newIdentity", newIdentityValue}, {"newHash", newHash}, {"updatedAt", timestampUtc()}};
}
void mark(const fs::path& journal, const json& value) { if (!journal.empty()) FileCommitter::recordJournal(journal, value.dump(2) + '\n'); }
bool exactFile(const fs::path& path, const std::string& identity, const std::string& hash) {
    try { return fs::is_regular_file(path) && fileIdentity(path) == identity && fileSha256(path) == hash; } catch (...) { return false; }
}
}
std::string FileCommitCapabilities::json() const {
    return nlohmann::json{{"localVolume", localVolume}, {"fileNoReplace", fileNoReplace}, {"directoryNoReplace", directoryNoReplace},
        {"hardLink", hardLink}, {"exchange", exchange}, {"flush", flush}, {"volumeIdentity", volumeIdentity}, {"limitation", limitation}}.dump();
}
struct OutputCommitLock::Impl {
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
    ~Impl() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
#else
    int descriptor = -1;
    ~Impl() { if (descriptor >= 0) { flock(descriptor, LOCK_UN); close(descriptor); } }
#endif
};
OutputCommitLock::OutputCommitLock(const fs::path& root) : impl_(std::make_unique<Impl>()) {
    noLinks(root); fs::create_directories(root); noLinks(root);
    auto path = root / ".animbridge-output.lock";
#ifdef _WIN32
    impl_->handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (impl_->handle == INVALID_HANDLE_VALUE) throw std::runtime_error("OUTPUT_BUSY: another writer owns the output directory");
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(impl_->handle, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) throw std::runtime_error("OUTPUT_WRITE_FAILED: invalid output lock");
#else
    auto parent = directory(root);
    impl_->descriptor = openat(parent.value, path.filename().c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (impl_->descriptor < 0) fail("cannot open output lock");
    struct stat info{};
    if (fstat(impl_->descriptor, &info) || !S_ISREG(info.st_mode) || info.st_uid != geteuid() || info.st_nlink != 1) throw std::runtime_error("OUTPUT_WRITE_FAILED: invalid output lock");
    if (flock(impl_->descriptor, LOCK_EX | LOCK_NB)) throw std::runtime_error("OUTPUT_BUSY: another writer owns the output directory");
#endif
}
OutputCommitLock::~OutputCommitLock() = default;
void FileCommitter::flushTree(const fs::path& path) {
    noLinks(path);
    if (fs::is_directory(path)) {
        for (auto& entry : fs::directory_iterator(path)) flushTree(entry.path());
        flushDirectory(path);
    } else if (fs::is_regular_file(path)) flushFile(path);
    else throw std::runtime_error("OUTPUT_WRITE_FAILED: staged output contains a special file");
}
PublishResult FileCommitter::noReplace(const fs::path& staged, const fs::path& target) {
    noLinks(staged); noLinks(target);
    if (staged.filename().empty() || target.filename().empty()) throw std::runtime_error("OUTPUT_WRITE_FAILED: empty publication filename");
    flushTree(staged);
    const auto stagedIdentity = fileIdentity(staged);
#ifdef _WIN32
    if (!MoveFileExW(staged.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) {
        auto error = GetLastError();
        if (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS || (error == ERROR_ACCESS_DENIED && fs::exists(target))) return PublishResult::Exists;
        SetLastError(error); fail("atomic no-replace publication failed");
    }
#else
    auto sourceParent = directory(staged.parent_path()), targetParent = directory(target.parent_path());
    sameVolume(sourceParent.value, targetParent.value);
    if (renameFlags(sourceParent.value, staged.filename(), targetParent.value, target.filename(), false)) {
        if (errno == EEXIST || errno == ENOTEMPTY) return PublishResult::Exists;
        fail("atomic no-replace publication unavailable or failed");
    }
    if (fsync(sourceParent.value) || fsync(targetParent.value)) fail("published output parent flush failed; inspect retained journal");
#endif
    if (fileIdentity(target) != stagedIdentity) throw std::runtime_error("OUTPUT_VERIFICATION_FAILED: published output identity changed; retain journal for manual recovery");
    return PublishResult::Committed;
}
void FileCommitter::writeAtomic(const fs::path& target, const std::string& bytes, bool overwrite) {
    noLinks(target); fs::create_directories(target.parent_path()); noLinks(target);
    auto temporary = target.parent_path() / (".animbridge-write-" + newIdentity() + ".tmp");
#ifdef _WIN32
    RemoveTemporary cleanup{temporary};
    Handle file(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.value == INVALID_HANDLE_VALUE) fail("cannot create unique atomic-write staging");
    cleanup.owned = true;
    std::size_t offset = 0;
    while (offset < bytes.size()) { DWORD written = 0; auto length = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 1 << 20)); if (!WriteFile(file.value, bytes.data() + offset, length, &written, nullptr) || !written) fail("cannot write atomic staging"); offset += written; }
    if (!FlushFileBuffers(file.value)) fail("cannot flush atomic staging");
    CloseHandle(file.value); file.value = INVALID_HANDLE_VALUE;
    try {
        if (!overwrite) { if (noReplace(temporary, target) == PublishResult::Exists) throw std::runtime_error("OUTPUT_CONFLICT: atomic-write target exists"); }
        else if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) fail("cannot replace app-owned atomic-write target");
    } catch (...) { std::error_code ignored; fs::remove(temporary, ignored); throw; }
#else
    auto parent = directory(target.parent_path());
    Descriptor file(openat(parent.value, temporary.filename().c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (file.value < 0) fail("cannot create unique atomic-write staging");
    try {
        std::size_t offset = 0;
        while (offset < bytes.size()) { auto written = write(file.value, bytes.data() + offset, bytes.size() - offset); if (written < 0 && errno == EINTR) continue; if (written <= 0) fail("cannot write atomic staging"); offset += static_cast<std::size_t>(written); }
        if (fsync(file.value)) fail("cannot flush atomic staging");
#ifdef __APPLE__
        if (fcntl(file.value, F_FULLFSYNC) < 0) fail("cannot flush atomic staging to stable storage");
#endif
        close(file.value); file.value = -1;
        if (!overwrite) {
            if (renameFlags(parent.value, temporary.filename(), parent.value, target.filename(), false)) {
                if (errno == EEXIST || errno == ENOTEMPTY) throw std::runtime_error("OUTPUT_CONFLICT: atomic-write target exists");
                fail("cannot publish app-owned atomic-write target");
            }
        } else {
            // Only app-owned state/report files use unconditional atomic replace.
            struct stat status{};
            if (!fstatat(parent.value, target.filename().c_str(), &status, AT_SYMLINK_NOFOLLOW) && !S_ISREG(status.st_mode)) throw std::runtime_error("OUTPUT_WRITE_FAILED: app-owned target is not a regular file");
            if (renameat(parent.value, temporary.filename().c_str(), parent.value, target.filename().c_str())) fail("cannot replace app-owned atomic-write target");
        }
        if (fsync(parent.value)) fail("cannot flush atomic-write parent");
    } catch (...) { unlinkat(parent.value, temporary.filename().c_str(), 0); throw; }
#endif
}
void FileCommitter::recordJournal(const fs::path& path, const std::string& value) { writeAtomic(path, value, true); }
void FileCommitter::replaceFile(const fs::path& staged, const fs::path& target, const fs::path& backup, const fs::path& journal) {
    noLinks(staged); noLinks(target); noLinks(backup);
    if (!fs::is_regular_file(staged) || !fs::is_regular_file(target)) throw std::runtime_error("OUTPUT_CONFLICT: overwrite only supports existing regular files");
    if (backup.empty() || journal.empty()) throw std::runtime_error("OUTPUT_WRITE_FAILED: overwrite requires backup and durable journal");
    fs::create_directories(backup.parent_path()); noLinks(backup);
    auto oldIdentity = fileIdentity(target), oldHash = fileSha256(target), stagedIdentity = fileIdentity(staged), stagedHash = fileSha256(staged);
    if (!exactFile(target, oldIdentity, oldHash)) throw std::runtime_error("OUTPUT_CONFLICT: target changed before overwrite");
    auto state = [&](const std::string& value) { mark(journal, journalValue(staged, target, backup, oldIdentity, oldHash, stagedIdentity, stagedHash, value)); };
    state("staged"); flushTree(staged);
#ifdef _WIN32
    if (fs::exists(backup)) throw std::runtime_error("OUTPUT_CONFLICT: overwrite backup already exists");
    state("backup-pending");
    // ReplaceFileW preserves the existing Windows atomic replacement contract.
    if (!ReplaceFileW(target.c_str(), staged.c_str(), backup.c_str(), REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr)) fail("atomic ReplaceFileW failed; inspect journal and backup");
    flushTree(target); flushTree(backup);
    if (!exactFile(target, stagedIdentity, stagedHash) || !exactFile(backup, oldIdentity, oldHash)) { state("manual-recovery-required"); throw std::runtime_error("OUTPUT_VERIFICATION_FAILED: Windows replacement changed unexpectedly; preserve target and backup for recovery"); }
    state("committed");
#else
    auto sourceParent = directory(staged.parent_path()), targetParent = directory(target.parent_path()), backupParent = directory(backup.parent_path());
    sameVolume(sourceParent.value, targetParent.value); sameVolume(targetParent.value, backupParent.value);
    state("backup-pending");
    if (linkat(targetParent.value, target.filename().c_str(), backupParent.value, backup.filename().c_str(), 0)) fail("cannot create unique same-volume hard-link backup");
    flushTree(backup); if (fsync(backupParent.value)) fail("cannot flush replacement backup directory");
    if (!exactFile(backup, oldIdentity, oldHash) || !exactFile(target, oldIdentity, oldHash)) { state("manual-recovery-required"); throw std::runtime_error("OUTPUT_CONFLICT: target changed while creating backup; exchange was not performed"); }
    state("backup"); state("exchange-pending");
    if (renameFlags(sourceParent.value, staged.filename(), targetParent.value, target.filename(), true)) fail("atomic overwrite exchange unavailable or failed");
    if (fsync(sourceParent.value) || fsync(targetParent.value)) { state("manual-recovery-required"); fail("cannot flush exchanged output"); }
    state("exchanged");
    if (!exactFile(staged, oldIdentity, oldHash) || !exactFile(target, stagedIdentity, stagedHash) || !exactFile(backup, oldIdentity, oldHash)) {
        // Restore only when both names still refer to the exact post-swap inodes.
        // Hash changes in either inode make restoration uncertain and retain all.
        std::string observedIdentity, observedHash;
        try { observedIdentity = fileIdentity(staged); observedHash = fileSha256(staged); } catch (...) {}
        auto rollbackRecord = journalValue(staged, target, backup, oldIdentity, oldHash, stagedIdentity, stagedHash, "rollback-pending");
        rollbackRecord["observedOldIdentity"] = observedIdentity; rollbackRecord["observedOldHash"] = observedHash;
        if (!observedIdentity.empty() && exactFile(staged, observedIdentity, observedHash) && exactFile(target, stagedIdentity, stagedHash)) {
            mark(journal, rollbackRecord);
            if (!renameFlags(sourceParent.value, staged.filename(), targetParent.value, target.filename(), true) && !fsync(sourceParent.value) && !fsync(targetParent.value) && exactFile(target, observedIdentity, observedHash) && exactFile(staged, stagedIdentity, stagedHash)) {
                rollbackRecord["state"] = "rolled-back"; mark(journal, rollbackRecord); throw std::runtime_error("OUTPUT_VERIFICATION_FAILED: exchange verification failed and was rolled back; backup retained");
            }
        }
        state("manual-recovery-required");
        throw std::runtime_error("OUTPUT_VERIFICATION_FAILED: external change prevents safe rollback; staged, target and backup retained for manual recovery");
    }
    state("committed");
    if (unlinkat(sourceParent.value, staged.filename().c_str(), 0) || fsync(sourceParent.value)) fail("replacement committed but old staging link cleanup failed; backup retained");
#endif
}
FileCommitCapabilities FileCommitter::probe(const fs::path& root) {
    FileCommitCapabilities capabilities;
    fs::path owned;
    try {
        noLinks(root); fs::create_directories(root); noLinks(root);
        capabilities.volumeIdentity = fileIdentity(root);
#ifdef _WIN32
        std::wstring volume(MAX_PATH, L'\0'), format(MAX_PATH, L'\0');
        if (!GetVolumePathNameW(root.c_str(), volume.data(), static_cast<DWORD>(volume.size()))) fail("cannot inspect output volume");
        if (GetDriveTypeW(volume.c_str()) == DRIVE_FIXED && GetVolumeInformationW(volume.c_str(), nullptr, 0, nullptr, nullptr, nullptr, format.data(), static_cast<DWORD>(format.size())))
            capabilities.localVolume = std::wstring(format.c_str()) == L"NTFS" || std::wstring(format.c_str()) == L"ReFS";
#else
        auto descriptor = directory(root);
        struct statfs filesystem{};
        if (fstatfs(descriptor.value, &filesystem)) fail("cannot inspect output filesystem");
#ifdef __APPLE__
        capabilities.localVolume = (filesystem.f_flags & MNT_LOCAL) && (std::strcmp(filesystem.f_fstypename, "apfs") == 0 || std::strcmp(filesystem.f_fstypename, "hfs") == 0);
#else
        // Local filesystems exercised by CI plus native ext4/xfs/btrfs. Network,
        // fuse, FAT/exFAT and unknown filesystems remain explicitly unsupported.
        capabilities.localVolume = filesystem.f_type == 0xef53 || filesystem.f_type == 0x58465342 || filesystem.f_type == 0x9123683e || filesystem.f_type == 0x01021994 || filesystem.f_type == 0x794c7630;
#endif
#endif
        if (!capabilities.localVolume) { capabilities.limitation = "Only verified local NTFS/ReFS/APFS/HFS+/ext4/xfs/btrfs/tmpfs/overlay output volumes are enabled; network and unknown volumes require separate validation"; return capabilities; }
        owned = root / (".animbridge-probe-" + newIdentity());
        if (!fs::create_directory(owned)) throw std::runtime_error("Cannot allocate output capability probe");
        writeAtomic(owned / ".animbridge-owner", "AnimBridge output capability probe\n");
        writeAtomic(owned / "source", "source"); writeAtomic(owned / "occupied", "occupied");
        auto before = fileSha256(owned / "occupied");
        auto conflict = noReplace(owned / "source", owned / "occupied");
        capabilities.fileNoReplace = conflict == PublishResult::Exists && fs::exists(owned / "source") && fileSha256(owned / "occupied") == before && noReplace(owned / "source", owned / "published") == PublishResult::Committed;
        fs::create_directory(owned / "directory"); fs::create_directory(owned / "occupied-directory");
        capabilities.directoryNoReplace = noReplace(owned / "directory", owned / "occupied-directory") == PublishResult::Exists && fs::is_directory(owned / "directory") && noReplace(owned / "directory", owned / "published-directory") == PublishResult::Committed;
        flushTree(owned); capabilities.flush = true;
        std::error_code linkError; fs::create_hard_link(owned / "published", owned / "hard-link", linkError);
        capabilities.hardLink = !linkError && sameFile(owned / "published", owned / "hard-link");
#ifdef _WIN32
        capabilities.exchange = false; // ReplaceFileW is the Windows overwrite API.
#else
        auto parent = directory(owned);
        auto first = fileIdentity(owned / "published"), second = fileIdentity(owned / "occupied");
        capabilities.exchange = !renameFlags(parent.value, "published", parent.value, "occupied", true) && fileIdentity(owned / "published") == second && fileIdentity(owned / "occupied") == first;
#endif
        if (!capabilities.fileNoReplace || !capabilities.directoryNoReplace || !capabilities.flush) capabilities.limitation = "Required atomic publication/flush primitive is unavailable on this output volume";
        else if (!capabilities.hardLink ||
#ifndef _WIN32
            !capabilities.exchange ||
#endif
            false) capabilities.limitation = "Explicit overwrite is unavailable; use fail, skip or rename";
    } catch (const std::exception& error) { capabilities.limitation = error.what(); }
    if (!owned.empty()) { std::error_code ignored; removeOwnedTree(owned, root, ignored); }
    return capabilities;
}
std::string FileCommitter::recoveryEntries(const fs::path& journalRoot, const fs::path& outputRoot) {
    json entries = json::array();
    std::error_code error;
    if (fs::is_directory(journalRoot, error)) for (fs::recursive_directory_iterator i(journalRoot, fs::directory_options::skip_permission_denied, error), end; !error && i != end; i.increment(error)) {
        try {
            if (fs::is_symlink(i->symlink_status())) { i.disable_recursion_pending(); continue; }
            if (!i->is_regular_file() || i->path().extension() != ".json" || i->file_size() > 1024 * 1024) continue;
            std::ifstream file(i->path(), std::ios::binary); auto record = json::parse(file);
            if (record.value("program", "") != "AnimBridge" || record.value("schemaVersion", 0) != 1) continue;
            auto target = fromUtf8(record.value("target", ""));
            if (target.empty() || !pathContains(outputRoot, target)) continue;
            auto state = record.value("state", ""); if (state == "committed" || state == "rolled-back" || state == "skipped") continue;
            record["journal"] = utf8(i->path()); record["recoveryAction"] = "manual-review";
            // Recovery is evidence-only: never delete/swap an ambiguous external
            // path automatically merely because an old journal names it.
            for (auto field : {"staged", "target", "backup"}) if (record.contains(field) && record[field].is_string()) {
                auto path = fromUtf8(record[field].get<std::string>());
                if (!path.empty() && fs::exists(path)) record["observed"][field] = {{"identity", fileIdentity(path)}, {"isFile", fs::is_regular_file(path)}};
            }
            entries.push_back(std::move(record));
        } catch (const std::exception&) {
            // Corrupt journals are visible recovery evidence, never instructions
            // to perform filesystem changes.
            try { entries.push_back({{"program", "AnimBridge"}, {"operation", "unreadableJournal"}, {"journal", utf8(i->path())}, {"state", "manual-recovery-required"}, {"recoveryAction", "manual-review"}}); } catch (...) {}
        }
    }
    error.clear();
    if (fs::is_directory(outputRoot, error)) for (fs::directory_iterator i(outputRoot, fs::directory_options::skip_permission_denied, error), end; !error && i != end; i.increment(error)) {
        try {
            if (!i->is_directory() || fs::is_symlink(i->symlink_status()) || !utf8(i->path().filename()).starts_with(".animbridge-tmp-")) continue;
            auto marker = i->path() / ".animbridge-owner.json";
            noLinks(marker);
            if (!fs::is_regular_file(marker) || fs::file_size(marker) > 65536) continue;
            std::ifstream file(marker, std::ios::binary); auto ownership = json::parse(file);
            if (ownership.value("program", "") != "AnimBridge" || !sameFile(fromUtf8(ownership.value("outputRoot", "")), outputRoot)) continue;
            auto attempt = ownership.value("attemptId", "");
            if (attempt.size() != 32 || attempt.find_first_not_of("0123456789abcdef") != std::string::npos) continue;
            entries.push_back({{"program", "AnimBridge"}, {"schemaVersion", 1}, {"operation", "retainedStaging"}, {"state", "interrupted-staging"}, {"attemptId", attempt},
                {"staging", utf8(i->path())}, {"marker", utf8(marker)}, {"identity", fileIdentity(i->path())}, {"recoveryAction", "manual-review"}});
        } catch (const std::exception&) { /* Unknown output directories stay untouched. */ }
    }
    return entries.dump();
}
}
