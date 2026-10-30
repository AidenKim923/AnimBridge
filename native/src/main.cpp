#include "animbridge/core.hpp"
#include "animbridge/instance_lock.hpp"
#include "animbridge/path_grants.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace animbridge;
namespace {
int run(const std::vector<std::string>& args) {
    if (args.size() < 2 || args[1] == "--help" || args[1] == "-h") {
        std::cout << "AnimBridge C++ animation transfer\n"
            "  AnimBridgeCli scan <path...> [--cache DIR]\n"
            "  AnimBridgeCli plan|run <path...> --all|--select ID --out DIR [options]\n"
            "Options: --direction unity-to-unreal|unreal-to-unity, --skeleton FBX,\n"
            "  --python EXE, --blender EXE, --repo DIR (development builds only)\n"
            "  --cache DIR, --data-dir DIR, --timeout SECONDS (per stage, default 600)\n"
            "  --job-timeout SECONDS (per clip/request, default 3600; no batch-wide deadline)\n"
            "  --unity-editor EXE (isolated Humanoid/binary Unity evaluation)\n"
            "  --target-editor EXE --target-project PATH [--target-skeleton /Game/... ]\n"
            "  --target-folder /Game/AnimBridge|Assets/AnimBridge\n"
            "Without target editor/project, run exports/stages FBX; engine import remains pending.\n"
            "Unreal input: drop the folder/manifest produced by unreal/animbridge_export.py.\n";
        return args.size() < 2 ? 2 : 0;
    }
    const auto command = args[1];
    if (command != "scan" && command != "plan" && command != "run") throw std::runtime_error("Unknown command: " + command);
    Options options; std::vector<fs::path> inputs; std::vector<std::string> selected; bool all = false;
    fs::path cache, dataRoot;
    for (std::size_t i = 2; i < args.size(); ++i) {
        auto arg = args[i];
        if (arg == "--all") { all = true; continue; }
        if (!arg.starts_with("--")) { inputs.push_back(fromUtf8(arg)); continue; }
        if (++i == args.size()) throw std::runtime_error("Missing value for " + arg);
        auto value = args[i];
        if (arg == "--cache") cache = fromUtf8(value);
        else if (arg == "--data-dir") dataRoot = fromUtf8(value);
        else if (arg == "--out") options.output = fromUtf8(value);
        else if (arg == "--select") selected.push_back(value);
        else if (arg == "--skeleton") options.skeleton = fromUtf8(value);
        else if (arg == "--python") options.python = fromUtf8(value);
        else if (arg == "--blender") options.blender = fromUtf8(value);
        else if (arg == "--repo") options.repository = fromUtf8(value);
        else if (arg == "--unity-editor") options.unityEditor = fromUtf8(value);
        else if (arg == "--target-editor") options.targetEditor = fromUtf8(value);
        else if (arg == "--target-project") options.targetProject = fromUtf8(value);
        else if (arg == "--target-skeleton") options.targetSkeleton = value;
        else if (arg == "--target-folder") options.targetFolder = value;
        else if (arg == "--timeout" || arg == "--job-timeout") {
            std::size_t consumed = 0; auto n = std::stoul(value, &consumed);
            if (!n || n > 86400 || consumed != value.size()) throw std::runtime_error("Timeout must be 1..86400 seconds");
            if (arg == "--job-timeout") options.jobTimeoutSeconds = static_cast<unsigned>(n);
            else options.timeoutSeconds = static_cast<unsigned>(n);
        } else if (arg == "--direction") {
            if (value == "unity-to-unreal") options.direction = Direction::UnityToUnreal;
            else if (value == "unreal-to-unity") options.direction = Direction::UnrealToUnity;
            else throw std::runtime_error("Unknown direction: " + value);
        } else throw std::runtime_error("Unknown option: " + arg);
    }
    if (inputs.empty()) throw std::runtime_error("Drop or specify at least one input path");
    validateRuntimeOverrides(options.repository, options.python, options.blender);
    auto context = resolveRuntimeContext(dataRoot);
    options.runtimeContext = context;
    InstanceLock ownership(context.dataRoot);
    if (!ownership.acquired()) throw std::runtime_error("DATA_ROOT_BUSY: " + ownership.diagnostic());
    if (cache.empty()) cache = context.cacheRoot;
    options.workRoot = cache / "jobs";
    options.journalRoot = context.stateRoot / "attempts";
    PathGrantRegistry authority(context.cacheRoot / "probes");
    for (const auto& input : inputs) authority.issue(input, GrantPurpose::Read, GrantSource::CommandLine);
    if (!options.skeleton.empty()) authority.issue(options.skeleton, GrantPurpose::Read, GrantSource::CommandLine);
    if (!options.output.empty()) {
        fs::create_directories(options.output);
        authority.issue(options.output, GrantPurpose::ExportRoot, GrantSource::CommandLine);
    }
    if (!options.unityEditor.empty()) {
        auto approval = authority.issue(options.unityEditor, GrantPurpose::Editor, GrantSource::CommandLine, "unity");
        options.unityEditor = authority.resolve(approval.at("grant").get<std::string>(), GrantPurpose::Editor);
    }
    const auto targetEngine = options.direction == Direction::UnityToUnreal ? "unreal" : "unity";
    if (!options.targetEditor.empty()) {
        auto approval = authority.issue(options.targetEditor, GrantPurpose::Editor, GrantSource::CommandLine, targetEngine);
        options.targetEditor = authority.resolve(approval.at("grant").get<std::string>(), GrantPurpose::Editor);
    }
    if (!options.targetProject.empty()) {
        if (options.targetFolder.empty()) options.targetFolder = options.direction == Direction::UnityToUnreal ? "/Game/AnimBridge" : "Assets/AnimBridge";
        auto approval = authority.issue(options.targetProject, GrantPurpose::EngineProject, GrantSource::CommandLine, targetEngine, options.targetFolder);
        authority.approveTarget(approval.at("grant").get<std::string>(), targetEngine, options.targetFolder);
    }
    Session session; session.ingest(inputs, cache, {}, context);
    if (command == "scan") { std::cout << sessionJson(session) << '\n'; return 0; }
    if (all) session.selectAll(true, ExportMode::Bake, !options.unityEditor.empty());
    for (auto& id : selected) if (!session.select(id, true, ExportMode::Bake, !options.unityEditor.empty())) throw std::runtime_error("Unknown or blocked clip ID: " + id);
    auto plan = makePlan(session, options);
    if (command == "plan") { std::cout << planJson(plan) << '\n'; return 0; }
    auto results = execute(plan); std::cout << resultsJson(results) << '\n';
    for (auto& result : results) if (result.status == "error" || result.status == "blocked") return 1;
    return 0;
}
}
#ifdef _WIN32
int wmain(int count, wchar_t** values) {
    std::vector<std::string> args; for (int i = 0; i < count; ++i) args.push_back(utf8(fs::path(values[i])));
#else
int main(int count, char** values) {
    std::vector<std::string> args(values, values + count);
#endif
    try { return run(args); }
    catch (const std::exception& e) { std::cerr << "AnimBridge: " << e.what() << '\n'; return 2; }
}
