#pragma once
#include "animbridge/path_identity.hpp"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

namespace animbridge::supervisor_detail {
// Unity's Editor spawns build helpers that can outlive it. Only this closed list
// may be adopted by name; anything else (notably Unity.Licensing.Client, which
// Unity Hub shares between editors) is never in it and stays preserved.
inline bool isKnownUnityBuildHelper(const std::filesystem::path& image) {
    auto name = image.filename().string();
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (name.size() > 4 && name.ends_with(".exe")) name.resize(name.size() - 4);
    static const char* const known[] = {"bee_backend", "unityshadercompiler", "unitypackagemanager", "unitycrashhandler64", "unitycrashhandler32"};
    return std::any_of(std::begin(known), std::end(known), [&](const char* entry) { return name == entry; });
}
// The folder whose contents belong to an approved Unity Editor, or empty when the
// executable is not Unity itself (Blender, Python, Unreal...). Windows/Linux keep
// helpers beside or below Unity(.exe); macOS keeps them inside the Unity.app bundle.
inline std::filesystem::path unityInstallRoot(const std::filesystem::path& editor) {
    auto name = editor.filename().string();
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (name != "unity" && name != "unity.exe") return {};
    for (auto parent = editor.parent_path(); !parent.empty() && parent != parent.parent_path(); parent = parent.parent_path()) {
        auto text = parent.filename().string();
        if (text.size() > 4 && text.ends_with(".app")) return parent;
    }
    return editor.parent_path();
}
// Decides whether a process that is ALREADY a member of this run's job object /
// process group (the caller's proof it descends from our worker) may also be
// terminated. The name allow-list alone is not enough: the image must live in an
// Editor install folder we were told about, so a same-named binary elsewhere
// (a Hub copy, a user tool) is never adopted.
inline bool isOwnedUnityHelper(const std::filesystem::path& image, const std::vector<std::filesystem::path>& editorRoots) {
    if (image.empty() || !isKnownUnityBuildHelper(image)) return false;
    for (auto& root : editorRoots) {
        try { if (!root.empty() && pathContains(root, image)) return true; }
        catch (...) { } // unreadable path: stay conservative and keep the process preserved
    }
    return false;
}
}
