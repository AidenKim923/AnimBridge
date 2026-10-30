#pragma once
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace animbridge {
struct FileCommitCapabilities {
    bool localVolume = false, fileNoReplace = false, directoryNoReplace = false;
    bool hardLink = false, exchange = false, flush = false;
    std::string volumeIdentity, limitation;
    std::string json() const;
};
enum class PublishResult { Committed, Exists };
// Serializes writers which cooperate with AnimBridge. It does not claim to lock
// arbitrary programs; commit rechecks identities and fails closed on changes.
class OutputCommitLock {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    explicit OutputCommitLock(const std::filesystem::path& root);
    ~OutputCommitLock();
    OutputCommitLock(const OutputCommitLock&) = delete;
    OutputCommitLock& operator=(const OutputCommitLock&) = delete;
};
class FileCommitter {
public:
    static FileCommitCapabilities probe(const std::filesystem::path& outputRoot);
    static PublishResult noReplace(const std::filesystem::path& staged, const std::filesystem::path& target);
    // Durable unique temp + atomic publication. Existing bytes are only replaced
    // when overwrite is authorized. This is for app-owned reports/state.
    static void writeAtomic(const std::filesystem::path& target, const std::string& bytes, bool overwrite = false);
    // Explicit user-file overwrite. backup must be unique and on target's volume.
    // journal is persisted before every mutating stage; uncertain recovery keeps
    // all evidence and is never converted to success.
    static void replaceFile(const std::filesystem::path& staged, const std::filesystem::path& target,
        const std::filesystem::path& backup, const std::filesystem::path& journal);
    static void flushTree(const std::filesystem::path& path);
    static void recordJournal(const std::filesystem::path& path, const std::string& value);
    static std::string recoveryEntries(const std::filesystem::path& journalRoot, const std::filesystem::path& outputRoot);
};
}
