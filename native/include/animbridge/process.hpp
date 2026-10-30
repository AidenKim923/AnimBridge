#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace animbridge {
struct ProcessResult;
struct ProcessRunOptions {
    unsigned jobTimeoutSeconds = 3600;
    // steady_clock milliseconds; shared by every worker within one plan item.
    std::int64_t absoluteDeadlineMilliseconds = 0;
    bool stageLeases = false;
    std::filesystem::path journalRoot;
    // Only explicit, verified executable file identities can be terminated.
    std::vector<std::filesystem::path> privateExecutables, sharedExemptExecutables;
};
ProcessResult runSupervisedProcess(const std::filesystem::path& executable,
    const std::vector<std::string>& arguments, const std::filesystem::path& cwd,
    const std::filesystem::path& log, unsigned stageTimeoutSeconds,
    const std::vector<std::pair<std::string, std::string>>& environment,
    std::shared_ptr<std::atomic_bool> cancel, const ProcessRunOptions& options);
// Recovery is diagnostic only: PID/PGID values from an old journal never grant
// authority to terminate an existing process after a supervisor crash.
std::vector<std::string> processRecoveryWarnings(const std::filesystem::path& journalRoot);
// Internal entry point used only by the dedicated, job-external executable.
int processSupervisorMain(int argc, char** argv);
}
