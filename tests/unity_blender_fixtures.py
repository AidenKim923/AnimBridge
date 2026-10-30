"""Build skinned FBX fixtures with explicit axes and units inside Blender."""
import runpy
import sys
from pathlib import Path

import bpy
from mathutils import Quaternion

out, kind = sys.argv[sys.argv.index("--") + 1:]
unit = "cm" if kind == "zup_cm" else "m"
# Reuse the round-trip tests' bone layout.
sys.argv = [__file__, "--", "make_skeleton", out, unit]
helpers = runpy.run_path(str(Path(__file__).with_name("blender_helpers.py")))
ob = bpy.context.view_layer.objects.active
if kind == "humanoid":
    bpy.data.objects.remove(ob, do_unlink=True)
    arm = bpy.data.armatures.new("Human")
    ob = bpy.data.objects.new("Armature", arm)
    bpy.context.scene.collection.objects.link(ob)
    bpy.context.view_layer.objects.active = ob
    bpy.ops.object.mode_set(mode="EDIT")
    definitions = [
        ("Hips", None, (0, 0, .95), (0, 0, 1.1)),
        ("Spine", "Hips", (0, 0, 1.1), (0, 0, 1.3)),
        ("Chest", "Spine", (0, 0, 1.3), (0, 0, 1.5)),
        ("Neck", "Chest", (0, 0, 1.5), (0, 0, 1.65)),
        ("Head", "Neck", (0, 0, 1.65), (0, 0, 1.85)),
    ]
    for side, sign in (("Left", 1), ("Right", -1)):
        definitions += [
            (side + "UpperArm", "Chest", (.2 * sign, 0, 1.5), (.55 * sign, 0, 1.5)),
            (side + "LowerArm", side + "UpperArm", (.55 * sign, 0, 1.5), (.85 * sign, 0, 1.5)),
            (side + "Hand", side + "LowerArm", (.85 * sign, 0, 1.5), (1 * sign, 0, 1.5)),
            (side + "UpperLeg", "Hips", (.12 * sign, 0, .95), (.12 * sign, 0, .53)),
            (side + "LowerLeg", side + "UpperLeg", (.12 * sign, 0, .53), (.12 * sign, 0, .1)),
            (side + "Foot", side + "LowerLeg", (.12 * sign, 0, .1), (.12 * sign, -.2, .1)),
        ]
    for name, parent, head, tail in definitions:
        bone = arm.edit_bones.new(name)
        bone.head, bone.tail = head, tail
        if parent:
            bone.parent = arm.edit_bones[parent]
    bpy.ops.object.mode_set(mode="OBJECT")
mesh = bpy.data.meshes.new("Skin")
mesh.from_pydata([(-.1, 0, 1), (.1, 0, 1), (0, .1, 1.3), (.2, 0, 1.7), (.5, .3, 1.8)], [],
                 [(0, 1, 2), (1, 2, 3), (2, 3, 4)])
skin = bpy.data.objects.new("Skin", mesh)
bpy.context.scene.collection.objects.link(skin)
skin.parent = ob
mod = skin.modifiers.new("Armature", "ARMATURE")
mod.object = ob
for name, ids in (("Hips", [0, 1]), ("Spine", [2, 3]), ("LeftHand" if kind == "humanoid" else "Hand", [4])):
    skin.vertex_groups.new(name=name).add(ids, 1.0, "REPLACE")

animated = kind in ("offset_take", "humanoid")
if animated:
    bpy.context.scene.render.fps = 24
    pb = ob.pose.bones["Spine"]
    pb.rotation_mode = "QUATERNION"
    frame_angle = ((48, 0), (78, 1.2)) if kind == "offset_take" else ((0, 0), (30, .5))
    for f, angle in frame_angle:
        pb.rotation_quaternion = Quaternion((1, 0, 0), angle)
        pb.keyframe_insert("rotation_quaternion", frame=f)
    if kind == "humanoid":
        for frame, x in ((0, 0), (30, .3)):
            ob.pose.bones["Hips"].location.x = x
            ob.pose.bones["Hips"].keyframe_insert("location", frame=frame)
    ob.animation_data.action.name = "TakeOffset" if kind == "offset_take" else "HumanMotion"
    for layer in ob.animation_data.action.layers:
        for strip in layer.strips:
            for bag in strip.channelbags:
                for fc in bag.fcurves:
                    for key in fc.keyframe_points:
                        key.interpolation = "LINEAR"
    bpy.context.scene.frame_start, bpy.context.scene.frame_end = (48, 78) if kind == "offset_take" else (0, 30)

for obj in bpy.context.scene.objects:
    obj.select_set(obj in (ob, skin))
bpy.ops.export_scene.fbx(filepath=out, use_selection=True, object_types={"ARMATURE", "MESH"},
                         add_leaf_bones=False, bake_anim=animated, bake_anim_use_all_actions=False,
                         bake_anim_use_nla_strips=False, bake_anim_simplify_factor=0.0,
                         axis_up="Z" if kind == "zup_cm" else "Y",
                         axis_forward="Y" if kind == "zup_cm" else "-Z",
                         apply_unit_scale=True, apply_scale_options="FBX_SCALE_UNITS")
