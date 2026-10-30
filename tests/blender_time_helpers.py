"""Sparse FBX keys and fractional trim-boundary fixtures."""
import json
import math
from pathlib import Path
import runpy
import sys

import bpy

args = sys.argv[sys.argv.index("--") + 1:]
command = args[0]
if command == "make":
    # Reuse the established skeleton, then export with an explicit sparse bake step.
    sys.argv = [__file__, "--", "make_anim_fbx", args[1]]
    runpy.run_path(str(Path(__file__).with_name("blender_helpers.py")))
    scene = bpy.context.scene
    scene.render.fps, scene.render.fps_base = 30, 30 / 29.97
    bpy.ops.export_scene.fbx(filepath=args[1], use_selection=True, object_types={"ARMATURE"},
                            add_leaf_bones=False, bake_anim=True, bake_anim_use_all_actions=False,
                            bake_anim_use_nla_strips=False, bake_anim_step=10,
                            bake_anim_simplify_factor=0.0)
elif command == "sample":
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.fbx(filepath=args[1], automatic_bone_orientation=False, anim_offset=0.0)
    arm = next(o for o in bpy.context.scene.objects if o.type == "ARMATURE")
    scene = bpy.context.scene
    fps = scene.render.fps / scene.render.fps_base
    action = arm.animation_data.action
    key_times = sorted({float(k.co.x) / scene.render.fps for layer in action.layers
                        for strip in layer.strips for bag in strip.channelbags
                        for fc in bag.fcurves for k in fc.keyframe_points})
    frames = []
    for seconds in json.loads(args[3]):
        frame = seconds * scene.render.fps
        scene.frame_set(math.floor(frame), subframe=frame - math.floor(frame))
        frames.append({b.name: [float(v) for row in
                       (b.parent.matrix.inverted() @ b.matrix if b.parent else b.matrix)
                       for v in row] for b in arm.pose.bones})
    Path(args[2]).write_text(json.dumps({"fps": fps, "keys": key_times, "frames": frames}))
