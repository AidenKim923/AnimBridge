#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "process_ownership.hpp"
#include "process_supervisor_detail.hpp"
#include <algorithm>
#include <sstream>
#include <thread>
#include <set>
#ifdef _WIN32
#include <processthreadsapi.h>
#else
#include <signal.h>
#include <sys/stat.h>
#ifdef __APPLE__
#include <libproc.h>
#include <sys/proc_info.h>
#include <sys/proc.h>
#else
#include <sys/prctl.h>
#endif
#endif

namespace animbridge {
namespace {
using namespace supervisor_detail;
struct BinaryIdentity {
    std::uint64_t volume = 0, file = 0;
    bool operator<(const BinaryIdentity& b) const { return volume < b.volume || (volume == b.volume && file < b.file); }
};
BinaryIdentity binaryIdentity(const fs::path& path) {
#ifdef _WIN32
    Handle file(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr));
    BY_HANDLE_FILE_INFORMATION information{};
    if (file.value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(file.value, &information)) throw std::runtime_error("Cannot verify executable file identity");
    return {information.dwVolumeSerialNumber, (static_cast<std::uint64_t>(information.nFileIndexHigh) << 32) | information.nFileIndexLow};
#else
    struct stat information{}; if (stat(path.c_str(), &information)) throw std::runtime_error("Cannot verify executable file identity");
    return {static_cast<std::uint64_t>(information.st_dev), static_cast<std::uint64_t>(information.st_ino)};
#endif
}
struct Member {
    unsigned long pid = 0; std::uint64_t start = 0;
    std::string classification = "unclassified", executable;
#ifdef _WIN32
    std::shared_ptr<Handle> handle;
#endif
};
struct Runner {
    json config, result;
    fs::path root;
    std::set<BinaryIdentity> privateFiles, exemptFiles;
    std::vector<fs::path> editorRoots; // Unity Editor install folders whose build helpers we may reap
    std::map<unsigned long, Member> members;
    std::set<std::string> warnings;
    std::string stage = "process", stageBuffer;
    std::int64_t jobDeadline = 0, stageDeadline = 0, start = monotonicMs();
    unsigned stageCap = 0;
    bool leases = false, timedOut = false, cancelled = false, parentGone = false;
    bool stageActive = false, cleanupComplete = true, ownedCleanupComplete = true;
    std::string timeoutLayer;
    int exitCode = 3;
#ifdef _WIN32
    Handle control, stageRead, stageWrite, child, thread, job, output, input;
#else
    Fd control, stageRead, stageWrite, output;
    pid_t leader = -1;
#endif
    std::string classify(const fs::path& executable) {
        try { auto identity = binaryIdentity(executable); if (exemptFiles.contains(identity)) return "shared-exempt"; if (privateFiles.contains(identity)) return "owned-private"; }
        catch (...) { }
        return "unclassified";
    }
    // Job/process-group membership proves the process descends from our worker. Known
    // Unity build helpers inside the approved Editor folder are then ours to clean up.
    void adoptUnityHelper(Member& member) {
        if (member.classification == "unclassified" && isOwnedUnityHelper(fromUtf8(member.executable), editorRoots)) member.classification = "owned-private";
    }
    void warn(const std::string& message) { warnings.insert(message); }
    void journal(const std::string& phase) {
        json ownership = json::array();
        for (auto& [pid, member] : members) ownership.push_back({{"pid", pid}, {"startIdentity", member.start}, {"classification", member.classification}, {"executable", member.executable}});
        writeJson(root / "journal.json", {{"schemaVersion", 1}, {"nonce", config.at("nonce")}, {"phase", phase},
#ifdef _WIN32
            {"supervisorPid", GetCurrentProcessId()},
#else
            {"supervisorPid", getpid()}, {"pgid", leader},
#endif
            {"members", ownership}, {"stage", stage}, {"deadlineMilliseconds", jobDeadline}, {"elapsedMilliseconds", monotonicMs() - start}});
    }
#ifdef _WIN32
    static std::uint64_t processStart(HANDLE handle) {
        FILETIME created{}, exited{}, kernel{}, user{};
        if (!GetProcessTimes(handle, &created, &exited, &kernel, &user)) return 0;
        return (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    }
    bool alive(const Member& member) {
        return member.handle && processStart(member.handle->value) == member.start && WaitForSingleObject(member.handle->value, 0) == WAIT_TIMEOUT;
    }
    void refresh() {
        std::vector<ULONG_PTR> storage(128);
        for (;;) {
            auto info = reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST*>(storage.data());
            if (QueryInformationJobObject(job.value, JobObjectBasicProcessIdList, info, static_cast<DWORD>(storage.size() * sizeof(ULONG_PTR)), nullptr)) {
                for (DWORD index = 0; index < info->NumberOfProcessIdsInList; ++index) {
                    auto pid = static_cast<DWORD>(info->ProcessIdList[index]);
                    if (members.contains(pid) && alive(members.at(pid))) continue;
                    auto handle = std::make_shared<Handle>(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid));
                    if (!handle->value) { warn("Cannot inspect job member pid=" + std::to_string(pid)); cleanupComplete = false; continue; }
                    std::wstring image(32768, L'\0'); DWORD size = static_cast<DWORD>(image.size());
                    Member member; member.pid = pid; member.handle = handle; member.start = processStart(handle->value);
                    if (QueryFullProcessImageNameW(handle->value, 0, image.data(), &size)) { image.resize(size); member.executable = utf8(fs::path(image)); member.classification = classify(fs::path(image)); adoptUnityHelper(member); }
                    if (!member.start) member.classification = "unclassified";
                    members[pid] = member;
                }
                break;
            }
            if (GetLastError() != ERROR_MORE_DATA || storage.size() > 65536) { warn("Cannot enumerate owned process job"); cleanupComplete = ownedCleanupComplete = false; break; }
            storage.resize(storage.size() * 2);
        }
    }
    std::string readPipe(HANDLE pipe, bool& eof) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) { auto error = GetLastError(); eof = error == ERROR_BROKEN_PIPE; if (!eof) throw std::runtime_error("Cannot inspect supervisor pipe"); return {}; }
        if (!available) return {};
        char buffer[4096]; DWORD count = 0;
        if (!ReadFile(pipe, buffer, std::min<DWORD>(available, sizeof(buffer)), &count, nullptr)) throw std::runtime_error("Cannot read supervisor pipe");
        return std::string(buffer, count);
    }
#else
    bool processInfo(unsigned long pid, Member& member, pid_t& group) {
#ifdef __APPLE__
        proc_bsdinfo info{};
        if (proc_pidinfo(static_cast<int>(pid), PROC_PIDTBSDINFO, 0, &info, sizeof(info)) != sizeof(info)) return false;
        group = static_cast<pid_t>(info.pbi_pgid); member.start = static_cast<std::uint64_t>(info.pbi_start_tvsec) * 1000000 + info.pbi_start_tvusec;
        char path[PROC_PIDPATHINFO_MAXSIZE]{};
        if (proc_pidpath(static_cast<int>(pid), path, sizeof(path)) > 0) member.executable = path;
        return info.pbi_status != SZOMB;
#else
        auto prefix = fs::path("/proc") / std::to_string(pid);
        std::ifstream file(prefix / "stat"); std::string line; if (!std::getline(file, line)) return false;
        auto close = line.rfind(')'); if (close == std::string::npos) return false;
        std::istringstream fields(line.substr(close + 2)); std::string field; char state = 0;
        for (unsigned index = 0; index <= 19; ++index) { if (!(fields >> field)) return false; if (index == 0) state = field.front(); else if (index == 2) group = static_cast<pid_t>(std::stol(field)); else if (index == 19) member.start = std::stoull(field); }
        char path[32768]; auto count = readlink((prefix / "exe").c_str(), path, sizeof(path));
        if (count > 0 && count < static_cast<ssize_t>(sizeof(path))) member.executable.assign(path, static_cast<std::size_t>(count));
        return state != 'Z' && state != 'X';
#endif
    }
    bool alive(const Member& member) {
        Member current; pid_t group = 0;
        return processInfo(member.pid, current, group) && current.start == member.start;
    }
    void refresh() {
        std::vector<unsigned long> ids;
#ifdef __APPLE__
        auto bytes = proc_listpids(PROC_PGRP_ONLY, static_cast<uint32_t>(leader), nullptr, 0);
        if (bytes <= 0) return; std::vector<pid_t> pids(static_cast<std::size_t>(bytes) / sizeof(pid_t) + 32);
        bytes = proc_listpids(PROC_PGRP_ONLY, static_cast<uint32_t>(leader), pids.data(), static_cast<int>(pids.size() * sizeof(pid_t)));
        if (bytes < 0) { warn("Cannot enumerate process group"); cleanupComplete = ownedCleanupComplete = false; return; }
        for (int index = 0; index < bytes / static_cast<int>(sizeof(pid_t)); ++index) if (pids[index] > 0) ids.push_back(static_cast<unsigned long>(pids[index]));
#else
        std::error_code error;
        for (auto& entry : fs::directory_iterator("/proc", error)) { auto text = entry.path().filename().string(); if (!text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; })) ids.push_back(std::stoul(text)); }
        if (error) { warn("Cannot enumerate /proc process group"); cleanupComplete = ownedCleanupComplete = false; }
#endif
        for (auto pid : ids) {
            Member member; member.pid = pid; pid_t group = 0;
            if (!processInfo(pid, member, group) || group != leader) continue;
            if (members.contains(pid) && members.at(pid).start == member.start) continue;
            if (!member.executable.empty() && member.start) {
#ifdef __APPLE__
                member.classification = classify(fromUtf8(member.executable));
#else
                member.classification = classify(fs::path("/proc") / std::to_string(pid) / "exe");
#endif
            }
            adoptUnityHelper(member);
            members[pid] = member;
        }
    }
    std::string readPipe(int fd, bool& eof) {
        char buffer[4096]; auto count = read(fd, buffer, sizeof(buffer));
        if (count > 0) return std::string(buffer, static_cast<std::size_t>(count));
        if (!count) eof = true;
        else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) throw std::runtime_error("Cannot read supervisor pipe");
        return {};
    }
#endif
    void receiveStages() {
        bool eof = false;
        stageBuffer += readPipe(stageRead.value, eof);
        if (stageBuffer.size() > 65536) throw std::runtime_error("Stage control message exceeds limit");
        for (;;) {
            auto newline = stageBuffer.find('\n'); if (newline == std::string::npos) break;
            auto line = stageBuffer.substr(0, newline); stageBuffer.erase(0, newline + 1);
            auto event = json::parse(line); auto type = event.at("event").get<std::string>();
            if (type == "heartbeat") continue; // never renew an absolute lease
            if (!leases) continue;
            if (type == "stage-start") {
                if (stageActive) throw std::runtime_error("A stage is already active");
                auto budget = event.at("budgetSeconds").get<double>(); auto name = event.at("name").get<std::string>();
                if (!(budget > 0 && budget <= stageCap) || name.empty() || name.size() > 128) throw std::runtime_error("Invalid stage lease");
                stage = name; stageActive = true;
                stageDeadline = std::min(jobDeadline, monotonicMs() + static_cast<std::int64_t>(budget * 1000) + 30000);
                journal("running");
            } else if (type == "stage-end") {
                if (!stageActive) throw std::runtime_error("Stage end without a stage");
                stageActive = false; stageDeadline = std::min(jobDeadline, monotonicMs() + 30000);
            } else throw std::runtime_error("Unknown stage control message");
        }
    }
    bool anyOwned() {
        for (auto& [pid, member] : members) if (member.classification == "owned-private" && alive(member)) return true;
        return false;
    }
    void signalOwned(bool force) {
        refresh();
        for (auto& [pid, member] : members) {
            if (!alive(member)) continue;
            if (member.classification != "owned-private") {
                warn("Preserved " + member.classification + " pid=" + std::to_string(pid) + "; keep log/cache and require ownership review");
                cleanupComplete = false; continue;
            }
#ifdef _WIN32
            if (!TerminateProcess(member.handle->value, timedOut ? 3 : cancelled ? 4 : 0) && GetLastError() != ERROR_ACCESS_DENIED) { warn("Cannot stop owned pid=" + std::to_string(pid)); cleanupComplete = ownedCleanupComplete = false; }
#else
            // Recheck the immutable process start identity immediately before
            // signaling; never issue a negative-PGID or name-based signal.
            if (alive(member) && kill(static_cast<pid_t>(pid), force ? SIGKILL : SIGTERM) && errno != ESRCH) { warn("Cannot signal owned pid=" + std::to_string(pid)); cleanupComplete = ownedCleanupComplete = false; }
#endif
        }
    }
    void reap() {
#ifndef _WIN32
        int status = 0;
        for (;;) { auto pid = waitpid(-1, &status, WNOHANG); if (pid > 0) continue; if (pid < 0 && errno == EINTR) continue; if (pid < 0 && errno != ECHILD) { warn("Cannot reap subprocess"); cleanupComplete = false; } break; }
#endif
    }
    void cleanup() {
        journal("cleanup"); signalOwned(false);
        auto until = monotonicMs() + 5000;
        while (anyOwned() && monotonicMs() < until) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); reap(); refresh(); }
        signalOwned(true); until = monotonicMs() + 5000;
        while (anyOwned() && monotonicMs() < until) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); reap(); refresh(); }
        reap();
        if (anyOwned()) { warn("Owned processes did not release resources within cleanup deadline"); cleanupComplete = ownedCleanupComplete = false; }
        // We snapshot the pathname; no log pipe EOF wait can be held hostage by
        // a service that intentionally outlives the launcher.
        journal(cleanupComplete ? "finished" : "cleanup-warning");
    }
    void launch(std::uintptr_t channel) {
        for (auto& text : config.at("privateExecutables")) privateFiles.insert(binaryIdentity(fromUtf8(text.get<std::string>())));
        for (auto& text : config.value("unityEditorRoots", json::array())) editorRoots.push_back(fromUtf8(text.get<std::string>()));
        for (auto& text : config.at("sharedExemptExecutables")) exemptFiles.insert(binaryIdentity(fromUtf8(text.get<std::string>())));
#ifdef _WIN32
        // The worker runs under a hidden console (CREATE_NO_WINDOW) so console-subsystem
        // descendants never open a visible window. That console host joins the job; the
        // system conhost.exe (identity-checked, never by name) is a private member and ends with the job.
        try {
            std::wstring system(MAX_PATH, L'\0'); auto length = GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()));
            if (length > 0 && length < system.size()) { system.resize(length); auto host = binaryIdentity(fs::path(system) / L"conhost.exe"); if (!exemptFiles.contains(host)) privateFiles.insert(host); }
        } catch (...) { }
#endif
        for (auto& identity : exemptFiles) if (privateFiles.contains(identity)) throw std::runtime_error("An executable cannot be both private and shared-exempt");
        auto environment = config.at("environment").get<std::vector<std::pair<std::string, std::string>>>();
        stageCap = config.at("stageTimeoutSeconds").get<unsigned>(); jobDeadline = config.at("deadlineMilliseconds").get<std::int64_t>(); leases = config.at("stageLeases").get<bool>();
        stageDeadline = std::min(jobDeadline, start + (leases ? 30000 : static_cast<std::int64_t>(stageCap) * 1000));
        environment.emplace_back("UAB_STAGE_TIMEOUT_SECONDS", std::to_string(stageCap));
        environment.emplace_back("UAB_JOB_DEADLINE_MONOTONIC_MS", std::to_string(jobDeadline));
        environment.emplace_back("UAB_ATTEMPT_NONCE", config.at("nonce").get<std::string>());
#ifdef _WIN32
        control.reset(reinterpret_cast<HANDLE>(channel)); SetHandleInformation(control.value, HANDLE_FLAG_INHERIT, 0);
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE}; HANDLE read = nullptr, write = nullptr;
        if (!CreatePipe(&read, &write, &security, 0)) throw std::runtime_error("Cannot create stage channel"); stageRead.reset(read); stageWrite.reset(write);
        if (!SetHandleInformation(stageRead.value, HANDLE_FLAG_INHERIT, 0)) throw std::runtime_error("Cannot protect stage channel");
        environment.emplace_back("UAB_STAGE_HANDLE", std::to_string(reinterpret_cast<std::uintptr_t>(stageWrite.value)));
        output.reset(CreateFileW(fromUtf8(config.at("log").get<std::string>()).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &security, CREATE_ALWAYS, 0, nullptr));
        input.reset(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr));
        if (output.value == INVALID_HANDLE_VALUE || input.value == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot open worker stdio");
        job.reset(CreateJobObjectW(nullptr, nullptr)); if (!job.value) throw std::runtime_error("Cannot create owned worker job");
        // Deliberately no KILL_ON_JOB_CLOSE: unclassified/shared services must
        // survive host and supervisor loss rather than be terminated blindly.
        auto info = spawn(fromUtf8(config.at("executable").get<std::string>()), config.at("args").get<std::vector<std::string>>(), fromUtf8(config.at("cwd").get<std::string>()), processEnvironment(environment), {stageWrite.value, output.value, input.value}, output.value, input.value, true);
        child.reset(info.hProcess); thread.reset(info.hThread);
        if (!AssignProcessToJobObject(job.value, child.value)) { TerminateProcess(child.value, 3); throw std::runtime_error("Cannot assign owned worker to job"); }
        if (ResumeThread(thread.value) == static_cast<DWORD>(-1)) { TerminateProcess(child.value, 3); throw std::runtime_error("Cannot resume owned worker"); }
        stageWrite.reset(); refresh(); journal("running");
#else
        control.reset(static_cast<int>(channel)); if (fcntl(control.value, F_SETFD, FD_CLOEXEC) || fcntl(control.value, F_SETFL, O_NONBLOCK)) throw std::runtime_error("Cannot protect supervisor control channel");
#ifndef __APPLE__
        if (prctl(PR_SET_CHILD_SUBREAPER, 1)) throw std::runtime_error("Cannot establish supervisor subreaper");
#endif
        int pipes[2]; makePipe(pipes); stageRead.reset(pipes[0]); stageWrite.reset(pipes[1]);
        if (fcntl(stageRead.value, F_SETFL, O_NONBLOCK)) throw std::runtime_error("Cannot configure stage channel");
        environment.emplace_back("UAB_STAGE_FD", "3");
        output.reset(open(fromUtf8(config.at("log").get<std::string>()).c_str(), O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600));
        if (output.value < 0) throw std::runtime_error("Cannot create process log");
        leader = spawn(fromUtf8(config.at("executable").get<std::string>()), config.at("args").get<std::vector<std::string>>(), fromUtf8(config.at("cwd").get<std::string>()), processEnvironment(environment), stageWrite.value, output.value, true);
        stageWrite.reset(); refresh(); journal("running");
#endif
    }
    void run() {
        for (;;) {
            refresh();
            bool eof = false; auto command = readPipe(control.value, eof);
            if (eof) { parentGone = true; exitCode = 3; warn("Host control pipe closed; interrupted request cleaned selectively"); break; }
            if (command.find('C') != std::string::npos) { cancelled = true; exitCode = 4; break; }
            receiveStages(); auto now = monotonicMs();
            if (command.find('T') != std::string::npos || now >= jobDeadline) { timedOut = true; timeoutLayer = "supervisor"; exitCode = 3; break; }
            if (now >= stageDeadline) { timedOut = true; timeoutLayer = leases ? "supervisor" : "stage"; exitCode = 3; break; }
#ifdef _WIN32
            auto done = WaitForSingleObject(child.value, 0);
            if (done == WAIT_OBJECT_0) { DWORD value = 3; if (!GetExitCodeProcess(child.value, &value)) throw std::runtime_error("Cannot read process exit code"); exitCode = static_cast<int>(value); break; }
            if (done != WAIT_TIMEOUT) throw std::runtime_error("Cannot wait for owned process");
#else
            int status = 0; auto done = waitpid(leader, &status, WNOHANG);
            if (done == leader) { exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 3; break; }
            if (done < 0 && errno != EINTR) throw std::runtime_error("Cannot wait for owned process");
#endif
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        cleanup();
    }
    void finish() {
        writeJson(root / "result.json", {{"nonce", config.at("nonce")}, {"exitCode", exitCode}, {"timedOut", timedOut}, {"cancelled", cancelled},
            {"parentGone", parentGone}, {"timeoutLayer", timeoutLayer}, {"stage", stage}, {"elapsedMilliseconds", monotonicMs() - start},
            {"cleanupComplete", cleanupComplete}, {"ownedCleanupComplete", ownedCleanupComplete}, {"cleanupWarnings", warnings}});
    }
};
}
int processSupervisorMain(int argc, char** argv) {
    if (argc != 3) return 64;
#ifdef _WIN32
    // Loader failures must report through the result channel, never block the
    // noninteractive supervisor behind a Windows critical-error dialog.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
    Runner runner;
    try {
        auto request = fs::absolute(fromUtf8(argv[1])); runner.root = request.parent_path(); runner.config = json::parse(std::ifstream(request));
        if (runner.config.at("schemaVersion") != 1) throw std::runtime_error("Unsupported supervisor protocol");
        auto channel = static_cast<std::uintptr_t>(std::stoull(argv[2])); runner.launch(channel); runner.run(); runner.finish(); return 0;
    } catch (const std::exception& error) {
        runner.warn(std::string("Supervisor failure: ") + error.what()); runner.exitCode = 3; runner.cleanupComplete = false;
        try {
#ifdef _WIN32
            if (runner.job.value) runner.cleanup();
#else
            if (runner.leader > 0) runner.cleanup();
#endif
            if (!runner.config.is_null()) runner.finish();
        } catch (...) { }
        return 3;
    }
}
}
