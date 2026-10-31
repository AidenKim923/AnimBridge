"""Apply Unity animation samples to an FBX skeleton and export FBX inside Blender.

Usage: blender -b --factory-startup --python uab_blender.py -- job.json
"""
import json
import math
import sys
# Resources can be installed read-only; Blender embeds its own Python interpreter.
sys.dont_write_bytecode = True
import traceback
from pathlib import Path

import bpy
import numpy as np

JOB = json.loads(Path(sys.argv[sys.argv.index("--") + 1]).read_text(encoding="utf-8"))
sys.path.insert(0, JOB["src_path"])
from unityanimbridge import coords  # noqa: E402

IMPORT_OPTS = dict(
    automatic_bone_orientation=False,
    use_prepost_rot=True,
    bake_space_transform=False,
    ignore_leaf_bones=False,
    global_scale=1.0,
    use_manual_orientation=False,
    primary_bone_axis="Y",
    secondary_bone_axis="X",
    anim_offset=0.0,
    use_custom_props=False,
)
EXPORT_OPTS = dict(
    use_selection=True,
    object_types={"ARMATURE"},
    add_leaf_bones=False,
    armature_nodetype="NULL",
    bake_anim=True,
    bake_anim_use_all_actions=False,
    bake_anim_use_nla_strips=False,
    bake_anim_use_all_bones=True,
    bake_anim_force_startend_keying=True,
    bake_anim_step=1.0,
    bake_anim_simplify_factor=0.0,
    global_scale=1.0,
    apply_unit_scale=True,
    apply_scale_options="FBX_SCALE_NONE",
    axis_forward="-Z",
    axis_up="Y",
    primary_bone_axis="Y",
    secondary_bone_axis="X",
    use_custom_props=False,
    path_mode="AUTO",
)
LINEAR = 1  # FCurve keyframe interpolation enum value


class JobError(Exception):
    pass


# FBX metadata

def read_fbx_facts(path):
    """Read GlobalSettings and the Model hierarchy with Blender's FBX parser."""
    from io_scene_fbx import import_fbx as fbx_reader, parse_fbx, fbx_utils
    from mathutils import Matrix

    root, _version = parse_fbx.parse(path)

    def child(elem, name):
        return next((e for e in elem.elems if e.id == name), None)

    props = {}
    gs = child(root, b"GlobalSettings")
    p70 = child(gs, b"Properties70") if gs is not None else None
    for p in (p70.elems if p70 is not None else []):
        if p.id == b"P" and len(p.props) >= 5:
            props[p.props[0].decode("utf-8", "replace")] = p.props[4]

    models = {}
    stacks = []
    # A Model's default local transform and its skin BindPose are different
    # FBX data. Blender builds edit bones from BindPose, while Unity's model
    # GameObject hierarchy (the helper's `rest`) keeps the default transform.
    # Use the pinned importer's FBX pivot/pre/post-rotation evaluator so this
    # reference is independent of both Unity samples and Blender's bind bones.
    template_props = None
    definitions = child(root, b"Definitions")
    for object_type in (definitions.elems if definitions is not None else []):
        if object_type.id != b"ObjectType" or not object_type.props or object_type.props[0] != b"Model":
            continue
        for template in object_type.elems:
            if template.id == b"PropertyTemplate" and template.props and template.props[0] in (b"FbxNode", b"KFbxNode"):
                template_props = child(template, b"Properties70")
    objs = child(root, b"Objects")
    bind_world = {}
    cluster_world = {}
    for e in (objs.elems if objs is not None else []):
        if e.id == b"Model":
            name = e.props[1].split(b"\x00\x01")[0].decode("utf-8", "replace")
            transform_data = fbx_reader.blen_read_object_transform_preprocess(
                (child(e, b"Properties70"), template_props), e, Matrix(), True)
            _with_geometry, default_local, _geometry = fbx_reader.blen_read_object_transform_do(transform_data)
            models[e.props[0]] = {"name": name, "type": e.props[2].decode("utf-8", "replace"), "parent": 0,
                                  "default_local": m4(default_local)}
        elif e.id == b"AnimationStack":
            stacks.append(e.props[1].split(b"\x00\x01")[0].decode("utf-8", "replace"))
        elif e.id == b"Deformer" and len(e.props) > 2 and e.props[2] == b"Cluster":
            link = child(e,b"TransformLink")
            if link is not None:
                cluster_world[e.props[0]] = m4(fbx_utils.array_to_matrix4(link.props[0]))
        elif e.id == b"Pose" and len(e.props) > 2 and e.props[2] == b"BindPose":
            for node in e.elems:
                if node.id != b"PoseNode":
                    continue
                uid, values = child(node,b"Node"), child(node,b"Matrix")
                if uid is None or values is None:
                    continue
                matrix = m4(fbx_utils.array_to_matrix4(values.props[0]))
                uid = uid.props[0]
                if uid in bind_world and not np.allclose(bind_world[uid],matrix,atol=1e-5,rtol=1e-6):
                    raise JobError("source has conflicting skin BindPose matrices for one node")
                bind_world[uid] = matrix
    conns = child(root, b"Connections")
    connected_parents = {}
    for c in (conns.elems if conns is not None else []):
        if c.id == b"C" and c.props[0] == b"OO" and c.props[1] in models and c.props[2] in cluster_world:
            uid, matrix = c.props[1], cluster_world[c.props[2]]
            if uid in bind_world and not np.allclose(bind_world[uid],matrix,atol=1e-5,rtol=1e-6):
                raise JobError("source has conflicting Pose/Cluster skin bind matrices for one node")
            bind_world[uid] = matrix
        if (c.id == b"C" and c.props[0] == b"OO" and c.props[1] in models
                and (c.props[2] == 0 or c.props[2] in models)):
            uid, parent_uid = c.props[1:3]
            if uid in connected_parents and connected_parents[uid] != parent_uid:
                raise JobError("source FBX Model has multiple parent Models")
            connected_parents[uid] = parent_uid
            models[uid]["parent"] = parent_uid

    def unity_path(uid):
        parts = []
        seen = set()
        while uid in models:
            if uid in seen:
                raise JobError("source FBX Model hierarchy contains a cycle")
            seen.add(uid)
            parts.append(models[uid]["name"])
            uid = models[uid]["parent"]
        return "/".join(reversed(parts))

    by_name = {}
    bind_parent_cache = {}
    def bind_parent_world(uid):
        if uid in bind_world:
            return bind_world[uid]
        if uid not in models:
            return np.eye(4)
        if uid not in bind_parent_cache:
            node = models[uid]
            bind_parent_cache[uid] = bind_parent_world(node["parent"]) @ node["default_local"]
        return bind_parent_cache[uid]
    for uid, m in models.items():
        m["path"] = unity_path(uid)
        m["top_level"] = m["parent"] == 0 or m["parent"] not in models
        parent_bind = bind_parent_world(m["parent"])
        m["bind_local"] = (np.linalg.inv(parent_bind) @ bind_world[uid]
                           if uid in bind_world else None)
        by_name.setdefault(m["name"], []).append(m)
    creator = child(root, b"Creator")
    stack_names = stacks
    return {
        "creator": creator.props[0].decode("utf-8", "replace") if creator is not None and creator.props else "",
        "duplicate_stacks": len(set(stack_names)) != len(stack_names),
        "unit_scale_factor": float(props.get("UnitScaleFactor", 1.0)),
        "axes": tuple(int(props.get(k, d)) for k, d in (
            ("UpAxis", 1), ("UpAxisSign", 1), ("FrontAxis", 2), ("FrontAxisSign", 1),
            ("CoordAxis", 0), ("CoordAxisSign", 1))),
        "models_by_name": by_name,
        "stacks": stacks,
    }


def _null_rig_hint(facts):
    """Explain the common case where every joint is a plain Null model."""
    if not facts:
        return ""
    nodes = [node for group in facts["models_by_name"].values() for node in group]
    if any(node["type"] in ("LimbNode", "Limb", "Root") for node in nodes):
        return ""
    nulls = sum(node["type"] == "Null" for node in nodes)
    if not nulls:
        return ""
    return (f": its {nulls} joints are plain Null objects with no skinned mesh (an animation-only export). "
            "Use the character model FBX that contains the skinned mesh as the skeleton")


# Scene setup

def reset_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)


def import_fbx(path, use_anim):
    bpy.ops.import_scene.fbx(filepath=path, use_anim=use_anim, **IMPORT_OPTS)


def pick_armature(name, facts=None):
    arms = [o for o in bpy.context.scene.objects if o.type == "ARMATURE"]
    if name:
        arms = [o for o in arms if o.name == name]
    if not arms:
        raise JobError("no armature found in skeleton FBX" + (f" named {name!r}" if name else "") + _null_rig_hint(facts))
    if len(arms) > 1:
        raise JobError("more than one armature; pass --armature: " + ", ".join(o.name for o in arms))
    return arms[0]


def allow_local_bone_translations(arm):
    """FBX has parent links, but no Blender connected-head translation lock.

    The importer can connect coincident joints. That Blender edit convenience
    clamps a child's pose translation, including legitimate Unity Humanoid
    translations. Keep every rest transform and parent, and remove only that
    constraint before constructing or evaluating sampled bone bases.
    """
    if not any(bone.use_connect for bone in arm.data.bones):
        return
    bpy.context.view_layer.objects.active = arm
    arm.select_set(True)
    bpy.ops.object.mode_set(mode="EDIT")
    for bone in arm.data.edit_bones:
        bone.use_connect = False
    bpy.ops.object.mode_set(mode="OBJECT")


def m4(mat):
    return np.array([list(r) for r in mat], dtype=np.float64)


class Skeleton:
    """Rest data of the armature in FBX node-local terms."""

    def __init__(self, arm, facts, warnings):
        self.arm = arm
        self.arm_default_world = m4(arm.matrix_world)
        self.facts = facts
        by_name = facts["models_by_name"]
        real = arm.name in by_name and any(m["type"] not in ("LimbNode","Limb") for m in by_name[arm.name])
        self.armature_is_real = real
        self.retained_root = None
        if real and arm.name.lower() != "armature":
            matches = by_name[arm.name]
            if len(matches) == 1 and matches[0]["top_level"] and arm.parent is None:
                self.retained_root = matches[0]
        self.reference_validation = {}
        self.bones = {}
        lengths = []
        for b in arm.data.bones:
            rest = m4(b.matrix_local)
            rel = rest if b.parent is None else np.linalg.inv(m4(b.parent.matrix_local)) @ rest
            fbx = by_name.get(b.name)
            if fbx is None:
                fbx = self._match_truncated(b.name, by_name, warnings)
            fbx = self._match_node(b, fbx, by_name, real)
            self.bones[b.name] = {
                "bone": b,
                "rel_blender": rel,
                "fbx": fbx,
                "length": b.length,
            }
            lengths.append(b.length)
            self._check_no_scale(b.name, rest)
            self._check_no_scale(b.name + " FBX Model default",fbx["default_local"])
            if fbx["bind_local"] is not None:
                self._check_no_scale(b.name + " FBX skin bind",fbx["bind_local"])
        self.u = self._units_per_fbx_unit(warnings)
        # FBX-unit node-local rest matrices
        for info in self.bones.values():
            m = info["rel_blender"].copy()
            m[:3, 3] /= self.u
            info["rest_fbx"] = m
            # Preserve bind-relative data exclusively for pose basis conversion.
            # Validate/fill Unity default locals using the independent FBX Model.
            info["default_fbx"] = info["fbx"]["default_local"]
        self.path_index = {}
        for name, info in self.bones.items():
            if info["fbx"] is not None:
                self.path_index[info["fbx"]["path"]] = name

    def _match_node(self, bone, candidates, by_name, real):
        if not candidates:
            raise JobError(f"bone {bone.name!r} has no independent FBX Model default node")
        limbs = [node for node in candidates if node["type"] in ("LimbNode","Limb")]
        candidates = limbs or candidates
        chain = []
        parent = bone
        while parent is not None:
            chain.append(parent.name)
            parent = parent.parent
        chain.reverse()
        if real:
            armature = [node for node in by_name.get(self.arm.name,[]) if node["type"] not in ("LimbNode","Limb")]
            if len(armature) != 1:
                raise JobError(f"armature {self.arm.name!r} has no unique independent FBX Model path")
            chain = armature[0]["path"].split("/") + chain
        def matches(node):
            source = node["path"].split("/")
            return len(source) == len(chain) and all(
                imported == original or len(imported.encode("utf-8")) >= 63 and original.startswith(imported)
                for imported,original in zip(chain,source))
        exact = [node for node in candidates if matches(node)]
        if len(exact) != 1:
            raise JobError(f"bone {bone.name!r} has no unique FBX Model node matching its parent hierarchy")
        return exact[0]

    @staticmethod
    def _match_truncated(name, by_name, warnings):
        if len(name.encode("utf-8")) < 63:
            return None
        cands = [k for k in by_name if k.startswith(name)]
        if len(cands) == 1:
            warnings.append(f"bone {name!r} was truncated by Blender; matched FBX node {cands[0]!r}")
            return by_name[cands[0]]
        return None

    @staticmethod
    def _check_no_scale(name, rest):
        if rest.shape != (4,4) or not np.all(np.isfinite(rest)) or not np.allclose(rest[3],[0,0,0,1],atol=1e-8,rtol=0):
            raise JobError(f"bone {name!r} has an invalid affine reference matrix")
        r = rest[:3, :3]
        cols = np.linalg.norm(r, axis=0)
        if np.max(np.abs(cols - 1)) > 1e-4 or np.max(np.abs(r.T @ r / np.outer(cols, cols) - np.eye(3))) > 1e-4:
            raise JobError(f"bone {name!r} has non-unit scale or shear in its rest matrix (unsupported)")

    def _units_per_fbx_unit(self, warnings):
        """Blender armature units per FBX file unit.

        With bake_space_transform off, Blender keeps raw FBX units in armature
        space and applies unit/axis conversion to the object matrix.
        """
        return 1.0

    def top_level(self, bone_name):
        info = self.bones[bone_name]
        if info["bone"].parent is not None:
            return False
        if self.armature_is_real:
            return False
        return info["fbx"] is None or info["fbx"]["top_level"]

    def unity_path(self, bone_name):
        info = self.bones[bone_name]
        if info["fbx"] is not None:
            return info["fbx"]["path"]
        return bone_name


def map_paths(skel, paths, bone_map, warnings, dropped):
    """Map Unity paths to skeleton bone names."""
    leaf_index = {}
    for b in skel.bones:
        leaf_index.setdefault(b, []).append(b)
    result = {}
    for p in paths:
        if p == "":
            continue
        if bone_map and (p in bone_map or p.split("/")[-1] in bone_map):
            target = bone_map.get(p, bone_map.get(p.split("/")[-1]))
            if target in skel.bones:
                result[p] = target
                continue
            dropped.append({"path": p, "reason": f"bone_map target {target!r} not in skeleton"})
            continue
        if p in skel.path_index:
            result[p] = skel.path_index[p]
            continue
        leaf = p.split("/")[-1]
        cands = leaf_index.get(leaf, [])
        if len(cands) == 1:
            result[p] = cands[0]
            continue
        dropped.append({"path": p, "reason": "bone_not_found" if not cands else "ambiguous_bone_name"})
    return result


# Apply sampled animation

def to_array(v, n, width):
    if v is None:
        return None
    a = np.array([[np.nan if x is None else x for x in row] for row in v], dtype=np.float64)
    if a.shape != (n, width):
        raise JobError(f"track has shape {a.shape}, expected {(n, width)}")
    return a


def correction_for(skel, bone, k, C_inv, unity_rest_entry, warnings):
    """Build the analytic basis; use the Unity rest pose only to validate it."""
    analytic = C_inv if skel.top_level(bone) else None
    if unity_rest_entry is None:
        return analytic
    expect = analytic if analytic is not None else np.eye(4)
    scale_len = max(skel.bones[bone]["length"] / skel.u, 1.0)
    errors = []
    # Unity's initial hierarchy can use Model defaults or the skin bind pose
    # (notably for Humanoid imports). Both references come from the source FBX;
    # neither is a calibration fitted to an animation sample.
    references = [("model-default", skel.bones[bone]["default_fbx"])]
    if skel.bones[bone]["fbx"]["bind_local"] is not None:
        references.append(("skin-bind",skel.bones[bone]["fbx"]["bind_local"]))
    for label, rest in references:
        a = coords.rest_correction(rest, np.array(unity_rest_entry["t"], float),
            np.array(unity_rest_entry["r"], float), np.array(unity_rest_entry["s"], float), k)
        ang, trans, scl = coords.correction_deviation(np.linalg.inv(expect) @ a, scale_len)
        if ang <= 0.01 and trans <= 1e-4 and scl <= 1e-3:
            skel.reference_validation[bone] = label
            return analytic
        errors.append(f"{label}: rot {ang:.4f} deg, trans {trans:.3g}, scale {scl:.3g}")
    raise JobError(f"{bone!r}: Unity rest pose does not match the FBX default local transform or skin bind pose "
                   f"under the analytic conversion ({'; '.join(errors)}); unsupported import settings or wrong k={k}")


def unity_rest_trs(skel, bone, k, a, unity_rest_entry):
    if unity_rest_entry is not None:
        return (np.array(unity_rest_entry["t"], float), np.array(unity_rest_entry["r"], float),
                np.array(unity_rest_entry["s"], float))
    t, q, s = coords.fbx_to_unity(skel.bones[bone]["default_fbx"][None], k, a)
    return t[0], q[0], s[0]


def fill(arr, rest_vec, n):
    if arr is None:
        return np.tile(rest_vec, (n, 1))
    out = arr.copy()
    mask = np.isnan(out)
    if mask.any():
        out[mask] = np.broadcast_to(rest_vec, out.shape)[mask]
    return out


def track_differs_from_rest(track, rest, n):
    """Whether ignoring this local TRS would discard an authored transform."""
    rest = rest or {"t":[0,0,0],"r":[0,0,0,1],"s":[1,1,1]}
    t = fill(to_array(track.get("t"),n,3),np.asarray(rest["t"],float),n)
    s = fill(to_array(track.get("s"),n,3),np.asarray(rest["s"],float),n)
    q = to_array(track.get("r"),n,4)
    qr = np.asarray(rest["r"],float)
    q = np.tile(qr,(n,1)) if q is None else q
    norms = np.linalg.norm(q,axis=1)
    if not all(np.all(np.isfinite(value)) for value in (t,s,q,qr)):
        raise JobError("unmapped root track has a non-finite transform")
    if np.any(norms < 1e-12) or np.linalg.norm(qr) < 1e-12:
        raise JobError("unmapped root track has an invalid rotation")
    dot = np.clip(np.abs((q/norms[:,None]) @ (qr/np.linalg.norm(qr))),0,1)
    angle = np.degrees(2*np.arccos(dot))
    return (np.max(np.abs(t-np.asarray(rest["t"]))) > 1e-4
            or np.max(np.abs(s-np.asarray(rest["s"]))) > 1e-3 or np.max(angle) > .01)


def channelbag_fcurves(action):
    for layer in action.layers:
        for strip in layer.strips:
            for cb in strip.channelbags:
                yield from cb.fcurves


def write_action(arm, name, bone_values, n, object_values=None):
    """bone_values: bone -> (loc[N,3], quat[N,4] w-first, scale[N,3]) as matrix_basis components."""
    action = bpy.data.actions.new(name)
    ad = arm.animation_data_create()
    ad.action = action
    frames = np.arange(n, dtype=np.float64)
    channels = []
    for bone, values in bone_values.items():
        arm.pose.bones[bone].rotation_mode = "QUATERNION"
        channels.append((bone, f'pose.bones["{bpy.utils.escape_identifier(bone)}"].', values))
    if object_values is not None:
        arm.rotation_mode = "QUATERNION"
        channels.append((arm.name, "", object_values))
    for bone, base, (loc, quat, scl) in channels:
        for prop, data in (("location", loc), ("rotation_quaternion", quat), ("scale", scl)):
            for i in range(data.shape[1]):
                fc = action.fcurve_ensure_for_datablock(arm, f"{base}{prop}", index=i, group_name=bone)
                fc.keyframe_points.clear()
                fc.keyframe_points.add(n)
                co = np.empty(2 * n)
                co[0::2] = frames
                co[1::2] = data[:, i]
                fc.keyframe_points.foreach_set("co", co)
                fc.keyframe_points.foreach_set("interpolation", [LINEAR] * n)
                fc.update()
    return action


def set_scene_time(fps, n):
    scene = bpy.context.scene
    if abs(fps - round(fps)) < 1e-6:
        scene.render.fps, scene.render.fps_base = int(round(fps)), 1.0
    else:
        scene.render.fps = int(math.ceil(fps))
        scene.render.fps_base = math.ceil(fps) / fps
    scene.frame_start = 0
    scene.frame_end = max(0, n - 1)


def export(arm, out_path, action):
    for o in bpy.context.scene.objects:
        o.select_set(o == arm)
    bpy.context.view_layer.objects.active = arm
    arm.animation_data.action = action
    Path(out_path).parent.mkdir(parents=True, exist_ok=True)
    bpy.ops.export_scene.fbx(filepath=out_path, **EXPORT_OPTS)


def apply_armature_policy(arm, skel, facts):
    """Unreal drops a top-level Null named 'armature' only in Blender-made FBX files."""
    if not skel.armature_is_real:
        arm.name = "Armature"
        return
    if arm.name.lower() == "armature" and "blender" not in facts["creator"].lower():
        raise JobError(
            "source FBX has a real 'Armature' node and was not made by Blender; Unreal keeps it as a bone in the "
            "source Skeleton but would drop it from the Blender-made output (hierarchy mismatch)"
        )


def retained_root_values(skel, tracks, unity_rest, k, C_inv, n, root_to_bone):
    """Animate a real top-level armature node retained in the Unreal Skeleton.

    Source FBX units/axes -> Blender object space is the same analytic global
    matrix used by the pinned FBX importer. It is not fitted from Unity rest.
    """
    root = skel.retained_root
    if root is None:
        return None
    if root_to_bone not in (None, skel.arm.name):
        raise JobError(f"real Skeleton root {skel.arm.name!r} must receive model root motion; "
                       "--root-to-bone cannot target one of its children")
    from bpy_extras.io_utils import axis_conversion
    from io_scene_fbx import fbx_utils
    from mathutils import Matrix
    axes = skel.facts["axes"]
    axis_key = ((axes[0],axes[1]),(axes[2],axes[3]),(axes[4],axes[5]))
    mapping = {value:key for key,value in fbx_utils.RIGHT_HAND_AXES.items()}
    if axis_key not in mapping:
        raise JobError("source FBX axes are not supported by the Blender importer")
    up, forward = mapping[axis_key]
    scale = skel.facts["unit_scale_factor"] / fbx_utils.units_blender_to_fbx_factor(bpy.context.scene)
    global_matrix = m4(Matrix.Scale(scale,4) @ axis_conversion(from_forward=forward,from_up=up).to_4x4())
    expected_default = global_matrix @ root["default_local"]
    if not np.allclose(skel.arm_default_world, expected_default, atol=1e-4, rtol=1e-5):
        raise JobError("real FBX armature object does not match the analytic default transform")
    path = root["path"]
    entry = unity_rest.get(path)
    default = coords.fbx_to_unity(root["default_local"][None],k,C_inv)
    rt,rq,rs = (value[0] for value in default)
    if entry is not None:
        correction = coords.rest_correction(root["default_local"], np.asarray(entry["t"]),
            np.asarray(entry["r"]),np.asarray(entry["s"]),k)
        ang,trans,scl = coords.correction_deviation(np.linalg.inv(C_inv) @ correction,1.0)
        if ang > .01 or trans > 1e-4 or scl > 1e-3:
            raise JobError(f"real root {path!r} does not match the FBX Model default under the analytic conversion")
        skel.reference_validation[path] = "model-default-object"
        rt,rq,rs = (np.asarray(entry[key],dtype=float) for key in ("t","r","s"))
    tr = tracks.get(path,{})
    t = fill(to_array(tr.get("t"),n,3),rt,n)
    q = to_array(tr.get("r"),n,4)
    q = np.tile(rq,(n,1)) if q is None else q
    s = fill(to_array(tr.get("s"),n,3),rs,n)
    if "" in tracks:
        t,q,s = compose_root(tracks[""],unity_rest.get(""),t,q,s,n)
    desired = global_matrix @ coords.unity_to_fbx(t,q,s,k,C_inv)
    t,q,s = coords.mat_to_trs(desired)
    q = q[:,[3,0,1,2]]
    for index in range(1,n):
        if np.dot(q[index],q[index-1]) < 0:
            q[index] = -q[index]
    return t,q,s


def run_samples(job, result):
    reset_scene()
    import_fbx(job["skeleton_fbx"], use_anim=False)
    facts = read_fbx_facts(job["skeleton_fbx"])
    warnings = result["warnings"]
    arm = pick_armature(job.get("armature"), facts)
    allow_local_bone_translations(arm)
    skel = Skeleton(arm, facts, warnings)
    apply_armature_policy(arm, skel, facts)
    k = job["k"] if job.get("k") else (
        (facts["unit_scale_factor"] / 100.0 if job.get("use_file_scale", True) else 1.0) * job.get("global_scale", 1.0)
    )
    result["k"] = k
    result["armature_is_real"] = skel.armature_is_real
    C_inv = np.linalg.inv(coords.axis_conversion(*facts["axes"]))
    unity_rest = job.get("unity_rest") or {}
    bone_map = job.get("bone_map") or {}

    for clip in job["clips"]:
        cres = {"name": clip["name"], "out_fbx": clip["out_fbx"], "dropped": [], "warnings": [], "status": "ok"}
        result["clips"].append(cres)
        try:
            n = int(clip["frames"])
            tracks = clip["tracks"]
            object_path = skel.retained_root["path"] if skel.retained_root else None
            mapping = map_paths(skel, [path for path in tracks if path != object_path], bone_map, cres["warnings"], cres["dropped"])
            root_tr = tracks.get("")
            if root_tr is not None and not job.get("root_to_bone") and skel.retained_root is None:
                if track_differs_from_rest(root_tr,unity_rest.get(""),n):
                    raise JobError("model root transform differs from rest and cannot be discarded; "
                                   "use --root-to-bone to preserve root motion")
                cres["dropped"].append({"path": "", "reason": "static model root is not a Skeleton node"})
            for dropped in cres["dropped"]:
                path = dropped["path"]
                source_nodes = [node for nodes in facts["models_by_name"].values() for node in nodes if node["path"] == path]
                skeletal_node = any(node["type"] in ("LimbNode","Limb","Root") or
                    node["type"] == "Null" and any(info["fbx"]["path"].startswith(path+"/") for info in skel.bones.values())
                    for node in source_nodes)
                reference = unity_rest.get(path)
                if reference is None and len(source_nodes) == 1:
                    node = source_nodes[0]
                    values = coords.fbx_to_unity(node["default_local"][None],k,C_inv if node["top_level"] else None)
                    reference = {key:value[0] for key,value in zip(("t","r","s"),values)}
                if path in tracks and skeletal_node and track_differs_from_rest(tracks[path],reference,n):
                    raise JobError(f"animated source Skeleton node {path!r} could not be mapped; refusing motion loss")
            bone_values = {}
            targets = dict(mapping)
            if job.get("root_to_bone") and skel.retained_root is None:
                rb = job["root_to_bone"]
                if rb not in skel.bones or skel.bones[rb]["bone"].parent is not None:
                    raise JobError(f"--root-to-bone {rb!r} must be a parent-less bone of the skeleton")
                if not any(v == rb for v in targets.values()):
                    targets[skel.unity_path(rb)] = rb
                    tracks = dict(tracks)
                    tracks.setdefault(skel.unity_path(rb), {"t": None, "r": None, "s": None})
            for path, bone in targets.items():
                tr = tracks[path]
                a = correction_for(skel, bone, k, C_inv, unity_rest.get(path), cres["warnings"])
                rt, rq, rs = unity_rest_trs(skel, bone, k, a, unity_rest.get(path))
                t = fill(to_array(tr.get("t"), n, 3), rt, n)
                q = to_array(tr.get("r"), n, 4)
                q = np.tile(rq, (n, 1)) if q is None else q
                s = fill(to_array(tr.get("s"), n, 3), rs, n)
                if job.get("root_to_bone") == bone and root_tr is not None:
                    t, q, s = compose_root(root_tr, unity_rest.get(""), t, q, s, n)
                if np.any(np.ptp(s, axis=1) > 1e-6):
                    message = (f"{bone}: nonuniform bone scale may lose precision during Blender FBX round trips; "
                               "verify the exported animation")
                    if job.get("strict"):
                        raise JobError(message)
                    cres["warnings"].append(message)
                local_fbx = coords.unity_to_fbx(t, q, s, k, a)
                local_bl = local_fbx.copy()
                local_bl[:, :3, 3] *= skel.u
                basis = np.linalg.inv(skel.bones[bone]["rel_blender"]) @ local_bl
                bt, bq, bs = coords.mat_to_trs(basis)
                bq = bq[:, [3, 0, 1, 2]]  # Blender stores w first
                for i in range(1, n):
                    if np.dot(bq[i], bq[i - 1]) < 0:
                        bq[i] = -bq[i]
                bone_values[bone] = (bt, bq, bs)
            set_scene_time(float(clip["fps"]), n)
            object_values = retained_root_values(skel,tracks,unity_rest,k,C_inv,n,job.get("root_to_bone"))
            action = write_action(arm, clip["name"], bone_values, n, object_values)
            export(arm, clip["out_fbx"], action)
            cres["bones_animated"] = len(bone_values)
            cres["real_root_animated"] = arm.name if object_values is not None else None
            cres["reference_validation"] = dict(skel.reference_validation)
            if cres["dropped"] or cres["warnings"]:
                cres["status"] = "warning"
        except Exception as e:  # noqa: BLE001 - report per clip
            cres["status"] = "error"
            cres["error"] = f"{type(e).__name__}: {e}"
            cres["trace"] = traceback.format_exc()


def compose_root(root_tr, root_rest, t, q, s, n):
    """Fold root motion into a bone: L' = R0^-1 . R(f) . L, so R0 . L' = R(f) . L."""
    rt = to_array(root_tr.get("t"), n, 3)
    rq = to_array(root_tr.get("r"), n, 4)
    rs = to_array(root_tr.get("s"), n, 3)
    if root_rest is not None:
        r0t, r0q, r0s = (np.array(root_rest[x], float) for x in ("t", "r", "s"))
    else:
        r0t, r0q, r0s = np.zeros(3), np.array([0, 0, 0, 1.0]), np.ones(3)
    rt = fill(rt, r0t, n)
    rq = np.tile(r0q, (n, 1)) if rq is None else rq
    rs = fill(rs, r0s, n)
    R = coords.trs_to_mat(rt, rq, rs)
    R0 = coords.trs_to_mat(r0t[None], r0q[None], r0s[None])[0]
    B = coords.trs_to_mat(t, q, s)
    return coords.mat_to_trs(np.linalg.inv(R0) @ R @ B)


# Split FBX takes

def run_split(job, result):
    reset_scene()
    import_fbx(job["source_fbx"], use_anim=True)
    facts = read_fbx_facts(job["source_fbx"])
    arm = pick_armature(job.get("armature"), facts)
    allow_local_bone_translations(arm)
    scene = bpy.context.scene
    fps_int = scene.render.fps
    f_src = scene.render.fps / scene.render.fps_base
    result["source_fps"] = f_src
    if "|" in arm.name or any("|" in s for s in facts["stacks"]) or facts["duplicate_stacks"]:
        raise JobError("take/armature names contain '|' or duplicate takes; Blender action names would be ambiguous")
    skel = Skeleton(arm, facts, result["warnings"])
    apply_armature_policy(arm, skel, facts)
    prefix = arm.name + "|"
    stack_actions = {}
    for act in bpy.data.actions:
        if not act.name.startswith(prefix):
            continue
        parts = act.name[len(prefix):].split("|")
        stack_actions.setdefault(parts[0], []).append(act)
    result["stacks"] = sorted(stack_actions)
    if job.get("probe"):
        return
    clips = job.get("split_clips") or [
        {"name": s, "take": s, "first": None, "last": None, "loop": False, "events": []} for s in sorted(stack_actions)
    ]
    for clip in clips:
        cres = {"name": clip["name"], "out_fbx": clip["out_fbx"], "dropped": [], "warnings": [], "status": "ok"}
        result["clips"].append(cres)
        try:
            take = clip["take"] or (facts["stacks"][0] if facts["stacks"] else "")
            acts = stack_actions.get(take)
            if not acts and len(stack_actions) == 1:
                cres["warnings"].append(f"take {take!r} not found; using the only take {next(iter(stack_actions))!r}")
                acts = next(iter(stack_actions.values()))
            if not acts:
                raise JobError(f"take {take!r} not found; available: {sorted(stack_actions)}")
            if len(acts) > 1:
                raise JobError(f"take {take!r} has {len(acts)} animation layers (unsupported)")
            src = acts[0]
            fcs = list(channelbag_fcurves(src))
            if not fcs:
                raise JobError(f"take {take!r} has no curves for armature {arm.name!r}")
            lo = min(fc.range()[0] for fc in fcs)
            hi = max(fc.range()[1] for fc in fcs)
            t0 = clip["first"] / f_src if clip["first"] is not None else lo / fps_int
            t1 = clip["last"] / f_src if clip["last"] is not None else hi / fps_int
            fps_out = float(job.get("fps") or f_src)
            d = max(0.0, t1 - t0)
            n = 1 if d == 0 else max(2, int(round(d * fps_out)) + 1)
            times = t0 + np.minimum(np.arange(n) / fps_out, d)
            sparse = False
            for fc in fcs:
                kp = fc.keyframe_points
                if len(kp) > 1:
                    xs = np.empty(2 * len(kp))
                    kp.foreach_get("co", xs)
                    if np.max(np.diff(xs[0::2])) > 1.0001 * fps_int / f_src:
                        sparse = True
            if sparse:
                msg = "source keys are sparser than one per frame; Blender imported them as linear (prefer Unity bake, path B)"
                if job.get("strict"):
                    raise JobError(msg)
                cres["warnings"].append(msg)
            new = bpy.data.actions.new(clip["name"])
            ad = arm.animation_data_create()
            ad.action = new
            frames = np.arange(n, dtype=np.float64)
            for fc in fcs:
                vals = np.array([fc.evaluate(t * fps_int) for t in times])
                group = fc.group.name if fc.group else ""
                nfc = new.fcurve_ensure_for_datablock(arm, fc.data_path, index=fc.array_index, group_name=group)
                nfc.keyframe_points.add(n)
                co = np.empty(2 * n)
                co[0::2] = frames
                co[1::2] = vals
                nfc.keyframe_points.foreach_set("co", co)
                nfc.keyframe_points.foreach_set("interpolation", [LINEAR] * n)
                nfc.update()
            set_scene_time(fps_out, n)
            export(arm, clip["out_fbx"], new)
            cres["fps"] = fps_out
            cres["frames"] = n
            cres["duration"] = (n - 1) / fps_out
            out_d = (n - 1) / fps_out
            cres["events"] = []
            for e in clip.get("events", []):
                t = e["time"] * d
                if t > out_d + 1e-9:
                    cres["warnings"].append(f"event {e['name']!r} at {t:.4f}s moved to the clip end {out_d:.4f}s")
                    t = out_d
                cres["events"].append(dict(e, time=t))
            if cres["warnings"]:
                cres["status"] = "warning"
        except Exception as e:  # noqa: BLE001
            cres["status"] = "error"
            cres["error"] = f"{type(e).__name__}: {e}"
            cres["trace"] = traceback.format_exc()


def main():
    result = {"clips": [], "warnings": [], "status": "ok"}
    try:
        if JOB["mode"] == "samples":
            run_samples(JOB, result)
        elif JOB["mode"] == "split":
            run_split(JOB, result)
        else:
            raise JobError(f"unknown mode {JOB['mode']!r}")
    except Exception as e:  # noqa: BLE001
        result["status"] = "error"
        result["error"] = f"{type(e).__name__}: {e}"
        result["trace"] = traceback.format_exc()
    Path(JOB["result"]).write_text(json.dumps(result, indent=2), encoding="utf-8")


main()
