#pragma once
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace animbridge {
struct RuntimePaths {
    std::filesystem::path bundleRoot, backend, ui, python, blender, webView2;
    std::filesystem::path supervisor, bootstrap, manifest;
    std::string targetId, architecture;
    bool bundled = false;
    bool manifestValidated = false;
    bool development = false;
};
// Resolution never depends on the caller's current directory. An existing backend
// directory marks an installed bundle, even when one of its resources is missing.
RuntimePaths resolveRuntimePaths(const std::filesystem::path& executable,
                                const std::filesystem::path& sourceFallback = {},
                                const std::string& expectedTarget = {});
const RuntimePaths& runtimePaths();
bool developmentMode();
std::filesystem::path executablePath();
std::filesystem::path defaultDataRoot();
std::filesystem::path defaultExportRoot();
std::string runtimeTargetId();
struct RuntimeContext {
    std::filesystem::path dataRoot, cacheRoot, stateRoot, logRoot, exportRoot, tempRoot;
};
RuntimeContext resolveRuntimeContext(const std::filesystem::path& dataOverride = {});
// Release callers must reject overrides before planning or launching work.
void validateRuntimeOverrides(const std::filesystem::path& backend,
                              const std::filesystem::path& python,
                              const std::filesystem::path& blender);
std::vector<std::pair<std::string, std::string>> processEnvironment(
    const std::vector<std::pair<std::string, std::string>>& trusted = {});
void clearBrowserEnvironmentOverrides();
std::vector<std::pair<std::string, std::string>> pythonEnvironment(
    const std::filesystem::path& backend, const std::filesystem::path& workRoot);
std::vector<std::pair<std::string, std::string>> pythonEnvironment(
    const std::filesystem::path& backend, const std::filesystem::path& workRoot,
    const RuntimeContext& context);
std::vector<std::string> pythonArguments(const std::vector<std::string>& arguments);
std::vector<std::string> pythonArguments(const std::vector<std::string>& arguments,
                                       const std::filesystem::path& backend);
// Only call after establishing ownership of the complete temporary tree.
// Windows cleanup uses extended paths even when system long paths are disabled.
std::uintmax_t removeOwnedTree(const std::filesystem::path& root, const std::filesystem::path& owner, std::error_code& error);
}
