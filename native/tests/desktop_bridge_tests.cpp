#include "animbridge/desktop_bridge.hpp"
#include "animbridge/app_config.hpp"
#include "animbridge/ui_message.hpp"
#include "animbridge/update_check.hpp"
#include "animbridge/path_identity.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <map>
#include <future>
#include <stdexcept>

using namespace animbridge;
using Json = nlohmann::json;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void write(const fs::path& path, const std::string& text) { fs::create_directories(path.parent_path()); std::ofstream out(path, std::ios::binary); out << text; }
std::string read(const fs::path& path) { std::ifstream file(path, std::ios::binary); return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()}; }
struct Harness {
    std::mutex mutex;
    std::condition_variable changed;
    std::map<std::string, Json> replies;
    std::vector<Json> events;
    std::function<void(const Json&)> beforeSend;
    std::unique_ptr<DesktopBridge> bridge;
    fs::path fixtureRoot;
    std::vector<fs::path> pickerChoices;
    std::map<std::string, std::string> approved;
    unsigned serial = 0;
    explicit Harness(const fs::path& root) : fixtureRoot(root) {
        bridge = std::make_unique<DesktopBridge>([this](const Json& value) {
            if (beforeSend) beforeSend(value);
            std::lock_guard lock(mutex);
            if (value.contains("id")) replies[value["id"].get<std::string>()] = value;
            else events.push_back(value);
            changed.notify_all();
        }, [this](const Json&, bool) { return pickerChoices; }, [](const Json&) { return fs::path{}; }, nullptr, root);
    }
    ~Harness() { bridge.reset(); }
    std::string approve(fs::path path, const std::string& field) {
        if (!path.is_absolute()) path = fixtureRoot / path;
        path = fs::absolute(path).lexically_normal();
        auto key = utf8(path) + ":" + field;
        if (approved.contains(key)) return approved.at(key);
        const bool folder = field == "outputRoot" || field == "output" || field.find("Project") != std::string::npos;
        if (folder) fs::create_directories(path);
        else if (!fs::exists(path)) write(path, "approved native test fixture");
#ifndef _WIN32
        if (field == "python" || field == "blender") fs::permissions(path, fs::perms::owner_exec, fs::perm_options::add);
#endif
        pickerChoices = {path}; auto id = std::to_string(++serial);
        bridge->handle({{"id", id}, {"action", folder ? "chooseFolder" : "chooseFiles"}, {"payload", {{"purpose", "setting-" + field}, {"engine", (field == "python" || field == "blender") ? "runtime" : "unity"}}}});
        auto picked = response(id); if (!picked.value("ok", false)) throw std::runtime_error(picked.dump());
        auto until = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (bridge->working()) { if (std::chrono::steady_clock::now() > until) throw std::runtime_error("Picker completion timeout"); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        return approved[key] = picked["result"]["grants"][0]["grant"].get<std::string>();
    }
    Json authorized(Json payload, const std::string& action) {
        for (const auto* nested : {"options", "settings"}) if (payload.contains(nested) && payload[nested].is_object()) payload[nested] = authorized(payload[nested], action);
        if (action == "scan" && payload.contains("paths") && !bridge->working()) {
            payload["grants"] = Json::array(); for (const auto& path : payload["paths"]) payload["grants"].push_back(approve(fromUtf8(path.get<std::string>()), "source"));
        }
        for (const auto* field : {"source", "path", "skeleton", "model", "outputRoot", "output", "python", "blender"}) {
            if (!payload.contains(field) || !payload[field].is_string() || payload[field].get<std::string>().empty() || payload.contains(std::string(field) + "Grant")) continue;
            auto path = fromUtf8(payload[field].get<std::string>()); if (!path.is_absolute()) path = fixtureRoot / path;
            auto key = utf8(fs::absolute(path).lexically_normal()) + ":" + field;
            if (bridge->working() && !approved.contains(key)) continue;
            payload[std::string(field) + "Grant"] = approve(path, field); payload[field] = utf8(fs::canonical(path));
        }
        return payload;
    }
    std::string send(const std::string& action, Json payload = Json::object()) {
        payload = authorized(std::move(payload), action);
        auto id = std::to_string(++serial); bridge->handle({{"id", id}, {"action", action}, {"payload", payload}}); return id;
    }
    Json raw(const std::string& action, Json payload) { auto id = std::to_string(++serial); bridge->handle({{"id", id}, {"action", action}, {"payload", payload}}); return response(id); }
    Json response(const std::string& id) {
        std::unique_lock lock(mutex);
        if (!changed.wait_for(lock, std::chrono::seconds(30), [&] { return replies.contains(id); })) throw std::runtime_error("IPC response timeout");
        auto value = replies.at(id); replies.erase(id); return value;
    }
    Json call(const std::string& action, Json payload = Json::object()) { return response(send(action, std::move(payload))); }
    Json ok(const std::string& action, Json payload = Json::object()) {
        auto value = call(action, std::move(payload)); if (!value.value("ok", false)) throw std::runtime_error(value.dump()); return value["result"];
    }
    Json idle() {
        auto end = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (bridge->working()) { if (std::chrono::steady_clock::now() > end) throw std::runtime_error("Worker timeout"); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
        return ok("getState");
    }
};
}
int main(int argc, char** argv) {
    // A real child process acts as the evaluator, so concurrency, cancellation
    // and owned-process cleanup are exercised through the production IPC path.
    // The Unity Editor probe runs "<editor> -version"; a copy of this binary stands in as the Editor.
    if (argc == 2 && std::string(argv[1]) == "-version") { std::cout << "6000.4.7f1\n"; return 0; }
    auto module = 1;
    for (int index = 1; index < argc; ++index) if (std::string(argv[index]) == "-m") { module = index; break; }
    if (argc > module + 6 && std::string(argv[module]) == "-m" && std::string(argv[module + 1]) == "unityanimbridge.desktop") {
        fs::path request, response;
        for (int i = module + 3; i + 1 < argc; ++i) { if (std::string(argv[i]) == "--request") request = fromUtf8(argv[i + 1]); if (std::string(argv[i]) == "--out") response = fromUtf8(argv[i + 1]); }
        auto payload = Json::parse(std::ifstream(request));
        auto probe = fromUtf8(payload.at("parallelProbe").get<std::string>());
        auto name = payload.at("probeId").get<std::string>();
        write(probe / (name + ".started"), request.string());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (!fs::exists(probe / "release") && !fs::exists(probe / (name + ".release")) && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        write(response, Json{{"status", "ready"}, {"source", payload.value("source", "")}, {"skeleton", payload.value("skeleton", "")}, {"request", utf8(request)}}.dump());
        return 0;
    }
    try {
        auto root = fs::absolute(argc > 1 ? fromUtf8(argv[1]) : fs::path("desktop-tests")) / newIdentity(); fs::create_directories(root);
        unsigned passed = 0;
        {
            // Preview concurrency follows logical cores, keeps 1.5 GB of RAM per preview, and stays within 2..16.
            const std::uint64_t gb = 1024ull * 1024 * 1024;
            require(previewReadConcurrency(16, 32 * gb) == 16, "16 cores with plenty of RAM should run 16 previews");
            require(previewReadConcurrency(16, 6 * gb) == 4, "RAM should cap the preview count");
            require(previewReadConcurrency(64, 256 * gb) == 16, "preview count is capped at 16");
            require(previewReadConcurrency(1, 2 * gb) == 2 && previewReadConcurrency(0, 0) == 2, "at least two previews run at once");
            require(previewReadConcurrency(8, 0) == 8, "unknown RAM falls back to the core count");
            ++passed;
        }
        // The scenarios below check the read pool against a fixed bound of two.
#ifdef _WIN32
        _putenv_s("ANIMBRIDGE_READ_CONCURRENCY", "2");
#else
        setenv("ANIMBRIDGE_READ_CONCURRENCY", "2", 1);
#endif
        require(machineReadConcurrency() == 2, "ANIMBRIDGE_READ_CONCURRENCY override is ignored");
        {
            const auto localized = messageData(UiError("native.helperNoResult", "Evaluator failed: <raw log>", {{"log", "<raw log>"}, {"exitCode", 7}}));
            require(localized["messageKey"] == "native.helperNoResult" && localized["messageParams"]["log"] == "<raw log>", "Native message parameters were altered");
            const auto external = messageData(std::runtime_error("External engine log: raw {detail}"));
            require(external["message"] == "External engine log: raw {detail}" && !external.contains("messageKey"), "External diagnostic was marked for translation");
            ++passed;
        }
        {
            // The default output folder is chosen by the app, never in a picker, so
            // the app must create and approve it itself; otherwise the first export
            // plan fails with a missing approval.
            Harness h(root / "default-output");
            const auto state = h.ok("getState");
            const auto expected = utf8(fs::canonical(root / "default-output" / "exports"));
            require(state["settings"]["outputRoot"] == expected && !state["settings"].contains("outputRootApproval"), "Default output folder was not created and approved");
            bool granted = false;
            for (const auto& grant : state["pathGrants"]) if (grant["purpose"] == "exportRoot" && grant["source"] == "app-output" && grant["path"] == expected) granted = true;
            require(granted, "Default output folder has no export approval");
            ++passed;
        }
        {
            Harness h(root / "app-info");
            const auto info = h.ok("getAppInfo");
            require(info["version"].get<std::string>() == app::Version, "Desktop version differs from build metadata");
            for (const auto link : {"github", "fairy", "githubSponsors"}) {
                require(info["links"][link] == !app::externalLinkUrl(link).empty(), "Link availability differs from configured URL");
                if (app::externalLinkUrl(link).empty()) {
                    const auto response = h.call("openExternalLink", {{"link", link}, {"url", "https://example.com"}});
                    require(!response.value("ok", true) && response["error"]["code"] == "LINK_NOT_CONFIGURED", "UI-supplied URL bypassed the fixed link configuration");
                }
            }
            const auto unknown = h.call("openExternalLink", {{"link", "https://example.com"}});
            require(!unknown.value("ok", true) && unknown["error"]["code"] == "UNKNOWN_LINK", "Unknown external destination was accepted");
            const auto beforeCheck = h.call("openExternalLink", {{"link", "latestRelease"}});
            require(!beforeCheck.value("ok", true) && beforeCheck["error"]["code"] == "LINK_NOT_CONFIGURED", "latestRelease opened before any update check");
            const auto unsupported = h.ok("checkForUpdate");
            require(unsupported["supported"] == false && unsupported["available"] == false && unsupported["current"] == std::string(app::Version), "Update check without a network service was not reported unsupported");
            ++passed;
        }
        {
            // Update check with an injected network service; no real network access.
            std::mutex m; std::condition_variable cv; std::map<std::string, Json> replies; std::vector<std::string> opened; std::string requestedUrl;
            std::string body = "[]"; bool fail = false;
            DesktopPlatformServices services;
            services.httpGet = [&](const std::string& url) { requestedUrl = url; if (fail) throw std::runtime_error("offline"); return body; };
            services.openExternal = [&](const std::string& url) { opened.push_back(url); };
            DesktopBridge bridge([&](const Json& v) { std::lock_guard lock(m); if (v.contains("id")) replies[v["id"].get<std::string>()] = v; cv.notify_all(); }, services, root / "update-check");
            auto call = [&](const std::string& id, const std::string& action, Json payload) {
                bridge.handle({{"id", id}, {"action", action}, {"payload", payload}});
                std::unique_lock lock(m);
                if (!cv.wait_for(lock, std::chrono::seconds(30), [&] { return replies.contains(id); })) throw std::runtime_error("Update reply timeout");
                auto v = replies.at(id); replies.erase(id); return v;
            };
            const std::string current(app::Version);
            const bool configured = !update::releasesApiUrl(app::GithubUrl).empty();
            const auto none = call("u0", "checkForUpdate", Json::object());
            if (!configured) require(none["result"]["supported"] == false, "Unconfigured repository reported update support");
            else {
                require(none["ok"] == true && none["result"]["supported"] == true && none["result"]["available"] == false && none["result"]["current"] == current, "Empty release list was not 'no update'");
                require(requestedUrl.rfind("https://api.github.com/repos/", 0) == 0, "Update check used a non-GitHub API URL");
                body = Json::array({{{"tag_name", "v999.0.1"}, {"name", "Future"}, {"prerelease", true}, {"draft", false}},
                                    {{"tag_name", "v0.0.1"}, {"name", "Old"}}}).dump();
                const auto found = call("u1", "checkForUpdate", Json::object());
                const auto& r = found["result"];
                require(found["ok"] == true && r["available"] == true && r["latest"] == "999.0.1" && r["name"] == "Future" && r["prerelease"] == true && r["current"] == current, "Newer release was not reported");
                const auto link = call("u2", "openExternalLink", {{"link", "latestRelease"}, {"url", "https://example.com"}});
                require(link["ok"] == true && opened.size() == 1 && opened[0] == std::string(app::GithubUrl) + "/releases/tag/v999.0.1", "latestRelease did not open the checked release tag");
                body = Json::array({{{"tag_name", "v0.0.1"}}}).dump();
                const auto older = call("u3", "checkForUpdate", Json::object());
                require(older["ok"] == true && older["result"]["available"] == false && older["result"]["latest"] == "0.0.1", "Older release was reported as an update");
                body = "not json";
                const auto bad = call("u4", "checkForUpdate", Json::object());
                require(bad["ok"] == false && bad["error"]["code"] == "UPDATE_CHECK_FAILED", "Malformed release list was not reported as a failure");
                fail = true;
                const auto offline = call("u5", "checkForUpdate", Json::object());
                require(offline["ok"] == false && offline["error"]["code"] == "UPDATE_CHECK_FAILED", "Network failure was not reported");
            }
            ++passed;
        }
        {
            using namespace update;
            require(releasesApiUrl("https://github.com/AidenKim923/AnimBridge") == "https://api.github.com/repos/AidenKim923/AnimBridge/releases?per_page=20", "Releases API URL not derived");
            require(releasesApiUrl("https://github.com/Owner/Repo.git/") == "https://api.github.com/repos/Owner/Repo/releases?per_page=20", "Releases API URL ignored .git/");
            for (const char* bad : {"", "http://github.com/a/b", "https://evil.example/a/b", "https://github.com/a", "https://github.com/a/b/c", "https://github.com/a/b?x=1", "https://github.com.evil.io/a/b"})
                require(releasesApiUrl(bad).empty(), "Unexpected releases API URL accepted");
            require(isNewer("0.2.6", "0.2.5") && isNewer("v0.3.0", "0.2.9") && isNewer("1.0.0", "0.99.99"), "isNewer missed a newer version");
            require(!isNewer("0.2.5", "0.2.5") && !isNewer("0.2.4", "0.2.5") && !isNewer("garbage", "0.2.5") && !isNewer("0.2.6", "garbage"), "isNewer accepted a non-newer version");
            require(versionParts("v0.2.6-beta.1") == std::vector<int>({0, 2, 6}) && versionParts("0.2") .empty() && versionParts("v1.2.3/../x").empty(), "versionParts mishandled tags");
            const Json list = Json::array({
                {{"tag_name", "v0.2.4"}, {"name", "Old"}},
                {{"tag_name", "v9.9.9"}, {"draft", true}},
                {{"tag_name", "latest"}},
                {{"tag_name", 7}},
                {{"tag_name", "v0.2.7"}, {"name", 5}, {"prerelease", "yes"}},
                "string entry",
                {{"tag_name", "v0.2.6"}, {"name", "Mid"}, {"prerelease", true}},
                {{"tag_name", "v8.0.0"}, {"draft", "no"}}});
            const auto newest = newestRelease(list);
            require(newest && newest->tag == "v0.2.7" && newest->version == "0.2.7" && newest->name.empty() && !newest->prerelease, "newestRelease picked the wrong release");
            require(!newestRelease(Json::array()) && !newestRelease(Json::object()), "newestRelease invented a release");
            const auto pre = newestRelease(Json::array({{{"tag_name", "v1.0.0"}, {"name", "One"}, {"prerelease", true}}}));
            require(pre && pre->prerelease && pre->name == "One", "Prerelease flag lost");
            ++passed;
        }
        {
            // In-app update helpers: installer name, download host allowlist, SHA256SUMS parsing, asset selection.
            using namespace update;
            require(installerAssetName("1.0.3") == "AnimBridge-1.0.3-Setup-x64.exe", "Installer asset name changed");
            for (const char* good : {"https://github.com/AidenKim923/AnimBridge/releases/download/v1.0.3/AnimBridge-1.0.3-Setup-x64.exe",
                                     "https://objects.githubusercontent.com/github-production-release-asset/1?X-Amz-Signature=ab%2Fc&response-content-disposition=attachment",
                                     "https://release-assets.githubusercontent.com/github-production-release-asset/1/2?sp=r&sv=2018-11-09"})
                require(isAllowedDownloadUrl(good), "Allowed GitHub download URL rejected");
            for (const char* bad : {"", "http://github.com/a/b", "https://evil.example/x", "https://github.com.evil.io/x", "https://evilgithub.com/x",
                                    "https://user@github.com/x", "https://github.com:8443/x", "https://github.com", "https://GITHUB.com/x",
                                    "https://github.com/x#frag", "https://github.com/x y", "https://api.github.com/x", "file:///C:/x.exe"})
                require(!isAllowedDownloadUrl(bad), "Untrusted download URL accepted");

            const std::string hashA(64, 'a'), hashB(64, 'b');
            const std::string sums = hashB + "  AnimBridge-1.0.2-Setup-x64.exe\r\n" + std::string(64, 'A') + " *AnimBridge-1.0.3-Setup-x64.exe\r\n# comment\n\n";
            require(checksumFor(sums, "AnimBridge-1.0.3-Setup-x64.exe") == hashA, "Binary-mode checksum line not parsed (or not lowercased)");
            require(checksumFor(sums, "AnimBridge-1.0.2-Setup-x64.exe") == hashB, "Text-mode checksum line not parsed");
            require(!checksumFor(sums, "AnimBridge-1.0.3-Setup-x64.ex"), "Partial file name matched a checksum");
            require(!checksumFor(sums, "AnimBridge-9.9.9-Setup-x64.exe") && !checksumFor("", "x"), "Missing file produced a checksum");
            require(!checksumFor("abc  AnimBridge-1.0.3-Setup-x64.exe\n", "AnimBridge-1.0.3-Setup-x64.exe"), "Short hash accepted");
            require(!checksumFor(hashA + "  f.exe\n" + hashB + "  f.exe\n", "f.exe"), "Conflicting checksum lines accepted");
            require(checksumFor(hashA + "  f.exe\n" + hashA + " *f.exe", "f.exe") == hashA, "Repeated identical checksum rejected");

            const std::string base = "https://github.com/AidenKim923/AnimBridge/releases/download/v1.0.3/";
            Json release = {{"tag_name", "v1.0.3"}, {"assets", Json::array({
                {{"name", "AnimBridge-1.0.3-Setup-x64.exe.sig"}, {"browser_download_url", base + "AnimBridge-1.0.3-Setup-x64.exe.sig"}},
                {{"name", "AnimBridge-1.0.3-Setup-x64.exe"}, {"browser_download_url", base + "AnimBridge-1.0.3-Setup-x64.exe"}, {"size", 1234}},
                {{"name", "SHA256SUMS.txt"}, {"browser_download_url", base + "SHA256SUMS.txt"}}})}};
            const auto assets = selectAssets(release, "1.0.3");
            require(assets && assets->installerUrl == base + "AnimBridge-1.0.3-Setup-x64.exe" && assets->checksumsUrl == base + "SHA256SUMS.txt" && assets->installerSize == 1234, "Release assets not selected");
            require(!selectAssets(release, "1.0.4"), "Installer for another version accepted");
            auto noSums = release; noSums["assets"].erase(2);
            require(!selectAssets(noSums, "1.0.3"), "Release without SHA256SUMS.txt was installable");
            auto offsite = release; offsite["assets"][1]["browser_download_url"] = "https://evil.example/AnimBridge-1.0.3-Setup-x64.exe";
            require(!selectAssets(offsite, "1.0.3"), "Installer hosted outside GitHub accepted");
            require(newestRelease(Json::array({release}))->assets.has_value(), "newestRelease dropped installable assets");
            ++passed;
        }
        if (!update::releasesApiUrl(app::GithubUrl).empty()) {
            // installUpdate end to end with fake network and launch services: no real download or installer.
            std::mutex m; std::condition_variable cv; std::map<std::string, Json> replies; std::vector<Json> updates;
            const std::string installerBytes = "fake installer bytes";
            std::string sumsBody, launchedArgs; fs::path launched; int exits = 0; bool slowInstaller = false;
            const std::string base = "https://github.com/AidenKim923/AnimBridge/releases/download/v999.0.1/";
            const auto installerName = update::installerAssetName("999.0.1");
            DesktopPlatformServices services;
            services.httpGet = [&](const std::string&) {
                return Json::array({{{"tag_name", "v999.0.1"}, {"assets", Json::array({
                    {{"name", installerName}, {"browser_download_url", base + installerName}},
                    {{"name", "SHA256SUMS.txt"}, {"browser_download_url", base + "SHA256SUMS.txt"}}})}}}).dump();
            };
            services.download = [&](const std::string& url, const fs::path& destination, std::uint64_t maxBytes, const DesktopPlatformServices::DownloadProgress& progress) {
                const bool sums = url == base + "SHA256SUMS.txt";
                require(sums || url == base + installerName, "Update downloaded an unexpected URL");
                require(sums ? maxBytes < 16u * 1024 * 1024 : maxBytes >= 1024u * 1024 * 1024, "Unexpected download size limit");
                if (!sums && slowInstaller) {
                    // Keep reporting progress until the bridge asks to stop (cancel test).
                    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
                    for (std::uint64_t received = 0; std::chrono::steady_clock::now() < until; ++received) {
                        if (!progress(received, 1000)) return false;
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    }
                    throw std::runtime_error("cancel never arrived");
                }
                write(destination, sums ? sumsBody : installerBytes);
                return progress(installerBytes.size(), installerBytes.size());
            };
            services.launchInstaller = [&](const fs::path& installer, const std::vector<std::string>& arguments) {
                launched = installer; launchedArgs.clear();
                for (const auto& argument : arguments) launchedArgs += argument + " ";
            };
            services.requestExit = [&] { ++exits; };
            DesktopBridge bridge([&](const Json& v) {
                std::lock_guard lock(m);
                if (v.contains("id")) replies[v["id"].get<std::string>()] = v;
                else if (v.value("event", "") == "updateProgress") updates.push_back(v["data"]);
                cv.notify_all();
            }, services, root / "update-install");
            auto call = [&](const std::string& id, const std::string& action) {
                bridge.handle({{"id", id}, {"action", action}, {"payload", Json::object()}});
                std::unique_lock lock(m);
                if (!cv.wait_for(lock, std::chrono::seconds(30), [&] { return replies.contains(id); })) throw std::runtime_error("Update reply timeout");
                auto v = replies.at(id); replies.erase(id); return v;
            };
            // Waits for the final event of one install attempt and returns its state.
            auto finalState = [&] {
                std::unique_lock lock(m);
                const auto isFinal = [&] { return !updates.empty() && std::set<std::string>{"installing", "failed", "cancelled"}.contains(updates.back().value("state", "")); };
                if (!cv.wait_for(lock, std::chrono::seconds(30), isFinal)) throw std::runtime_error("Update never finished");
                auto last = updates.back(); updates.clear(); return last;
            };
            auto waitIdle = [&] { for (int i = 0; i < 2000 && call("idle-" + std::to_string(i), "cancelUpdate")["result"]["cancelling"] == true; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5)); };

            const auto early = call("i0", "installUpdate");
            require(early["ok"] == false && early["error"]["messageKey"] == "native.updateNotReady", "installUpdate ran before a successful check");
            const auto checked = call("i1", "checkForUpdate");
            require(checked["ok"] == true && checked["result"]["installable"] == true, "Release with installer and checksums was not installable");

            // Wrong checksum: nothing launched and nothing left on disk.
            sumsBody = std::string(64, '0') + " *" + installerName + "\n";
            require(call("i2", "installUpdate")["ok"] == true, "installUpdate was not accepted");
            auto last = finalState(); waitIdle();
            require(last["state"] == "failed" && last["messageKey"] == "native.updateChecksumMismatch", "Checksum mismatch was not reported");
            require(launched.empty() && exits == 0, "Unverified installer was launched");
            const auto updateFolder = root / "update-install" / "cache" / "updates";
            require(!fs::exists(updateFolder / installerName) && !fs::exists(updateFolder / (installerName + ".partial")), "Rejected installer was left on disk");

            // No checksum line for the installer: refused before the large download.
            sumsBody = std::string(64, '0') + " *SomethingElse.exe\n";
            require(call("i3", "installUpdate")["ok"] == true, "installUpdate was not accepted");
            last = finalState(); waitIdle();
            require(last["state"] == "failed" && last["messageKey"] == "native.updateChecksumMissing" && launched.empty(), "Missing checksum was not refused");

            // Cancel during the download.
            sumsBody = textSha256(installerBytes) + " *" + installerName + "\n";
            slowInstaller = true;
            require(call("i4", "installUpdate")["ok"] == true, "installUpdate was not accepted");
            {
                std::unique_lock lock(m);
                cv.wait_for(lock, std::chrono::seconds(30), [&] { return std::any_of(updates.begin(), updates.end(), [](const Json& u) { return u.value("received", 0) > 0; }); });
            }
            require(call("i5", "cancelUpdate")["ok"] == true, "cancelUpdate failed");
            last = finalState(); waitIdle();
            require(last["state"] == "cancelled" && launched.empty() && exits == 0, "Cancelled update still launched");

            // Matching checksum: the verified installer is launched silently and the app asked to close.
            slowInstaller = false;
            require(call("i6", "installUpdate")["ok"] == true, "installUpdate was not accepted");
            last = finalState(); waitIdle();
            require(last["state"] == "installing" && last["version"] == "999.0.1", "Verified update did not reach installing");
            require(launched.filename() == fromUtf8(installerName) && read(launched) == installerBytes, "Launched file is not the verified installer");
            require(launchedArgs.find("/VERYSILENT") != std::string::npos && launchedArgs.find("/RELAUNCH=1") != std::string::npos, "Installer switches missing");
            require(exits == 1, "App was not asked to close after launching the installer");
            ++passed;
        }
        {
            Harness h(root / "raw-authority"); auto input = h.fixtureRoot / "input.anim"; write(input, "native fixture");
            require(!h.raw("scan", {{"paths", {utf8(input)}}}).value("ok", true), "Raw UI paths acquired source authority");
            require(!h.raw("preview", {{"source", utf8(input)}}).value("ok", true), "Raw helper source acquired read authority");
            require(!h.raw("saveSettings", {{"settings", {{"outputRoot", utf8(h.fixtureRoot)}}}}).value("ok", true), "Raw setting acquired output authority");
            auto unauthorized = h.fixtureRoot / "must-not-be-created" / "nested";
            require(!h.raw("saveSettings", {{"settings", {{"outputRoot", utf8(unauthorized)}}}}).value("ok", true), "Raw setting created output authority");
            require(!fs::exists(unauthorized.parent_path()), "Unauthorized settings created directories before grant validation");
            require(!h.raw("scan", {{"grants", {"forged"}}}).value("ok", true), "Forged opaque grant acquired read authority");
            ++passed;
        }
        for (const auto* language : {"en", "es", "ja", "ko", "zh-CN", "zh-TW", "pl", "hi", "ru", "ar", "pt-BR", "pt-PT", "fr", "th", "tr"}) {
            auto folder = root / ("language-" + std::string(language));
            {
                Harness h(folder);
                require(h.ok("getState")["settings"]["language"] == "en", "Default interface language is not English");
                require(h.ok("saveSettings", {{"settings", {{"language", language}}}})["language"] == language, "Supported language was not saved");
            }
            Harness loaded(folder);
            require(loaded.ok("getState")["settings"]["language"] == language, "Language changed after restart");
            ++passed;
        }
        {
            Harness h(root / "invalid-language-save");
            h.ok("saveSettings", {{"settings", {{"language", "ja"}}}});
            for (const auto& invalid : Json::array({"unsupported", "EN", "", 42, nullptr, Json::object(), Json::array()})) {
                const auto response = h.call("saveSettings", {{"settings", {{"language", invalid}}}});
                require(!response.value("ok", true) && response["error"]["messageKey"] == "native.unsupportedLanguage", "Invalid language was accepted or lost its translation key");
                require(h.ok("getState")["settings"]["language"] == "ja", "Invalid language changed current settings");
            }
            ++passed;
        }
        for (const auto& invalid : Json::array({"unsupported", 42, nullptr, Json::object(), Json::array()})) {
            auto folder = root / ("invalid-language-load-" + std::to_string(passed));
            write(folder / "state.json", Json{{"schemaVersion", 1}, {"settings", {{"language", invalid}, {"uiScale", 125}}}}.dump());
            Harness loaded(folder);
            auto state = loaded.ok("getState");
            require(state["settings"]["language"] == "en", "Invalid saved language did not fall back to English");
            require(state["settings"]["uiScale"] == 125 && state["session"]["warnings"].empty(), "Invalid saved language discarded unrelated settings");
            ++passed;
        }
        for (const auto& bytes : {std::string("not valid JSON {"), std::string("{\"schemaVersion\":99,\"history\":[{\"proof\":\"preserve\"}]}"), std::string("{\"schemaVersion\":2,\"jobs\":[{\"state\":null}]}" )}) {
            auto folder = root / ("recovery-" + std::to_string(passed)); write(folder / "state-v2.json", bytes);
            Harness h(folder); auto state = h.ok("getState"); require(!state["session"]["warnings"].empty(), "Corrupt state recovery warning missing");
            require(state["session"]["warningMessages"][0]["messageKey"] == "native.stateRecovered", "State recovery warning lost its translation key");
            if (bytes.find("99") != std::string::npos) require(!h.call("saveSettings", {{"settings", {{"uiScale", 125}}}}).value("ok", true), "Future schema unexpectedly allowed persistence");
            else h.ok("saveSettings", {{"settings", {{"uiScale", 125}}}});
            bool found = false; for (const auto& file : fs::directory_iterator(folder)) if (file.path().filename().string().starts_with("state.recovery-")) found |= read(file.path()) == bytes;
            require(found, "Original state bytes were not preserved");
            if (bytes.find("99") != std::string::npos) require(read(folder / "state-v2.json") == bytes && state["statePersistenceEnabled"] == false, "Future state schema was overwritten");
            else require(Json::parse(read(folder / "state-v2.json"))["schemaVersion"] == 2, "Recovered state was not writable"); ++passed;
        }
        {
            auto folder = root / "deduplicate";
            Json a = {{"attemptId", "a"}, {"jobId", "same-job"}, {"state", "completed"}}, b = {{"attemptId", "b"}, {"jobId", "same-job"}, {"state", "failed"}};
            write(folder / "state.json", Json{{"schemaVersion", 1}, {"jobs", {a, a}}, {"history", {a, b, b}}}.dump());
            Harness loaded(folder); loaded.ok("saveSettings", {{"settings", {{"uiScale", 100}}}});
            auto saved = Json::parse(read(folder / "state-v2.json")); require(saved["jobs"].size() == 1 && saved["history"].size() == 1, "Attempt snapshots were stored more than once");
            require(saved["history"][0]["attemptId"] == "b", "Distinct retry attempt was lost during deduplication"); ++passed;
        }
        {
            auto folder = root / "terminal-evidence";
            Json running = {{"attemptId", "final"}, {"state", "running"}}, complete = {{"attemptId", "final"}, {"state", "completed"}, {"results", {{{"status", "ok"}}}}};
            write(folder / "state.json", Json{{"schemaVersion", 1}, {"jobs", {running}}, {"history", {complete}}}.dump());
            Harness loaded(folder); auto state = loaded.ok("getState"); require(state["jobs"][0]["state"] == "completed" && state["jobs"][0]["results"].size() == 1, "Deduplication discarded terminal evidence"); ++passed;
        }
        {
            auto folder = root / "relocated-runtime";
            Json oldDefaults = {{"python", "X:/previous location/runtime/python/python.exe"}, {"blender", "X:/previous location/runtime/blender/blender.exe"}};
            write(folder / "state.json", Json{{"schemaVersion", 1}, {"settings", oldDefaults}, {"runtimeDefaults", oldDefaults}}.dump());
            Harness loaded(folder); auto state = loaded.ok("getState");
            require(state["settings"]["python"] == utf8(runtimePaths().python) && state["settings"]["blender"] == utf8(runtimePaths().blender), "Relocated bundle retained old runtime defaults"); ++passed;
        }
        {
            auto folder = root / "custom-runtime";
            write(folder / "state.json", Json{{"schemaVersion", 1}, {"settings", {{"python", "X:/custom/python.exe"}}}, {"runtimeDefaults", {{"python", "X:/previous location/runtime/python/python.exe"}}}}.dump());
            Harness loaded(folder); require(loaded.ok("getState")["settings"]["python"] == utf8(runtimePaths().python), "Unapproved legacy executable retained execution authority");
            require(read(folder / "state.json").find("X:/custom/python.exe") != std::string::npos, "Runtime migration lost original custom selection backup"); ++passed;
        }
        {
            auto folder = root / "parallel-reads", probe = folder / "probe";
            Harness h(folder);
            for (auto name : {"first", "second", "third", "cancelled", "after-cancel"}) h.approve(std::string(name) + ".anim", "source");
            h.ok("saveSettings", {{"settings", {{"python", utf8(fs::absolute(fromUtf8(argv[0])) )}, {"skeleton", "snapshot-model.fbx"}}}});
            auto request = [&](const std::string& name) { return Json{{"source", name + ".anim"}, {"parallelProbe", utf8(probe)}, {"probeId", name}}; };
            auto first = h.send("preview", request("first")), second = h.send("inspect", request("second"));
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (!(fs::exists(probe / "first.started") && fs::exists(probe / "second.started"))) {
                if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("Independent evaluators did not overlap");
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            auto state = h.ok("getState");
            require(state["readBusy"] == true && state["busy"] == false && state["readActive"] == 2 && state["readConcurrency"] == 2, "Read pool count/state is incorrect");
            auto third = h.call("preview", request("third"));
            require(!third.value("ok", true) && third["error"]["code"] == "READ_IN_PROGRESS", "Read pool exceeded its bound");
            auto changed = h.call("saveSettings", {{"settings", {{"skeleton", "changed.fbx"}}}});
            require(!changed.value("ok", true), "Settings changed during read evaluations");
            require(h.ok("getState")["settings"]["skeleton"] == utf8(folder / "snapshot-model.fbx"), "Rejected settings changed the snapshot");
            auto scan = h.call("scan", {{"paths", {"not-scanned.anim"}}});
            require(!scan.value("ok", true), "Source mutation overlapped read evaluations");
            auto validate = h.call("validateSkeleton", {{"path", "other.fbx"}});
            require(!validate.value("ok", true), "Validation changed shared state during read evaluations");
            h.ok("select", {{"ids", Json::array()}});
            write(probe / "release", "release both evaluators");
            auto a = h.response(first), b = h.response(second);
            require(a.value("ok", false) && b.value("ok", false), "Parallel evaluations failed");
            require(a["result"]["source"] == utf8(folder / "first.anim") && b["result"]["source"] == utf8(folder / "second.anim"), "Read requests mixed their sources");
            require(a["result"]["request"] != b["result"]["request"], "Parallel evaluators shared an IPC directory");
            require(a["result"]["skeleton"] == utf8(folder / "snapshot-model.fbx") && b["result"]["skeleton"] == utf8(folder / "snapshot-model.fbx"), "Read settings snapshots changed");
            state = h.idle(); require(state["readBusy"] == false && state["readActive"] == 0, "Read pool did not become idle"); ++passed;

            auto cancelledProbe = folder / "cancelled-probe";
            auto cancelPayload = request("cancelled"); cancelPayload["parallelProbe"] = utf8(cancelledProbe);
            first = h.send("preview", cancelPayload); cancelPayload["probeId"] = "also-cancelled";
            second = h.send("inspect", cancelPayload);
            const auto cancelDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (!(fs::exists(cancelledProbe / "cancelled.started") && fs::exists(cancelledProbe / "also-cancelled.started"))) {
                if (std::chrono::steady_clock::now() >= cancelDeadline) throw std::runtime_error("Cancellation evaluators did not start");
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            h.ok("cancel");
            a = h.response(first); b = h.response(second); h.idle();
            require(!a.value("ok", true) && a["error"]["code"] == "CANCELLED" && !b.value("ok", true) && b["error"]["code"] == "CANCELLED", "Cancellation did not stop both read lanes");
            auto again = h.call("preview", request("after-cancel")); h.idle();
            require(again.value("ok", false), "A cancelled batch poisoned subsequent evaluations"); ++passed;
        }
        {
            auto folder = root / "read-state-order", probe = folder / "probe";
            std::atomic_bool armed{false}, stalled{false}, releaseSend{false};
            Harness h(folder);
            h.ok("saveSettings", {{"settings", {{"python", utf8(fs::absolute(fromUtf8(argv[0])))}}}});
            h.beforeSend = [&](const Json& value) {
                if (armed.load() && value.value("event", "") == "state" && value["data"].value("readActive", 0u) == 1 && !stalled.exchange(true)) {
                    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                    while (!releaseSend.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            };
            auto first = h.send("preview", {{"parallelProbe", utf8(probe)}, {"probeId", "first"}});
            auto second = h.send("inspect", {{"parallelProbe", utf8(probe)}, {"probeId", "second"}});
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (!(fs::exists(probe / "first.started") && fs::exists(probe / "second.started"))) {
                if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("Ordered-state evaluators did not start");
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            armed.store(true); write(probe / "first.release", "first evaluator finishes");
            while (!stalled.load()) {
                if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("Read state publication did not reach the barrier");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            write(probe / "second.release", "second evaluator finishes while first publication is stalled");
            while (h.bridge->working()) {
                if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("Second evaluator did not complete behind publication barrier");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            releaseSend.store(true);
            require(h.response(first).value("ok", false) && h.response(second).value("ok", false), "Ordered-state evaluations failed");
            { std::lock_guard lock(h.mutex);
              auto latest = std::find_if(h.events.rbegin(), h.events.rend(), [](const Json& event) { return event.value("event", "") == "state"; });
              require(latest != h.events.rend() && (*latest)["data"]["readActive"] == 0 && (*latest)["data"]["readBusy"] == false, "A stale read state followed the final idle state"); }
            ++passed;
        }
        {
            auto folder = root / "read-destruction", probe = folder / "probe";
            Harness h(folder);
            h.ok("saveSettings", {{"settings", {{"python", utf8(fs::absolute(fromUtf8(argv[0])))}}}});
            auto first = h.send("preview", {{"parallelProbe", utf8(probe)}, {"probeId", "first"}});
            auto second = h.send("inspect", {{"parallelProbe", utf8(probe)}, {"probeId", "second"}});
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (!(fs::exists(probe / "first.started") && fs::exists(probe / "second.started"))) {
                if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("Destruction evaluators did not start");
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            const auto started = std::chrono::steady_clock::now(); h.bridge.reset();
            require(std::chrono::steady_clock::now() - started < std::chrono::seconds(5), "Destructor left a read evaluator running");
            require(h.response(first)["error"]["code"] == "CANCELLED" && h.response(second)["error"]["code"] == "CANCELLED", "Destructor failed to drain both read workers"); ++passed;
        }
        {
            // Removing a SOURCE LIBRARY entry forgets it everywhere but keeps the user's files.
            auto folder = root / "remove-source"; Harness h(folder);
            auto kept = folder / "kept/Kept.fbx", gone = folder / "gone/Gone.fbx";
            write(kept, "kept source bytes"); write(gone, "removed source bytes");
            h.ok("scan", {{"paths", {utf8(kept), utf8(gone)}}}); auto state = h.idle();
            require(state["clips"].size() == 2 && state["packages"].size() == 2, "Remove-source fixture missing");
            Json ids = Json::array(); for (const auto& clip : state["clips"]) ids.push_back(clip["id"]);
            h.ok("select", {{"ids", ids}});
            auto goneRow = *std::find_if(state["packages"].begin(), state["packages"].end(), [](const Json& row) { return row["name"] == "Gone.fbx"; });
            require(h.raw("removeSource", {{"sourcePath", goneRow["path"]}}).value("ok", false), "removeSource request failed");
            state = h.idle();
            require(state["clips"].size() == 1 && state["clips"][0]["name"] == "Kept", "Removed source clips stayed in the session");
            require(state["packages"].size() == 1 && state["packages"][0]["name"] == "Kept.fbx", "Removed source stayed in Recent");
            require(state["selectedIds"].size() == 1, "Removed clip stayed selected");
            require(fs::is_regular_file(gone) && fs::is_regular_file(kept), "Source removal deleted an original file");
            auto unknown = h.raw("removeSource", {{"sourcePath", goneRow["path"]}}); h.idle();
            require(!unknown.value("ok", true) && unknown["error"]["code"] == "OPERATION_FAILED", "Removing an unknown source succeeded");
            require(!h.raw("removeSource", Json::object()).value("ok", true), "Removing without a path succeeded");
            ++passed;
        }
        auto folder = root / "workflow"; Harness h(folder);
        auto fbx = folder / "cache/active/source.fbx", anim = folder / "source.anim";
        write(fbx, "preserved source bytes; this fixture makes no FBX structure claim");
        write(anim, "%YAML 1.1\n--- !u!74 &7400000\nAnimationClip:\n  m_Name: Test\n  m_SampleRate: 30\n  m_StopTime: 1\n  m_PositionCurves: []\n");
        h.ok("scan", {{"paths", {utf8(fbx), utf8(anim)}}}); auto state = h.idle(); require(state["session"]["clips"].size() == 2, "Scan fixture missing");
        auto animClip = *std::find_if(state["clips"].begin(), state["clips"].end(), [](const Json& c) { return c["kind"] == "anim"; });
        auto fbxClip = *std::find_if(state["clips"].begin(), state["clips"].end(), [](const Json& c) { return c["kind"] == "fbx"; });
        write(folder / "cache/jobs/unused/stale.bin", "unused cache"); write(folder / "cache/unclassified-user-file.bin", "preserve unknown files");
        h.ok("clearCache"); state = h.idle(); require(fs::exists(fbx), "Active source deleted by cache cleanup"); require(!fs::exists(folder / "cache/jobs/unused"), "Unused owned cache retained"); require(fs::exists(folder / "cache/unclassified-user-file.bin"), "Unknown cache content was deleted"); require(state["clips"].size() == 2, "Cache cleanup cleared live session"); ++passed;
        require(!h.call("saveSettings", {{"settings", {{"cacheRoot", utf8(root)}}}}).value("ok", true), "Read-only cache location accepted a change"); ++passed;
        Json bake = {{"mode", "bake"}, {"outputRoot", utf8(folder / "bake")}, {"collisionPolicy", "overwrite"}, {"explicitOverwrite", true}};
        h.ok("start", {{"options", bake}, {"clipIds", {animClip["id"]}}}); state = h.idle(); auto failedAttempt = state["jobs"].back()["attemptId"];
        require(state["retryAttemptId"] == failedAttempt, "Failed attempt was not exposed for retry");
        auto rejected = h.call("retryFailed", {{"attemptId", failedAttempt}}); require(!rejected.value("ok", true), "Retry inherited previous overwrite consent"); ++passed;
        h.ok("retryFailed", {{"attemptId", failedAttempt}, {"explicitOverwrite", true}}); state = h.idle(); require(state["jobs"].back()["attemptId"] != failedAttempt, "Retry reused attempt identity"); ++passed;
        Json extract = {{"mode", "extractOriginal"}, {"outputRoot", utf8(folder / "out")}, {"collisionPolicy", "rename"}, {"sampleRate", 0}};
        auto staleConsent = extract; staleConsent["explicitOverwrite"] = true;
        auto normalized = h.ok("plan", {{"options", staleConsent}, {"clipIds", {fbxClip["id"]}}}); h.idle();
        require(normalized["options"]["explicitOverwrite"] == false, "Non-overwrite policy retained stale consent"); ++passed;
        h.ok("start", {{"options", extract}, {"clipIds", {fbxClip["id"]}}}); state = h.idle(); require(state["jobs"].back()["state"] == "completed", "Original extraction failed");
        extract["collisionPolicy"] = "fail";
        h.ok("start", {{"options", extract}, {"clipIds", {fbxClip["id"]}}}); state = h.idle();
        require(state["jobs"].back()["state"] == "failed", "Whole attempt failure missing"); require(state["jobs"].back()["results"].empty(), "Unexpected results for preflight failure"); require(state["retryAttemptId"] == "", "Old results paired with a new failed plan"); ++passed;
        { std::lock_guard lock(h.mutex); bool idleSeen = false, resultSeen = false; for (const auto& event : h.events) {
            if (event.value("event", "") == "state") idleSeen = !event["data"].value("busy", true) && !event["data"].value("readBusy", true);
            if (event.value("event", "") == "result") { require(idleSeen, "Result event preceded idle state"); resultSeen = true; }
        } require(resultSeen, "Terminal result event missing"); } ++passed;
        h.ok("saveSettings", {{"settings", {{"python", utf8(folder / "missing-python.exe")}}}});
        fs::remove(folder / "missing-python.exe"); // approval becomes stale before launch
        Json mapping = {{"source-bone", "target-bone"}};
        auto validation = h.send("validateSkeleton", {{"path", utf8(folder / "model-b.fbx")}, {"clipId", animClip["id"]}, {"mapping", mapping}});
        auto failed = h.response(validation); require(!failed.value("ok", true), "Missing evaluator unexpectedly succeeded"); state = h.idle();
        require(state["skeleton"]["state"] == "error" && state["skeleton"]["valid"] == false, "Validation failure retained valid state"); require(state["skeleton"]["clipId"] == animClip["id"], "Validation clip identity missing");
        require(state["skeleton"]["path"] == utf8(folder / "model-b.fbx"), "Validation path identity missing"); ++passed;
        require(state["skeleton"]["validationMapping"] == mapping, "Validation mapping snapshot missing"); ++passed;
        require(!h.call("connectUnity", {{"editorPath", "missing.exe"}}).value("ok", true), "Unapproved Editor was executed"); state = h.idle(); require(state["unity"]["connected"] == false, "Unapproved Editor retained connection"); ++passed;
        {
            // Unity Hub auto-discovery with an injected root; the real Program Files is never read.
            const auto self = fs::absolute(fromUtf8(argv[0]));
            const auto autoRoot = root / "auto-unity";
            auto fakeEditor = [&](const fs::path& hub, const std::string& version) { auto exe = hub / version / "Editor" / "Unity.exe"; fs::create_directories(exe.parent_path()); fs::copy_file(self, exe, fs::copy_options::overwrite_existing); return exe; };
            struct Rig {
                std::mutex m; std::condition_variable cv; std::map<std::string, Json> replies; unsigned serial = 0; std::unique_ptr<DesktopBridge> bridge;
                Json call(const std::string& action, Json payload = Json::object()) {
                    auto id = std::to_string(++serial); bridge->handle({{"id", id}, {"action", action}, {"payload", payload}});
                    std::unique_lock lock(m); if (!cv.wait_for(lock, std::chrono::seconds(60), [&] { return replies.contains(id); })) throw std::runtime_error("Auto-connect reply timeout");
                    return replies.at(id);
                }
                Json settled() {
                    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(60);
                    for (;;) {
                        auto state = call("getState")["result"];
                        if (state["unityAuto"]["state"] != "searching" && !bridge->working()) return state;
                        if (std::chrono::steady_clock::now() > until) throw std::runtime_error("Unity auto-discovery did not finish");
                        std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    }
                }
            };
            auto open = [&](Rig& rig, const fs::path& hub, const fs::path& data, DesktopPlatformServices services = {}) {
                if (!services.unityHubEditorRoot) services.unityHubEditorRoot = [hub] { return hub; };
                rig.bridge = std::make_unique<DesktopBridge>([&rig](const Json& v) { std::lock_guard lock(rig.m); if (v.contains("id")) rig.replies[v["id"].get<std::string>()] = v; rig.cv.notify_all(); }, std::move(services), data);
            };
            {   // Discovery disabled when the shell injects no root.
                Rig rig; rig.bridge = std::make_unique<DesktopBridge>([&rig](const Json& v) { std::lock_guard lock(rig.m); if (v.contains("id")) rig.replies[v["id"].get<std::string>()] = v; rig.cv.notify_all(); }, DesktopPlatformServices{}, autoRoot / "data-disabled");
                auto state = rig.call("getState")["result"];
                require(state["unityAuto"]["state"] == "disabled" && state["settings"]["unityEditor"] == "", "Auto-discovery ran without an injected root"); ++passed;
            }
            {   // Nothing installed: stays unconnected.
                Rig rig; open(rig, autoRoot / "empty-hub", autoRoot / "data-empty"); auto state = rig.settled();
                require(state["settings"]["unityEditor"] == "" && state["unityAuto"]["state"] == "none", "Missing Hub root connected an editor"); ++passed;
            }
            {   // Only a non-baseline version: the existing version policy leaves it unconnected.
                auto otherHub = autoRoot / "other-version-hub"; fakeEditor(otherHub, "6000.4.6f1");
                Rig rig; open(rig, otherHub, autoRoot / "data-other"); auto state = rig.settled();
                require(state["settings"]["unityEditor"] == "" && state["unityAuto"]["state"] == "none", "A non-baseline Unity version was auto-connected"); ++passed;
            }
#ifdef _WIN32
            const auto hub = autoRoot / "hub"; const auto exe = fakeEditor(hub, "6000.4.7f1"); fakeEditor(hub, "6000.4.6f1");
            {   // Empty setting + Hub baseline editor: grant, settings and state are produced.
                Rig rig; open(rig, hub, autoRoot / "data-connect"); auto state = rig.settled();
                require(state["unityAuto"]["state"] == "connected" && sameFile(fromUtf8(state["settings"]["unityEditor"].get<std::string>()), exe), "Hub editor was not auto-connected");
                bool granted = false; for (const auto& grant : state["pathGrants"]) if (grant["purpose"] == "editor" && grant["source"] == "auto-discovered" && grant["engine"] == "unity" && grant["engineProbe"]["version"] == "6000.4.7f1") granted = true;
                require(granted, "Auto-connected editor has no Editor approval record");
                // The saved approval survives a restart (re-probed) and keeps the note.
                rig.bridge.reset(); Rig again; open(again, hub, autoRoot / "data-connect"); auto restored = again.settled();
                require(restored["unityAuto"]["state"] == "connected" && restored["settings"]["unityEditor"] == state["settings"]["unityEditor"], "Auto-connected editor was not restored"); ++passed;
                // Clearing it is a decision: a restart must not reconnect it.
                again.call("saveSettings", {{"settings", {{"unityEditor", ""}}}});
                again.bridge.reset(); Rig third; open(third, hub, autoRoot / "data-connect"); auto cleared = third.settled();
                require(cleared["settings"]["unityEditor"] == "" && cleared["unityAuto"]["state"] == "declined", "A cleared editor was auto-connected again"); ++passed;
            }
            {   // A user choice made while discovery is in flight is never overwritten.
                std::promise<void> release; auto gate = release.get_future().share();
                DesktopPlatformServices services; services.unityHubEditorRoot = [gate, hub] { gate.wait(); return hub; };
                const auto userEditor = fakeEditor(autoRoot / "user-hub", "6000.4.7f1");
                services.pick = [userEditor](const Json&, bool, DesktopPlatformServices::PickCompletion done) { done({userEditor}, {}); };
                Rig rig; open(rig, hub, autoRoot / "data-user", services);
                auto picked = rig.call("chooseFiles", {{"purpose", "unity-editor"}, {"engine", "unity"}});
                require(picked["ok"] == true, "Picker did not complete");
                while (rig.bridge->working()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
                auto grant = picked["result"]["grants"][0]["grant"].get<std::string>();
                require(rig.call("saveSettings", {{"settings", {{"unityEditor", utf8(userEditor)}, {"unityEditorGrant", grant}}}})["ok"] == true, "Could not save the user's editor");
                release.set_value(); auto state = rig.settled();
                require(sameFile(fromUtf8(state["settings"]["unityEditor"].get<std::string>()), userEditor) && state["unityAuto"]["state"] == "none", "A user-chosen editor was overwritten by auto-connect"); ++passed;
            }
            {   // A junction that escapes the Hub root is rejected.
                auto outside = autoRoot / "outside"; fakeEditor(outside, "6000.4.7f1");
                auto escHub = autoRoot / "escape-hub"; fs::create_directories(escHub);
                auto cmd = "cmd /c mklink /J \"" + (escHub / "6000.4.7f1").string() + "\" \"" + (outside / "6000.4.7f1").string() + "\" >nul";
                if (std::system(cmd.c_str()) == 0 && fs::exists(escHub / "6000.4.7f1" / "Editor" / "Unity.exe")) {
                    Rig rig; open(rig, escHub, autoRoot / "data-escape"); auto state = rig.settled();
                    require(state["settings"]["unityEditor"] == "" && state["unityAuto"]["state"] == "none", "A junction escaping the Hub root was connected"); ++passed;
                }
            }
#endif
        }
        {
            // Unreal Editor auto-discovery and the first-run language, both through injected services:
            // the real Epic launcher list, Program Files and installer files are never read.
            const auto self = fs::absolute(fromUtf8(argv[0]));
            const auto unrealRoot = root / "auto-unreal";
            struct Rig {
                std::mutex m; std::condition_variable cv; std::map<std::string, Json> replies; unsigned serial = 0; std::unique_ptr<DesktopBridge> bridge;
                Json call(const std::string& action, Json payload = Json::object()) {
                    auto id = std::to_string(++serial); bridge->handle({{"id", id}, {"action", action}, {"payload", payload}});
                    std::unique_lock lock(m); if (!cv.wait_for(lock, std::chrono::seconds(60), [&] { return replies.contains(id); })) throw std::runtime_error("Unreal auto-connect reply timeout");
                    return replies.at(id);
                }
                Json settled() {
                    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(60);
                    for (;;) {
                        auto state = call("getState")["result"];
                        if (state["unrealAuto"]["state"] != "searching" && !bridge->working()) return state;
                        if (std::chrono::steady_clock::now() > until) throw std::runtime_error("Unreal auto-discovery did not finish");
                        std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    }
                }
            };
            auto open = [&](Rig& rig, const fs::path& data, DesktopPlatformServices services) {
                rig.bridge = std::make_unique<DesktopBridge>([&rig](const Json& v) { std::lock_guard lock(rig.m); if (v.contains("id")) rig.replies[v["id"].get<std::string>()] = v; rig.cv.notify_all(); }, std::move(services), data);
            };
            // A minimal engine tree: version file, both editor executables and the two Manny templates.
            auto fakeEngine = [&](const fs::path& base, const std::string& folder, int minor, int patch) {
                const auto engineRoot = base / folder;
                write(engineRoot / "Engine/Build/Build.version", Json{{"MajorVersion", 5}, {"MinorVersion", minor}, {"PatchVersion", patch}}.dump());
                for (const auto* name : {"UnrealEditor.exe", "UnrealEditor-Cmd.exe"}) {
                    fs::create_directories(engineRoot / "Engine/Binaries/Win64");
                    fs::copy_file(self, engineRoot / "Engine/Binaries/Win64" / name, fs::copy_options::overwrite_existing);
                }
                for (const auto* name : {"SKM_Manny_Simple.uasset", "SK_Mannequin.uasset"}) write(engineRoot / "Templates/TemplateResources/High/Characters/Content/Mannequins/Meshes" / name, name);
                return engineRoot;
            };
            auto servicesFor = [](std::vector<discovery::UnrealInstall> installs) {
                DesktopPlatformServices services;
                services.unrealInstalls = [installs] { return installs; };
                return services;
            };
            {   // Discovery is off when the shell injects no list.
                Rig rig; open(rig, unrealRoot / "data-disabled", {});
                auto state = rig.call("getState")["result"];
                require(state["unrealAuto"]["state"] == "disabled" && state["settings"]["unreal_editor"] == "", "Unreal auto-discovery ran without an injected list"); ++passed;
            }
            {   // Language: nothing saved -> the injected default; unsupported codes -> English.
                DesktopPlatformServices korean; korean.defaultLanguage = [] { return std::string("ko"); };
                Rig rig; open(rig, unrealRoot / "data-language", korean);
                require(rig.call("getState")["result"]["settings"]["language"] == "ko", "First run ignored the installer language");
                // Once state is saved, a different installer language must not replace it.
                require(rig.call("saveSettings", {{"settings", {{"theme", "dark"}}}})["ok"] == true, "Could not save settings");
                rig.bridge.reset();
                DesktopPlatformServices japanese; japanese.defaultLanguage = [] { return std::string("ja"); };
                Rig again; open(again, unrealRoot / "data-language", japanese);
                require(again.call("getState")["result"]["settings"]["language"] == "ko", "A saved language was replaced by the installer language");
                DesktopPlatformServices invalid; invalid.defaultLanguage = [] { return std::string("xx"); };
                Rig fallback; open(fallback, unrealRoot / "data-language-invalid", invalid);
                require(fallback.call("getState")["result"]["settings"]["language"] == "en", "An unsupported default language was accepted"); ++passed;
            }
#ifdef _WIN32
            {   // Newest supported install wins: 5.9 and 5.7 report versions the integration rejects.
                const auto base = unrealRoot / "installs";
                const auto v59 = fakeEngine(base, "UE_5.9", 9, 0), v58 = fakeEngine(base, "UE_5.8", 8, 2), v57 = fakeEngine(base, "UE_5.7", 7, 0);
                // Only the folder name is listed; the engine version itself comes from Build.version.
                auto services = servicesFor({{5, 7, utf8(v57)}, {5, 8, utf8(v58)}, {5, 9, utf8(v59)}, {5, 8, utf8(v58)}});
                Rig rig; open(rig, unrealRoot / "data-connect", services); auto state = rig.settled();
                const auto expected = v58 / "Engine/Binaries/Win64/UnrealEditor.exe";
                require(state["unrealAuto"]["state"] == "connected" && sameFile(fromUtf8(state["settings"]["unreal_editor"].get<std::string>()), expected), "Newest supported Unreal Editor was not auto-connected");
                bool editor = false, command = false; int templates = 0;
                for (const auto& grant : state["pathGrants"]) {
                    if (grant["source"] != "auto-discovered" || grant["engine"] != "unreal") continue;
                    const auto path = fromUtf8(grant["path"].get<std::string>());
                    if (grant["purpose"] == "editor" && sameFile(path, expected)) editor = true;
                    if (grant["purpose"] == "editor" && sameFile(path, v58 / "Engine/Binaries/Win64/UnrealEditor-Cmd.exe")) command = true;
                    if (grant["purpose"] == "read") ++templates;
                }
                require(editor && command && templates == 2, "Auto-connected Unreal Editor lacks its Editor, Cmd and template approvals");
                // The saved approval survives a restart (re-probed) and keeps the note.
                rig.bridge.reset(); Rig again; open(again, unrealRoot / "data-connect", services); auto restored = again.settled();
                require(restored["unrealAuto"]["state"] == "connected" && restored["settings"]["unreal_editor"] == state["settings"]["unreal_editor"], "Auto-connected Unreal Editor was not restored");
                // Clearing it is a decision: a restart must not reconnect it.
                again.call("saveSettings", {{"settings", {{"unreal_editor", ""}}}});
                again.bridge.reset(); Rig third; open(third, unrealRoot / "data-connect", services); auto cleared = third.settled();
                require(cleared["settings"]["unreal_editor"] == "" && cleared["unrealAuto"]["state"] == "declined", "A cleared Unreal Editor was auto-connected again"); ++passed;
            }
            {   // A user choice made while discovery is in flight is never overwritten.
                const auto autoEngine = fakeEngine(unrealRoot / "auto-engines", "UE_5.8", 8, 2), userEngine = fakeEngine(unrealRoot / "user-engines", "UE_5.8", 8, 2);
                const auto userEditor = userEngine / "Engine/Binaries/Win64/UnrealEditor.exe";
                std::promise<void> release; auto gate = release.get_future().share();
                DesktopPlatformServices services;
                services.unrealInstalls = [gate, location = utf8(autoEngine)] { gate.wait(); return std::vector<discovery::UnrealInstall>{{5, 8, location}}; };
                services.pick = [userEditor](const Json&, bool, DesktopPlatformServices::PickCompletion done) { done({userEditor}, {}); };
                Rig rig; open(rig, unrealRoot / "data-user", services);
                auto picked = rig.call("chooseFiles", {{"purpose", "unreal-editor"}, {"engine", "unreal"}});
                require(picked["ok"] == true, "Unreal picker did not complete");
                while (rig.bridge->working()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
                auto grant = picked["result"]["grants"][0]["grant"].get<std::string>();
                require(rig.call("saveSettings", {{"settings", {{"unreal_editor", utf8(userEditor)}, {"unreal_editorGrant", grant}}}})["ok"] == true, "Could not save the user's Unreal Editor");
                release.set_value(); auto state = rig.settled();
                require(sameFile(fromUtf8(state["settings"]["unreal_editor"].get<std::string>()), userEditor) && state["unrealAuto"]["state"] == "none", "A user-chosen Unreal Editor was overwritten by auto-connect"); ++passed;
            }
            {   // A junction that sends Binaries outside the install folder is rejected.
                const auto outside = fakeEngine(unrealRoot / "outside", "UE_5.8", 8, 2);
                const auto escape = unrealRoot / "escape" / "UE_5.8";
                write(escape / "Engine/Build/Build.version", Json{{"MajorVersion", 5}, {"MinorVersion", 8}, {"PatchVersion", 2}}.dump());
                const auto link = "cmd /c mklink /J \"" + (escape / "Engine/Binaries").string() + "\" \"" + (outside / "Engine/Binaries").string() + "\" >nul";
                if (std::system(link.c_str()) == 0 && fs::exists(escape / "Engine/Binaries/Win64/UnrealEditor.exe")) {
                    Rig rig; open(rig, unrealRoot / "data-escape", servicesFor({{5, 8, utf8(escape)}})); auto state = rig.settled();
                    require(state["settings"]["unreal_editor"] == "" && state["unrealAuto"]["state"] == "none", "A junction escaping the Unreal install was connected"); ++passed;
                }
            }
            {   // An install missing its Cmd companion cannot run conversions and is skipped.
                const auto incomplete = fakeEngine(unrealRoot / "incomplete", "UE_5.8", 8, 2);
                fs::remove(incomplete / "Engine/Binaries/Win64/UnrealEditor-Cmd.exe");
                Rig rig; open(rig, unrealRoot / "data-incomplete", servicesFor({{5, 8, utf8(incomplete)}})); auto state = rig.settled();
                require(state["settings"]["unreal_editor"] == "" && state["unrealAuto"]["state"] == "none", "An install without UnrealEditor-Cmd was connected"); ++passed;
            }
#endif
        }
        std::cout << passed << " desktop bridge regression checks passed\n"; return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
