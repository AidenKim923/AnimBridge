#pragma once
#include <filesystem>
#include <string>

namespace animbridge {
// A quick look at what kind of rig an FBX file carries, read from the
// Objects section without evaluating any animation. Unreal and Blender
// build a skeleton only from LimbNode/Limb/Root models or from a skinned
// mesh. Some exporters (3ds Max animation-only files, for example) write
// every bone as a plain Null model instead, and such a file cannot be
// imported as an animation on its own.
struct FbxRig {
    bool parsed = false;   // false: not an FBX we could read; make no claims
    int boneModels = 0;    // Model nodes of class LimbNode, Limb or Root
    int nullModels = 0;    // Model nodes of class Null
    int meshes = 0;        // Geometry or Model nodes of class Mesh
    int skins = 0;         // Deformer nodes of class Skin
};

FbxRig inspectFbxRig(const std::filesystem::path& path);

// True only when the file was read and has neither bone models nor a skin.
bool lacksSkeleton(const FbxRig& rig);

// One sentence on what is missing, for plan and result messages.
std::string describeMissingSkeleton(const FbxRig& rig);
}
