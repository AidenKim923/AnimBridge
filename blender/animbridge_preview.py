"""Read-only FBX inspection / real evaluated joint preview, run inside Blender."""
import json
import math
from pathlib import Path
import sys
sys.dont_write_bytecode = True
import traceback

import bpy


def facts(path):
    from io_scene_fbx import parse_fbx
    root, _version = parse_fbx.parse(str(path))
    props, models, parents = {}, {}, {}
    for block in root.elems:
        if block.id == b"GlobalSettings":
            for child in block.elems:
                if child.id == b"Properties70":
                    for prop in child.elems:
                        if prop.id == b"P" and len(prop.props) >= 5:
                            value = prop.props[4]
                            props[prop.props[0].decode("utf-8", "replace")] = value.decode("utf-8", "replace") if isinstance(value, bytes) else value
        if block.id == b"Objects":
            for model in block.elems:
                if model.id == b"Model":
                    name = model.props[1].split(b"\x00")[0].decode("utf-8", "replace")
                    kind = model.props[2].decode("utf-8", "replace")
                    models[int(model.props[0])] = {"name": name, "kind": kind}
        if block.id == b"Connections":
            for link in block.elems:
                if link.id == b"C" and link.props[0] == b"OO":
                    parents[int(link.props[1])] = int(link.props[2])
    bones = [m["name"] for m in models.values() if m["kind"] in ("LimbNode", "Root")]
    duplicates = sorted({name for name in bones if bones.count(name) > 1})
    return {"global_settings": props, "duplicate_bones": duplicates}


def point(value):
    # Blender evaluates imported FBX in metric, Z-up right-handed world space.
    # Display convention is metric Y-up right-handed, including real root motion.
    return [float(value.x), float(value.z), -float(value.y)]


def action_curves(arm, action):
    if not action:
        return []
    if hasattr(action, "layers"):
        return [fc for layer in action.layers for strip in layer.strips
                for bag in strip.channelbags for fc in bag.fcurves]
    return list(action.fcurves)


def action_for_object(obj, actions, take):
    """FBX imports one action per object and animation layer, not per take."""
    matches = [action for action in actions if action.name == take
               or action.name.endswith("|" + take)
               or ("|" + take + "|") in action.name]
    owned = [action for action in matches if action.name.startswith(obj.name + "|")]
    if not owned and obj.type == "ARMATURE":
        owned = [action for action in matches
                 if any(curve.data_path.startswith('pose.bones[')
                        for curve in action_curves(obj, action))]
    if not owned and len(matches) == 1 and obj.animation_data and obj.animation_data.action == matches[0]:
        owned = matches
    if len(owned) > 1 or (obj.type == "ARMATURE" and len(owned) != 1):
        raise ValueError(f"Clip {take!r} does not identify exactly one action for {obj.name!r}; "
                         f"available: {[a.name for a in actions]}")
    return owned[0] if owned else None


def assign_action(obj, action):
    animation = obj.animation_data_create()
    animation.action = action
    animation.use_nla = False
    slots = [slot for slot in getattr(action, "slots", []) if slot.target_id_type == "OBJECT"]
    named = [slot for slot in slots if slot.name_display == obj.name]
    if len(named) == 1:
        animation.action_slot = named[0]
    elif len(slots) == 1:
        animation.action_slot = slots[0]
    elif slots:
        raise ValueError(f"Ambiguous animation slots for {obj.name!r} in {action.name!r}")


def run(job):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    path = Path(job["source"])
    info = facts(path)
    bpy.ops.import_scene.fbx(filepath=str(path), automatic_bone_orientation=False,
                            use_prepost_rot=True, bake_space_transform=False,
                            ignore_leaf_bones=False, global_scale=1.0, anim_offset=0.0)
    arms = [obj for obj in bpy.context.scene.objects if obj.type == "ARMATURE"]
    if len(arms) != 1:
        raise ValueError(f"Expected one armature, found {len(arms)}")
    arm = arms[0]
    names = [bone.name for bone in arm.data.bones]
    if len(names) > 4096:
        raise ValueError("Skeleton exceeds the 4096-joint desktop preview limit")
    indices = {name: index for index, name in enumerate(names)}
    bones = []
    for bone in arm.data.bones:
        chain, current = [bone.name], bone.parent
        while current:
            chain.append(current.name)
            current = current.parent
        matrix = bone.matrix_local
        bones.append({"name": bone.name, "parent": indices.get(bone.parent.name, -1) if bone.parent else -1,
                      "path": arm.name + "/" + "/".join(reversed(chain)),
                      "rest_matrix": [float(value) for row in matrix for value in row],
                      "rest_position": point(arm.matrix_world @ bone.head_local)})
    actions = list(bpy.data.actions)
    selected = job.get("clip")
    if selected:
        action = action_for_object(arm, actions, selected)
        # A mesh or helper can have its own action for the same take. Evaluate
        # those together, while rejecting genuine ambiguity for each object.
        for obj in bpy.context.scene.objects:
            selected_action = action if obj == arm else action_for_object(obj, actions, selected)
            if selected_action:
                assign_action(obj, selected_action)
    else:
        action = arm.animation_data.action if arm.animation_data else None
    scene = bpy.context.scene
    fps = scene.render.fps / scene.render.fps_base
    curve_fps = scene.render.fps
    clips = [{"name": action.name, "first_frame": float(action.frame_range[0] / curve_fps * fps),
              "last_frame": float(action.frame_range[1] / curve_fps * fps),
              "duration": float((action.frame_range[1] - action.frame_range[0]) / curve_fps)} for action in actions]
    base = {"schema_version": 1, "status": "ok", "source": str(path), "bones": bones,
            "clips": clips, "fps": float(fps), "facts": info,
            "type": "fbx", "warnings": [], "armature": arm.name}
    if info["duplicate_bones"]:
        base["warnings"].append("FBX contains duplicate original bone names")
    if job.get("operation") == "inspect":
        return base
    if action:
        lo, hi = [float(value) for value in action.frame_range]
    else:
        lo = hi = 0.0
        base["warnings"].append("No animation take: showing the source rest pose")
    if "first_frame" in job:
        lo = float(job["first_frame"]) / fps * curve_fps
        hi = float(job["last_frame"]) / fps * curve_fps
        if hi < lo:
            raise ValueError("Metadata clip ends before it starts")
    source_fps = fps
    output_fps = float(job.get("fps") or fps)
    if not math.isfinite(output_fps) or output_fps <= 0 or output_fps > 240:
        raise ValueError("Preview fps must be between 0 and 240")
    duration = (hi - lo) / curve_fps
    count = max(1, round(duration * output_fps) + 1)
    max_frames = min(10000, max(1, int(job.get("max_frames", 2000))))
    if count > max_frames:
        output_fps = (max_frames - 1) / duration if max_frames > 1 and duration > 0 else output_fps
        count = max_frames
        base["warnings"].append(f"Preview sampling reduced to {count} frames; source duration preserved")
    all_frames, mesh_frames, triangles = [], [], []
    meshes = [obj for obj in bpy.context.scene.objects if obj.type == "MESH"] if job.get("include_mesh") else []
    if sum(len(obj.data.vertices) for obj in meshes) > 5000 or count > 301:
        meshes = []
        base["warnings"].append("Mesh preview omitted above 5000 vertices or 301 frames; evaluated bones remain available")
    point_budget = min(150000, max(1, int(job.get("max_point_frames", 150000))))
    points_per_frame = len(names) + sum(len(obj.data.vertices) for obj in meshes)
    budget_count = max(2 if duration > 0 else 1, point_budget // max(points_per_frame, 1))
    if count > budget_count:
        count = budget_count
        output_fps = (count - 1) / duration if duration > 0 else output_fps
        base["warnings"].append(f"Preview reduced to {count} samples for the 150000 joint/vertex-frame payload budget; duration preserved")
    for index in range(count):
        seconds = duration if index == count - 1 and count > 1 else min(index / output_fps, duration) if count > 1 else 0
        frame = lo + seconds * curve_fps
        scene.frame_set(math.floor(frame), subframe=frame - math.floor(frame))
        bpy.context.view_layer.update()
        all_frames.append([point(arm.matrix_world @ arm.pose.bones[name].head) for name in names])
        if meshes:
            values, offset = [], 0
            graph = bpy.context.evaluated_depsgraph_get()
            for obj in meshes:
                evaluated = obj.evaluated_get(graph)
                mesh = evaluated.to_mesh()
                mesh.calc_loop_triangles()
                values.extend(point(evaluated.matrix_world @ vertex.co) for vertex in mesh.vertices)
                if index == 0:
                    triangles.extend([[int(v) + offset for v in tri.vertices] for tri in mesh.loop_triangles])
                offset += len(mesh.vertices)
                evaluated.to_mesh_clear()
            mesh_frames.append(values)
    flat = [value for frame in all_frames for value in frame]
    base.update(status="ready", kind="bone-motion", coordinate_system="right-handed-y-up-meters",
                clip=action.name if action else None, source_fps=source_fps, fps=output_fps,
                duration=duration, frame_count=count, frames=all_frames,
                bounds={"min": [min(p[c] for p in flat) for c in range(3)],
                        "max": [max(p[c] for p in flat) for c in range(3)]} if flat else None)
    if mesh_frames:
        base["mesh"] = {"triangles": triangles, "frames": mesh_frames, "kind": "evaluated-skinned-mesh"}
    return base


def main():
    args = sys.argv[sys.argv.index("--") + 1:]
    job = json.loads(Path(args[0]).read_text(encoding="utf-8-sig"))
    try:
        result = run(job)
    except Exception as error:
        result = {"schema_version": 1, "status": "error", "reason": str(error), "trace": traceback.format_exc()}
    Path(args[1]).write_text(json.dumps(result, ensure_ascii=False, allow_nan=False), encoding="utf-8")


main()
