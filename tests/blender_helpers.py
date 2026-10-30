"""Run inside Blender by the tests. Usage: blender -b --python blender_helpers.py -- <cmd> <args...>

make_skeleton <out.fbx> <unit:m|cm>   build a 3-bone armature and export it
dump_pose <in.fbx> <out.json>         import an animated FBX and dump bone node-local matrices per frame
make_anim_fbx <out.fbx>               skeleton with animated take "Take1" (frames 0..30)
"""
import json
import sys

import bpy

args = sys.argv[sys.argv.index("--") + 1:]
cmd = args[0]


def build_armature():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    arm = bpy.data.armatures.new("Arm")
    ob = bpy.data.objects.new("Armature", arm)
    bpy.context.scene.collection.objects.link(ob)
    bpy.context.view_layer.objects.active = ob
    bpy.ops.object.mode_set(mode="EDIT")
    a = arm.edit_bones.new("Hips")
    a.head, a.tail, a.roll = (0, 0, 1), (0, 0.1, 1.3), 0.3
    b = arm.edit_bones.new("Spine")
    b.head, b.tail, b.roll, b.parent = (0, 0.1, 1.3), (0.2, 0.1, 1.7), -0.7, a
    c = arm.edit_bones.new("Hand")
    c.head, c.tail, c.parent = (0.2, 0.1, 1.7), (0.5, 0.3, 1.8), b
    bpy.ops.object.mode_set(mode="OBJECT")
    return ob


def export(path, ob, bake):
    for o in bpy.context.scene.objects:
        o.select_set(o == ob)
    bpy.ops.export_scene.fbx(filepath=path, use_selection=True, object_types={"ARMATURE"},
                             add_leaf_bones=False, bake_anim=bake, bake_anim_use_all_actions=False,
                             bake_anim_use_nla_strips=False, bake_anim_simplify_factor=0.0)


if cmd == "make_skeleton":
    ob = build_armature()
    if args[2] == "cm":
        bpy.context.scene.unit_settings.scale_length = 0.01
    export(args[1], ob, False)

elif cmd == "make_anim_fbx":
    ob = build_armature()
    pb = ob.pose.bones["Spine"]
    pb.rotation_mode = "QUATERNION"
    ob.animation_data_create()
    for f, ang in ((0, 0.0), (30, 1.2)):
        bpy.context.scene.frame_set(f)
        import mathutils
        pb.rotation_quaternion = mathutils.Quaternion((1, 0, 0), ang)
        pb.keyframe_insert("rotation_quaternion", frame=f)
    ob.animation_data.action.name = "Take1"
    for layer in ob.animation_data.action.layers:
        for strip in layer.strips:
            for cb in strip.channelbags:
                for fc in cb.fcurves:
                    for kp in fc.keyframe_points:
                        kp.interpolation = "LINEAR"
    bpy.context.scene.frame_start, bpy.context.scene.frame_end = 0, 30
    export(args[1], ob, True)

elif cmd == "dump_pose":
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.fbx(filepath=args[1], automatic_bone_orientation=False, anim_offset=0.0)
    ob = next(o for o in bpy.context.scene.objects if o.type == "ARMATURE")
    act = ob.animation_data.action if ob.animation_data else None
    lo, hi = (int(round(v)) for v in act.frame_range) if act else (0, 0)
    out = {"frames": [], "fps": bpy.context.scene.render.fps / bpy.context.scene.render.fps_base,
           "bones": [b.name for b in ob.data.bones], "object": ob.name}
    for f in range(lo, hi + 1):
        bpy.context.scene.frame_set(f)
        frame = {}
        for pb in ob.pose.bones:
            m = pb.matrix if pb.parent is None else pb.parent.matrix.inverted() @ pb.matrix
            frame[pb.name] = [v for row in m for v in row]
        out["frames"].append(frame)
    with open(args[2], "w") as fh:
        json.dump(out, fh)
