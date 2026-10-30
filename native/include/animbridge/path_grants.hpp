#pragma once
#include "core.hpp"
#include <nlohmann/json.hpp>
#include <mutex>
#include <map>

namespace animbridge {
enum class GrantPurpose { Read, ExportRoot, Editor, EngineProject };
// AppOutput marks results the app produced itself inside its own cache (for
// example an Unreal .uasset conversion), never a user-chosen location.
enum class GrantSource { Picker, NativeDrop, NativeDocument, CommandLine, AutoDiscovered, AppOutput };
// Tokens never persist. Restored approvals are rechecked and get fresh tokens.
class PathGrantRegistry {
public:
    using Json = nlohmann::json;
    explicit PathGrantRegistry(fs::path probeRoot = {}) : probeRoot_(std::move(probeRoot)) {}
    Json issue(const fs::path&, GrantPurpose, GrantSource, const std::string& engine = {}, const std::string& targetFolder = {});
    fs::path resolve(const std::string& token, GrantPurpose, const fs::path& child = {}) const;
    bool permits(const fs::path&, GrantPurpose) const;
    Json records() const;
    Json publicRecords() const;
    Json restore(const Json&);
    void approveTarget(const std::string& token, const std::string& engine, const std::string& targetFolder) const;
private:
    struct Grant { std::string token; Json record; };
    mutable std::mutex mutex_;
    std::map<std::string, Grant> grants_;
    fs::path probeRoot_;
};
bool pathWithin(const fs::path&, const fs::path&);
void writeDesktopJson(const fs::path&, const nlohmann::json&);
}
