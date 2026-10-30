#include "animbridge/core.hpp"
#include "animbridge/instance_lock.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#endif

using namespace animbridge;
namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void fixture(const fs::path& path) { fs::create_directories(path.parent_path()); std::ofstream(path) << "runtime path fixture"; }
void failure(const std::function<void()>& action, const char* message) { try { action(); } catch (...) { return; } throw std::runtime_error(message); }
#ifdef _WIN32
bool junction(const fs::path& link, const fs::path& target) {
    fs::create_directory(link);
    auto handle = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    struct Record { DWORD tag; WORD length, reserved, substituteOffset, substituteLength, printOffset, printLength; wchar_t paths[4096]; } record{};
    auto print = fs::absolute(target).make_preferred().wstring(), substitute = L"\\??\\" + print;
    if (substitute.size() + print.size() + 2 >= std::size(record.paths)) { CloseHandle(handle); return false; }
    record.tag = IO_REPARSE_TAG_MOUNT_POINT;
    record.substituteLength = static_cast<WORD>(substitute.size() * sizeof(wchar_t)); record.printOffset = record.substituteLength + sizeof(wchar_t);
    record.printLength = static_cast<WORD>(print.size() * sizeof(wchar_t)); record.length = 8 + record.printOffset + record.printLength + sizeof(wchar_t);
    std::copy(substitute.begin(), substitute.end(), record.paths); std::copy(print.begin(), print.end(), record.paths + record.printOffset / sizeof(wchar_t));
    DWORD returned = 0; bool result = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &record, record.length + 8, nullptr, 0, &returned, nullptr) != FALSE;
    CloseHandle(handle); return result;
}
#endif
}
int main(int count, char** values) {
    try {
        if (count != 2) throw std::runtime_error("Expected dedicated runtime test directory");
        auto root = fs::absolute(fromUtf8(values[1])) / newIdentity();
        auto source = root / "development checkout";
        fixture(source / "src/unityanimbridge/__init__.py"); fixture(source / "native/ui/index.html");
        auto executable = root / fromUtf8("설치 경로 with spaces") / "AnimBridge.exe";
        fixture(executable);
        auto development = resolveRuntimePaths(executable, source, "windows-x64");
        require(!development.bundled && development.backend == source && development.ui == source / "native/ui", "Developer fallback did not resolve");
        require(development.python == fs::path("python"), "Developer Python fallback changed");
        auto isolated = resolveRuntimePaths(executable, {}, "windows-x64");
        require(isolated.backend == executable.parent_path() / "backend" && isolated.ui == executable.parent_path() / "ui", "Source-free resolution consulted a checkout");
        fixture(executable.parent_path() / "backend/src/unityanimbridge/__init__.py");
        auto incomplete = resolveRuntimePaths(executable, source, "windows-x64");
        require(incomplete.bundled && incomplete.backend == executable.parent_path() / "backend", "Incomplete bundle escaped to a source checkout");
        require(incomplete.python == executable.parent_path() / "runtime/python/python.exe", "Bundle escaped to system Python");
        require(incomplete.blender == executable.parent_path() / "runtime/blender/blender.exe", "Bundle escaped to system Blender");
        fixture(executable.parent_path() / "ui/index.html"); fixture(executable.parent_path() / "runtime/webview2/msedgewebview2.exe");
        auto bundle = resolveRuntimePaths(executable, source, "windows-x64");
        require(bundle.ui == executable.parent_path() / "ui" && bundle.webView2 == executable.parent_path() / "runtime/webview2", "Adjacent UI or fixed WebView2 runtime missing");
        auto environment = pythonEnvironment(bundle.backend, root / "writable data/jobs");
        bool src = false, work = false, noBytecode = false;
        for (auto& [name, value] : environment) {
            if (name == "PYTHONPATH") src = fs::weakly_canonical(fromUtf8(value)) == fs::weakly_canonical(bundle.backend / "src");
            if (name == "UAB_WORK_DIR") work = fs::weakly_canonical(fromUtf8(value)) == fs::weakly_canonical(root / "writable data/jobs");
            if (name == "PYTHONDONTWRITEBYTECODE") noBytecode = value == "1";
        }
#ifdef _WIN32
        require(src && work && noBytecode, "Python resources and writable work directory were not separated");
#else
        require(!src && work && noBytecode, "Unix isolated Python must not depend on PYTHONPATH");
#endif
        auto arguments = pythonArguments({"-m", "unityanimbridge.desktop"});
#ifdef _WIN32
        require(arguments == std::vector<std::string>({"-B", "-X", "utf8", "-m", "unityanimbridge.desktop"}), "Embedded Python flags were not explicit");
#else
        require(arguments.size() >= 9 && arguments[0] == "-I" && arguments[1] == "-B" && arguments[4] == utf8(runtimePaths().bootstrap), "Unix isolated bootstrap arguments were not explicit");
#endif
        require(!defaultDataRoot().empty() && !defaultExportRoot().empty(), "User folder resolution failed");
        require(!executablePath().empty(), "Executable path resolution failed");
        for (auto target : {"windows-x64", "macos-arm64", "linux-x64-ubuntu22.04", "linux-x64-ubuntu24.04"}) {
            using nlohmann::json;
            bool mac = std::string(target).starts_with("macos"), isLinux = std::string(target).starts_with("linux");
            auto prefix = root / target / (mac ? "AnimBridge.app/Contents" : "prefix");
            auto resources = mac ? prefix / "Resources" : isLinux ? prefix / "share" : prefix;
            auto binary = prefix / (mac ? "MacOS/AnimBridge" : isLinux ? "bin/AnimBridge" : "AnimBridge.exe");
            fixture(binary); fixture(resources / "backend/src/unityanimbridge/__init__.py"); fixture(resources / "ui/index.html");
            fixture(prefix / "runtime/python"); fixture(prefix / "runtime/blender"); fixture(prefix / "runtime/supervisor"); fixture(resources / "backend/bootstrap.py");
            fixture(prefix / "runtime/webview2/msedgewebview2.exe");
            std::string arch = mac ? "arm64" : "x64";
            auto resourcePath = [&](const fs::path& path) { return path.lexically_relative(prefix).generic_string(); };
            json components;
            for (auto name : {"backend", "ui"}) components[name] = {{"resourcePath", resourcePath(resources / name)}, {"version", "fixture"}, {"architecture", arch}};
            for (auto name : {"python", "blender", "supervisor"}) components[name] = {{"executablePath", std::string("runtime/") + name}, {"version", "fixture"}, {"architecture", arch}};
            components["bootstrap"] = {{"resourcePath", resourcePath(resources / "backend/bootstrap.py")}, {"version", "fixture"}, {"architecture", arch}};
            components["webview2"] = {{"resourcePath", "runtime/webview2"}, {"version", "fixture"}, {"architecture", arch}};
            json manifest{{"schemaVersion", 1}, {"appVersion", "fixture"}, {"targetId", target}, {"architecture", arch}, {"components", components}};
            auto manifestPath = resources / "runtime-manifest.json";
            auto save = [&] { std::ofstream(manifestPath) << manifest.dump(); }; save();
            auto resolved = resolveRuntimePaths(binary, source, target);
            require(resolved.manifestValidated && resolved.bundled && !resolved.development && resolved.backend == resources / "backend", "Target manifest layout failed");
            manifest["components"]["python"]["executablePath"] = "../outside"; save();
            failure([&] { resolveRuntimePaths(binary, source, target); }, "Manifest traversal escaped into fallback");
            manifest["components"] = components; manifest["architecture"] = "wrong"; save();
            failure([&] { resolveRuntimePaths(binary, source, target); }, "Manifest architecture mismatch accepted");
            manifest["architecture"] = arch; manifest["targetId"] = "unrecognized-target"; save();
            failure([&] { resolveRuntimePaths(binary, source, target); }, "Manifest target mismatch accepted");
            manifest["targetId"] = target; save(); fs::remove(prefix / "runtime/python");
            failure([&] { resolveRuntimePaths(binary, source, target); }, "Missing runtime escaped into a source checkout");
        }
        auto context = resolveRuntimeContext(root / "independent context");
        require(context.cacheRoot == context.dataRoot / "cache" && context.stateRoot == context.dataRoot && context.logRoot == context.dataRoot / "logs", "Explicit root mixed independent contexts");
        auto isolatedCache = root / "outside-data-cache", data = root / "persistent-data";
        fixture(isolatedCache / "entry/payload"); fixture(data / "state-v2.json");
        std::error_code isolatedError;
        require(removeOwnedTree(isolatedCache / "entry", isolatedCache, isolatedError) > 0 && !isolatedError && fs::exists(data / "state-v2.json"), "Cache ownership incorrectly required containment under data root");
        auto lockRoot = root / "same-root-lock";
        { InstanceLock first(lockRoot); InstanceLock second(lockRoot); InstanceLock other(root / "other-root-lock");
          require(first.acquired() && !second.acquired() && other.acquired() && !second.diagnostic().empty(), "GUI/CLI root ownership did not exclude a second writer"); }
        { InstanceLock released(lockRoot); require(released.acquired(), "Instance lock was not released"); }
#ifdef _WIN32
        auto previousAllUsers = std::getenv("ALLUSERSPROFILE");
        std::string savedAllUsers = previousAllUsers ? previousAllUsers : "";
        require(_putenv_s("ALLUSERSPROFILE", "C:\\ProgramData") == 0, "Cannot set standard Windows environment fixture");
#endif
        auto cleanEnvironment = processEnvironment({{"UAB_WORK_DIR", "trusted"}});
#ifdef _WIN32
        require(_putenv_s("ALLUSERSPROFILE", savedAllUsers.c_str()) == 0, "Cannot restore standard Windows environment fixture");
        bool allUsers = false;
#endif
        for (auto& [key, value] : cleanEnvironment) {
            require(key != "PYTHONHOME" && key != "PYTHONPATH" && key != "LD_PRELOAD" && key != "WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS", "Child environment inherited a loader/browser/Python override");
            if (key == "UAB_WORK_DIR") require(value == "trusted", "Trusted context lost to inherited environment");
#ifdef _WIN32
            if (key == "ALLUSERSPROFILE") allUsers = value == "C:\\ProgramData";
#endif
        }
#ifdef _WIN32
        require(allUsers, "Unity Package Manager standard profile environment was not preserved");
#endif
        Options options;
        require(options.repository == runtimePaths().backend && options.python == runtimePaths().python && options.blender == runtimePaths().blender, "Options did not use runtime defaults");
        auto owned = root / "owned-deep-cache", deep = owned;
        for (unsigned index = 0; index < 5; ++index) deep /= std::string(55, char('a' + index));
#ifdef _WIN32
        auto extended = fs::path(L"\\\\?\\" + fs::absolute(deep).make_preferred().wstring());
        fixture(extended / "payload.bin");
#else
        fixture(deep / "payload.bin");
#endif
        std::error_code cleanupError; require(removeOwnedTree(owned, root, cleanupError) > 0 && !cleanupError && !fs::exists(owned), "Owned long-path cache cleanup failed");
        require(removeOwnedTree(root, root, cleanupError) == 0 && cleanupError, "Cleanup accepted the owner boundary itself");
#ifdef _WIN32
        auto cache = root / "junction-cache", external = root / "separate-user-data";
        fixture(cache / "owned.bin"); fixture(external / "preserve.bin");
        auto link = cache / "outside-link";
        require(junction(link, external), "Cannot create junction safety fixture");
        require(removeOwnedTree(link, cache, cleanupError) == 0 && cleanupError && fs::exists(external / "preserve.bin"), "Cleanup followed a replaced junction root");
        require(removeOwnedTree(external, cache, cleanupError) == 0 && cleanupError, "Cleanup accepted a target outside its owner");
        require(removeOwnedTree(cache, root, cleanupError) > 0 && !cleanupError && fs::exists(external / "preserve.bin"), "Cleanup followed a descendant junction outside its owned tree");
#else
        auto cache = root / "symlink-cache", external = root / "separate-user-data";
        fixture(cache / "owned.bin"); fixture(external / "preserve.bin"); fs::create_directory_symlink(external, cache / "outside-link");
        require(removeOwnedTree(cache / "outside-link", cache, cleanupError) == 0 && cleanupError && fs::exists(external / "preserve.bin"), "Cleanup followed a replaced symlink root");
        require(removeOwnedTree(cache, root, cleanupError) > 0 && !cleanupError && fs::exists(external / "preserve.bin"), "Descriptor-based cleanup followed a descendant link");
#endif
        std::cout << "PASS 11 runtime path cases (developer, source-free, incomplete bundle, bundled Python/Blender/UI/WebView2, writable separation, interpreter flags, user folders, executable, Options, owned long-path cleanup, cleanup boundary/junction protection)\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
