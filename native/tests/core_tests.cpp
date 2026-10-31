#include "animbridge/core.hpp"
#include "animbridge/fbx_rig.hpp"
#include "animbridge/file_commit.hpp"
#include "animbridge/path_identity.hpp"
#include <nlohmann/json.hpp>
#include <zlib.h>
#include <array>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <functional>
#include <cstdio>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif

using namespace animbridge;
using json = nlohmann::json;
namespace {
int passed = 0;
void check(bool condition, const std::string& message) { if (!condition) throw std::runtime_error(message); }
void expectFailure(const std::function<void()>& action, const std::string& message) {
    try { action(); } catch (const std::exception&) { return; }
    throw std::runtime_error("Expected failure: " + message);
}
void write(const fs::path& path, const std::string& value) { fs::create_directories(path.parent_path()); std::ofstream f(path, std::ios::binary); f << value; }
struct TarEntry { std::string path, content; char type = '0'; };
void package(const fs::path& path, const std::vector<TarEntry>& entries) {
#ifdef _WIN32
    gzFile file = gzopen_w(path.c_str(), "wb");
#else
    gzFile file = gzopen(path.c_str(), "wb");
#endif
    if (!file) throw std::runtime_error("Cannot create fixture");
    for (auto& entry : entries) {
        std::array<char, 512> header{}; if (entry.path.size() >= 100) throw std::runtime_error("Fixture path too long");
        std::copy(entry.path.begin(), entry.path.end(), header.begin());
        std::snprintf(header.data() + 100, 8, "%07o", 0644);
        std::snprintf(header.data() + 124, 12, "%011llo", static_cast<unsigned long long>(entry.content.size()));
        std::fill(header.begin() + 148, header.begin() + 156, ' '); header[156] = entry.type;
        std::copy_n("ustar", 5, header.begin() + 257);
        unsigned sum = 0; for (unsigned char c : header) sum += c;
        std::snprintf(header.data() + 148, 7, "%06o", sum); header[155] = ' ';
        gzwrite(file, header.data(), 512); gzwrite(file, entry.content.data(), static_cast<unsigned>(entry.content.size()));
        std::array<char, 512> zero{}; auto padding = (512 - entry.content.size() % 512) % 512;
        if (padding) gzwrite(file, zero.data(), static_cast<unsigned>(padding));
    }
    std::array<char, 1024> end{}; gzwrite(file, end.data(), 1024); gzclose(file);
}

// Minimal binary FBX 7.3 writer for rig-detection tests: an Objects section
// whose children are (node name, class) pairs such as ("Model", "Null").
std::string fbxU32(std::uint32_t value) { return std::string(reinterpret_cast<const char*>(&value), 4); }
std::string fbxNode(const std::string& name, const std::string& properties, std::uint32_t count, const std::vector<std::string>& children, std::uint32_t offset) {
    auto header = static_cast<std::uint32_t>(13 + name.size());
    std::string body;
    for (const auto& child : children) body += child;
    if (!children.empty()) body += std::string(13, '\0');
    auto end = offset + header + static_cast<std::uint32_t>(properties.size() + body.size());
    return fbxU32(end) + fbxU32(count) + fbxU32(static_cast<std::uint32_t>(properties.size())) + char(name.size()) + name + properties + body;
}
std::string binaryFbx(const std::vector<std::pair<std::string, std::string>>& objects) {
    std::string file = std::string("Kaydara FBX Binary  ") + '\0' + "\x1a" + '\0' + fbxU32(7300);
    // Children start after the Objects header (13 bytes + 7-letter name, no properties).
    auto childOffset = static_cast<std::uint32_t>(file.size() + 13 + 7);
    std::vector<std::string> children; std::int64_t id = 1000;
    for (const auto& [node, type] : objects) {
        auto label = "Joint" + std::to_string(id) + std::string("\0\x01", 2) + node;
        std::string properties = "L" + std::string(reinterpret_cast<const char*>(&id), 8)
            + "S" + fbxU32(static_cast<std::uint32_t>(label.size())) + label
            + "S" + fbxU32(static_cast<std::uint32_t>(type.size())) + type;
        children.push_back(fbxNode(node, properties, 3, {}, childOffset));
        childOffset += static_cast<std::uint32_t>(children.back().size()); ++id;
    }
    file += fbxNode("Objects", "", 0, children, static_cast<std::uint32_t>(file.size()));
    return file + std::string(13, '\0');
}
void test(const std::string& name, const std::function<void()>& action) { action(); ++passed; std::cout << "PASS " << name << '\n'; }
}
int main(int count, char** args) {
#ifdef _WIN32
    if (count == 3 && std::string(args[1]) == "--hold-lock") {
        auto path = fromUtf8(args[2]); auto handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return 2;
        write(fromUtf8(utf8(path) + ".ready"), "ready"); Sleep(10000); CloseHandle(handle); return 0;
    }
    if (count == 3 && std::string(args[1]) == "--orphan-lock") {
        auto path = fromUtf8(args[2]); auto command = L"\"" + fs::absolute(fromUtf8(args[0])).wstring() + L"\" --hold-lock \"" + path.wstring() + L"\"";
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION child{};
        if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child)) return 2;
        CloseHandle(child.hThread); CloseHandle(child.hProcess);
        for (int i = 0; i < 200 && !fs::exists(fromUtf8(utf8(path) + ".ready")); ++i) Sleep(10);
        return fs::exists(fromUtf8(utf8(path) + ".ready")) ? 0 : 2;
    }
#endif
    if (count == 2 && std::string(args[1]) == "--sleep") { std::this_thread::sleep_for(std::chrono::seconds(5)); return 0; }
    if (count == 4 && std::string(args[1]) == "--echo") { std::cout << args[2] << '\n' << args[3]; return 0; }
    if (count == 3 && std::string(args[1]) == "--env") { auto value = std::getenv(args[2]); if (value) std::cout << value; return value ? 0 : 2; }
    if (count == 6 && std::string(args[1]) == "--helper-smoke") {
        try {
            Options options; options.skeleton = fromUtf8(args[3]); options.unityEditor = fromUtf8(args[4]); options.output = fromUtf8(args[5]); options.workRoot = options.output / "cache/jobs";
            Session session; session.ingest({fromUtf8(args[2])}, options.output / "analysis");
            for (auto& clip : session.clips) clip.selected = true;
            auto plan = makePlan(session, options); std::cout << planJson(plan) << '\n'; auto results = execute(plan); std::cout << resultsJson(results) << '\n';
            for (auto& result : results) if (result.status == "error" || result.status == "blocked") return 1; return 0;
        } catch (const std::exception& error) { std::cerr << error.what(); return 2; }
    }
    auto module = 1;
    for (int index = 1; index < count; ++index) if (std::string(args[index]) == "-m") { module = index; break; }
    if (count > module + 6 && std::string(args[module]) == "-m" && std::string(args[module + 1]) == "unityanimbridge.desktop") {
        fs::path request, response;
        for (int i = module + 3; i + 1 < count; ++i) { if (std::string(args[i]) == "--request") request = fromUtf8(args[i + 1]); if (std::string(args[i]) == "--out") response = fromUtf8(args[i + 1]); }
        auto payload = json::parse(std::ifstream(request));
        if (payload.value("clip", "") != "SelectedHuman" || !payload.value("humanoid", false)) return 2;
        auto cache = fromUtf8(payload.at("cache").get<std::string>());
        if (utf8(cache).find("helper-work-root") == std::string::npos) return 2;
        write(cache / "temporary.txt", "owned helper cache");
        auto output = fromUtf8(payload.at("output").get<std::string>());
        write(output / "SelectedHuman.fbx", "; FBX 7.4.0\nmock selected helper output");
        write(output / "manifest.json", json{{"uab_version", 1}, {"tool_version", "native-helper-mock"}, {"clips", {{{"clip", "SelectedHuman"}, {"fbx", utf8(output / "SelectedHuman.fbx")}, {"status", "ok"}}}}}.dump());
        write(response, "{\"status\":\"ok\"}"); return 0;
    }
    if (count > module + 4 && std::string(args[module]) == "-m" && std::string(args[module + 1]) == "unityanimbridge.cli") {
        fs::path output, mapping;
        for (int i = module + 2; i + 1 < count; ++i) { if (std::string(args[i]) == "-o") output = fromUtf8(args[i + 1]); if (std::string(args[i]) == "--bone-map") mapping = fromUtf8(args[i + 1]); }
        if (mapping.empty() || !fs::is_regular_file(mapping) || json::parse(std::ifstream(mapping)).value("Hips", "") != "HipBone") return 2;
        write(output / "Mapped.fbx", "; FBX 7.4.0\nmock adapter output"); write(output / "Mapped.uab.json", "{\"warnings\":[]}");
        write(output / "manifest.json", json{{"uab_version", 1}, {"tool_version", "native-mock"}, {"warnings", json::array()}, {"clips", json::array({{{"clip", "Mapped"}, {"fbx", utf8(output / "Mapped.fbx")}, {"sidecar", utf8(output / "Mapped.uab.json")}, {"status", "ok"}}})}}.dump());
        return 0;
    }
    if (count > 2 && std::string(args[1]) == "-batchmode") {
        fs::path manifest, report;
        for (int i = 2; i + 1 < count; ++i) {
            if (std::string(args[i]) == "-animBridgeManifest") manifest = fromUtf8(args[i + 1]);
            if (std::string(args[i]) == "-animBridgeReport") report = fromUtf8(args[i + 1]);
        }
        auto input = json::parse(std::ifstream(manifest)); json clips = json::array(); bool first = true;
        for (auto& clip : input["clips"]) { clips.push_back({{"id", clip["id"]}, {"status", first ? "ok" : "error"}, {"message", first ? "imported" : "fixture import failed"}}); first = false; }
        write(report, json{{"clips", clips}}.dump()); return clips.size() > 1 ? 1 : 0;
    }
    try {
        if (count != 2) throw std::runtime_error("Expected dedicated test work directory");
        fs::path root = fs::absolute(args[1]);
        // No recursive deletion: fixtures use a new child directory on every test run.
        fs::create_directories(root); root /= "run-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()); fs::create_directory(root);
        auto cache = root / "cache";
        const std::string guid(32, 'a');
        test("SHA-256 standard streaming vectors", [&] {
            check(textSha256("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "empty SHA-256");
            check(textSha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "abc SHA-256");
            auto large = root / "million-a.bin"; write(large, std::string(1000000, 'a'));
            check(fileSha256(large) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "streamed million-a SHA-256");
        });
        test("portable name version 2 uses NFC and full Unicode case folding", [&] {
            check(portableNameKey("Stra\xc3\x9f" "e") == portableNameKey("STRASSE"), "full multi-character case fold");
            check(portableNameKey("Caf\xc3\xa9") == portableNameKey("Cafe\xcc\x81"), "NFC/NFD equivalence");
            check(portableNameKey("\xce\xa3") == portableNameKey("\xcf\x82"), "Greek final sigma case fold");
            expectFailure([&] { portableNameKey(std::string("bad\xff", 4)); }, "invalid UTF-8");
            expectFailure([&] { fromUtf8(std::string("embedded\0NUL", 12)); }, "embedded NUL");
            expectFailure([&] { fromUtf8("\xed\xa0\x80"); }, "UTF-8 encoded surrogate");
        });
        test("atomic no-replace preserves existing files and empty directories", [&] {
            auto directory = root / "commit-no-replace"; fs::create_directory(directory);
            write(directory / "staged", "new"); write(directory / "target", "old");
            check(FileCommitter::noReplace(directory / "staged", directory / "target") == PublishResult::Exists, "file collision reported");
            check(fileSha256(directory / "target") == textSha256("old") && fileSha256(directory / "staged") == textSha256("new"), "file collision preserves both inputs");
            fs::create_directory(directory / "staged-dir"); fs::create_directory(directory / "target-dir");
            check(FileCommitter::noReplace(directory / "staged-dir", directory / "target-dir") == PublishResult::Exists, "empty directory collision reported");
            check(fs::is_directory(directory / "staged-dir") && fs::is_empty(directory / "target-dir"), "no implicit replacement of empty directories");
        });
        test("atomic publication races have exactly one winner", [&] {
            auto directory = root / "commit-race"; fs::create_directory(directory);
            for (bool directories : {false, true}) {
                auto first = directory / (directories ? "first-dir" : "first"), second = directory / (directories ? "second-dir" : "second"), target = directory / (directories ? "winner-dir" : "winner");
                if (directories) { fs::create_directory(first); fs::create_directory(second); write(first / "identity", "first"); write(second / "identity", "second"); }
                else { write(first, "first"); write(second, "second"); }
                std::atomic<int> ready = 0, won = 0, exists = 0; std::exception_ptr errors[2];
                auto compete = [&](const fs::path& staged, unsigned index) { try { ++ready; while (ready.load() != 2) std::this_thread::yield(); auto status = FileCommitter::noReplace(staged, target); if (status == PublishResult::Committed) ++won; else ++exists; } catch (...) { errors[index] = std::current_exception(); } };
                std::thread a(compete, first, 0), b(compete, second, 1); a.join(); b.join();
                for (auto& error : errors) if (error) std::rethrow_exception(error);
                check(won == 1 && exists == 1 && fs::exists(first) != fs::exists(second), "exactly one atomic winner and one retained staging");
            }
        });
        test("same-volume overwrite preserves a verified backup and durable journal", [&] {
            auto directory = root / "commit-overwrite"; fs::create_directory(directory);
            auto capabilities = FileCommitter::probe(directory);
            check(capabilities.fileNoReplace && capabilities.directoryNoReplace && capabilities.flush, "required local volume capabilities");
            write(directory / "staged", "new target"); write(directory / "target", "old target");
            auto oldIdentity = fileIdentity(directory / "target");
            FileCommitter::replaceFile(directory / "staged", directory / "target", directory / "backups/old", root / "commit-journals/overwrite.json");
            check(fileSha256(directory / "target") == textSha256("new target") && fileSha256(directory / "backups/old") == textSha256("old target"), "new bytes committed and old bytes backed up");
            check(fileIdentity(directory / "backups/old") == oldIdentity, "backup preserves original file identity");
            check(json::parse(std::ifstream(root / "commit-journals/overwrite.json"))["state"] == "committed", "journal durably records commit");
        });
        test("interrupted journal recovery lists evidence without mutating user files", [&] {
            auto directory = root / "recovery-output"; fs::create_directory(directory); write(directory / "target", "external bytes");
            auto journal = root / "recovery-journals/one.json";
            FileCommitter::recordJournal(journal, json{{"program", "AnimBridge"}, {"schemaVersion", 1}, {"operation", "replaceFile"}, {"state", "exchange-pending"}, {"target", utf8(directory / "target")}, {"staged", utf8(directory / ".animbridge-tmp-missing")}, {"backup", ""}}.dump());
            auto recovery = json::parse(FileCommitter::recoveryEntries(journal.parent_path(), directory));
            check(recovery.size() == 1 && recovery[0]["recoveryAction"] == "manual-review", "interrupted operation exposed for review");
            check(fileSha256(directory / "target") == textSha256("external bytes"), "recovery never overwrites external bytes");
            auto attempt = newIdentity(); auto staging = directory / (".animbridge-tmp-" + attempt + "-0"); fs::create_directory(staging);
            FileCommitter::writeAtomic(staging / ".animbridge-owner.json", json{{"program", "AnimBridge"}, {"attemptId", attempt}, {"outputRoot", utf8(directory)}}.dump());
            FileCommitter::recordJournal(journal.parent_path() / "committed.json", json{{"program", "AnimBridge"}, {"schemaVersion", 1}, {"operation", "publish"}, {"state", "committed"}, {"target", utf8(directory / "target")}}.dump());
            recovery = json::parse(FileCommitter::recoveryEntries(journal.parent_path(), directory));
            check(recovery.size() == 2 && std::any_of(recovery.begin(), recovery.end(), [](const auto& entry) { return entry.value("operation", "") == "retainedStaging"; }), "owned staging survives crash after committed journal and remains discoverable");
        });
        test("overwrite backup collision cannot exchange or overwrite any existing bytes", [&] {
            auto directory = root / "backup-failure"; write(directory / "staged", "new"); write(directory / "target", "old"); write(directory / "backup", "foreign backup");
            expectFailure([&] { FileCommitter::replaceFile(directory / "staged", directory / "target", directory / "backup", root / "commit-journals/backup-failure.json"); }, "existing backup");
            check(fileSha256(directory / "staged") == textSha256("new") && fileSha256(directory / "target") == textSha256("old") && fileSha256(directory / "backup") == textSha256("foreign backup"), "backup failure occurs before exchange and preserves all three files");
        });
        const std::string anim = "%YAML 1.1\n--- !u!74 &7400000\nAnimationClip:\n  m_Name: Walk\n  m_SampleRate: 30\n";
        auto valid = root / "valid.unitypackage";
        package(valid, {{"./", "", '5'}, {guid + "/pathname", "Assets/Hero/Walk.anim\n"}, {guid + "/asset", anim}, {guid + "/asset.meta", "fileFormatVersion: 2\n"}});
        test("unitypackage reconstructs asset and metadata", [&] {
            Session session; session.ingest({valid}, cache); check(session.clips.size() == 1, "one clip");
            check(session.clips[0].name == "Walk", "parsed animation name"); check(session.clips[0].virtualPath == "Assets/Hero/Walk.anim", "logical asset pathname");
            check(fs::is_regular_file(fromUtf8(utf8(session.clips[0].source) + ".meta")), "metadata reconstructed");
            Session second; second.ingest({valid}, cache); check(second.clips[0].id == session.clips[0].id, "stable ID across extraction directories");
        });
        test("commercial package paths preserve identity without excessive cache nesting", [&] {
            auto source = root / "commercial-path.unitypackage";
            const std::string virtualPath = "Assets/Animation_Set_Fixture/Animation/Humanoid/Inplace/move_goingDown_inplace.fbx";
            const std::string contents = "commercial hierarchy fixture";
            package(source, {{guid + "/pathname", virtualPath}, {guid + "/asset", contents}, {guid + "/asset.meta", "fileFormatVersion: 2\nModelImporter:\n  animationType: 3\n"}});
            Session session; session.ingest({source}, root / "standard-user-profile-cache");
            check(session.clips.size() == 1 && session.clips[0].virtualPath == virtualPath, "original asset pathname retained");
            auto asset = session.clips[0].source;
            check(fileSha256(asset) == textSha256(contents) && fs::is_regular_file(fromUtf8(utf8(asset) + ".meta")), "original bytes and adjacent metadata retained");
            auto identity = json::parse(std::ifstream(session.extractedRoots[0].parent_path() / ".animbridge-package.json"));
            check(identity["sourceHash"] == fileSha256(source) && identity["parserVersion"] == ParserVersion, "full package hash and parser identity retained");
            Session second; second.ingest({source}, root / "standard-user-profile-cache");
            check(second.clips[0].id == session.clips[0].id && second.clips[0].source == session.clips[0].source, "reopening reuses the extraction, so clip paths stay stable for the preview cache");
            check(second.extractedRoots[0] == session.extractedRoots[0], "unchanged package bytes are not extracted again");
        });
        test("changed or damaged package extractions are not reused", [&] {
            auto source = root / "reopen.unitypackage", reopenCache = root / "reopen-cache";
            auto packageCount = [&] { return std::distance(fs::directory_iterator(reopenCache / "packages"), fs::directory_iterator{}); };
            package(source, {{guid + "/pathname", "Assets/Hero/Walk.anim"}, {guid + "/asset", anim}});
            Session first; first.ingest({source}, reopenCache);
            package(source, {{guid + "/pathname", "Assets/Hero/Walk.anim"}, {guid + "/asset", anim + "  m_WrapMode: 2\n"}});
            Session changed; changed.ingest({source}, reopenCache);
            check(changed.extractedRoots[0] != first.extractedRoots[0] && packageCount() == 2, "new package bytes get a fresh extraction");
            fs::remove(changed.clips[0].source);
            Session repaired; repaired.ingest({source}, reopenCache);
            check(repaired.extractedRoots[0] != changed.extractedRoots[0] && fs::is_regular_file(repaired.clips[0].source), "an extraction missing an asset is extracted again");
        });
        test("removing a source drops only its clips and releases only its extraction", [&] {
            auto removeCache = root / "remove-cache", loose = root / "remove-loose";
            auto source = root / "remove.unitypackage";
            package(source, {{guid + "/pathname", "Assets/Hero/Walk.anim"}, {guid + "/asset", anim}});
            write(loose / "Idle.fbx", "loose model bytes");
            Session session; session.ingest({source, loose}, removeCache);
            check(session.clips.size() == 2 && session.packages.size() == 2 && session.extractedRoots.size() == 1, "two sources loaded");
            auto packageRoot = session.extractedRoots[0];
            auto removed = session.removeSource(source);
            check(removed.found && removed.clipIds.size() == 1 && removed.extractedRoots.size() == 1 && removed.extractedRoots[0] == packageRoot, "package clips and extraction reported");
            check(session.clips.size() == 1 && session.clips[0].name == "Idle" && session.packages.size() == 1 && session.extractedRoots.empty(), "only the package source was forgotten");
            check(fs::is_regular_file(source) && fs::is_regular_file(loose / "Idle.fbx") && fs::exists(packageRoot), "removal never deletes files itself");
            auto again = session.removeSource(source);
            check(!again.found && again.clipIds.empty(), "removing twice is a harmless no-op");
            auto folder = session.removeSource(loose);
            check(folder.found && folder.extractedRoots.empty() && session.clips.empty() && fs::is_regular_file(loose / "Idle.fbx"), "a folder source keeps its original files");
        });
        test("package change after extraction rolls back the owned extraction directory", [&] {
            auto source = root / "changing-package.unitypackage", changingCache = root / "changing-cache";
            std::vector<TarEntry> entries;
            for (unsigned i = 1; i <= 1024; ++i) { char key[33]{}; std::snprintf(key, sizeof(key), "%032x", i); entries.push_back({std::string(key) + "/pathname", "Assets/Model" + std::to_string(i) + ".fbx"}); entries.push_back({std::string(key) + "/asset", "fixture"}); }
            package(source, entries); std::atomic_bool stopped = false, changed = false;
            std::thread modifier([&] { while (!stopped) {
                std::error_code error;
                if (fs::is_directory(changingCache / "packages", error)) for (auto& folder : fs::directory_iterator(changingCache / "packages")) {
                    if (fs::is_regular_file(folder.path() / ".animbridge-package.json", error)) { std::ofstream file(source, std::ios::binary | std::ios::app); file << "changed after extraction"; file.close(); changed = true; return; }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } });
            std::string failure; Session session;
            try { session.ingest({source}, changingCache); } catch (const std::exception& error) { failure = error.what(); }
            stopped = true; modifier.join();
            check(changed && failure.find("Package bytes changed during extraction") != std::string::npos, "post-extraction hash guard was exercised");
            check(session.clips.empty() && fs::is_empty(changingCache / "packages"), "failed hash verification retained no extracted package");
        });
        test("archive path traversal rejected and extraction rolled back", [&] {
            auto p = root / "escape.unitypackage"; package(p, {{"../outside", "bad"}});
            auto before = std::distance(fs::directory_iterator(cache), fs::directory_iterator{});
            expectFailure([&] { extractUnityPackage(p, cache, {}); }, "tar traversal");
            check(std::distance(fs::directory_iterator(cache), fs::directory_iterator{}) == before, "partial extraction removed");
            check(!fs::exists(root / "outside"), "did not write outside cache");
        });
        test("Unity pathname traversal and Windows device rejected", [&] {
            auto p = root / "pathname.unitypackage";
            for (auto pathname : {"Assets/../../escape.anim", "C:/escape.anim", "Assets/CON.anim", "Assets/a.anim:stream", "Assets/trailing./A.anim"}) {
                package(p, {{guid + "/pathname", pathname}, {guid + "/asset", anim}});
                expectFailure([&] { extractUnityPackage(p, cache, {}); }, pathname);
            }
        });
        test("archive links rejected", [&] {
            auto p = root / "links.unitypackage";
            for (char type : {'1', '2', '6'}) { package(p, {{guid + "/asset", "", type}}); expectFailure([&] { extractUnityPackage(p, cache, {}); }, "special entry"); }
        });
        test("archive duplicate paths and absent pathname rejected", [&] {
            auto p = root / "duplicate.unitypackage";
            package(p, {{guid + "/asset", anim}, {guid + "/asset", anim}}); expectFailure([&] { extractUnityPackage(p, cache, {}); }, "duplicate");
            package(p, {{guid + "/asset", anim}}); expectFailure([&] { extractUnityPackage(p, cache, {}); }, "missing pathname");
        });
        test("package Unicode normalization and non-ASCII case collisions are rejected", [&] {
            auto p = root / "unicode-collision.unitypackage"; const std::string secondGuid(32, 'b');
            for (auto names : {std::pair<std::string, std::string>{"Assets/Caf\xc3\xa9.fbx", "Assets/Cafe\xcc\x81.fbx"}, {"Assets/Stra\xc3\x9f" "e.fbx", "Assets/STRASSE.fbx"}, {"Assets/\xce\xa3.fbx", "Assets/\xcf\x82.fbx"}, {"Assets/Caf\xc3\xa9/One.fbx", "Assets/Cafe\xcc\x81/Two.fbx"}, {"Assets/Hero/One.fbx", "Assets/hero/Two.fbx"}}) {
                package(p, {{guid + "/pathname", names.first}, {guid + "/asset", "first"}, {secondGuid + "/pathname", names.second}, {secondGuid + "/asset", "second"}});
                expectFailure([&] { extractUnityPackage(p, cache, {}); }, "portable Unicode collision");
            }
            package(p, {{guid + "/pathname", std::string("Assets/invalid\xff.fbx")}, {guid + "/asset", "first"}});
            expectFailure([&] { extractUnityPackage(p, cache, {}); }, "package pathname invalid UTF-8 rejects whole package");
        });
        test("archive size expansion and count limits", [&] {
            Limits limits; limits.maxFileBytes = 4; expectFailure([&] { extractUnityPackage(valid, cache, limits); }, "file size");
            limits = {}; limits.maxExpandedBytes = 700; expectFailure([&] { extractUnityPackage(valid, cache, limits); }, "expanded bytes");
            limits = {}; limits.maxEntries = 1; expectFailure([&] { extractUnityPackage(valid, cache, limits); }, "entry count");
        });
        test("corrupt/truncated packages rejected", [&] {
            auto p = root / "broken.unitypackage"; write(p, "not gzip or tar"); expectFailure([&] { extractUnityPackage(p, cache, {}); }, "corrupt");
        });
        test("packaged Unreal manifests cannot reference files outside extracted assets", [&] {
            auto outside = root / "external.fbx"; write(outside, "external FBX");
            auto manifest = json{{"animbridge_version", 1}, {"source_engine", "unreal"}, {"clips", json::array({{{"name", "External"}, {"fbx", utf8(outside)}}})}}.dump();
            auto p = root / "manifest-in-package.unitypackage"; const std::string manifestGuid(32, 'b');
            package(p, {{guid + "/pathname", "Assets/Walk.anim"}, {guid + "/asset", anim}, {manifestGuid + "/pathname", "Assets/animbridge-manifest.json"}, {manifestGuid + "/asset", manifest}});
            Session session; session.ingest({p}, cache); check(session.clips.size() == 1 && session.clips[0].engine == Engine::Unity, "untrusted embedded manifest ignored");
        });
        auto files = root / "inputs"; fs::create_directories(files);
        test("opened file identities merge hard-link aliases within one project", [&] {
            auto directory = root / "identity-alias"; write(directory / "First.fbx", "model bytes");
            fs::create_hard_link(directory / "First.fbx", directory / "Alias.fbx");
            Session session; session.ingest({directory / "First.fbx"}, cache);
            auto identity = session.clips[0].id, firstPath = utf8(session.clips[0].source);
            session.ingest({directory / "Alias.fbx"}, cache);
            check(session.clips.size() == 1 && session.clips[0].id == identity && utf8(session.clips[0].source) == firstPath, "first verified path and internal ID reused");
            check(session.clips[0].sourceIdentityVersion == 2 && session.clips[0].identityDigest.size() == 64, "full SHA-256 internal identity retained");
            auto first = sourceIdentityDigest(directory / "First.fbx", "a", "bc", "d"), second = sourceIdentityDigest(directory / "First.fbx", "ab", "c", "d");
            check(first != second, "length-delimited identity fields cannot alias by concatenation");
#ifdef _WIN32
            check(session.clips[0].exportIdentityVersion == 1 && session.clips[0].exportId == legacyClipId(directory / "First.fbx", session.clips[0].name), "unambiguous Windows legacy external ID preserved");
#else
            check(session.clips[0].exportIdentityVersion == 2 && session.clips[0].exportId.size() == 35 && session.clips[0].exportId.starts_with("v2_"), "new Unix external ID has fixed 35-character format");
#endif
        });
        test("identical file identities in different project namespaces stay separate", [&] {
            auto first = root / "project-one", second = root / "project-two";
            write(first / "Assets/Model.fbx", "shared model bytes"); fs::create_directory(first / "ProjectSettings");
            fs::create_directories(second / "Assets"); fs::create_directory(second / "ProjectSettings");
            fs::create_hard_link(first / "Assets/Model.fbx", second / "Assets/Model.fbx");
            Session session; session.ingest({first / "Assets/Model.fbx", second / "Assets/Model.fbx"}, cache);
            check(session.clips.size() == 2 && session.clips[0].id != session.clips[1].id && session.clips[0].projectNamespace != session.clips[1].projectNamespace, "cross-project hard links are not deduplicated");
        });
        {
            auto directory = root / "case-sensitive-identity"; write(directory / "Foo.fbx", "upper"); write(directory / "foo.fbx", "lower");
            if (!sameFile(directory / "Foo.fbx", directory / "foo.fbx")) test("case-sensitive local filenames retain distinct internal identities", [&] {
                Session session; session.ingest({directory}, cache);
                check(session.clips.size() == 2 && session.clips[0].id != session.clips[1].id, "case-sensitive originals stay distinct");
            });
            else std::cout << "SKIP case-sensitive local identity: this test volume merges case variants\n";
        }
#ifndef _WIN32
        test("invalid POSIX filenames skip only the invalid directory item", [&] {
            auto directory = root / "invalid-filename"; write(directory / "Good.fbx", "good"); write(directory / fs::path(std::string("Invalid\xff.fbx")), "invalid");
            Session session; session.ingest({directory}, cache);
            check(session.clips.size() == 1 && session.clips[0].name == "Good" && !session.warnings.empty(), "valid neighbour remains available");
            auto serialized = sessionJson(session); check(serialized.find("INVALID_ENCODING") != std::string::npos && serialized.find('\xff') == std::string::npos, "diagnostic JSON never contains invalid filename bytes");
        });
#endif
        test("original source hard-link aliases are blocked during overwrite planning", [&] {
            auto source = root / "protected-identity/Source.fbx"; write(source, "original model");
            auto output = root / "protected-identity-output"; fs::create_directory(output); fs::create_hard_link(source, output / "Source.fbx");
            Session session; session.ingest({source}, cache); session.selectAll(true, ExportMode::ExtractOriginal);
            Options options; options.mode = ExportMode::ExtractOriginal; options.output = output; options.collisionPolicy = CollisionPolicy::Overwrite; options.explicitOverwrite = true;
            auto plan = makePlan(session, options); check(plan.jobs[0].blockedCode == "OUTPUT_CONFLICT", "source identity alias cannot be overwritten");
            check(fileSha256(source) == textSha256("original model"), "planning preserved source bytes");
        });
        test("truncated export ID collisions are rejected using the full digest", [&] {
            auto source = root / "export-ledger/Model.fbx"; write(source, "model");
            Session session; session.ingest({source}, cache); session.selectAll(true);
            session.clips[0].identityDigest = std::string(64, 'a'); session.clips[0].exportIdentityVersion = 2; session.clips[0].exportId = exportIdForDigest(session.clips[0].identityDigest);
            session.clips[0].exportIdentityDigest = session.clips[0].identityDigest;
            auto collision = session.clips[0]; collision.id = "second-internal-id"; collision.name = "second"; collision.identityDigest[63] = 'b'; collision.exportIdentityDigest[63] = 'b'; session.clips.push_back(collision);
            Options options; options.output = root / "export-ledger-output";
            expectFailure([&] { makePlan(session, options); }, "same external 128-bit prefix with different full digests");
        });
        test("Unreal path preflight includes the external ID and physical project path", [&] {
            auto source = root / "unreal-length/Model.fbx"; write(source, "model");
            auto project = root / "unreal-length-project/Length.uproject"; write(project, "{}");
            Session session; session.ingest({source}, cache); session.selectAll(true);
            Options options; options.output = root / "unreal-length-output"; options.targetEditor = fs::absolute(args[0]); options.targetProject = project; options.targetSkeleton = "/Game/Character/Skeleton.Skeleton";
            options.targetFolder = "/Game/" + std::string(250, 'a');
            auto plan = makePlan(session, options);
            check(plan.jobs[0].blockedCode == "OUTPUT_CONFLICT" && plan.jobs[0].blockedReason.find("path") != std::string::npos, "overlong engine destination is blocked before import");
        });
        test("distinct engine object IDs sharing one FBX remain distinct internally", [&] {
            auto directory = root / "manifest-object-identities"; write(directory / "Shared.fbx", "shared exported data");
            write(directory / "animbridge-manifest.json", json{{"animbridge_version", 1}, {"source_engine", "unreal"}, {"clips", json::array({
                {{"id", "/Game/First.Asset"}, {"name", "Same"}, {"fbx", "Shared.fbx"}}, {{"id", "/Game/Second.Asset"}, {"name", "Same"}, {"fbx", "Shared.fbx"}}})}}.dump());
            Session session; session.ingest({directory}, cache);
            check(session.clips.size() == 2 && session.clips[0].id != session.clips[1].id, "engine object identity is part of the length-delimited internal digest");
        });
        test("reverse manifests preserve external v2 identity separately from local IDs", [&] {
            auto directory = root / "manifest-export-identity"; write(directory / "Exported.fbx", "exported data");
            auto exportDigest = std::string(64, 'd'), exportId = exportIdForDigest(exportDigest);
            write(directory / "animbridge-manifest.json", json{{"animbridge_version", 1}, {"source_engine", "unreal"}, {"clips", json::array({
                {{"id", "/Game/Original.Asset"}, {"name", "Exported"}, {"fbx", "Exported.fbx"}, {"exportIdentityVersion", 2}, {"exportId", exportId}, {"exportIdentityDigest", exportDigest}}})}}.dump());
            Session session; session.ingest({directory}, cache); session.selectAll(true);
            check(session.clips.size() == 1 && session.clips[0].exportId == exportId && session.clips[0].exportIdentityDigest == exportDigest && session.clips[0].identityDigest != exportDigest, "external ID survives local source re-identification");
            Options options; options.direction = Direction::UnrealToUnity; options.output = root / "manifest-export-identity-output";
            auto plan = makePlan(session, options); check(plan.jobs[0].blockedReason.empty() && plan.jobs[0].clip.exportIdentityVersion == 2, "preserved external digest passes planning on every host OS");
        });
        test("absolute Unreal manifest companions cannot leave the approved directory", [&] {
            auto directory = root / "bounded-manifest"; auto outside = root / "bounded-outside/External.fbx"; write(outside, "external bytes");
            write(directory / "animbridge-manifest.json", json{{"animbridge_version", 1}, {"source_engine", "unreal"}, {"clips", json::array({{{"name", "External"}, {"fbx", utf8(outside)}}})}}.dump());
            expectFailure([&] { Session session; session.ingest({directory}, cache); }, "absolute FBX leaves native-approved companion boundary");
        });
        {
            auto directory = root / "linked-metadata"; write(directory / "Model.fbx", "model"); write(root / "foreign-meta.txt", "ModelImporter:\n  animations:\n    clipAnimations:\n    - name: SecretName\n");
            std::error_code linkError; fs::create_symlink(root / "foreign-meta.txt", directory / "Model.fbx.meta", linkError);
            if (!linkError) test("linked metadata never grants a companion read outside its directory", [&] {
                Session session; session.ingest({directory / "Model.fbx"}, cache);
                check(session.clips.size() == 1 && session.clips[0].name == "Model" && !session.clips[0].selectable && session.clips[0].note.find("Linked") != std::string::npos, "linked metadata rejected before its private clip names are read");
            });
            else std::cout << "SKIP linked metadata fixture: symbolic link creation is unavailable on this host\n";
        }
        write(files / "Run.anim", anim); write(files / "Model.fbx", "fixture");
        write(files / "Model.fbx.meta", "ModelImporter:\n  animations:\n    clipAnimations:\n    - serializedVersion: 16\n      name: Run\n      takeName: Take 001\n      firstFrame: 0\n      lastFrame: 30\n    - serializedVersion: 16\n      name: Walk\n      takeName: Take 001\n      firstFrame: 31\n      lastFrame: 60\n  animationType: 2\n");
        write(files / "A.uasset", "opaque"); write(files / "Binary.anim", "binary");
        test("unreal object paths follow the Content mount layout", [&] {
            auto content = root / "Proj/Content";
            check(unrealObjectPath(content, "/Game", content / "A/B/Run.uasset") == "/Game/A/B/Run.Run", "nested /Game asset");
            check(unrealObjectPath(content, "/Game", content / "Idle.uasset") == "/Game/Idle.Idle", "asset at the Content root");
            check(unrealObjectPath(content, "/Tools", content / "Anims/Walk.uasset") == "/Tools/Anims/Walk.Walk", "plugin mount point");
            check(unrealObjectPath(content, "/Game", root / "Proj/Other/Run.uasset").empty(), "asset outside Content");
            check(unrealObjectPath(content, "/Game", content / "A/Run.fbx").empty(), "only .uasset maps to an object");
            check(unrealObjectPath(content, "/Game", content / "A/Bad;Name.uasset").empty(), "export list separator is rejected");
        });
        test("uasset discovery finds the owning project and plugin mounts", [&] {
            auto project = root / "UnrealProject"; write(project / "Game.uproject", "{}");
            auto anim = project / "Content/Characters/Anims/Death_Back.uasset"; write(anim, "opaque");
            auto found = locateUnrealAsset(anim);
            check(found && fs::equivalent(found->project, project / "Game.uproject") && found->objectPath == "/Game/Characters/Anims/Death_Back.Death_Back", "project Content asset");
            auto plugin = project / "Plugins/Marketplace/MoCap"; write(plugin / "MoCap.uplugin", "{}");
            auto pluginAnim = plugin / "Content/Runs/Sprint.uasset"; write(pluginAnim, "opaque");
            found = locateUnrealAsset(pluginAnim);
            check(found && fs::equivalent(found->project, project / "Game.uproject") && found->objectPath == "/MoCap/Runs/Sprint.Sprint", "project plugin asset");
            auto loose = root / "LooseAssets/Content/Jump.uasset"; write(loose, "opaque");
            check(!locateUnrealAsset(loose), "no .uproject means no project");
            auto engineOnly = root / "EngineCopy/Plugins/Fx/Content/Wave.uasset"; write(root / "EngineCopy/Plugins/Fx/Fx.uplugin", "{}"); write(engineOnly, "opaque");
            check(!locateUnrealAsset(engineOnly), "plugin outside any project");
            auto twoProjects = root / "Ambiguous"; write(twoProjects / "A.uproject", "{}"); write(twoProjects / "B.uproject", "{}"); write(twoProjects / "Content/X.uasset", "opaque");
            check(!locateUnrealAsset(twoProjects / "Content/X.uasset"), "several .uproject files are not guessed");
            Session session; session.ingest({anim, loose}, cache);
            for (const auto& clip : session.clips) {
                auto metadata = json::parse(clip.metadataJson);
                check(clip.kind == "uasset" && clip.engine == Engine::Unreal && !clip.selectable, "uasset stays a non-exportable source");
                if (clip.source.filename() == "Death_Back.uasset") check(metadata.value("unrealConvertible", false) && metadata.value("unrealObjectPath", "") == "/Game/Characters/Anims/Death_Back.Death_Back", "convertible metadata");
                else check(!metadata.value("unrealConvertible", true) && clip.note.find("no .uproject found") != std::string::npos, "unconvertible reason");
            }
        });
        test("folder enumerates individual meta clips and blocked assets", [&] {
            Session session; session.ingest({files}, cache); check(session.clips.size() == 5, "anim+2 meta+uasset+binary");
            session.selectAll(true); int selected = 0, blocked = 0;
            for (auto& clip : session.clips) { if (clip.selected) ++selected; if (!clip.selectable) { ++blocked; check(!session.select(clip.id, true), "cannot select blocked input"); } }
            check(selected == 3 && blocked == 2, "select all excludes blocked formats");
            session.ingest({files / "Run.anim"}, cache); check(session.clips.size() == 5, "duplicate drops de-duplicate");
        });
        test("duplicate metadata names remain visible and explicitly blocked", [&] {
            auto fbx = root / "duplicate-meta/Model.fbx"; write(fbx, "fixture");
            write(fromUtf8(utf8(fbx) + ".meta"), "ModelImporter:\n  animations:\n    clipAnimations:\n    - name: Walk\n      takeName: Take 001\n    - name: Walk\n      takeName: Take 002\n");
            Session session; session.ingest({fbx}, cache); check(session.clips.size() == 2, "both duplicate rows remain visible");
            check(!session.clips[0].selectable && !session.clips[1].selectable && session.clips[0].id != session.clips[1].id, "ambiguity blocks exact selection");
        });
        test("selected clip produces exact Python arguments", [&] {
            Session session; session.ingest({files / "Model.fbx"}, cache); check(session.select(session.clips[1].id, true), "select clip");
            Options options; options.output = root / "plans"; auto plan = makePlan(session, options);
            check(plan.jobs.size() == 1 && plan.jobs[0].clip.name == "Walk", "one selected clip");
            auto args = plan.jobs[0].arguments; check(std::find(args.begin(), args.end(), "--clip=Walk") != args.end(), "selected named clip passed to converter");
            check(plan.jobs[0].blockedReason.empty(), "FBX does not require external skeleton");
        });
        test("Humanoid FBX selected metadata clip uses isolated Unity helper", [&] {
            auto fbx = root / "humanoid-meta/Model.fbx"; write(fbx, "fixture");
            write(fromUtf8(utf8(fbx) + ".meta"), "ModelImporter:\n  animations:\n    clipAnimations:\n    - name: OtherHuman\n      takeName: First\n    - name: SelectedHuman\n      takeName: Second\n  animationType: 3\n");
            Session session; session.ingest({fbx}, cache);
            check(session.clips.size() == 2 && session.clips[1].type == "humanoid" && session.clips[1].rigType == "humanoid" && session.clips[1].route == "requires-unity-bake", "Humanoid metadata classified");
            check(session.select(session.clips[1].id, true), "selected embedded Humanoid clip");
            Options options; options.output = root / "human-selected-output";
            check(makePlan(session, options).jobs[0].blockedCode == "HELPER_UNAVAILABLE", "Humanoid FBX cannot fall back to raw Blender conversion");
            options.unityEditor = options.python = fs::absolute(args[0]); options.skeleton = files / "Model.fbx"; options.workRoot = root / "helper-work-root";
            auto plan = makePlan(session, options);
            check(plan.jobs.size() == 1 && plan.jobs[0].action == "unity-helper-bake" && plan.jobs[0].clip.name == "SelectedHuman", "only exact selected clip planned for Unity evaluation");
            auto result = execute(plan); check(result.size() == 1 && result[0].status == "ok", "helper received exact clip and Humanoid requirement");
            check(fs::is_empty(options.workRoot / "helper"), "successful helper cache cleaned outside user outputs");
            // Animation FBX files have no mesh; Unreal needs the character model first.
            auto model = options.output / "Character" / "Model.fbx";
            check(fs::is_regular_file(model) && fileSha256(model) == fileSha256(options.skeleton), "bake copies the character model next to the animations");
            auto report = json::parse(std::ifstream(options.output / "animbridge-results.json"));
            check(report.value("characterModel", "") == utf8(model), "results report names the character model copy");
            options.collisionPolicy = CollisionPolicy::Rename;
            check(execute(makePlan(session, options))[0].status == "ok", "second bake into the same folder succeeds");
            check(std::distance(fs::directory_iterator(options.output / "Character"), fs::directory_iterator{}) == 1, "an identical character model is reused, not duplicated");
            options.collisionPolicy = CollisionPolicy::Fail;
            session.selectAll(true, ExportMode::ExtractOriginal); options.mode = ExportMode::ExtractOriginal; options.output = root / "human-original-output";
            auto original = makePlan(session, options); check(original.jobs.size() == 1 && original.jobs[0].action == "extract-original", "Humanoid original extraction remains deduplicated and independent");
        });
        test("duplicate Humanoid metadata cannot bypass ambiguity through helper", [&] {
            auto fbx = root / "human-duplicate/Model.fbx"; write(fbx, "fixture");
            write(fromUtf8(utf8(fbx) + ".meta"), "ModelImporter:\n  animations:\n    clipAnimations:\n    - name: Duplicate\n    - name: Duplicate\n  animationType: 3\n");
            Session session; session.ingest({fbx}, cache); session.clips[0].selected = true;
            Options options; options.output = root / "human-duplicate-output"; options.unityEditor = fs::absolute(args[0]); options.skeleton = files / "Model.fbx";
            check(!makePlan(session, options).jobs[0].blockedReason.empty(), "ambiguous Humanoid clip was blocked");
        });
        test("dash-prefixed clip name is one argparse value", [&] {
            Session session; Clip clip; clip.id = "dash-clip"; clip.name = "--help"; clip.source = files / "Model.fbx";
            clip.kind = "fbx"; clip.engine = Engine::Unity; clip.namedClip = true; clip.selected = true; session.clips.push_back(clip);
            Options options; options.output = root / "dash-plan"; auto plan = makePlan(session, options);
            check(std::find(plan.jobs[0].arguments.begin(), plan.jobs[0].arguments.end(), "--clip=--help") != plan.jobs[0].arguments.end(), "dash name passed as exact value");
        });
        test("missing skeleton and empty selection cannot silently run", [&] {
            Session session; session.ingest({files / "Run.anim"}, cache); Options options; options.output = root / "blocked";
            expectFailure([&] { makePlan(session, options); }, "empty selection"); session.selectAll(true);
            check(!makePlan(session, options).jobs[0].blockedReason.empty(), "missing skeleton blocks anim");
        });
        test("explicit Unity helper selection includes binary anim without enabling unsupported assets", [&] {
            Session session; session.ingest({files / "Binary.anim", files / "A.uasset"}, cache);
            auto binary = std::find_if(session.clips.begin(), session.clips.end(), [](auto& clip) { return clip.kind == "anim"; });
            check(!session.select(binary->id, true), "ordinary selection still blocks unevaluated binary clips");
            check(session.select(binary->id, true, ExportMode::Bake, true), "explicit helper selection accepted binary clip");
            session.selectAll(true, ExportMode::Bake, true);
            check(std::count_if(session.clips.begin(), session.clips.end(), [](auto& clip) { return clip.selected; }) == 1, "helper selection did not enable raw uasset");
            Options options; options.output = root / "binary-helper-plan"; options.unityEditor = fs::absolute(args[0]); options.skeleton = files / "Model.fbx";
            check(makePlan(session, options).jobs[0].action == "unity-helper-bake", "binary clip planned through isolated helper");
        });
        test("Unreal manifest stages only selected FBX for Unity", [&] {
            auto exported = root / "unreal"; write(exported / "A.fbx", "; FBX 7.4.0\nengine exported A"); write(exported / "B.fbx", "; FBX 7.4.0\nengine exported B");
            json clips = json::array({{{"id", "/Game/A.A"}, {"name", "CON"}, {"fbx", "A.fbx"}, {"source_asset", "/Game/A.A"}}, {{"id", "/Game/B.B"}, {"name", "B"}, {"fbx", "B.fbx"}, {"source_asset", "/Game/B.B"}}});
            clips[0]["events"] = json::array({{{"name", "FootL"}, {"time", 0.25}}}); clips[0]["frames"] = 31;
            write(exported / "animbridge-manifest.json", json{{"animbridge_version", 1}, {"source_engine", "unreal"}, {"clips", clips}}.dump());
            Session session; session.ingest({exported}, cache); check(session.clips.size() == 2, "manifest does not duplicate FBX inputs");
            auto selectedId = session.clips[0].id;
            session.select(selectedId, true); Options options; options.direction = Direction::UnrealToUnity; options.output = root / "unity-staged";
            auto plan = makePlan(session, options); check(plan.jobs.size() == 1, "selected subset");
            auto results = execute(plan); check(results[0].status == "ready-for-unity-import", "does not claim engine import completion");
            auto manifest = json::parse(std::ifstream(options.output / "animbridge-manifest.json"));
            check(manifest["clips"].size() == 1 && manifest["clips"][0]["id"] == selectedId, "selected only output manifest");
            check(manifest["clips"][0]["events"] == clips[0]["events"] && manifest["clips"][0]["frames"] == 31, "events and frame metadata preserved");
            check(fs::is_regular_file(options.output / "_CON/_CON.fbx"), "Windows reserved filename sanitized");
            expectFailure([&] { execute(plan); }, "existing outputs must not be overwritten");
        });
        test("manifest path traversal rejected", [&] {
            auto p = root / "badmanifest/animbridge-manifest.json";
            write(p, json{{"animbridge_version", 1}, {"source_engine", "unreal"}, {"clips", json::array({{{"fbx", "../inputs/Model.fbx"}}})}}.dump());
            Session session; expectFailure([&] { session.ingest({p}, cache); }, "relative manifest traversal"); check(session.clips.empty(), "failed ingestion transactional");
        });
        test("same Unreal object path from different exports remains independently selectable", [&] {
            auto collection = root / "manifest-collection";
            for (auto directory : {"project-a-export", "project-b-export"}) {
                auto exported = collection / directory; write(exported / "Walk.fbx", std::string("; FBX 7.4.0\n") + directory);
                write(exported / "animbridge-manifest.json", json{{"animbridge_version", 1}, {"source_engine", "unreal"}, {"clips", json::array({{{"id", "/Game/Walk.Walk"}, {"name", "Walk"}, {"fbx", "Walk.fbx"}}})}}.dump());
            }
            Session session; session.ingest({collection}, cache);
            check(session.clips.size() == 2 && session.clips[0].id != session.clips[1].id, "project-qualified identities");
            check(session.select(session.clips[0].id, true) && session.select(session.clips[1].id, true), "independent selection");
        });
        test("partial Unity import report retains individual success despite nonzero editor exit", [&] {
            Session session; session.ingest({root / "manifest-collection"}, cache); session.selectAll(true);
            Options options; options.direction = Direction::UnrealToUnity; options.output = root / "partial-unity";
            options.targetEditor = fs::absolute(args[0]); options.targetProject = root / "mock-unity-project";
            fs::create_directories(options.targetProject / "Assets");
            auto results = execute(makePlan(session, options)); check(results.size() == 2, "two requested imports");
            check(results[0].status == "imported" && results[1].status == "error" && results[1].message == "fixture import failed", "per-clip success and failure mapping");
        });
        test("engine import preserves each item deadline without imposing a batch deadline", [&] {
            Session session; session.ingest({root / "manifest-collection"}, cache); session.selectAll(true);
            Options options; options.direction = Direction::UnrealToUnity; options.output = root / "item-deadline-unity";
            options.targetEditor = fs::absolute(args[0]); options.targetProject = root / "mock-unity-project"; options.jobTimeoutSeconds = 2;
            auto plan = makePlan(session, options); bool delayed = false;
            auto results = execute(plan, [&](const Result& result) { if (!delayed && result.step == "completed") { delayed = true; std::this_thread::sleep_for(std::chrono::milliseconds(2300)); } });
            check(results.size() == 2 && results[0].errorCode == "PROCESS_TIMEOUT" && results[1].status == "imported", "expired first item does not receive a fresh import budget and later item keeps its own budget");
        });
        test("Unicode and shell metacharacters survive process invocation", [&] {
            auto exe = fs::absolute(args[0]); auto log = root / fromUtf8("process 유니코드.log");
            auto process = runProcess(exe, {"--echo", "space \"quote\" & $(not-a-command) ;", "tail\\"}, root, log, 10);
            check(process.exitCode == 0 && !process.timedOut, "argv subprocess success");
            auto text = std::string(std::istreambuf_iterator<char>(std::ifstream(log, std::ios::binary).rdbuf()), {});
            check(text.find("space \"quote\" & $(not-a-command) ;") != std::string::npos && text.find("tail\\") != std::string::npos, "exact args retained");
        });
        test("subprocess environment is isolated and explicit", [&] {
            auto exe = fs::absolute(args[0]); auto log = root / "process-environment.log";
            auto process = runProcess(exe, {"--env", "ANIMBRIDGE_NATIVE_TEST_VALUE"}, root, log, 10, {{"ANIMBRIDGE_NATIVE_TEST_VALUE", "space = value & literal"}});
            check(process.exitCode == 0, "child override applied");
            std::ifstream f(log); std::string text(std::istreambuf_iterator<char>(f), {});
            check(text == "space = value & literal", "child environment value retained");
            check(std::getenv("ANIMBRIDGE_NATIVE_TEST_VALUE") == nullptr, "parent environment unchanged");
        });
#ifdef _WIN32
        test("normal launcher exit waits for its owned descendant tree to release files", [&] {
            auto lock = root / "owned-child-lock.bin";
            auto start = std::chrono::steady_clock::now();
            auto process = runProcess(fs::absolute(args[0]), {"--orphan-lock", utf8(lock)}, root, root / "owned-child.log", 10);
            check(process.exitCode == 0 && fs::exists(fromUtf8(utf8(lock) + ".ready")), "owned child actually held the file");
            std::error_code error; bool removed = fs::remove(lock, error);
            check(removed && !error, "normal launcher exit left a live descendant holding a cache file: " + error.message());
            check(std::chrono::steady_clock::now() - start < std::chrono::seconds(3), "owned descendant was not stopped promptly");
        });
#endif
        test("SHA-256 uses the standard digest and original extraction preserves bytes", [&] {
            check(textSha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "standard SHA-256 vector");
            Session session; session.ingest({files / "Binary.anim"}, cache); session.selectAll(true, ExportMode::ExtractOriginal);
            Options options; options.mode = ExportMode::ExtractOriginal; options.output = root / "extract-original";
            auto sourceHash = fileSha256(files / "Binary.anim"); auto plan = makePlan(session, options); bool premature = false;
            auto results = execute(plan, [&](const Result& result) { if (result.status == "ok" && result.step != "completed") premature = true; });
            check(!premature, "successful state only emitted after the final commit");
            check(results[0].status == "ok" && results[0].outputHash == sourceHash, "original hash check passed");
            check(fileSha256(results[0].output) == sourceHash && fileSha256(files / "Binary.anim") == sourceHash, "source unchanged and output identical");
            auto manifest = json::parse(std::ifstream(options.output / "animbridge-results.json"));
            check(manifest["results"][0]["checks"]["byteHash"] == "passed" && manifest["checks"]["enginePlayback"] == "notRun", "only performed checks claimed");
            check(!plan.jobId.empty() && !plan.attemptId.empty() && !plan.planSnapshot.empty(), "immutable attempt identity");
        });
        test("original FBX extraction deduplicates selected embedded clip sources", [&] {
            Session session; session.ingest({files / "Model.fbx"}, cache); session.selectAll(true, ExportMode::ExtractOriginal);
            Options options; options.mode = ExportMode::ExtractOriginal; options.output = root / "extract-fbx";
            auto plan = makePlan(session, options); check(plan.jobs.size() == 1 && plan.jobs[0].clipIds.size() == 2, "one original file for two selected clip records");
            auto result = execute(plan); check(result.size() == 1 && fileSha256(result[0].output) == fileSha256(files / "Model.fbx"), "FBX copied without rebaking");
            options.sampleRate = 60; expectFailure([&] { makePlan(session, options); }, "FPS is inapplicable to original extraction");
        });
#ifdef _WIN32
        test("temporary cleanup failure is reported without losing committed output", [&] {
            Session session; session.ingest({files / "Binary.anim"}, cache); session.selectAll(true, ExportMode::ExtractOriginal);
            Options options; options.mode = ExportMode::ExtractOriginal; options.output = root / "cleanup-warning";
            auto plan = makePlan(session, options); auto scratch = options.output / (".animbridge-tmp-" + plan.attemptId + "-0");
            HANDLE held = INVALID_HANDLE_VALUE;
            auto result = execute(plan, [&](const Result& progress) {
                if (progress.step == "committing") held = CreateFileW((scratch / "held.bin").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            });
            if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
            check(held != INVALID_HANDLE_VALUE && result[0].status == "warning" && !result[0].warnings.empty(), "cleanup failure was not reported");
            auto checks = json::parse(result[0].checksJson);
            check(checks["temporaryCleanup"]["status"] == "failed" && checks["temporaryCleanup"]["remainingPath"] == utf8(scratch), "remaining temporary path was not retained in checks");
            check(fileSha256(result[0].output) == fileSha256(files / "Binary.anim"), "cleanup warning damaged the committed output");
            fs::remove_all(scratch);
        });
#endif
        test("collision policies preserve, rename or explicitly replace existing originals", [&] {
            Session session; session.ingest({files / "Binary.anim"}, cache); session.selectAll(true, ExportMode::ExtractOriginal);
            Options options; options.mode = ExportMode::ExtractOriginal; options.output = root / "collision";
            auto existing = options.output / "Binary.anim"; write(existing, "keep old"); auto old = fileSha256(existing);
            options.collisionPolicy = CollisionPolicy::Skip; auto skipped = execute(makePlan(session, options));
            check(skipped[0].status == "skipped" && fileSha256(existing) == old, "skip keeps original output");
            options.collisionPolicy = CollisionPolicy::Rename; auto renamed = execute(makePlan(session, options));
            check(renamed[0].status == "ok" && renamed[0].output.filename() == "Binary_2.anim" && fileSha256(existing) == old, "rename creates separate output");
            options.collisionPolicy = CollisionPolicy::Overwrite; expectFailure([&] { makePlan(session, options); }, "overwrite is explicit");
            options.explicitOverwrite = true; auto replaced = execute(makePlan(session, options));
            check(replaced[0].status == "ok" && fileSha256(existing) == fileSha256(files / "Binary.anim"), "explicit overwrite commits verified bytes");
            auto backup = json::parse(replaced[0].checksJson).at("replacedFileBackup"); auto backupPath = fromUtf8(backup.at("path").get<std::string>());
            check(fs::is_regular_file(backupPath) && fileSha256(backupPath) == old && backup["sha256"] == old, "previous user bytes preserved in a verified replacement backup");
            check(backupPath.parent_path().filename() == "replaced" && backupPath.parent_path().parent_path().filename() == replaced[0].attemptId, "backup belongs to this attempt history");
            auto manifest = json::parse(std::ifstream(replaced[0].manifestPath));
            check(manifest["results"][0]["checks"]["replacedFileBackup"] == backup && backup["originalTarget"] == utf8(existing), "backup path, target and digest recorded in the output manifest");
        });
        test("cancellation at a safe boundary discards temporary outputs", [&] {
            Session session; session.ingest({files / "Binary.anim"}, cache); session.selectAll(true, ExportMode::ExtractOriginal);
            Options options; options.mode = ExportMode::ExtractOriginal; options.output = root / "cancelled";
            auto cancel = std::make_shared<std::atomic_bool>(false); auto plan = makePlan(session, options);
            auto results = execute(plan, [&](const Result& result) { if (result.step == "verifying") cancel->store(true); }, cancel);
            check(results[0].status == "cancelled" && !fs::exists(plan.jobs[0].output), "cancelled temporary file was never published");
            for (auto& entry : fs::directory_iterator(options.output)) check(!utf8(entry.path().filename()).starts_with(".animbridge-tmp-"), "cancelled temporary directory removed");
        });
        test("plan mutation and source changes require a newly validated plan", [&] {
            auto source = root / "mutable/Source.anim"; write(source, "initial bytes"); Session session; session.ingest({source}, cache); session.selectAll(true, ExportMode::ExtractOriginal);
            Options options; options.mode = ExportMode::ExtractOriginal; options.output = root / "source-change";
            auto plan = makePlan(session, options); auto changedPlan = plan; changedPlan.options.output = root / "other-output";
            expectFailure([&] { execute(changedPlan); }, "changed confirmed plan is rejected");
            write(source, "changed bytes"); auto results = execute(plan);
            check(results[0].status == "error" && results[0].errorCode == "SOURCE_CHANGED" && !fs::exists(plan.jobs[0].output), "source hash frozen at confirmation");
        });
        test("retry creates a new attempt containing only failed items", [&] {
            auto directory = root / "retry-input"; write(directory / "A.anim", "A bytes"); write(directory / "B.anim", "B bytes");
            Session session; session.ingest({directory}, cache); session.selectAll(true, ExportMode::ExtractOriginal);
            Options options; options.mode = ExportMode::ExtractOriginal; options.output = root / "retry-output";
            auto plan = makePlan(session, options); auto failedSource = plan.jobs[1].clip.source; fs::rename(failedSource, fromUtf8(utf8(failedSource) + ".retained"));
            auto results = execute(plan); check(results[0].status == "ok" && results[1].status == "error", "partial failure retained");
            auto successHash = fileSha256(results[0].output); auto retry = retryFailed(plan, results);
            check(retry.jobs.size() == 1 && retry.jobId == plan.jobId && retry.attemptId != plan.attemptId && retry.parentAttemptId == plan.attemptId, "retry identity links the previous attempt");
            write(failedSource, failedSource.filename() == "B.anim" ? "B bytes" : "A bytes");
            auto retried = execute(retry); check(retried[0].status == "error" && retried[0].errorCode == "SOURCE_CHANGED" && fileSha256(results[0].output) == successHash, "replacement source identity requires re-analysis and successful item is preserved");
            Session refreshed; refreshed.ingest({failedSource}, cache); refreshed.selectAll(true, ExportMode::ExtractOriginal);
            auto refreshedResults = execute(makePlan(refreshed, options));
            check(refreshedResults[0].status == "ok" && fileSha256(results[0].output) == successHash, "newly analysed replacement can be exported without repeating successful item");
            check(fs::is_regular_file(options.output / ".animbridge-history" / plan.attemptId / "output-manifest.json") && fs::is_regular_file(options.output / ".animbridge-history" / retry.attemptId / "output-manifest.json"), "both attempt reports preserved");
        });
        test("unknown metadata stays null and a damaged clip does not hide valid neighbours", [&] {
            auto directory = root / "partial-analysis"; write(directory / "Bad.anim", "%YAML 1.1\nnot an AnimationClip\n"); write(directory / "Good.anim", anim); write(directory / "NoMetadata.fbx", "opaque FBX");
            Session session; session.ingest({directory}, cache); check(session.clips.size() == 3, "all safe original files remain listed");
            auto data = json::parse(sessionJson(session));
            for (auto& clip : data["clips"]) {
                check(clip["sourceHash"].get<std::string>().size() == 64, "real asset digest recorded");
                if (clip["kind"] == "fbx") check(clip["fps"].is_null() && clip["duration"].is_null() && clip["rigType"].is_null(), "unknown time and rig are null");
                if (clip["name"] == "Bad") check(clip["type"].is_null() && clip["capabilities"]["extract"] == true && clip["capabilities"]["bake"] == false, "damaged data is preserved without fake conversion support");
            }
        });
        test("changing the original package invalidates a previously confirmed extraction", [&] {
            auto source = root / "package-change.unitypackage"; package(source, {{guid + "/pathname", "Assets/A.anim"}, {guid + "/asset", anim}});
            Session session; session.ingest({source}, cache); session.selectAll(true, ExportMode::ExtractOriginal);
            Options options; options.mode = ExportMode::ExtractOriginal; options.output = root / "package-change-out"; auto plan = makePlan(session, options);
            package(source, {{guid + "/pathname", "Assets/A.anim"}, {guid + "/asset", anim + "# changed package bytes\n"}});
            auto results = execute(plan); check(results[0].status == "error" && results[0].errorCode == "SOURCE_CHANGED" && !fs::exists(plan.jobs[0].output), "original package snapshot enforced");
        });
        test("a foreign report at the output root is never overwritten", [&] {
            Session session; session.ingest({files / "Binary.anim"}, cache); session.selectAll(true, ExportMode::ExtractOriginal);
            Options options; options.mode = ExportMode::ExtractOriginal; options.output = root / "foreign-report";
            write(options.output / "animbridge-results.json", "user-owned report"); auto previous = fileSha256(options.output / "animbridge-results.json");
            auto plan = makePlan(session, options); auto results = execute(plan);
            check(results[0].status == "ok" && fileSha256(options.output / "animbridge-results.json") == previous, "foreign report bytes preserved");
            check(fs::is_regular_file(results[0].manifestPath) && fs::is_regular_file(options.output / ("animbridge-results-" + plan.attemptId + ".json")), "new report has a separate attempt identity");
        });
        test("target profile is part of the snapshot and cannot contradict the bake direction", [&] {
            Session session; session.ingest({files / "Model.fbx"}, cache); session.selectAll(true);
            Options options; options.output = root / "profiles"; auto plan = makePlan(session, options);
            check(json::parse(plan.planSnapshot)["targetProfile"] == "unreal", "default profile inferred from existing direction");
            options.targetProfile = "unity"; expectFailure([&] { makePlan(session, options); }, "contradictory target profile rejected");
        });
        test("Generic bone mapping reaches the converter as a frozen JSON file", [&] {
            Session session; session.ingest({files / "Run.anim"}, cache); session.selectAll(true);
            Options options; options.output = root / "mapped-bake"; options.skeleton = files / "Model.fbx"; options.python = fs::absolute(args[0]); options.boneMapJson = "{\"Hips\":\"HipBone\"}";
            auto plan = makePlan(session, options); auto result = execute(plan);
            check(result[0].status == "ok" && fs::is_regular_file(result[0].output / "Mapped.fbx"), "mock converter validated the actual --bone-map payload");
            check(json::parse(plan.planSnapshot)["options"]["boneMap"]["Hips"] == "HipBone", "mapping preserved in the confirmed snapshot");
        });
        test("existing bake directories cannot be deleted even with explicit overwrite", [&] {
            Session session; session.ingest({files / "Run.anim"}, cache); session.selectAll(true);
            Options options; options.output = root / "protected-bake"; options.skeleton = files / "Model.fbx"; options.python = fs::absolute(args[0]); options.boneMapJson = "{\"Hips\":\"HipBone\"}";
            options.collisionPolicy = CollisionPolicy::Overwrite; options.explicitOverwrite = true;
            auto initial = makePlan(session, options); auto userFile = initial.jobs[0].output / "user-owned.txt"; write(userFile, "keep every byte"); auto hash = fileSha256(userFile);
            auto plan = makePlan(session, options); check(plan.jobs[0].blockedCode == "OUTPUT_CONFLICT", "overwrite is blocked during preflight");
            auto result = execute(plan); check(result[0].status == "blocked" && result[0].errorCode == "OUTPUT_CONFLICT" && fileSha256(userFile) == hash, "existing arbitrary directory remains intact");
        });
        test("known blocked causes are distinguished and unknown exceptions are not diagnosed as disk failures", [&] {
            Options options; options.output = root / "honest-errors";
            Session binary; binary.ingest({files / "Binary.anim"}, cache); binary.clips[0].selected = true;
            auto helper = execute(makePlan(binary, options)); check(helper[0].errorCode == "HELPER_UNAVAILABLE", "missing Helper is not a generic dependency diagnosis");
            Session opaque; opaque.ingest({files / "A.uasset"}, cache); opaque.clips[0].selected = true; options.output = root / "unsupported-errors";
            auto unsupported = execute(makePlan(opaque, options)); check(unsupported[0].errorCode == "UNSUPPORTED_DATA", "opaque .uasset has a supported-boundary error");
            Session generic; generic.ingest({files / "Model.fbx"}, cache); generic.clips[0].selected = true; options.output = root / "unknown-errors";
            auto plan = makePlan(generic, options); plan.jobs[0].action = "unrecognised-adapter-for-test"; plan.planSnapshot.clear();
            auto unknown = execute(plan); check(unknown[0].errorCode == "UNKNOWN_ERROR" && unknown[0].recovery.find("log") != std::string::npos, "unclassified adapter exception keeps its evidence without inventing a write failure");
        });
        test("read-only subprocess cancellation terminates promptly without waiting for its timeout", [&] {
            auto cancel = std::make_shared<std::atomic_bool>(false); auto start = std::chrono::steady_clock::now();
            std::jthread request([&] { std::this_thread::sleep_for(std::chrono::milliseconds(100)); cancel->store(true); });
            auto process = runProcess(fs::absolute(args[0]), {"--sleep"}, root, root / "cancel-process.log", 10, {}, cancel);
            check(process.cancelled && !process.timedOut && process.exitCode == 4, "cancellation has a distinct process result");
            check(std::chrono::steady_clock::now() - start < std::chrono::seconds(2), "process did not wait five seconds or the ten-second timeout");
        });
        test("original extraction ignores saved skeleton and evaluation metadata dependencies", [&] {
            auto directory = root / "extract-no-skeleton";
            auto source = directory / "Source.anim"; write(source, "original animation bytes"); auto sourceMeta = fromUtf8(utf8(source) + ".meta"); write(sourceMeta, "old source metadata");
            auto skeleton = directory / "Skeleton.fbx"; write(skeleton, "previously saved skeleton"); auto skeletonMeta = fromUtf8(utf8(skeleton) + ".meta"); write(skeletonMeta, "old skeleton metadata");
            Session session; session.ingest({source}, cache); session.selectAll(true, ExportMode::ExtractOriginal);
            Options options; options.mode = ExportMode::ExtractOriginal; options.skeleton = skeleton; options.output = root / "extract-no-skeleton-output";
            auto plan = makePlan(session, options);
            check(plan.jobs[0].metadataHash.empty() && plan.jobs[0].skeletonHash.empty() && plan.jobs[0].skeletonMetadataHash.empty(), "extract plan has no evaluation dependency hashes");
            fs::remove(skeleton); fs::remove(skeletonMeta); write(sourceMeta, "changed metadata that does not change original file bytes");
            auto result = execute(plan); check(result[0].status == "ok" && fileSha256(result[0].output) == fileSha256(source), "unrelated skeleton deletion cannot block original extraction");
        });
        test("FBX rig inspection tells Null-only joints from real skeletons", [&] {
            auto nulls = root / "rig/Nulls.fbx"; write(nulls, binaryFbx({{"Model", "Null"}, {"Model", "Null"}, {"Model", "Null"}}));
            auto rig = inspectFbxRig(nulls);
            check(rig.parsed && rig.nullModels == 3 && rig.boneModels == 0 && lacksSkeleton(rig), "Null-only joints are reported as no skeleton");
            check(describeMissingSkeleton(rig).find("3 joints are plain Null objects") != std::string::npos, "message names the Null joints");
            auto limbs = root / "rig/Limbs.fbx"; write(limbs, binaryFbx({{"Model", "Null"}, {"Model", "LimbNode"}, {"Geometry", "Mesh"}}));
            rig = inspectFbxRig(limbs); check(rig.parsed && rig.boneModels == 1 && rig.meshes == 1 && !lacksSkeleton(rig), "LimbNode joints are a skeleton");
            auto skinned = root / "rig/Skinned.fbx"; write(skinned, binaryFbx({{"Model", "Null"}, {"Model", "Mesh"}, {"Deformer", "Skin"}}));
            check(!lacksSkeleton(inspectFbxRig(skinned)), "a skinned mesh lets importers build a skeleton from Null joints");
            auto ascii = root / "rig/Ascii.fbx";
            write(ascii, "; FBX 7.3.0 project file\nObjects:  {\n\tModel: 1, \"Model::Hips\", \"Null\" {\n\t}\n}\nConnections:  {\n}\n");
            rig = inspectFbxRig(ascii); check(rig.parsed && rig.nullModels == 1 && lacksSkeleton(rig), "ASCII Null-only file is detected");
            write(ascii, "; FBX 7.3.0 project file\nObjects:  {\n\tModel: 1, \"Model::Hips\", \"LimbNode\" {\n\t}\n}\n");
            check(!lacksSkeleton(inspectFbxRig(ascii)), "ASCII LimbNode file is a skeleton");
            auto truncated = root / "rig/Truncated.fbx"; auto bytes = binaryFbx({{"Model", "Null"}}); write(truncated, bytes.substr(0, bytes.size() - 30));
            check(!inspectFbxRig(truncated).parsed && !lacksSkeleton(inspectFbxRig(truncated)), "damaged files make no claim");
            auto other = root / "rig/Other.fbx"; write(other, "not an fbx");
            check(!inspectFbxRig(other).parsed, "unrecognised bytes make no claim");
        });
        test("bake planning blocks a skeleton FBX whose joints are only Null objects", [&] {
            auto source = root / "null-rig/Kick.fbx"; write(source, binaryFbx({{"Model", "Null"}, {"Model", "Null"}}));
            Session session; session.ingest({source}, cache); session.selectAll(true);
            Options options; options.output = root / "null-rig-output";
            auto plan = makePlan(session, options);
            check(plan.jobs[0].blockedCode == "DEPENDENCY_MISSING" && plan.jobs[0].blockedReason.find("character model FBX") != std::string::npos, "clip FBX without a skeleton is blocked before Blender runs");
            auto model = root / "null-rig/Model.fbx"; write(model, binaryFbx({{"Model", "LimbNode"}, {"Geometry", "Mesh"}, {"Deformer", "Skin"}}));
            options.skeleton = model; plan = makePlan(session, options);
            check(plan.jobs[0].blockedCode != "DEPENDENCY_MISSING", "a real character model clears the block");
            options.skeleton = source; plan = makePlan(session, options);
            check(plan.jobs[0].blockedReason.find("connected skeleton FBX cannot be used") != std::string::npos, "a Null-only skeleton is named as the problem");
        });
        test("original extraction for Unreal warns when the FBX has no skeleton", [&] {
            auto source = root / "null-extract/Kick.fbx"; write(source, binaryFbx({{"Model", "Null"}, {"Model", "Null"}}));
            Session session; session.ingest({source}, cache); session.selectAll(true, ExportMode::ExtractOriginal);
            Options options; options.mode = ExportMode::ExtractOriginal; options.output = root / "null-extract-output";
            auto result = execute(makePlan(session, options));
            check(result[0].status == "warning" && result[0].message.find("Unreal will not import this file on its own") != std::string::npos && !result[0].warnings.empty(), "extracted Null-only FBX carries a warning");
            check(fileSha256(result[0].output) == fileSha256(source), "the extracted bytes are still the original");
            options.direction = Direction::UnrealToUnity; options.output = root / "null-extract-unity";
            result = execute(makePlan(session, options));
            check(result[0].status == "ok", "Unity-bound extraction is unchanged");
        });
        std::cout << passed << " native tests passed\n"; return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
