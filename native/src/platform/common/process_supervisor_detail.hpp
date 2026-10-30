#pragma once
#include "animbridge/core.hpp"
#include "animbridge/process.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <fstream>
#include <map>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <spawn.h>
#include <unistd.h>
#include <sys/wait.h>
#include <cerrno>
#endif
namespace animbridge::supervisor_detail {
using json = nlohmann::json;
inline std::int64_t monotonicMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
inline void writeJson(const fs::path& path, const json& value) {
    auto pending = path; pending += ".pending";
    { std::ofstream file(pending, std::ios::binary | std::ios::trunc); file << value.dump(); file.flush(); if (!file) throw std::runtime_error("Cannot persist process supervisor record"); }
#ifdef _WIN32
    if (!MoveFileExW(pending.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("Cannot publish supervisor record");
#else
    auto fd = open(pending.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW); if (fd < 0 || fsync(fd)) { if (fd >= 0) close(fd); throw std::runtime_error("Cannot flush supervisor record"); } close(fd);
    fs::rename(pending, path);
#endif
}
#ifdef _WIN32
struct Handle {
    HANDLE value = nullptr;
    Handle() = default; explicit Handle(HANDLE h) : value(h) {}
    Handle(const Handle&) = delete; Handle& operator=(const Handle&) = delete;
    Handle(Handle&& b) noexcept : value(b.value) { b.value = nullptr; }
    ~Handle() { reset(); }
    void reset(HANDLE h = nullptr) { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); value = h; }
};
inline std::wstring quote(const std::wstring& value) {
    std::wstring result = L"\""; unsigned slashes = 0;
    for (wchar_t c : value) { if (c == L'\\') { ++slashes; continue; } if (c == L'\"') { result.append(slashes * 2 + 1, L'\\'); result += c; } else { result.append(slashes, L'\\'); result += c; } slashes = 0; }
    result.append(slashes * 2, L'\\'); result += L'\"'; return result;
}
inline std::vector<wchar_t> envBlock(const std::vector<std::pair<std::string, std::string>>& environment) {
    struct Compare { bool operator()(const std::wstring& a, const std::wstring& b) const { return _wcsicmp(a.c_str(), b.c_str()) < 0; } };
    std::map<std::wstring, std::wstring, Compare> sorted;
    for (auto& [k, v] : environment) sorted[fromUtf8(k).wstring()] = fromUtf8(v).wstring();
    std::vector<wchar_t> result;
    for (auto& [k, v] : sorted) { auto entry = k + L'=' + v; result.insert(result.end(), entry.begin(), entry.end()); result.push_back(0); }
    result.push_back(0); if (result.size() == 1) result.push_back(0); return result;
}
// Helper workers (and everything they launch: Unity, Blender, their splash screens and consoles)
// run on a private, never-displayed desktop so no window can reach the user's screen. The desktop
// is created once per process and kept open for its lifetime. Returns an empty string on failure
// (current behaviour: the interactive desktop) after logging a warning.
inline std::wstring helperDesktopName() {
    static const std::wstring name = [] {
        std::wstring station(256, L'\0'); DWORD needed = 0;
        if (!GetUserObjectInformationW(GetProcessWindowStation(), UOI_NAME, station.data(), static_cast<DWORD>(station.size() * sizeof(wchar_t)), &needed)) {
            OutputDebugStringW(L"AnimBridge: cannot query window station; helpers use the default desktop\n"); return std::wstring();
        }
        station.resize(wcslen(station.c_str()));
        constexpr wchar_t desktop[] = L"AnimBridgeHelpers";
        HDESK handle = CreateDesktopW(desktop, nullptr, nullptr, 0, DESKTOP_CREATEWINDOW | DESKTOP_CREATEMENU | DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS | DESKTOP_ENUMERATE, nullptr);
        if (!handle) { OutputDebugStringW(L"AnimBridge: CreateDesktopW failed; helpers use the default desktop\n"); return std::wstring(); }
        return station + L"\\" + desktop; // handle intentionally kept open until process exit
    }();
    return name;
}
inline PROCESS_INFORMATION spawn(const fs::path& executable, const std::vector<std::string>& args, const fs::path& cwd,
    const std::vector<std::pair<std::string, std::string>>& environment, const std::vector<HANDLE>& inherit, HANDLE output = nullptr, HANDLE input = nullptr, bool privateDesktop = false) {
    std::wstring command = quote(executable.wstring()); for (auto& arg : args) command += L' ' + quote(fromUtf8(arg).wstring());
    auto block = envBlock(environment);
    SIZE_T bytes = 0; InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<unsigned char> storage(bytes);
    auto list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(list, 1, 0, &bytes)) throw std::runtime_error("Cannot initialize subprocess handle policy");
    struct Delete { LPPROC_THREAD_ATTRIBUTE_LIST p; ~Delete() { DeleteProcThreadAttributeList(p); } } cleanup{list};
    if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, const_cast<HANDLE*>(inherit.data()), inherit.size() * sizeof(HANDLE), nullptr, nullptr)) throw std::runtime_error("Cannot restrict inherited subprocess handles");
    STARTUPINFOEXW start{}; start.StartupInfo.cb = sizeof(start); start.lpAttributeList = list;
    if (output) { start.StartupInfo.dwFlags = STARTF_USESTDHANDLES; start.StartupInfo.hStdOutput = start.StartupInfo.hStdError = output; start.StartupInfo.hStdInput = input; }
    std::wstring desktopName;
    if (privateDesktop) {
        desktopName = helperDesktopName();
        if (!desktopName.empty()) start.StartupInfo.lpDesktop = desktopName.data();
        // Second line of defence if the private desktop is unavailable: ask the helper to
        // start hidden, so it neither shows a window nor takes keyboard focus from the user.
        start.StartupInfo.dwFlags |= STARTF_USESHOWWINDOW;
        start.StartupInfo.wShowWindow = SW_HIDE;
    }
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
        block.data(), cwd.c_str(), &start.StartupInfo, &process)) throw std::runtime_error("Cannot spawn process (Windows " + std::to_string(GetLastError()) + "): " + utf8(executable));
    return process;
}
#else
struct Fd {
    int value = -1; Fd() = default; explicit Fd(int fd) : value(fd) {}
    Fd(const Fd&) = delete; Fd& operator=(const Fd&) = delete;
    ~Fd() { if (value >= 0) close(value); }
    void reset(int fd = -1) { if (value >= 0) close(value); value = fd; }
};
inline void makePipe(int (&pipes)[2]) {
#ifdef __APPLE__
    if (pipe(pipes)) throw std::runtime_error("Cannot create supervisor pipe");
    if (fcntl(pipes[0], F_SETFD, FD_CLOEXEC) || fcntl(pipes[1], F_SETFD, FD_CLOEXEC)) { close(pipes[0]); close(pipes[1]); throw std::runtime_error("Cannot protect supervisor pipe"); }
#else
    if (pipe2(pipes, O_CLOEXEC)) throw std::runtime_error("Cannot create supervisor pipe");
#endif
}
inline pid_t spawn(const fs::path& executable, const std::vector<std::string>& args, const fs::path& cwd,
    const std::vector<std::pair<std::string, std::string>>& environment, int control, int output = -1, bool group = false) {
    std::vector<std::string> values{utf8(executable)}; values.insert(values.end(), args.begin(), args.end());
    std::vector<char*> argv; for (auto& v : values) argv.push_back(v.data()); argv.push_back(nullptr);
    std::vector<std::string> entries; for (auto& [k, v] : environment) entries.push_back(k + '=' + v);
    std::vector<char*> envp; for (auto& e : entries) envp.push_back(e.data()); envp.push_back(nullptr);
    posix_spawn_file_actions_t actions; if (posix_spawn_file_actions_init(&actions)) throw std::runtime_error("Cannot initialize spawn file actions");
    posix_spawnattr_t attributes; if (posix_spawnattr_init(&attributes)) { posix_spawn_file_actions_destroy(&actions); throw std::runtime_error("Cannot initialize spawn attributes"); }
    struct Destroy { posix_spawn_file_actions_t* a; posix_spawnattr_t* b; ~Destroy() { posix_spawn_file_actions_destroy(a); posix_spawnattr_destroy(b); } } cleanup{&actions, &attributes};
    auto checked = [](int result) { if (result) throw std::runtime_error("Cannot configure spawn file descriptors (" + std::to_string(result) + ")"); };
    // Duplicate source FD away from 0..3: close-from must preserve only stdio
    // and the explicitly mapped control channel, with no inherited lock FDs.
    Fd safeControl(fcntl(control, F_DUPFD_CLOEXEC, 10)); if (safeControl.value < 0) throw std::runtime_error("Cannot duplicate control descriptor");
    checked(posix_spawn_file_actions_adddup2(&actions, safeControl.value, 3));
    if (output >= 0) { checked(posix_spawn_file_actions_adddup2(&actions, output, STDOUT_FILENO)); checked(posix_spawn_file_actions_adddup2(&actions, output, STDERR_FILENO)); }
    Fd input(open("/dev/null", O_RDONLY | O_CLOEXEC)); if (input.value < 0) throw std::runtime_error("Cannot open subprocess input");
    checked(posix_spawn_file_actions_adddup2(&actions, input.value, STDIN_FILENO));
    checked(posix_spawn_file_actions_addchdir_np(&actions, cwd.c_str()));
#ifdef __APPLE__
    checked(posix_spawn_file_actions_addclose(&actions, safeControl.value));
    short flags = POSIX_SPAWN_CLOEXEC_DEFAULT;
#else
    checked(posix_spawn_file_actions_addclosefrom_np(&actions, 4));
    short flags = 0;
#endif
    if (group) { flags |= POSIX_SPAWN_SETPGROUP; checked(posix_spawnattr_setpgroup(&attributes, 0)); }
    checked(posix_spawnattr_setflags(&attributes, flags));
    pid_t pid = -1; auto error = posix_spawn(&pid, executable.c_str(), &actions, &attributes, argv.data(), envp.data());
    if (error) throw std::runtime_error("Cannot spawn executable (" + std::to_string(error) + "): " + utf8(executable)); return pid;
}
#endif
}
