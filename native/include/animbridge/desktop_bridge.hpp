#pragma once
#include "core.hpp"
#include "platform_services.hpp"
#include "path_grants.hpp"
#include <nlohmann/json.hpp>
#include <atomic>
#include <array>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

namespace animbridge {
// Mutations use one exclusive worker; independent previews use a bounded read pool.
// Callbacks marshal JSON to the UI thread.
class DesktopBridge {
public:
    using Json = nlohmann::json;
    using Sender = std::function<void(const Json&)>;
    using Picker = std::function<std::vector<fs::path>(const Json&, bool)>;
    using SavePicker = std::function<fs::path(const Json&)>;
    DesktopBridge(Sender send, Picker pick, SavePicker save, void* window, const fs::path& dataRoot = {});
    DesktopBridge(Sender send, DesktopPlatformServices services, const fs::path& dataRoot = {});
    ~DesktopBridge();
    void handle(const Json& request, const std::vector<fs::path>& dropped = {});
    void ingest(const std::vector<fs::path>& paths, GrantSource source = GrantSource::NativeDrop);
    bool busy() const { return busy_.load(); }
    bool working() const { return working_.load() || readOutstanding_.load() != 0; }
    void cancel();
private:
    Json state() const;
    Json stateLocked() const;
    void emitState();
    void persist();
    void autoConnectUnity();
    void autoConnectUnreal();
    void compactAttemptsLocked();
    void reply(const Json& id, const Json& result);
    void error(const Json& id, const std::string& code, const std::exception& message);
    void background(const Json& id, std::function<Json()> task, bool replyNow = false, Json accepted = Json::object(), bool blocksWorkspace = true);
    void backgroundRead(const Json& id, const std::string& operation, Json payload);
    void readLoop();
    Json scan(const std::vector<fs::path>& paths);
    // Forgets a SOURCE LIBRARY root; releaseCache also deletes its unused extraction.
    Json removeSource(const fs::path& path, bool releaseCache);
    Json helper(const std::string& operation, Json payload);
    // Raw .uasset -> FBX through a headless Unreal Editor run by the worker.
    Json prepareUnrealConversion(const Json& payload) const;
    Json convertUnrealAsset(const Json& prepared);
    Options options(const Json& value) const;
    Plan plan(const Json& payload, bool preflight = false) const;
    Json run(Plan plan, Json optionSnapshot);
    void approveDefaultOutputRoot();
    bool allowedPath(const fs::path& path) const;
    Json authorizePayload(const Json& payload, const std::string& action) const;
    void verifyPlanGrants(const Plan&) const;
    Sender send_;
    DesktopPlatformServices services_;
    RuntimeContext context_;
    PathGrantRegistry grants_;
    Json staleGrants_ = Json::array();
    Json legacyState_;
    Json identityRecords_ = Json::array();
    std::string legacyStateHash_;
    bool migrated_ = false;
    std::shared_ptr<std::atomic_bool> alive_ = std::make_shared<std::atomic_bool>(true);
    mutable std::mutex mutex_;
    std::mutex persistMutex_;
    std::mutex stateEmitMutex_;
    Session session_;
    Json settings_, packages_, jobs_, history_, scan_, unity_, skeleton_, unityAuto_, unrealAuto_;
    Json warningMessages_ = Json::array();
    fs::path root_, statePath_, cache_;
    std::string cacheIdentity_;
    std::thread worker_;
    static constexpr unsigned ReadConcurrency = 2;
    std::array<std::thread, ReadConcurrency> readWorkers_;
    std::mutex workMutex_;
    std::condition_variable readReady_;
    std::deque<std::function<void()>> readTasks_;
    std::atomic_uint readOutstanding_{0};
    bool stopping_ = false;
    std::atomic_bool busy_{false};
    std::atomic_bool working_{false};
    std::atomic_bool pickerOutstanding_{false};
    bool persistAllowed_ = true;
    Json completedEvent_;
    std::shared_ptr<std::atomic_bool> cancel_ = std::make_shared<std::atomic_bool>(false);
    std::unique_ptr<Plan> lastPlan_;
    Json lastOptionSnapshot_ = Json::object();
    std::unique_ptr<Plan> pendingPlan_;
    std::vector<Result> lastResults_;
    std::set<std::string> savedSelection_;
    // Update checks never occupy the workspace worker; one dedicated thread runs at a time.
    std::thread updateWorker_;
    std::thread unityDiscoveryWorker_;
    std::thread unrealDiscoveryWorker_;
    std::atomic_bool updateRunning_{false};
    std::atomic_bool updateCancel_{false};
    std::string latestReleaseTag_;  // guarded by mutex_
    // Version and assets of the newer release found by the last check; guarded by mutex_.
    std::string pendingUpdateVersion_;
    std::string pendingInstallerUrl_, pendingChecksumsUrl_;
    std::uint64_t pendingInstallerSize_ = 0;
    // Runs on updateWorker_: download, verify, launch the installer, then close the app.
    void installUpdate(const std::string& version, const std::string& installerUrl, const std::string& checksumsUrl, std::uint64_t installerSize);
    void emitUpdate(Json data);
};
}
