#include "animbridge/runtime.hpp"
#include "animbridge/core.hpp"
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#else
#include <unistd.h>
#include <pwd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <cerrno>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif
#ifndef _WIN32
extern char** environ;
#endif

namespace animbridge {
namespace {
bool inside(const fs::path& child, const fs::path& parent, bool equal = false) {
    auto a = child.begin();
    for (auto b = parent.begin(); b != parent.end(); ++a, ++b) {
        if (a == child.end()) return false;
#ifdef _WIN32
        if (_wcsicmp(a->c_str(), b->c_str())) return false;
#else
        if (*a != *b) return false;
#endif
    }
    return equal || a != child.end();
}
fs::path resolved(const fs::path& value) { return fs::weakly_canonical(fs::absolute(value)); }
fs::path manifestPath(const fs::path& root, const std::string& text, bool directory) {
    auto value = fromUtf8(text);
    if (text.empty() || value.is_absolute() || value.has_root_name() || text.find('\\') != std::string::npos)
        throw std::runtime_error("Runtime manifest paths must be relative portable paths");
    for (auto& part : value) if (part == ".." || part == ".") throw std::runtime_error("Runtime manifest path traversal rejected");
    auto path = resolved(root / value);
    if (!inside(path, resolved(root))) throw std::runtime_error("Runtime manifest resource escapes the bundle");
    if (directory ? !fs::is_directory(path) : !fs::is_regular_file(path)) throw std::runtime_error("Missing runtime manifest resource: " + text);
    return path;
}
#ifdef _WIN32
fs::path knownFolder(REFKNOWNFOLDERID id) {
    PWSTR value = nullptr; auto result = SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &value);
    if (FAILED(result) || !value) throw std::runtime_error("Cannot locate the Windows user folder");
    fs::path path(value); CoTaskMemFree(value); return path;
}
#else
fs::path home() {
    auto value = std::getenv("HOME");
    if (value && *value && fromUtf8(value).is_absolute()) return resolved(fromUtf8(value));
    auto entry = getpwuid(getuid());
    if (entry && entry->pw_dir && fs::path(entry->pw_dir).is_absolute()) return resolved(fs::path(entry->pw_dir));
    throw std::runtime_error("Cannot locate an absolute user home directory");
}
fs::path xdg(const char* name, const fs::path& fallback) {
    auto value = std::getenv(name);
    return value && *value && fromUtf8(value).is_absolute() ? resolved(fromUtf8(value)) : fallback;
}
#endif
std::string upperAscii(std::string value) { for (auto& c : value) if (c >= 'a' && c <= 'z') c -= 'a' - 'A'; return value; }
bool unsafeEnvironment(const std::string& key) {
    auto name = upperAscii(key);
    for (auto prefix : {"PYTHON", "UAB_", "LD_", "DYLD_", "WEBKIT_", "WEBVIEW2_"}) if (name.starts_with(prefix)) return true;
    return false;
}
#ifndef _WIN32
struct DirectoryFd {
    int value = -1;
    explicit DirectoryFd(int fd) : value(fd) {}
    ~DirectoryFd() { if (value >= 0) close(value); }
};
int openDirectoryPath(const fs::path& absolute, std::error_code& error) {
    int fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) { error = {errno, std::generic_category()}; return -1; }
    for (auto& component : absolute.relative_path()) {
        auto next = openat(fd, component.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        auto code = errno; close(fd);
        if (next < 0) { error = {code, std::generic_category()}; return -1; }
        fd = next;
    }
    return fd;
}
std::uintmax_t removeTreeAt(int parent, const fs::path& name, dev_t device, std::error_code& error) {
    struct stat initial{};
    if (fstatat(parent, name.c_str(), &initial, AT_SYMLINK_NOFOLLOW)) {
        if (errno != ENOENT) error = {errno, std::generic_category()};
        return 0;
    }
    if (!S_ISDIR(initial.st_mode)) {
        if (unlinkat(parent, name.c_str(), 0)) { error = {errno, std::generic_category()}; return 0; }
        return 1;
    }
    if (initial.st_dev != device) { error = std::make_error_code(std::errc::cross_device_link); return 0; }
    DirectoryFd child(openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    struct stat opened{};
    if (child.value < 0 || fstat(child.value, &opened)) { error = {errno, std::generic_category()}; return 0; }
    if (opened.st_dev != initial.st_dev || opened.st_ino != initial.st_ino) { error = std::make_error_code(std::errc::state_not_recoverable); return 0; }
    auto duplicate = fcntl(child.value, F_DUPFD_CLOEXEC, 0);
    if (duplicate < 0) { error = {errno, std::generic_category()}; return 0; }
    DIR* listing = fdopendir(duplicate);
    if (!listing) { auto code = errno; close(duplicate); error = {code, std::generic_category()}; return 0; }
    std::uintmax_t count = 0;
    for (;;) {
        errno = 0; auto entry = readdir(listing);
        if (!entry) { if (errno) error = {errno, std::generic_category()}; break; }
        if (!std::strcmp(entry->d_name, ".") || !std::strcmp(entry->d_name, "..")) continue;
        count += removeTreeAt(child.value, fs::path(entry->d_name), device, error);
        if (error) break;
    }
    closedir(listing);
    if (error) return count;
    struct stat current{};
    if (fstatat(parent, name.c_str(), &current, AT_SYMLINK_NOFOLLOW)) { error = {errno, std::generic_category()}; return count; }
    if (current.st_dev != opened.st_dev || current.st_ino != opened.st_ino || !S_ISDIR(current.st_mode)) { error = std::make_error_code(std::errc::state_not_recoverable); return count; }
    if (unlinkat(parent, name.c_str(), AT_REMOVEDIR)) { error = {errno, std::generic_category()}; return count; }
    return count + 1;
}
#endif
}
std::string runtimeTargetId() {
#ifdef _WIN32
    return "windows-x64";
#elif defined(__APPLE__)
    return "macos-arm64";
#else
    // Both Ubuntu targets have the same executable ABI; a manifest still carries
    // its exact release target. The caller can require the distro-specific ID.
    return "linux-x64";
#endif
}
RuntimePaths resolveRuntimePaths(const fs::path& executable, const fs::path& sourceFallback, const std::string& expectedTarget) {
    RuntimePaths paths;
    auto exe = fs::absolute(executable).lexically_normal();
    auto target = expectedTarget.empty() ? runtimeTargetId() : expectedTarget;
    bool mac = target.starts_with("macos-"), isLinux = target.starts_with("linux-");
    paths.bundleRoot = exe.parent_path();
    if (mac && exe.parent_path().filename() == "MacOS" && exe.parent_path().parent_path().filename() == "Contents") paths.bundleRoot = exe.parent_path().parent_path();
    else if (isLinux && exe.parent_path().filename() == "bin") paths.bundleRoot = exe.parent_path().parent_path();
    auto resources = mac ? paths.bundleRoot / "Resources" : isLinux ? paths.bundleRoot / "share" : paths.bundleRoot;
    paths.manifest = resources / "runtime-manifest.json";
    if (!fs::is_regular_file(paths.manifest) && fs::is_regular_file(paths.bundleRoot / "runtime-manifest.json")) paths.manifest = paths.bundleRoot / "runtime-manifest.json";
    paths.architecture = target.starts_with("macos-") ? "arm64" : "x64"; paths.targetId = target;
    if (fs::exists(paths.manifest)) {
        if (!inside(resolved(paths.manifest), resolved(paths.bundleRoot))) throw std::runtime_error("Runtime manifest escapes the bundle");
        auto manifest = nlohmann::json::parse(std::ifstream(paths.manifest));
        if (manifest.at("schemaVersion") != 1 || !manifest.at("appVersion").is_string()) throw std::runtime_error("Unsupported runtime manifest schema");
        paths.targetId = manifest.at("targetId").get<std::string>();
        if (paths.targetId != target && !(target == "linux-x64" && (paths.targetId == "linux-x64-ubuntu22.04" || paths.targetId == "linux-x64-ubuntu24.04")))
            throw std::runtime_error("Runtime manifest target does not match this executable");
        if (manifest.at("architecture") != paths.architecture) throw std::runtime_error("Runtime manifest architecture mismatch");
        auto& components = manifest.at("components");
        auto component = [&](const char* name, bool directory) {
            auto& entry = components.at(name);
            if (!entry.at("version").is_string() || entry.at("version").get<std::string>().empty() || entry.at("architecture") != paths.architecture)
                throw std::runtime_error(std::string("Invalid runtime component metadata: ") + name);
            return manifestPath(paths.bundleRoot, entry.at(directory ? "resourcePath" : "executablePath").get<std::string>(), directory);
        };
        paths.backend = component("backend", true); paths.ui = component("ui", true);
        paths.python = component("python", false); paths.blender = component("blender", false); paths.supervisor = component("supervisor", false);
        // bootstrap is a resource file (rather than an executable component).
        auto& boot = components.at("bootstrap");
        if (boot.at("architecture") != paths.architecture || !boot.at("version").is_string()) throw std::runtime_error("Invalid bootstrap metadata");
        paths.bootstrap = manifestPath(paths.bundleRoot, boot.at("resourcePath").get<std::string>(), false);
        if (!fs::is_regular_file(paths.backend / "src/unityanimbridge/__init__.py") || !fs::is_regular_file(paths.ui / "index.html")) throw std::runtime_error("Incomplete runtime backend or UI payload");
        if (!mac && !isLinux) {
            paths.webView2 = component("webview2", true);
            if (!fs::is_regular_file(paths.webView2 / "msedgewebview2.exe")) throw std::runtime_error("Missing fixed WebView2 executable");
        }
        paths.bundled = paths.manifestValidated = true; return paths;
    }
    // Existing Windows installers predate the manifest. Their presence pins all
    // runtime paths locally, including absent components, and never falls back.
    paths.bundled = fs::is_directory(resources / "backend");
    if ((mac || isLinux) && paths.bundled) throw std::runtime_error("Missing runtime-manifest.json in an installed bundle");
    paths.backend = resources / "backend"; paths.ui = resources / "ui";
    if (!paths.bundled && !sourceFallback.empty() && fs::is_directory(sourceFallback / "src/unityanimbridge")) {
        paths.development = true;
        paths.backend = resolved(sourceFallback);
        if (!fs::is_regular_file(paths.ui / "index.html")) paths.ui = paths.backend / "native/ui";
    } else if ((mac || isLinux) && !paths.bundled) {
        throw std::runtime_error("Missing runtime-manifest.json; source fallback is unavailable");
    }
    auto runtime = mac ? resources / "runtime" : paths.bundleRoot / "runtime";
    auto python = runtime / (mac || isLinux ? "python/bin/python3" : "python/python.exe");
    paths.python = paths.bundled || fs::is_regular_file(python) ? python : fs::path(mac || isLinux ? "python3" : "python");
    if (paths.development) {
        auto privateDevelopmentPython = paths.backend / (mac || isLinux ? ".venv/bin/python" : ".venv/Scripts/python.exe");
        if (fs::is_regular_file(privateDevelopmentPython)) paths.python = fs::absolute(privateDevelopmentPython).lexically_normal();
    }
    paths.blender = paths.bundled ? runtime / (mac ? "blender/Blender.app/Contents/MacOS/Blender" : isLinux ? "blender/blender" : "blender/blender.exe") : fs::path{};
    paths.supervisor = exe.parent_path() / (mac || isLinux ? "AnimBridgeProcessSupervisor" : "AnimBridgeProcessSupervisor.exe");
    paths.bootstrap = paths.backend / (paths.development ? "src/unityanimbridge/bootstrap.py" : "bootstrap.py");
    if (!mac && !isLinux && fs::is_regular_file(runtime / "webview2/msedgewebview2.exe")) paths.webView2 = runtime / "webview2";
    return paths;
}
fs::path executablePath() {
#ifdef _WIN32
    std::wstring buffer(32768, L'\0');
    auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!length || length >= buffer.size()) throw std::runtime_error("Cannot locate the AnimBridge executable");
    buffer.resize(length); return fs::path(buffer);
#elif defined(__APPLE__)
    uint32_t size = 0; _NSGetExecutablePath(nullptr, &size); std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size)) throw std::runtime_error("Cannot locate the macOS executable");
    buffer.resize(std::char_traits<char>::length(buffer.c_str())); return fs::canonical(fromUtf8(buffer));
#else
    std::string buffer(32768, '\0'); auto length = readlink("/proc/self/exe", buffer.data(), buffer.size());
    if (length <= 0 || static_cast<std::size_t>(length) >= buffer.size()) throw std::runtime_error("Cannot locate the AnimBridge executable via /proc/self/exe");
    buffer.resize(static_cast<std::size_t>(length)); return fs::canonical(fromUtf8(buffer));
#endif
}
const RuntimePaths& runtimePaths() {
    static const auto paths = resolveRuntimePaths(executablePath(),
#ifdef ANIMBRIDGE_SOURCE_FALLBACK
        fs::path(ANIMBRIDGE_SOURCE_FALLBACK)
#else
        fs::path{}
#endif
    ); return paths;
}
bool developmentMode() { return runtimePaths().development; }
RuntimeContext resolveRuntimeContext(const fs::path& dataOverride) {
    RuntimeContext context;
    if (!dataOverride.empty()) {
        context.dataRoot = resolved(dataOverride); context.cacheRoot = context.dataRoot / "cache";
        context.stateRoot = context.dataRoot; context.logRoot = context.dataRoot / "logs";
    } else {
#ifdef _WIN32
        context.dataRoot = knownFolder(FOLDERID_LocalAppData) / "AnimBridge";
        context.cacheRoot = context.dataRoot / "cache"; context.stateRoot = context.dataRoot; context.logRoot = context.dataRoot / "logs";
#elif defined(__APPLE__)
        context.dataRoot = home() / "Library/Application Support/AnimBridge";
        context.cacheRoot = home() / "Library/Caches/AnimBridge";
        context.stateRoot = context.dataRoot; context.logRoot = home() / "Library/Logs/AnimBridge";
#else
        context.dataRoot = xdg("XDG_DATA_HOME", home() / ".local/share") / "AnimBridge";
        context.cacheRoot = xdg("XDG_CACHE_HOME", home() / ".cache") / "AnimBridge";
        context.stateRoot = xdg("XDG_STATE_HOME", home() / ".local/state") / "AnimBridge";
        context.logRoot = context.stateRoot / "logs";
#endif
    }
    context.tempRoot = context.cacheRoot / "jobs/tmp";
#ifdef _WIN32
    context.exportRoot = knownFolder(FOLDERID_Documents) / "AnimBridge";
#else
    context.exportRoot = home() / "Documents/AnimBridge";
#endif
    context.dataRoot = resolved(context.dataRoot); context.cacheRoot = resolved(context.cacheRoot);
    context.stateRoot = resolved(context.stateRoot); context.logRoot = resolved(context.logRoot);
    context.tempRoot = resolved(context.tempRoot); context.exportRoot = resolved(context.exportRoot);
    return context;
}
fs::path defaultDataRoot() { return resolveRuntimeContext().dataRoot; }
fs::path defaultExportRoot() { return resolveRuntimeContext().exportRoot; }
void validateRuntimeOverrides(const fs::path& backend, const fs::path& python, const fs::path& blender) {
    auto& runtime = runtimePaths();
    if (runtime.development) return;
    auto matches = [](const fs::path& a, const fs::path& b) { return !a.empty() && !b.empty() && resolved(a) == resolved(b); };
    if (!matches(backend, runtime.backend) || !matches(python, runtime.python) || (!blender.empty() && !matches(blender, runtime.blender)))
        throw std::runtime_error("Release mode rejects external --repo, --python and --blender overrides");
}
std::vector<std::pair<std::string, std::string>> processEnvironment(const std::vector<std::pair<std::string, std::string>>& trusted) {
    std::map<std::string, std::string> values;
    for (auto key : {"SystemRoot", "WINDIR", "COMSPEC", "PROGRAMDATA", "ALLUSERSPROFILE", "USERPROFILE", "LOCALAPPDATA", "APPDATA", "HOME", "LANG", "LC_ALL", "LC_CTYPE", "DISPLAY", "WAYLAND_DISPLAY", "XAUTHORITY", "DBUS_SESSION_BUS_ADDRESS", "XDG_RUNTIME_DIR"}) {
        auto value = std::getenv(key); if (!value || !*value) continue;
        std::string text(value);
        if (text.find('\n') != std::string::npos || text.find('\r') != std::string::npos) continue;
        if ((std::string(key) == "XDG_RUNTIME_DIR" || std::string(key) == "XAUTHORITY") && !fromUtf8(text).is_absolute()) continue;
        values[key] = text;
    }
#ifdef _WIN32
    auto root = values.contains("SystemRoot") ? values["SystemRoot"] : "C:\\Windows";
    values["PATH"] = root + "\\System32;" + root;
#else
    values["PATH"] = "/usr/bin:/bin:/usr/sbin:/sbin";
#endif
    for (auto& [key, value] : trusted) {
        if (key.empty() || key.find_first_of("=\0", 0, 2) != std::string::npos || value.find('\0') != std::string::npos) throw std::runtime_error("Invalid child environment entry");
        values[key] = value;
    }
    return {values.begin(), values.end()};
}
void clearBrowserEnvironmentOverrides() {
#ifdef _WIN32
    auto all = GetEnvironmentStringsW(); if (!all) throw std::runtime_error("Cannot inspect browser environment");
    std::vector<std::wstring> keys;
    for (auto p = all; *p; p += wcslen(p) + 1) { std::wstring entry(p); auto at = entry.find(L'='); if (at != std::wstring::npos && at) keys.push_back(entry.substr(0, at)); }
    FreeEnvironmentStringsW(all);
    for (auto& key : keys) if (unsafeEnvironment(utf8(fs::path(key)))) SetEnvironmentVariableW(key.c_str(), nullptr);
#else
    std::vector<std::string> keys;
    for (auto p = ::environ; p && *p; ++p) { std::string entry(*p); auto at = entry.find('='); if (at != std::string::npos && unsafeEnvironment(entry.substr(0, at))) keys.push_back(entry.substr(0, at)); }
    for (auto& key : keys) unsetenv(key.c_str());
#endif
}
std::vector<std::pair<std::string, std::string>> pythonEnvironment(const fs::path& backend, const fs::path& workRoot) {
    return pythonEnvironment(backend, workRoot, resolveRuntimeContext());
}
std::vector<std::pair<std::string, std::string>> pythonEnvironment(const fs::path& backend, const fs::path& workRoot, const RuntimeContext& context) {
    auto work = resolved(workRoot);
    std::vector<std::pair<std::string, std::string>> environment = {{"UAB_BACKEND_DIR", utf8(resolved(backend))}, {"UAB_WORK_DIR", utf8(work)},
        {"UAB_DATA_DIR", utf8(context.dataRoot)}, {"UAB_CACHE_DIR", utf8(context.cacheRoot)}, {"UAB_STATE_DIR", utf8(context.stateRoot)}, {"UAB_LOG_DIR", utf8(context.logRoot)},
        {"UAB_RUNTIME_MODE", developmentMode() ? "development" : "bundled"},
        {"TMPDIR", utf8(work)}, {"TMP", utf8(work)}, {"TEMP", utf8(work)}, {"PYTHONDONTWRITEBYTECODE", "1"}, {"PYTHONUTF8", "1"}, {"PYTHONIOENCODING", "utf-8"}};
#ifdef _WIN32
    environment.emplace_back("PYTHONPATH", utf8(resolved(backend / "src")));
#endif
    if (runtimePaths().bundled) environment.emplace_back("UAB_RUNTIME_ROOT", utf8(runtimePaths().bundleRoot));
    if (runtimePaths().manifestValidated) environment.emplace_back("UAB_RUNTIME_MANIFEST", utf8(runtimePaths().manifest));
    if (developmentMode() && !runtimePaths().blender.empty()) environment.emplace_back("UAB_BLENDER", utf8(runtimePaths().blender));
    return processEnvironment(environment);
}
std::vector<std::string> pythonArguments(const std::vector<std::string>& arguments) {
    return pythonArguments(arguments, runtimePaths().backend);
}
std::vector<std::string> pythonArguments(const std::vector<std::string>& arguments, const fs::path& backend) {
    auto selectedBackend = resolved(backend);
    if (!developmentMode() && selectedBackend != resolved(runtimePaths().backend)) throw std::runtime_error("Release Python bootstrap rejects an external backend");
#ifdef _WIN32
    std::vector<std::string> result{"-B", "-X", "utf8"};
#else
    auto bootstrap = developmentMode() ? selectedBackend / "src/unityanimbridge/bootstrap.py" : runtimePaths().bootstrap;
    std::vector<std::string> result{"-I", "-B", "-X", "utf8", utf8(bootstrap), "--backend", utf8(selectedBackend / "src"), "--"};
#endif
    result.insert(result.end(), arguments.begin(), arguments.end()); return result;
}
std::uintmax_t removeOwnedTree(const fs::path& root, const fs::path& owner, std::error_code& error) {
    error.clear();
    auto absolute = fs::absolute(root, error); if (error) return 0;
    absolute = absolute.lexically_normal();
    auto boundary = fs::absolute(owner, error); if (error) return 0;
    boundary = boundary.lexically_normal();
    auto inside = [](const fs::path& child, const fs::path& parent) {
        auto current = child.begin();
        for (auto part = parent.begin(); part != parent.end(); ++part, ++current) {
            if (current == child.end()) return false;
#ifdef _WIN32
            if (_wcsicmp(current->c_str(), part->c_str())) return false;
#else
            if (*current != *part) return false;
#endif
        }
        return current != child.end();
    };
    if (!inside(absolute, boundary)) { error = std::make_error_code(std::errc::permission_denied); return 0; }
    // Refuse a replaced root/ancestor. remove_all itself removes descendant
    // links rather than visiting their targets (covered by a junction test).
    for (auto current = absolute; !current.empty();) {
#ifdef _WIN32
        auto attributes = GetFileAttributesW(current.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) { error = std::make_error_code(std::errc::too_many_symbolic_link_levels); return 0; }
#else
        if (fs::is_symlink(fs::symlink_status(current, error))) { error = std::make_error_code(std::errc::too_many_symbolic_link_levels); return 0; }
        if (error == std::errc::no_such_file_or_directory) error.clear();
        if (error) return 0;
#endif
        auto parent = current.parent_path(); if (parent == current) break; current = parent;
    }
    auto resolved = fs::weakly_canonical(absolute, error); if (error) return 0;
    auto resolvedBoundary = fs::weakly_canonical(boundary, error); if (error) return 0;
    if (!inside(resolved, resolvedBoundary)) { error = std::make_error_code(std::errc::permission_denied); return 0; }
#ifdef _WIN32
    auto value = absolute.lexically_normal().make_preferred().wstring();
    if (!value.starts_with(L"\\\\?\\")) value = value.starts_with(L"\\\\") ? L"\\\\?\\UNC\\" + value.substr(2) : L"\\\\?\\" + value;
    return fs::remove_all(fs::path(value), error);
#else
    // Traverse from a pinned, non-symlink owner descriptor. Descendant links are
    // unlinked as entries; recursive opens never follow them, even after swaps.
    DirectoryFd ownerFd(openDirectoryPath(boundary, error)); if (ownerFd.value < 0) return 0;
    struct stat ownerInfo{}; if (fstat(ownerFd.value, &ownerInfo)) { error = {errno, std::generic_category()}; return 0; }
    auto relative = absolute.lexically_relative(boundary);
    int parent = fcntl(ownerFd.value, F_DUPFD_CLOEXEC, 0);
    if (parent < 0) { error = {errno, std::generic_category()}; return 0; }
    DirectoryFd parentFd(parent);
    for (auto& component : relative.parent_path()) {
        auto next = openat(parentFd.value, component.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (next < 0) { if (errno != ENOENT) error = {errno, std::generic_category()}; return 0; }
        close(parentFd.value); parentFd.value = next;
    }
    return removeTreeAt(parentFd.value, relative.filename(), ownerInfo.st_dev, error);
#endif
}
}
