"""Create a real FBX take shared by armature, mesh, and helper actions."""
import json
import sys
from pathlib import Path

import bpy

target = Path(sys.argv[sys.argv.index('--') + 1])
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.object.armature_add()
arm = bpy.context.object
arm.name = 'Character'
bpy.ops.object.mode_set(mode='EDIT')
child = arm.data.edit_bones.new('Tip')
child.head, child.tail = (0, 0, 1), (0, 0, 2)
child.parent = arm.data.edit_bones['Bone']
bpy.ops.object.mode_set(mode='OBJECT')
bpy.ops.mesh.primitive_cube_add(size=.2, location=(0, 0, 1))
mesh = bpy.context.object
mesh.name = 'Body'
group = mesh.vertex_groups.new(name='Bone')
group.add(list(range(len(mesh.data.vertices))), 1, 'REPLACE')
mesh.modifiers.new('Skin', 'ARMATURE').object = arm
bpy.ops.object.empty_add()
helper = bpy.context.object
helper.name = 'Footsteps'
bone = arm.pose.bones['Bone']
bone.rotation_mode = 'XYZ'
for frame, value in ((0, 0), (30, .6)):
    bone.rotation_euler.x = value
    bone.keyframe_insert('rotation_euler', frame=frame)
    mesh.location.x = value
    mesh.keyframe_insert('location', frame=frame)
    helper.location.x = value * 2
    helper.keyframe_insert('location', frame=frame)
scene = bpy.context.scene
scene.render.fps = 30
scene.frame_start, scene.frame_end = 0, 30
scene.frame_set(0)
bpy.ops.export_scene.fbx(filepath=str(target), object_types={'ARMATURE', 'MESH', 'EMPTY'},
                         add_leaf_bones=False, bake_anim=True,
                         bake_anim_use_all_actions=False, bake_anim_use_nla_strips=False,
                         bake_anim_simplify_factor=0)
from io_scene_fbx import parse_fbx
root, _ = parse_fbx.parse(str(target))
take = next(node.props[1].split(b'\x00')[0].decode('utf-8')
            for block in root.elems if block.id == b'Objects'
            for node in block.elems if node.id == b'AnimationStack')
target.with_suffix('.take.json').write_text(json.dumps({'take': take}), encoding='utf-8')
