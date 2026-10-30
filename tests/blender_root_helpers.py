"""Make/dump a real top-level armature node, separate from deform bones."""
import json
import sys

import bpy

args = sys.argv[sys.argv.index("--")+1:]
bpy.ops.wm.read_factory_settings(use_empty=True)
if args[0] == "make":
    data = bpy.data.armatures.new("RootRig")
    arm = bpy.data.objects.new("Bip001",data)
    bpy.context.scene.collection.objects.link(arm)
    bpy.context.view_layer.objects.active = arm
    bpy.ops.object.mode_set(mode="EDIT")
    bone = data.edit_bones.new("Pelvis")
    bone.head, bone.tail = (0,0,0),(0,1,0)
    bpy.ops.object.mode_set(mode="OBJECT")
    arm.select_set(True)
    bpy.ops.export_scene.fbx(filepath=args[1],use_selection=True,object_types={"ARMATURE"},
                            add_leaf_bones=False,bake_anim=False)
else:
    bpy.ops.import_scene.fbx(filepath=args[1],anim_offset=0.0,automatic_bone_orientation=False)
    arm = next(obj for obj in bpy.context.scene.objects if obj.type=="ARMATURE")
    action = arm.animation_data.action
    samples = []
    for frame in range(int(action.frame_range[0]),int(action.frame_range[1])+1):
        bpy.context.scene.frame_set(frame)
        samples.append([list(row)for row in arm.matrix_world])
    with open(args[2],"w") as output:
        json.dump({"object":arm.name,"frames":samples,"bones":[bone.name for bone in arm.data.bones]},output)
