"""Synthetic FBX whose Model default pose deliberately differs from BindPose."""
import sys

import bpy
from io_scene_fbx import encode_bin, parse_fbx


def child(element, identifier):
    return next(item for item in element.elems if item.id == identifier)


def encode(element):
    cloned = encode_bin.FBXElem(element.id)
    methods = {ord(code): "add_" + method for code,method in {
        "B":"bool", "C":"char", "Z":"int8", "Y":"int16", "I":"int32", "L":"int64",
        "F":"float32", "D":"float64", "R":"bytes", "S":"string", "i":"int32_array",
        "l":"int64_array", "f":"float32_array", "d":"float64_array", "b":"bool_array", "c":"byte_array"}.items()}
    for kind,value in zip(element.props_type,element.props):
        getattr(cloned,methods[kind])(value)
    cloned.elems = [encode(item) for item in element.elems]
    return cloned


output = sys.argv[sys.argv.index("--") + 1]
mode = sys.argv[sys.argv.index("--") + 2] if len(sys.argv) > sys.argv.index("--") + 2 else "default"
bpy.ops.wm.read_factory_settings(use_empty=True)
data = bpy.data.armatures.new("RestRig")
armature = bpy.data.objects.new("Armature", data)
bpy.context.scene.collection.objects.link(armature)
bpy.context.view_layer.objects.active = armature
bpy.ops.object.mode_set(mode="EDIT")
parent = None
for index, name in enumerate(("Hips", "Spine", "Hand")):
    bone = data.edit_bones.new(name)
    bone.head, bone.tail = (0,index,0), (0,index+1,0)
    bone.parent = parent
    parent = bone
bpy.ops.object.mode_set(mode="OBJECT")
mesh_data = bpy.data.meshes.new("Triangle")
mesh_data.from_pydata([(-.1,.9,0),(.1,.9,0),(0,1.1,0)], [], [(0,1,2)])
mesh = bpy.data.objects.new("BoundTriangle", mesh_data)
bpy.context.scene.collection.objects.link(mesh)
mesh.vertex_groups.new(name="Spine").add([0,1,2], 1.0, "REPLACE")
mesh.modifiers.new("Skin", "ARMATURE").object = armature
mesh.parent = armature
armature.select_set(True)
mesh.select_set(True)
bpy.ops.export_scene.fbx(filepath=output, use_selection=True, add_leaf_bones=False,
                        bake_anim=False, object_types={"ARMATURE","MESH"})
parsed, version = parse_fbx.parse(output)
root = parsed
spine = next(element for element in child(root,b"Objects").elems
             if element.id == b"Model" and element.props[1].split(b"\x00\x01")[0] == b"Spine")
properties = child(spine,b"Properties70")
translation = next(element for element in properties.elems if element.props[0] == b"Lcl Translation")
# Child transforms have no scene-axis rotation. Units are FBX centimetres.
# Keep the original skin BindPose at (0,100,0), but make the Model default
# local translation (25,100,0), just as a DCC can store independent poses.
translation.props[4:7] = [25.0,100.0,0.0]
def set_property(name,kind,values,codes):
    entry = next((element for element in properties.elems if element.props[0] == name),None)
    data = [name,kind,b"",b"" if kind in (b"enum",b"bool") else b"A",*values]
    if entry is None:
        properties.elems.append(parse_fbx.FBXElem(b"P",data,b"SSSS"+codes,[]))
    else:
        index = properties.elems.index(entry)
        properties.elems[index] = parse_fbx.FBXElem(b"P",data,b"SSSS"+codes,[])

if mode == "default_scale":
    scaling = next((element for element in properties.elems if element.props[0] == b"Lcl Scaling"),None)
    if scaling is None:
        scaling = parse_fbx.FBXElem(b"P",[b"Lcl Scaling",b"Lcl Scaling",b"",b"A",2.0,1.0,1.0],b"SSSSDDD",[])
        properties.elems.append(scaling)
    else:
        scaling.props[4:7] = [2.0,1.0,1.0]
elif mode in ("bind_scale","bind_shear"):
    for pose in child(root,b"Objects").elems:
        if pose.id != b"Pose":
            continue
        for node in pose.elems:
            if node.id == b"PoseNode" and child(node,b"Node").props[0] == spine.props[0]:
                matrix = child(node,b"Matrix").props[0]
                matrix[0 if mode == "bind_scale" else 4] += 1.0 if mode == "bind_scale" else .25
elif mode == "cluster_scale":
    cluster_uid = next(connection.props[2] for connection in child(root,b"Connections").elems
                       if connection.props[:2] == [b"OO",spine.props[0]]
                       and any(element.id == b"Deformer" and element.props[0] == connection.props[2]
                               and element.props[2] == b"Cluster" for element in child(root,b"Objects").elems))
    cluster = next(element for element in child(root,b"Objects").elems if element.props[0] == cluster_uid)
    child(cluster,b"TransformLink").props[0][0] += 1.0
elif mode == "pivot":
    for name,values in ((b"Lcl Rotation",[0.0,0.0,30.0]),(b"PreRotation",[0.0,20.0,0.0]),
                        (b"PostRotation",[10.0,0.0,0.0]),(b"RotationPivot",[10.0,0.0,0.0]),
                        (b"RotationOffset",[1.0,2.0,3.0])):
        set_property(name,b"Vector3D",values,b"DDD")
    set_property(b"RotationActive",b"bool",[1],b"I")
    set_property(b"RotationOrder",b"enum",[0],b"I")
elif mode == "duplicate_mesh_name":
    mesh_model = next(element for element in child(root,b"Objects").elems
                      if element.id == b"Model" and element.props[2] == b"Mesh")
    mesh_model.props[1] = b"Spine\x00\x01Model"
    child(root,b"Objects").elems.remove(mesh_model)
    child(root,b"Objects").elems.insert(0,mesh_model)
elif mode == "ambiguous_bone":
    import copy
    duplicate = copy.deepcopy(spine)
    duplicate.props[0] = max(element.props[0] for element in child(root,b"Objects").elems
                             if element.props and isinstance(element.props[0],int)) + 1000
    child(root,b"Objects").elems.append(duplicate)
    connections = child(root,b"Connections")
    original_parent = next(connection.props[2] for connection in connections.elems
                           if connection.props[:2] == [b"OO",spine.props[0]]
                           and any(model.id == b"Model" and model.props[0] == connection.props[2]
                                   for model in child(root,b"Objects").elems))
    connections.elems.append(parse_fbx.FBXElem(b"C",[b"OO",duplicate.props[0],original_parent],b"SLL",[]))
encode_bin.write(output, encode(root), version)
