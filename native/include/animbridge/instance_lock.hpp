#pragma once
#include <filesystem>
#include <memory>
#include <string>

namespace animbridge {
// Shared by GUI and CLI. Its OS handle is deliberately non-inheritable.
std::string instanceKey(const std::filesystem::path& dataRoot);
class InstanceLock {
public:
    explicit InstanceLock(const std::filesystem::path& dataRoot);
    ~InstanceLock();
    InstanceLock(const InstanceLock&) = delete;
    InstanceLock& operator=(const InstanceLock&) = delete;
    bool acquired() const noexcept;
    const std::string& diagnostic() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
