#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "animbridge/core.hpp"
#include "animbridge/process.hpp"
#include "platform/common/process_ownership.hpp"
#include "platform/common/process_supervisor_detail.hpp"
#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <thread>
#ifndef _WIN32
#include <csignal>
#include <pthread.h>
#endif

namespace animbridge {
using namespace supervisor_detail;
namespace {
fs::path absoluteExecutable(const fs::path& input) {
    if (input.is_absolute()) {
        if (!fs::is_regular_file(input)) throw std::runtime_error("Executable is missing: " + utf8(input));
        // A venv's symlinked python locates pyvenv.cfg from argv[0]. Ownership
        // uses file identity separately; execution preserves the chosen path.
        return input.lexically_normal();
    }
    if (input.has_parent_path()) throw std::runtime_error("Subprocess executable must be an absolute path");
    if (!developmentMode()) throw std::runtime_error("Release mode requires an absolute executable");
#ifdef _WIN32
    std::wstring buffer(32768, L'\0'); auto n = SearchPathW(nullptr, input.c_str(), L".exe", static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    if (n && n < buffer.size()) { buffer.resize(n); auto selected = fs::path(buffer); if (fs::is_regular_file(selected)) return fs::absolute(selected).lexically_normal(); }
#else
    // PATH lookup is restricted to development mode. Release execution always
    // requires the absolute path selected by the runtime manifest.
    auto inheritedPath = std::getenv("PATH");
    if (inheritedPath) {
        std::istringstream parts(inheritedPath); std::string part;
        while (std::getline(parts, part, ':')) {
            auto directory = fromUtf8(part); if (!directory.is_absolute()) continue;
            auto candidate = directory / input;
            if (fs::is_regular_file(candidate) && access(candidate.c_str(), X_OK) == 0) return candidate.lexically_normal();
        }
    }
    for (auto root : {"/usr/bin", "/bin", "/usr/local/bin"}) if (fs::is_regular_file(fs::path(root) / input)) return (fs::path(root) / input).lexically_normal();
#endif
    throw std::runtime_error("Cannot resolve a development executable: " + utf8(input));
}
}
ProcessResult runSupervisedProcess(const fs::path& executable, const std::vector<std::string>& args,
    const fs::path& workingDirectory, const fs::path& log, unsigned timeoutSeconds,
    const std::vector<std::pair<std::string, std::string>>& environment,
    std::shared_ptr<std::atomic_bool> cancel, const ProcessRunOptions& options) {
    if (cancel && cancel->load()) return {4, false, true};
    if (!timeoutSeconds || !options.jobTimeoutSeconds) throw std::runtime_error("Process time budgets must be positive");
    auto exe = absoluteExecutable(executable), cwd = fs::canonical(fs::absolute(workingDirectory));
    auto supervisor = runtimePaths().supervisor;
    if (!fs::is_regular_file(supervisor)) throw std::runtime_error("The private process supervisor is missing: " + utf8(supervisor));
    auto absoluteLog = fs::absolute(log); fs::create_directories(absoluteLog.parent_path());
    auto records = options.journalRoot.empty() ? absoluteLog.parent_path() : fs::absolute(options.journalRoot);
    fs::create_directories(records); auto nonce = newIdentity(); auto runRoot = records / (".animbridge-process-" + nonce);
    if (!fs::create_directory(runRoot)) throw std::runtime_error("Cannot reserve process supervisor journal");
#ifndef _WIN32
    fs::permissions(runRoot, fs::perms::owner_all, fs::perm_options::replace);
#endif
    auto childEnvironment = environment;
    auto temp = runRoot / "tmp";
    if (!fs::create_directory(temp)) throw std::runtime_error("Cannot reserve process scratch directory");
    for (auto key : {"TMPDIR", "TMP", "TEMP"}) {
        auto found = std::find_if(childEnvironment.begin(), childEnvironment.end(), [&](const auto& entry) {
#ifdef _WIN32
            return _stricmp(entry.first.c_str(), key) == 0;
#else
            return entry.first == key;
#endif
        });
        if (found == childEnvironment.end()) childEnvironment.emplace_back(key, utf8(temp));
    }
    auto configPath = runRoot / "request.json", statusPath = runRoot / "result.json";
    auto deadline = options.absoluteDeadlineMilliseconds > 0 ? options.absoluteDeadlineMilliseconds : monotonicMs() + static_cast<std::int64_t>(options.jobTimeoutSeconds) * 1000;
    if (deadline <= monotonicMs()) { ProcessResult expired{3, true, false}; expired.timeoutLayer = "supervisor"; return expired; }
    json config{{"schemaVersion", 1}, {"nonce", nonce}, {"executable", utf8(exe)}, {"args", args}, {"cwd", utf8(cwd)},
        {"log", utf8(absoluteLog)}, {"stageTimeoutSeconds", timeoutSeconds}, {"deadlineMilliseconds", deadline}, {"stageLeases", options.stageLeases}, {"environment", processEnvironment(childEnvironment)},
        {"privateExecutables", json::array()}, {"sharedExemptExecutables", json::array()}, {"unityEditorRoots", json::array()}};
    config["privateExecutables"].push_back(utf8(exe));
    for (auto& path : options.privateExecutables) {
        // Optional converter components may be absent for analysis-only work.
        // Their descendants remain unclassified; absence never widens ownership.
        std::error_code error;
        if (!path.empty() && fs::is_regular_file(path, error)) {
            auto canonical = fs::canonical(path);
            config["privateExecutables"].push_back(utf8(canonical));
            // An approved Unity Editor also approves its own install folder: the supervisor may
            // reap known build helpers there (bee_backend, shader compiler) that outlive Unity.
            if (auto root = unityInstallRoot(canonical); !root.empty()) config["unityEditorRoots"].push_back(utf8(root));
        }
    }
    // A manifest is the registration source for bundled converter executables.
    if (runtimePaths().manifestValidated) for (auto& path : {runtimePaths().python, runtimePaths().blender}) config["privateExecutables"].push_back(utf8(path));
    for (auto& path : options.sharedExemptExecutables) config["sharedExemptExecutables"].push_back(utf8(fs::canonical(path)));
    writeJson(configPath, config);
    const auto startupDeadline = monotonicMs() + 30000;
    std::int64_t stopRequestedAt = 0;
    bool hostTimeout = false, hostCancelled = false;
    auto requestedSignal = [&]() -> char {
        if (stopRequestedAt) return 0;
        if (cancel && cancel->load()) { hostCancelled = true; stopRequestedAt = monotonicMs(); return 'C'; }
        if (monotonicMs() >= deadline || (monotonicMs() >= startupDeadline && !fs::exists(runRoot / "journal.json"))) {
            hostTimeout = true; stopRequestedAt = monotonicMs(); return 'T';
        }
        return 0;
    };
#ifdef _WIN32
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE}; HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &security, 0)) throw std::runtime_error("Cannot create supervisor control pipe");
    Handle reader(read), writer(write);
    if (!SetHandleInformation(writer.value, HANDLE_FLAG_INHERIT, 0)) throw std::runtime_error("Cannot protect supervisor parent pipe");
    auto childInfo = spawn(supervisor, {utf8(configPath), std::to_string(reinterpret_cast<std::uintptr_t>(reader.value))}, cwd, processEnvironment(), {reader.value});
    Handle child(childInfo.hProcess), thread(childInfo.hThread);
    if (ResumeThread(thread.value) == static_cast<DWORD>(-1)) { TerminateProcess(child.value, 3); throw std::runtime_error("Cannot start process supervisor"); }
    reader.reset();
    for (;;) {
        auto wait = WaitForSingleObject(child.value, 25); if (wait == WAIT_OBJECT_0) break;
        if (wait != WAIT_TIMEOUT) throw std::runtime_error("Cannot wait for process supervisor");
        if (char signal = requestedSignal()) { DWORD count = 0; WriteFile(writer.value, &signal, 1, &count, nullptr); }
        if (stopRequestedAt && monotonicMs() >= stopRequestedAt + 10000) {
            TerminateProcess(child.value, 3); WaitForSingleObject(child.value, 1000); break;
        }
    }
#else
    int channel[2]; makePipe(channel); Fd reader(channel[0]), writer(channel[1]);
    auto pid = spawn(supervisor, {utf8(configPath), "3"}, cwd, processEnvironment(), reader.value);
    reader.reset(); int status = 0;
    for (;;) {
        auto done = waitpid(pid, &status, WNOHANG); if (done == pid) break;
        if (done < 0 && errno != EINTR) throw std::runtime_error("Cannot wait for process supervisor");
        if (char signal = requestedSignal()) {
            // The supervisor may already have completed. Avoid SIGPIPE changing
            // host signal policy by blocking it only for this write operation.
            sigset_t block, previous, pending; sigemptyset(&block); sigaddset(&block, SIGPIPE); pthread_sigmask(SIG_BLOCK, &block, &previous);
            sigpending(&pending); bool wasPending = sigismember(&pending, SIGPIPE); auto written = write(writer.value, &signal, 1);
            if (written < 0 && errno == EPIPE && !wasPending) { int signalNumber = 0; sigwait(&block, &signalNumber); }
            pthread_sigmask(SIG_SETMASK, &previous, nullptr);
        }
        if (stopRequestedAt && monotonicMs() >= stopRequestedAt + 10000) {
            kill(pid, SIGKILL); while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {} break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
#endif
    ProcessResult result{3, false, false};
    if (!fs::is_regular_file(statusPath)) {
        result.timedOut = hostTimeout; result.cancelled = hostCancelled;
        if (hostTimeout) result.timeoutLayer = "supervisor";
        result.cleanupComplete = false;
        result.ownedCleanupComplete = false;
        result.cleanupWarnings.push_back("Supervisor terminated without a result; preserve journal and unverified process ownership: " + utf8(runRoot));
        return result;
    }
    auto response = json::parse(std::ifstream(statusPath));
    if (response.value("nonce", "") != nonce) throw std::runtime_error("Process supervisor response identity mismatch");
    result.exitCode = response.value("exitCode", 3); result.timedOut = response.value("timedOut", false); result.cancelled = response.value("cancelled", false);
    result.timeoutLayer = response.value("timeoutLayer", ""); result.stage = response.value("stage", ""); result.cleanupComplete = response.value("cleanupComplete", false);
    result.ownedCleanupComplete = response.value("ownedCleanupComplete", false);
    result.cleanupWarnings = response.value("cleanupWarnings", std::vector<std::string>{});
    return result;
}
ProcessResult runProcess(const fs::path& executable, const std::vector<std::string>& args,
    const fs::path& cwd, const fs::path& log, unsigned timeoutSeconds,
    const std::vector<std::pair<std::string, std::string>>& environment, std::shared_ptr<std::atomic_bool> cancel) {
    return runSupervisedProcess(executable, args, cwd, log, timeoutSeconds, environment, cancel, {});
}
std::vector<std::string> processRecoveryWarnings(const fs::path& journalRoot) {
    std::vector<std::string> warnings; std::error_code error;
    if (!fs::is_directory(journalRoot, error) || error) return warnings;
    for (fs::directory_iterator it(journalRoot, error), end; !error && it != end; it.increment(error)) {
        if (fs::is_symlink(it->symlink_status(error)) || error || !it->is_directory(error)) continue;
        if (!it->path().filename().string().starts_with(".animbridge-process-")) continue;
        try {
            auto journal = json::parse(std::ifstream(it->path() / "journal.json"));
            if (journal.at("schemaVersion") != 1 || !journal.at("nonce").is_string()) continue;
            auto phase = journal.value("phase", "interrupted");
            if (phase != "finished") warnings.push_back("Interrupted or unresolved process cleanup (" + phase + "); preserve cache/log and review ownership before retrying: " + utf8(it->path()));
        } catch (...) { warnings.push_back("Unreadable process recovery journal; no process was terminated: " + utf8(it->path())); }
    }
    if (error) warnings.push_back("Process recovery journal is unavailable: " + error.message());
    return warnings;
}
}
