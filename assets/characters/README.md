# Bundled default preview character

`Default_Humanoid.fbx` is the character AnimBridge uses for previews when the
user has not connected a model of their own.

## Origin

- The body mesh was generated with Meshy AI (text-to-3D task
  `01a0dca8-c7a1-72d8-9683-116da3cf48da`).
- It was rigged and skinned in Blender 5.1 for the Mocapture project, which has
  the same author as AnimBridge. The original lives in
  `Mocapture/assets/mocapture-humanoid/`.
- Textures are embedded in the FBX.

## Rig summary

The file is deliberately not named after its root node: Unity names the imported
root after the file, and a matching `Mocapture_Humanoid` child node caused
"Duplicate Transform names" in the preview helper.


- 80 bones: root, 21 body bones, 30 finger bones, 28 toe bones.
- T-pose, about 1.8 m tall; 49,141 vertices, 97,719 triangles.
- Bone names match Unity's humanoid names one to one (Hips, Spine, Chest, Neck,
  Head, Left/Right Shoulder, UpperArm, LowerArm, Hand, UpperLeg, LowerLeg, Foot,
  Toes), so `src/unityanimbridge/default_humanoid.py` maps them directly
  (`BUNDLED_MAPPING`).

## Use

It is the default preview character. It is copied into the installer and
portable payload as `backend/assets/characters/`. Choosing a character FBX
yourself takes priority; choosing an Unreal Editor switches the preview to the
optional Unreal Manny path instead.

## Licensing

The mesh was generated on a paid Meshy plan, whose terms give the subscriber
ownership of the generated asset, and the rig was made by the AnimBridge
author. The author distributes this file with AnimBridge for previews. The
repository's MIT license covers AnimBridge source code; this asset is shipped
as part of the app and is not separately relicensed.
