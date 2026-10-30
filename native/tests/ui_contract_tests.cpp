#include "animbridge/platform_services.hpp"
#include "animbridge/path_grants.hpp"
#include "animbridge/ui_resources.hpp"
#include "animbridge/desktop_bridge.hpp"
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
using namespace animbridge;
using Json = nlohmann::json;
namespace {
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void failure(const std::function<void()>& action, const char* message) { try { action(); } catch (...) { return; } throw std::runtime_error(message); }
void write(const fs::path& path, const std::string& text) { fs::create_directories(path.parent_path()); std::ofstream(path) << text; }
struct BridgeHarness {
    std::mutex mutex;
    std::condition_variable changed;
    std::map<std::string, Json> replies;
    std::vector<fs::path> choices;
    std::unique_ptr<DesktopBridge> bridge;
    unsigned serial = 0;
    explicit BridgeHarness(const fs::path& root) {
        DesktopPlatformServices services;
        services.pick = [this](const Json&, bool, DesktopPlatformServices::PickCompletion done) { done(choices, {}); };
        bridge = std::make_unique<DesktopBridge>([this](const Json& response) {
            if (!response.contains("id")) return;
            { std::lock_guard lock(mutex); replies[response.at("id").get<std::string>()] = response; }
            changed.notify_all();
        }, std::move(services), root);
    }
    Json call(const std::string& action, Json payload = Json::object()) {
        auto id = std::to_string(++serial); bridge->handle({{"id", id}, {"action", action}, {"payload", payload}});
        std::unique_lock lock(mutex);
        if (!changed.wait_for(lock, std::chrono::seconds(30), [&] { return replies.contains(id); })) throw std::runtime_error("Editor authority fixture timed out");
        auto result = replies.at(id); replies.erase(id); lock.unlock();
        auto until = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (bridge->working() && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        check(!bridge->working(), "Editor authority worker did not finish"); return result;
    }
    Json pick(const fs::path& path, const std::string& purpose, const std::string& engine = "unity") {
        choices = {path}; auto response = call("chooseFiles", {{"purpose", purpose}, {"engine", engine}});
        if (!response.value("ok", false)) throw std::runtime_error(response.dump());
        return response.at("result").at("grants").at(0);
    }
};
}
int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "-version") { std::cout << "6000.4.7f1\n"; return 0; }
    for (int module = 1; module + 1 < argc; ++module) if (std::string(argv[module]) == "-m" && std::string(argv[module + 1]) == "unityanimbridge.desktop") {
        fs::path input, output;
        for (int index = module + 2; index + 1 < argc; ++index) { if (std::string(argv[index]) == "--request") input = fromUtf8(argv[index + 1]); if (std::string(argv[index]) == "--out") output = fromUtf8(argv[index + 1]); }
        const auto request = Json::parse(std::ifstream(input));
        if (request.contains("unreal_object_path")) {
            // Stand-in for the Unreal export worker: a mesh fails like the real script, an animation yields a manifest.
            const auto objectPath = request["unreal_object_path"].get<std::string>();
            if (objectPath.find("Mesh") != std::string::npos) {
                write(output, Json{{"status", "blocked"}, {"reason", objectPath + " is a SkeletalMesh: expected AnimSequence"}}.dump()); return 0;
            }
            const auto folder = fromUtf8(request.at("cache").get<std::string>()) / "unreal-export/fixture";
            write(folder / "Run.fbx", "fbx fixture");
            const Json clip = {{"id", objectPath}, {"name", "Run"}, {"fbx", "Run.fbx"}, {"source_asset", objectPath}, {"fps", 30.0}, {"duration", 1.0}};
            write(folder / "animbridge-manifest.json", Json{{"animbridge_version", 1}, {"source_engine", "unreal"}, {"clips", Json::array({clip})}}.dump());
            write(output, Json{{"status", "ok"}, {"manifest", utf8(folder / "animbridge-manifest.json")}, {"outputs", Json::array({utf8(folder / "Run.fbx")})},
                {"project", request.value("unreal_project", "")}, {"unrealCapability", request.value("_native_unreal_capability", Json())}}.dump());
            return 0;
        }
        write(output, Json{{"status", "ready"}, {"nativePolicy", request.value("_native_editor_policy", "")},
            {"unityEditor", request.value("unity_editor", "")}, {"unrealEditor", request.value("unreal_editor", "")},
            {"unrealCapability", request.value("_native_unreal_capability", Json())}}.dump()); return request.value("exitAfterResponse", false) ? 17 : 0;
    }
    try {
        auto root = fs::absolute(argc > 1 ? fromUtf8(argv[1]) : fs::path("ui-contract-work")) / newIdentity(); fs::create_directories(root);
        WebViewSession session; check(!session.ready(), "WebView accepted an uncommitted document");
        auto bootstrap = session.commit("animbridge://app/", "fixture", false);
        auto request = Json{{"protocolVersion", 1}, {"epoch", bootstrap["epoch"]}, {"capability", bootstrap["capability"]}, {"id", "first"}, {"action", "getState"}, {"payload", Json::object()}};
        check(!session.accept(request, "https://attacker/", true, true), "Wrong engine origin accepted");
        check(!session.accept(request, "animbridge://app/", false, true), "Subframe request accepted");
        check(!session.accept(request, "animbridge://app/", true, false), "Frame-capable document accepted");
        auto forged = request; forged["capability"] = "forged";
        check(!session.accept(forged, "animbridge://app/", true, true), "Forged capability accepted");
        check(session.accept(request, "animbridge://app/", true, true), "Valid native provenance was rejected");
        check(!session.accept(request, "animbridge://app/", true, true), "Duplicate request ID replay accepted");
        auto envelope = session.envelope(Json{{"ok", true}}); check(envelope["epoch"] == bootstrap["epoch"], "Reply epoch missing");
        auto oldEpoch = request; oldEpoch["id"] = "old-epoch"; session.invalidate();
        check(!session.ready() && !session.accept(oldEpoch, "animbridge://app/", true, true), "Navigation retained prior document authorization");
        auto next = session.commit("animbridge://app/", "fixture", true);
        check(next["epoch"] != bootstrap["epoch"] && !session.accept(oldEpoch, "animbridge://app/", true, true), "Old epoch survived recommit");
        for (auto action : {"exec", "openUrl", "readFile", "arbitrary"}) {
            auto invalid = request; invalid["epoch"] = next["epoch"]; invalid["capability"] = next["capability"]; invalid["id"] = action; invalid["action"] = action;
            check(!session.accept(invalid, "animbridge://app/", true, true), "Unregistered bridge action accepted");
        }
        auto ui = root / "ui"; write(ui / "index.html", "trusted"); write(ui / "app.js", "trusted-js"); write(ui / "secret.txt", "private");
        check(readUiResource(ui, "animbridge://app/index.html", "animbridge://app/").has_value(), "Registered UI resource missing");
        for (auto path : {"secret.txt", "../secret.txt", "%2e%2e/secret.txt", "index.html?x=1", "index.html#fragment", "%00index.html", "file:///etc/passwd", "app.js/../index.html", "%ZZ"})
            check(!readUiResource(ui, std::string("animbridge://app/") + path, "animbridge://app/"), "UI resource alias/traversal escaped allowlist");
        check(!readUiResource(ui, "https://attacker/index.html", "animbridge://app/"), "External resource origin accepted");
        PathGrantRegistry grants; auto source = root / "source.anim"; write(source, "source-v1");
        auto read = grants.issue(source, GrantPurpose::Read, GrantSource::Picker);
        auto token = read.at("grant").get<std::string>();
        check(grants.resolve(token, GrantPurpose::Read) == fs::canonical(source), "Read grant did not resolve");
        failure([&] { grants.resolve(token, GrantPurpose::Editor); }, "Read grant escalated into executable authority");
        failure([&] { grants.resolve("forged", GrantPurpose::Read); }, "Forged native grant accepted");
        auto records = grants.records(); check(!records[0].contains("grant"), "Session token persisted in approval records");
        PathGrantRegistry restored; check(restored.restore(records).empty(), "Unchanged approval became stale");
        auto fresh = restored.publicRecords(); check(fresh[0]["grant"] != token, "Restart reused an opaque token");
        write(source, "source-v2"); failure([&] { grants.resolve(token, GrantPurpose::Read); }, "Modified input retained old approval");
        PathGrantRegistry stale; check(stale.restore(records).size() == 1, "Modified approval was restored without revalidation");
        auto output = root / "output"; fs::create_directory(output);
        auto out = grants.issue(output, GrantPurpose::ExportRoot, GrantSource::Picker); auto outToken = out.at("grant").get<std::string>();
        check(grants.resolve(outToken, GrantPurpose::ExportRoot, output / "new-result.fbx") == output / "new-result.fbx", "Export root did not permit a new child");
        failure([&] { grants.resolve(outToken, GrantPurpose::ExportRoot, root / "escaped.fbx"); }, "Export grant escaped its boundary");
        auto upper = root / "CaseBoundary", lower = root / "caseboundary";
        fs::create_directories(upper); fs::create_directories(lower);
        if (!fs::equivalent(upper, lower)) {
            auto scoped = grants.issue(upper, GrantPurpose::ExportRoot, GrantSource::Picker);
            failure([&] { grants.resolve(scoped.at("grant").get<std::string>(), GrantPurpose::ExportRoot, lower / "outside.fbx"); }, "Case-sensitive sibling escaped a native grant");
        }
        auto project = root / "project"; fs::create_directories(project / "Assets"); fs::create_directories(project / "ProjectSettings");
        auto target = grants.issue(project, GrantPurpose::EngineProject, GrantSource::Picker, "unity", "Assets/AnimBridge");
        auto projectToken = target.at("grant").get<std::string>(); grants.approveTarget(projectToken, "unity", "Assets/AnimBridge");
        failure([&] { grants.approveTarget(projectToken, "unreal", "/Game/AnimBridge"); }, "Project grant switched engines");
        failure([&] { grants.approveTarget(projectToken, "unity", "Assets/Other"); }, "Project grant widened its target folder");
#ifndef _WIN32
        fs::create_directory_symlink(root, output / "new-link");
        failure([&] { grants.resolve(outToken, GrantPurpose::ExportRoot, output / "new-link/source.anim"); }, "Replaced symlink bypassed export approval");
#endif
        {
            BridgeHarness bridge(root / "editor-authority");
            check(!bridge.call("connectUnity").value("ok", true), "Missing Unity grant reached automatic Editor discovery");
            check(!bridge.call("preview", {{"unreal_editor", utf8(root / "unapproved/Engine/Binaries/UnrealEditor")}}).value("ok", true), "Raw Unreal executable bypassed native authority");
            for (auto field : {"_native_editor_policy", "_native_unreal_capability", "_native_manny_capability"}) for (auto wrapper : {"", "options", "settings"}) {
                Json forgedField = {{field, "forged-host-authority"}};
                auto payload = std::string(wrapper).empty() ? forgedField : Json{{wrapper, forgedField}};
                check(!bridge.call("preview", payload).value("ok", true), "UI forged a host-only engine capability through root/options/settings");
            }
            if (developmentMode()) {
                auto self = fs::canonical(fromUtf8(argv[0]));
                auto python = bridge.pick(self, "setting-python", "runtime");
                check(!bridge.call("preview", {{"unreal_editor", utf8(self)}, {"unreal_editorGrant", python["grant"]}}).value("ok", true), "A generic runtime grant was accepted as a probed Unreal Editor");
                check(bridge.call("saveSettings", {{"settings", {{"python", utf8(self)}, {"pythonGrant", python["grant"]}}}}).value("ok", false), "Evaluator fixture was not selected through the native picker");
                auto model = root / "approved-preview.fbx"; write(model, "source fixture"); auto modelGrant = bridge.pick(model, "source");
                auto generic = bridge.call("preview", {{"sourceGrant", modelGrant["grant"]}});
                check(generic.value("ok", false) && generic["result"]["nativePolicy"] == "explicit-only" && generic["result"]["unityEditor"] == "" && generic["result"]["unrealCapability"].is_null(), "An unapproved engine capability survived a generic preview request");
                check(!bridge.call("preview", {{"sourceGrant", modelGrant["grant"]}, {"exitAfterResponse", true}}).value("ok", true), "A failed evaluator process returned a successful ready result");
                auto engine = root / "selected-unreal";
#ifdef _WIN32
                auto binaries = engine / "Engine/Binaries/Win64"; auto editor = binaries / "UnrealEditor.exe", command = binaries / "UnrealEditor-Cmd.exe";
#elif defined(__APPLE__)
                auto binaries = engine / "Engine/Binaries/Mac"; auto editor = binaries / "UnrealEditor", command = binaries / "UnrealEditor-Cmd";
#else
                auto binaries = engine / "Engine/Binaries/Linux"; auto editor = binaries / "UnrealEditor", command = binaries / "UnrealEditor-Cmd";
#endif
                fs::create_directories(binaries); fs::copy_file(self, editor); fs::copy_file(self, command);
#ifndef _WIN32
                fs::permissions(editor, fs::perms::owner_exec, fs::perm_options::add); fs::permissions(command, fs::perms::owner_exec, fs::perm_options::add);
#endif
                auto version = engine / "Engine/Build/Build.version"; write(version, R"({"MajorVersion":5,"MinorVersion":8,"PatchVersion":2})");
                auto templates = engine / "Templates/TemplateResources/High/Characters/Content/Mannequins/Meshes";
                write(templates / "SKM_Manny_Simple.uasset", "fixed mesh"); write(templates / "SK_Mannequin.uasset", "fixed skeleton"); write(templates / "unrelated.uasset", "no authority");
                // Raw .uasset conversion: the UI can repair each refusal through a native picker.
                auto unrealGame = root / "UnrealGame"; write(unrealGame / "UnrealGame.uproject", "{}");
                write(unrealGame / "Content/Anims/Run.uasset", "opaque"); write(unrealGame / "Content/Meshes/Mesh.uasset", "opaque");
                write(root / "loose/Jump.uasset", "opaque"); write(root / "OtherGame/OtherGame.uproject", "{}");
                std::map<std::string, std::string> assetClips;
                for (const auto& file : {unrealGame / "Content/Anims/Run.uasset", unrealGame / "Content/Meshes/Mesh.uasset", root / "loose/Jump.uasset"}) {
                    auto grant = bridge.pick(file, "source");
                    auto scanned = bridge.call("scan", {{"grants", Json::array({grant["grant"]})}});
                    check(scanned.value("ok", false), "A .uasset source could not be scanned");
                }
                // Keep the state alive: iterating a member of a temporary would dangle.
                const auto scannedState = bridge.call("getState")["result"];
                for (const auto& clip : scannedState["clips"]) assetClips[clip["name"].get<std::string>()] = clip["id"].get<std::string>();
                auto convert = [&](const std::string& name, Json payload = Json::object()) { payload["clipId"] = assetClips.at(name); return bridge.call("convertUnrealAsset", payload); };
                auto errorCode = [](const Json& response) { return response.value("ok", true) ? std::string() : response["error"].value("code", ""); };
                check(errorCode(convert("Jump")) == "UNREAL_PROJECT_MISSING", "A .uasset outside any project reached Unreal");
                check(errorCode(convert("Run")) == "UNREAL_PROJECT_APPROVAL_REQUIRED", "A file read grant authorized loading its whole Unreal project");
                auto otherGame = bridge.pick(root / "OtherGame/OtherGame.uproject", "unreal-source-project", "unreal");
                check(errorCode(convert("Run", {{"unrealProjectGrant", otherGame["grant"]}})) == "UNREAL_PROJECT_MISMATCH", "A different project approval was accepted for this asset");
                check(!convert("Run", {{"unreal_project", utf8(unrealGame / "UnrealGame.uproject")}}).value("ok", true), "The UI supplied the worker's project path directly");
                auto gameGrant = bridge.pick(unrealGame / "UnrealGame.uproject", "unreal-source-project", "unreal");
                check(errorCode(convert("Run", {{"unrealProjectGrant", gameGrant["grant"]}})) == "UNREAL_EDITOR_REQUIRED", "Conversion started without a chosen Unreal Editor");
                auto unreal = bridge.pick(editor, "unreal-editor", "unreal");
                check(bridge.call("saveSettings", {{"settings", {{"unreal_editor", utf8(editor)}, {"unreal_editorGrant", unreal["grant"]}}}}).value("ok", false), "Approved Manny Editor could not be saved");
                auto approved = bridge.call("preview", {{"sourceGrant", modelGrant["grant"]}, {"humanoid", true}, {"unreal_editorGrant", unreal["grant"]}});
                check(approved.value("ok", false), "Approved fixed Unreal companions did not reach the evaluator");
                const auto cap = approved["result"]["unrealCapability"];
                check(fs::equivalent(fromUtf8(cap["editor"]["path"].get<std::string>()), command) && cap["templates"].size() == 2 && cap["version"] == "5.8.2", "Unreal capability widened or selected a different executable");
                auto state = bridge.call("getState")["result"]; bool unrelatedApproved = false, broadRootApproved = false;
                for (const auto& row : state["pathGrants"]) {
                    auto recorded = fromUtf8(row["path"].get<std::string>()); std::error_code error;
                    unrelatedApproved |= fs::equivalent(recorded, templates / "unrelated.uasset", error) && !error;
                    error.clear(); broadRootApproved |= row["purpose"] == "read" && fs::equivalent(recorded, engine, error) && !error;
                }
                check(!unrelatedApproved && !broadRootApproved, "Editor approval widened into an arbitrary engine subtree read");
                auto saved = bridge.call("preview", {{"sourceGrant", modelGrant["grant"]}, {"humanoid", true}});
                check(saved.value("ok", false) && fs::equivalent(fromUtf8(saved["result"]["unrealEditor"].get<std::string>()), command), "Saved approved Manny Editor was not bound to its fixed command companion");
                auto meshFailure = convert("Mesh");
                check(!meshFailure.value("ok", true) && meshFailure["error"].value("messageKey", "") == "native.unrealConvertFailed" &&
                    meshFailure["error"]["messageParams"].value("reason", "").find("expected AnimSequence") != std::string::npos, "A non-animation asset did not report a clear conversion failure");
                auto converted = convert("Run");
                check(converted.value("ok", false) && converted["result"]["objectPath"] == "/Game/Anims/Run.Run" && converted["result"]["clipIds"].size() == 1, "The converted FBX was not ingested");
                bool ingested = false;
                const auto convertedState = bridge.call("getState")["result"];
                for (const auto& clip : convertedState["clips"]) if (clip["id"] == converted["result"]["clipIds"][0])
                    ingested = clip["kind"] == "fbx" && clip["route"] == "unreal-export" && clip["virtual_path"] == "/Game/Anims/Run.Run" && clip["selectable"] == true;
                check(ingested, "The converted FBX did not become a normal Unreal export clip");
                // The fixture has no reveal service; the request must still pass the path authorization.
                auto reveal = bridge.call("openPath", {{"path", converted["result"]["outputs"][0]}});
                check(reveal.value("ok", false) || reveal["error"].value("messageKey", "") != "native.openPathNotAllowed", "The converted FBX cannot be revealed");
                { std::ofstream modified(command, std::ios::binary | std::ios::app); modified << "changed companion"; }
                check(!bridge.call("preview", {{"sourceGrant", modelGrant["grant"]}, {"humanoid", true}}).value("ok", true), "Changed Unreal command companion was silently reapproved");
                fs::copy_file(self, command, fs::copy_options::overwrite_existing);
                write(templates / "SKM_Manny_Simple.uasset", "changed mesh");
                check(!bridge.call("preview", {{"sourceGrant", modelGrant["grant"]}, {"humanoid", true}}).value("ok", true), "Changed Unreal template was silently reapproved");
                write(templates / "SKM_Manny_Simple.uasset", "fixed mesh");
                write(version, R"({"MajorVersion":5,"MinorVersion":8,"PatchVersion":3})");
                check(!bridge.call("preview", {{"sourceGrant", modelGrant["grant"]}, {"unreal_editorGrant", unreal["grant"]}}).value("ok", true), "Changed Unreal version metadata retained Editor authority");
            }
        }
        std::cout << "PASS UI contract: origin/frame/epoch/capability/replay, resource allowlist, grant purpose/identity/restore/containment/engine target\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
