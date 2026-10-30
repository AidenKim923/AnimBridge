#include "animbridge/core.hpp"
#include "animbridge/process.hpp"
#include "animbridge/instance_lock.hpp"
#include "../src/platform/common/process_ownership.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#else
#include <spawn.h>
#include <unistd.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <fcntl.h>
extern char** environ;
#endif
using namespace animbridge;
using supervisor_detail::isOwnedUnityHelper;
using supervisor_detail::unityInstallRoot;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void write(const fs::path& path, const std::string& value) { std::ofstream(path) << value; }
void waitReady(const fs::path& path) { for (int n = 0; n < 500 && !fs::exists(path); ++n) std::this_thread::sleep_for(std::chrono::milliseconds(10)); check(fs::exists(path), "Child fixture did not start"); }
void rawSpawn(const fs::path& exe, const fs::path& lock, unsigned seconds, bool stubborn = false) {
#ifdef _WIN32
    auto command = L"\"" + exe.wstring() + L"\" --hold \"" + lock.wstring() + L"\" " + std::to_wstring(seconds) + (stubborn ? L" stubborn" : L"");
    STARTUPINFOW start{}; start.cb = sizeof(start); PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr, nullptr, &start, &process)) throw std::runtime_error("Cannot launch descendant fixture");
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
#else
    std::string executable = utf8(exe), file = utf8(lock), lifetime = std::to_string(seconds), flag = "stubborn";
    char* values[]{executable.data(), const_cast<char*>("--hold"), file.data(), lifetime.data(), stubborn ? flag.data() : nullptr, nullptr};
    pid_t pid; if (posix_spawn(&pid, exe.c_str(), nullptr, nullptr, values, environ)) throw std::runtime_error("Cannot launch descendant fixture");
#endif
}
bool unlocked(const fs::path& path) {
#ifdef _WIN32
    auto handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr); if (handle == INVALID_HANDLE_VALUE) return false; CloseHandle(handle); return true;
#else
    auto fd = open(path.c_str(), O_WRONLY | O_CLOEXEC); if (fd < 0) return false; bool result = flock(fd, LOCK_EX | LOCK_NB) == 0; close(fd); return result;
#endif
}
void stage(const std::string& value) {
#ifdef _WIN32
    auto handle = std::getenv("UAB_STAGE_HANDLE"); if (!handle) return; DWORD count = 0; auto message = value + '\n'; WriteFile(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(std::stoull(handle))), message.data(), static_cast<DWORD>(message.size()), &count, nullptr);
#else
    auto fd = std::getenv("UAB_STAGE_FD"); if (!fd) return; auto message = value + '\n'; auto ignored = ::write(std::stoi(fd), message.data(), message.size()); (void)ignored;
#endif
}
}
int main(int argc, char** argv) {
    try {
        auto self = fs::canonical(fromUtf8(argv[0]));
#ifdef _WIN32
        if (argc == 3 && std::string(argv[1]) == "--desktop-name") {
            wchar_t name[256]{}; DWORD needed = 0;
            check(GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, name, sizeof(name), &needed), "Cannot query child desktop");
            write(fromUtf8(argv[2]), utf8(std::wstring(name))); return 0;
        }
#endif
        if (argc == 3 && std::string(argv[1]) == "--temp-environment") {
            nlohmann::json values;
            for (auto key : {"TMPDIR", "TMP", "TEMP"}) {
                auto value = std::getenv(key);
                check(value && *value, "Child process has no temporary directory");
                auto path = fromUtf8(value);
                check(path.is_absolute() && fs::is_directory(path), "Child temporary directory is missing or relative");
                std::ofstream probe(path / (std::string(key) + ".probe"));
                check(bool(probe), "Child temporary directory is not writable");
                values[key] = value;
            }
            write(fromUtf8(argv[2]), values.dump()); return 0;
        }
        if (argc >= 4 && std::string(argv[1]) == "--hold") {
            auto file = fromUtf8(argv[2]); auto seconds = std::stoul(argv[3]);
#ifndef _WIN32
            if (argc > 4) signal(SIGTERM, SIG_IGN);
#endif
#ifdef _WIN32
            auto handle = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr); if (handle == INVALID_HANDLE_VALUE) return 2;
#else
            auto fd = open(file.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600); if (fd < 0 || flock(fd, LOCK_EX | LOCK_NB)) return 2;
#endif
            write(file.string() + ".ready", "ready"); std::this_thread::sleep_for(std::chrono::seconds(seconds));
#ifdef _WIN32
            CloseHandle(handle);
#else
            close(fd);
#endif
            return 0;
        }
        if (argc >= 5 && std::string(argv[1]) == "--launcher") {
            auto lock = fromUtf8(argv[3]); rawSpawn(fromUtf8(argv[2]), lock, static_cast<unsigned>(std::stoul(argv[4])), argc > 5);
            waitReady(lock.string() + ".ready");
            if (argc > 6) std::this_thread::sleep_for(std::chrono::seconds(30));
            return 0;
        }
        if (argc == 3 && std::string(argv[1]) == "--crash-host") {
            auto root = fromUtf8(argv[2]), lock = root / "crash.lock";
            InstanceLock ownership(root / "host-data");
            std::thread crash([lock] { waitReady(lock.string() + ".ready"); std::_Exit(17); });
            runProcess(self, {"--launcher", utf8(self), utf8(lock), "30", "stubborn", "hold-launcher"}, root, root / "crash.log", 60);
            crash.join(); return 3;
        }
        if (argc == 2 && std::string(argv[1]) == "--stage") {
            stage(R"({"event":"stage-start","name":"parse","budgetSeconds":1})");
            stage(R"({"event":"heartbeat"})");
            std::this_thread::sleep_for(std::chrono::milliseconds(30)); stage(R"({"event":"stage-end"})"); return 0;
        }
        if (argc == 2 && std::string(argv[1]) == "--malformed-stage") { stage(R"({"event":"stage-start","name":"bad","budgetSeconds":999999})"); std::this_thread::sleep_for(std::chrono::seconds(30)); return 0; }
        if (argc != 2) return 64;
        auto root = fs::absolute(fromUtf8(argv[1])) / newIdentity(); fs::create_directories(root);
        auto tempReport = root / "temp-default.json";
        auto tempDefault = runProcess(self, {"--temp-environment", utf8(tempReport)}, root, root / "temp-default.log", 10);
        check(tempDefault.exitCode == 0 && tempDefault.cleanupComplete, "Default child temporary directory did not work");
#ifdef _WIN32
        { // Helper workers must never run on the interactive desktop.
            auto desktopReport = root / "desktop.txt";
            auto desktop = runProcess(self, {"--desktop-name", utf8(desktopReport)}, root, root / "desktop.log", 10);
            check(desktop.exitCode == 0, "Desktop probe child failed");
            std::string name; std::getline(std::ifstream(desktopReport), name);
            check(!name.empty() && name != "Default", "Worker child ran on the interactive desktop");
        }
#endif
        // Windows workers run under a hidden console; its conhost.exe member must not be reported as unclassified.
        check(tempDefault.cleanupWarnings.empty(), "Hidden console host produced a cleanup warning");
        auto defaultValues = nlohmann::json::parse(std::ifstream(tempReport));
        auto defaultTemp = fromUtf8(defaultValues.at("TMP").get<std::string>());
        check(defaultValues.at("TMPDIR") == defaultValues.at("TMP") && defaultValues.at("TEMP") == defaultValues.at("TMP") &&
              defaultTemp.filename() == "tmp" && defaultTemp.parent_path().parent_path() == root &&
              defaultTemp.parent_path().filename().string().starts_with(".animbridge-process-"),
              "Child inherited an OS temporary directory instead of its process journal scratch");
        auto trustedTemp = root / "trusted temporary directory"; fs::create_directory(trustedTemp);
        auto trustedReport = root / "temp-trusted.json";
        auto tempTrusted = runProcess(self, {"--temp-environment", utf8(trustedReport)}, root, root / "temp-trusted.log", 10,
            {{"TMPDIR", utf8(trustedTemp)}, {"TMP", utf8(trustedTemp)}, {"TEMP", utf8(trustedTemp)}});
        check(tempTrusted.exitCode == 0 && tempTrusted.cleanupComplete, "Trusted child temporary directory did not work");
        auto trustedValues = nlohmann::json::parse(std::ifstream(trustedReport));
        for (auto key : {"TMPDIR", "TMP", "TEMP"}) check(trustedValues.at(key) == utf8(trustedTemp), "Explicit trusted temporary directory was replaced");
#ifndef _WIN32
        auto baselineLog = root / "python-baseline.log";
        auto baseline = runProcess(runtimePaths().python,
            {"-I", "-B", "-c", "import sys; print(sys.executable); print(f'{sys.version_info.major}.{sys.version_info.minor}')"},
            root, baselineLog, 10);
        check(baseline.exitCode == 0, "Development Python could not report its interpreter identity");
        std::ifstream baselineFile(baselineLog); std::string pythonExecutable, pythonMinor;
        std::getline(baselineFile, pythonExecutable); std::getline(baselineFile, pythonMinor);
        auto basePython = fs::canonical(fromUtf8(pythonExecutable));
        auto selectedVenv = root / "selected venv", selectedPython = selectedVenv / "bin/python3";
        fs::create_directories(selectedPython.parent_path()); fs::create_symlink(basePython, selectedPython);
        write(selectedVenv / "pyvenv.cfg", "home = " + utf8(basePython.parent_path()) + "\ninclude-system-site-packages = false\n");
        auto package = selectedVenv / "lib" / ("python" + pythonMinor) / "site-packages/uab_venv_identity_fixture.py";
        fs::create_directories(package.parent_path()); write(package, "MARKER = 'selected-venv-package'\n");
        auto venvLog = root / "python-venv.log";
        auto venv = runProcess(selectedPython,
            {"-I", "-B", "-c", "import sys,uab_venv_identity_fixture; print(sys.prefix); print(uab_venv_identity_fixture.MARKER)"},
            root, venvLog, 10);
        std::ifstream venvFile(venvLog); std::string prefix, marker; std::getline(venvFile, prefix); std::getline(venvFile, marker);
        check(venv.exitCode == 0 && fromUtf8(prefix) == selectedVenv && marker == "selected-venv-package",
              "Canonicalizing a selected Python executable lost its venv and private dependency context");
#endif
        auto normalLock = root / "normal.lock";
        auto normal = runProcess(self, {"--launcher", utf8(self), utf8(normalLock), "30"}, root, root / "normal.log", 10);
        check(normal.exitCode == 0 && normal.cleanupComplete && unlocked(normalLock), "Normal launcher exit leaked a private descendant/lock");
        auto timeoutLock = root / "timeout.lock";
        auto timeout = runProcess(self, {"--hold", utf8(timeoutLock), "30", "stubborn"}, root, root / "timeout.log", 1);
        check(timeout.timedOut && timeout.timeoutLayer == "stage" && timeout.cleanupComplete && unlocked(timeoutLock), "Stage timeout did not reclaim a stubborn child");
        auto cancelToken = std::make_shared<std::atomic_bool>(false); auto cancelLock = root / "cancel.lock";
        std::thread cancellation([&] { waitReady(cancelLock.string() + ".ready"); cancelToken->store(true); });
        auto cancelled = runProcess(self, {"--hold", utf8(cancelLock), "30"}, root, root / "cancel.log", 20, {}, cancelToken); cancellation.join();
        check(cancelled.cancelled && !cancelled.timedOut && cancelled.cleanupComplete && unlocked(cancelLock), "Cancellation leaked an owned process");
        ProcessRunOptions policy; policy.stageLeases = true; policy.jobTimeoutSeconds = 3;
        auto leased = runSupervisedProcess(self, {"--stage"}, root, root / "stage.log", 2, {}, {}, policy);
        check(leased.exitCode == 0 && leased.cleanupComplete && leased.stage == "parse", "Stage lease contract failed");
        auto malformed = runSupervisedProcess(self, {"--malformed-stage"}, root, root / "bad-stage.log", 2, {}, {}, policy);
        check(malformed.exitCode != 0 && !malformed.cleanupWarnings.empty(), "Invalid worker lease was accepted");
        auto outerLock = root / "outer.lock"; policy.stageLeases = false; policy.jobTimeoutSeconds = 1;
        auto outer = runSupervisedProcess(self, {"--hold", utf8(outerLock), "30"}, root, root / "outer.log", 20, {}, {}, policy);
        check(outer.timedOut && outer.timeoutLayer == "supervisor" && outer.cleanupComplete, "Absolute item deadline was renewed by worker budget");
        auto other = root / self.filename(); fs::copy_file(self, other);
#ifndef _WIN32
        fs::permissions(other, fs::status(self).permissions());
#endif
        auto unknownLock = root / "unclassified.lock";
        auto unknown = runProcess(self, {"--launcher", utf8(other), utf8(unknownLock), "4"}, root, root / "unclassified.log", 10);
        check(unknown.exitCode == 0 && !unknown.cleanupComplete && !unknown.cleanupWarnings.empty() && !unlocked(unknownLock), "Unclassified service was terminated or misreported");
        while (!unlocked(unknownLock)) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        { // Unity build helpers that outlive the Editor: adopted only inside the approved install folder.
            auto extension = self.extension();
            auto editorDir = root / "Unity Editor"; auto toolsDir = editorDir / "Data" / "Tools"; auto elsewhere = root / "Hub";
            fs::create_directories(toolsDir); fs::create_directories(elsewhere);
            auto copy = [&](const fs::path& dir, const std::string& name) { auto target = dir / (name + extension.string()); fs::copy_file(self, target); return target; };
            auto editor = copy(editorDir, "Unity"), bee = copy(toolsDir, "bee_backend"), licensing = copy(toolsDir, "Unity.Licensing.Client"), hubBee = copy(elsewhere, "bee_backend");
            auto editorCanonical = fs::canonical(editor);
            check(unityInstallRoot(editorCanonical) == editorCanonical.parent_path() && unityInstallRoot(self).empty() &&
                  unityInstallRoot("/Applications/Unity/Unity.app/Contents/MacOS/Unity") == fs::path("/Applications/Unity/Unity.app"), "Unity install root detection is wrong");
            std::vector<fs::path> roots{editorCanonical.parent_path()};
            check(isOwnedUnityHelper(fs::canonical(bee), roots), "Known build helper inside the Editor folder was not adopted");
            check(!isOwnedUnityHelper(fs::canonical(licensing), roots), "Shared licensing client must never be adopted");
            check(!isOwnedUnityHelper(fs::canonical(hubBee), roots), "Same-named helper outside the Editor folder was adopted");
            check(!isOwnedUnityHelper(fs::canonical(editor), roots) && !isOwnedUnityHelper({}, roots) && !isOwnedUnityHelper(fs::canonical(bee), {}), "Non-helper or rootless path was adopted");
            ProcessRunOptions unityPolicy; unityPolicy.privateExecutables = {editor}; unityPolicy.jobTimeoutSeconds = 20;
            auto beeLock = root / "bee.lock";
            auto adopted = runSupervisedProcess(self, {"--launcher", utf8(bee), utf8(beeLock), "30"}, root, root / "bee.log", 10, {}, {}, unityPolicy);
            check(adopted.exitCode == 0 && adopted.cleanupComplete && adopted.ownedCleanupComplete && adopted.cleanupWarnings.empty() && unlocked(beeLock), "Unity build helper outlived the run or was reported as unclassified");
            auto sharedLock = root / "licensing.lock";
            auto shared = runSupervisedProcess(self, {"--launcher", utf8(licensing), utf8(sharedLock), "4"}, root, root / "licensing.log", 10, {}, {}, unityPolicy);
            check(shared.exitCode == 0 && !shared.cleanupComplete && !unlocked(sharedLock), "Licensing client was terminated");
            while (!unlocked(sharedLock)) std::this_thread::sleep_for(std::chrono::milliseconds(20));
            auto hubLock = root / "hub-bee.lock";
            auto outside = runSupervisedProcess(self, {"--launcher", utf8(hubBee), utf8(hubLock), "4"}, root, root / "hub-bee.log", 10, {}, {}, unityPolicy);
            check(outside.exitCode == 0 && !outside.cleanupComplete && !unlocked(hubLock), "Helper outside the Editor folder was terminated");
            while (!unlocked(hubLock)) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        auto exemptLock = root / "exempt.lock"; policy.jobTimeoutSeconds = 10; policy.sharedExemptExecutables = {other};
        auto exempt = runSupervisedProcess(self, {"--launcher", utf8(other), utf8(exemptLock), "4"}, root, root / "exempt.log", 10, {}, {}, policy);
        check(exempt.exitCode == 0 && !exempt.cleanupComplete && !unlocked(exemptLock), "Shared-exempt service was killed");
        while (!unlocked(exemptLock)) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        auto crash = runProcess(self, {"--crash-host", utf8(root)}, root, root / "host.log", 15);
        check(crash.exitCode == 17, "Parent-crash fixture did not terminate the host");
        for (int n = 0; n < 600 && !unlocked(root / "crash.lock"); ++n) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        check(unlocked(root / "crash.lock"), "Host EOF did not clean a stubborn private process tree");
        InstanceLock afterCrash(root / "host-data"); check(afterCrash.acquired(), "Instance lock leaked into a worker/supervisor");
        auto killedRoot = root / "killed-supervisor"; fs::create_directory(killedRoot);
        auto killedLock = root / "supervisor-crash.lock";
        std::thread killSupervisor([&] {
            for (int attempt = 0; attempt < 500; ++attempt) {
                for (auto& entry : fs::directory_iterator(killedRoot)) {
                    auto journal = entry.path() / "journal.json"; if (!fs::exists(journal)) continue;
                    auto record = nlohmann::json::parse(std::ifstream(journal));
                    if (record.value("phase", "") != "running" || !fs::exists(killedLock.string() + ".ready")) continue;
                    auto pid = record.at("supervisorPid").get<unsigned long>();
#ifdef _WIN32
                    auto handle = OpenProcess(PROCESS_TERMINATE, FALSE, pid); if (handle) { TerminateProcess(handle, 19); CloseHandle(handle); }
#else
                    kill(static_cast<pid_t>(pid), SIGKILL);
#endif
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
        policy.sharedExemptExecutables.clear(); policy.journalRoot = killedRoot;
        auto killed = runSupervisedProcess(self, {"--hold", utf8(killedLock), "2"}, root, root / "killed.log", 10, {}, {}, policy); killSupervisor.join();
        check(!killed.ownedCleanupComplete && !killed.cleanupWarnings.empty(), "Supervisor crash pretended successful process cleanup");
        check(!processRecoveryWarnings(killedRoot).empty(), "Supervisor crash did not preserve interrupted recovery evidence");
        while (!unlocked(killedLock)) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        std::cout << "PASS process supervisor: normal descendants, timeout, cancel, stage lease, invalid lease, item deadline, unclassified/exempt preservation, Unity helper adoption, parent crash, FD/instance-lock release\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
