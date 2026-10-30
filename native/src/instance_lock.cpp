#include "animbridge/instance_lock.hpp"
#include "animbridge/core.hpp"
#include <fstream>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#ifdef __APPLE__
#include <sys/mount.h>
#else
#include <sys/vfs.h>
#endif
#endif

namespace animbridge {
std::string instanceKey(const fs::path& dataRoot) {
    auto canonical = fs::weakly_canonical(fs::absolute(dataRoot)).make_preferred();
#ifdef _WIN32
    auto text = canonical.wstring();
    if (!text.empty()) CharLowerBuffW(text.data(), static_cast<DWORD>(text.size()));
    return textSha256(utf8(fs::path(text)));
#else
    return textSha256(utf8(canonical));
#endif
}
struct InstanceLock::Impl {
    bool owns = false; std::string detail;
#ifdef _WIN32
    HANDLE handle = nullptr;
    ~Impl() { if (handle) CloseHandle(handle); }
#else
    int fd = -1;
    ~Impl() { if (fd >= 0) close(fd); }
#endif
};
InstanceLock::InstanceLock(const fs::path& dataRoot) : impl_(std::make_unique<Impl>()) {
    auto root = fs::weakly_canonical(fs::absolute(dataRoot));
    fs::create_directories(root);
#ifdef _WIN32
    if ((root.native().starts_with(L"\\\\") && !root.native().starts_with(L"\\\\?\\")) || root.native().starts_with(L"\\\\?\\UNC\\")) throw std::runtime_error("Instance state requires a local data root");
    if (GetDriveTypeW(root.root_path().c_str()) == DRIVE_REMOTE) throw std::runtime_error("Instance state requires a local data root");
    auto name = L"Local\\AnimBridge.Desktop." + fromUtf8(instanceKey(root)).wstring();
    SetLastError(ERROR_SUCCESS); impl_->handle = CreateMutexW(nullptr, FALSE, name.c_str()); auto code = GetLastError();
    if (!impl_->handle) throw std::runtime_error("Cannot create data-root instance mutex (" + std::to_string(code) + ")");
    impl_->owns = code != ERROR_ALREADY_EXISTS;
    auto ownerPath = root / ".animbridge-instance-owner.txt";
    if (impl_->owns) {
        auto owner = "pid=" + std::to_string(GetCurrentProcessId()) + " started=" + timestampUtc() + " root=" + utf8(root);
        auto temporary = root / (".animbridge-instance-owner-" + newIdentity() + ".tmp");
        { std::ofstream file(temporary, std::ios::binary); file << owner; file.flush(); if (!file) throw std::runtime_error("Cannot record the data-root lock owner"); }
        if (!MoveFileExW(temporary.c_str(), ownerPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { fs::remove(temporary); throw std::runtime_error("Cannot publish the data-root lock owner"); }
        impl_->detail = owner;
    } else {
        impl_->detail = "AnimBridge data root is busy: " + utf8(root);
        std::ifstream owner(ownerPath, std::ios::binary); std::string text; std::getline(owner, text);
        if (!text.empty() && text.size() <= 4096) impl_->detail += " (" + text + ")";
        else impl_->detail += " (owner unavailable)";
    }
#else
    // Only local, same-user directories are supported. Never unlink this file:
    // a second inode could otherwise allow two simultaneous writers.
    struct stat st{};
    struct statfs filesystem{};
    if (statfs(root.c_str(), &filesystem)) throw std::runtime_error("Cannot inspect the data-root filesystem");
#ifdef __APPLE__
    if (!(filesystem.f_flags & MNT_LOCAL)) throw std::runtime_error("Instance state requires a local data-root filesystem");
#else
    if (static_cast<unsigned long>(filesystem.f_type) == 0x6969ul || static_cast<unsigned long>(filesystem.f_type) == 0xff534d42ul || static_cast<unsigned long>(filesystem.f_type) == 0xfe534d42ul)
        throw std::runtime_error("NFS/SMB data roots are unsupported; choose a local data root");
#endif
    if (stat(root.c_str(), &st) || st.st_uid != getuid()) throw std::runtime_error("Data root must belong to the current user");
    if (chmod(root.c_str(), st.st_mode & ~0077)) throw std::runtime_error("Cannot protect the data root");
    impl_->fd = open((root / ".animbridge-instance.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (impl_->fd < 0) throw std::runtime_error("Cannot open the data-root lock: " + std::string(std::strerror(errno)));
    if (fstat(impl_->fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != getuid() || st.st_nlink != 1) throw std::runtime_error("Unsafe data-root lock file");
    if (flock(impl_->fd, LOCK_EX | LOCK_NB)) {
        if (errno != EWOULDBLOCK && errno != EAGAIN) throw std::runtime_error("Cannot acquire data-root lock");
        char buffer[2048]{}; auto length = pread(impl_->fd, buffer, sizeof(buffer) - 1, 0);
        impl_->detail = "AnimBridge data root is busy: " + utf8(root);
        if (length > 0) impl_->detail += " (" + std::string(buffer, static_cast<std::size_t>(length)) + ")";
        return;
    }
    impl_->owns = true;
    auto owner = "pid=" + std::to_string(getpid()) + " started=" + timestampUtc() + " root=" + utf8(root);
    if (ftruncate(impl_->fd, 0) || pwrite(impl_->fd, owner.data(), owner.size(), 0) != static_cast<ssize_t>(owner.size()) || fsync(impl_->fd)) throw std::runtime_error("Cannot record data-root lock owner");
    impl_->detail = owner;
#endif
}
InstanceLock::~InstanceLock() = default;
bool InstanceLock::acquired() const noexcept { return impl_->owns; }
const std::string& InstanceLock::diagnostic() const noexcept { return impl_->detail; }
}
