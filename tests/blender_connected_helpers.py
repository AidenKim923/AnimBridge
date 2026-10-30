"""Export a connected-joint rig with child translation, then dump safely."""
import json
import sys

import bpy

args = sys.argv[sys.argv.index("--")+1:]
bpy.ops.wm.read_factory_settings(use_empty=True)
if args[0] == "make":
    data = bpy.data.armatures.new("ConnectedRig")
    arm = bpy.data.objects.new("Armature",data)
    bpy.context.scene.collection.objects.link(arm)
    bpy.context.view_layer.objects.active = arm
    bpy.ops.object.mode_set(mode="EDIT")
    parent = data.edit_bones.new("Hips")
    parent.head,parent.tail = (0,0,0),(0,1,0)
    child = data.edit_bones.new("Spine")
    child.head,child.tail,child.parent = (0,1,0),(0,2,0),parent
    # Leave the Blender source editable; FBX import reconnects the coincident
    # endpoints. The FBX animation itself carries a valid child translation.
    child.use_connect = False
    bpy.ops.object.mode_set(mode="OBJECT")
    arm.pose.bones["Spine"].location.x = 0
    arm.pose.bones["Spine"].keyframe_insert("location",frame=0)
    arm.pose.bones["Spine"].location.x = .6
    arm.pose.bones["Spine"].keyframe_insert("location",frame=30)
    arm.animation_data.action.name = "TranslateChild"
    bpy.context.scene.frame_start,bpy.context.scene.frame_end = 0,30
    bpy.context.scene.frame_set(0)
    arm.select_set(True)
    bpy.ops.export_scene.fbx(filepath=args[1],use_selection=True,object_types={"ARMATURE"},
        add_leaf_bones=False,bake_anim=True,bake_anim_use_all_actions=False,
        bake_anim_use_nla_strips=False,bake_anim_simplify_factor=0.0)
else:
    bpy.ops.import_scene.fbx(filepath=args[1],automatic_bone_orientation=False,anim_offset=0.0)
    arm = next(obj for obj in bpy.context.scene.objects if obj.type=="ARMATURE")
    was_connected = [bone.name for bone in arm.data.bones if bone.use_connect]
    bpy.context.view_layer.objects.active = arm
    bpy.ops.object.mode_set(mode="EDIT")
    for bone in arm.data.edit_bones:
        bone.use_connect = False
    bpy.ops.object.mode_set(mode="OBJECT")
    action = arm.animation_data.action
    samples = []
    for frame in range(round(action.frame_range[0]),round(action.frame_range[1])+1):
        bpy.context.scene.frame_set(frame)
        parent = arm.pose.bones["Hips"].matrix
        local = parent.inverted() @ arm.pose.bones["Spine"].matrix
        samples.append([list(row)for row in local])
    with open(args[2],"w") as output:
        json.dump({"connected_after_import":was_connected,"frames":samples},output)
