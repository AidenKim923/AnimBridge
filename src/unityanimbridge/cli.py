"""uab: Unity animation -> Unreal-ready FBX."""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

from . import __version__, process, runtime
from .anim_parser import AnimParseError, parse_anim
from .blender_runner import BlenderError, find_blender, run_job
from .meta_parser import meta_path_for, read_model_meta
from .sampler import arr_json, sample_clip
from .unity_yaml import is_text_yaml

EXIT_OK, EXIT_PARTIAL, EXIT_INPUT, EXIT_BLENDER = 0, 1, 2, 3


class InputError(Exception):
    pass


def select_clips(clips, requested, name):
    """Resolve exact clip names before running any conversion (no silent fallback)."""
    if not requested:
        return clips
    wanted = set(requested)
    names = [name(c) for c in clips]
    missing = wanted - set(names)
    if missing:
        raise InputError("selected clip(s) not found: " + ", ".join(sorted(missing)))
    ambiguous = {n for n in wanted if names.count(n) > 1}
    if ambiguous:
        raise InputError("selected clip name(s) are ambiguous: " + ", ".join(sorted(ambiguous)))
    return [c for c in clips if name(c) in wanted]


def safe_name(name: str, used: set[str]) -> str:
    base = re.sub(r'[\\/:*?"<>|\x00-\x1f]', "_", name).strip().rstrip(". ") or "Clip"
    stem = base.split(".", 1)[0].upper()
    if stem in {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"} or re.fullmatch(r"(?:COM|LPT)[0-9¹²³]", stem):
        base = "_" + base
    cand, i = base, 2
    while cand.lower() in used:
        cand, i = f"{base}_{i}", i + 1
    used.add(cand.lower())
    return cand


def cmd_inspect(args) -> int:
    with process.job(), process.stage("inspect"):
        return _cmd_inspect(args)


def _cmd_inspect(args) -> int:
    p = Path(args.file)
    if p.suffix.lower() == ".fbx":
        mp = Path(args.meta) if args.meta else meta_path_for(p)
        info = {"file": str(p), "route": "C", "meta": str(mp) if mp.is_file() else None}
        if mp.is_file():
            m = read_model_meta(mp)
            info.update(
                animation_type=m.animation_type, global_scale=m.global_scale, use_file_scale=m.use_file_scale,
                clips=[c.__dict__ for c in m.clips], warnings=m.warnings,
            )
        print(json.dumps(info, indent=2, ensure_ascii=False))
        return EXIT_OK
    if not is_text_yaml(p):
        print(json.dumps({"file": str(p), "route": "B", "reason": "binary .anim"}, indent=2))
        return EXIT_OK
    clip = parse_anim(p)
    info = {
        "file": str(p),
        "name": clip.name,
        "route": "B" if clip.is_humanoid else "A",
        "sample_rate": clip.sample_rate,
        "start": clip.start_time,
        "stop": clip.stop_time,
        "loop": clip.loop,
        "paths": {k: sorted(v) for k, v in clip.channels.items()},
        "humanoid_attributes": clip.humanoid_attributes[:20],
        "float_curves": [f"{t.path}:{t.attribute} (classID {t.class_id})" for t in clip.float_tracks],
        "events": [e.__dict__ for e in clip.events],
        "unsupported": clip.unsupported,
        "warnings": clip.warnings,
    }
    print(json.dumps(info, indent=2, ensure_ascii=False))
    return EXIT_OK


def load_clip_samples(anim: Path, fps):
    if not is_text_yaml(anim):
        raise InputError(f"{anim}: binary .anim; bake it with the Unity helper (path B) and use --baked")
    try:
        with process.stage("parse"):
            clip = parse_anim(anim)
    except AnimParseError as e:
        raise InputError(str(e)) from e
    if clip.is_humanoid:
        raise InputError(
            f"{anim}: Humanoid (muscle) curves found ({', '.join(clip.humanoid_attributes[:3])}...); "
            "bake it with the Unity helper (path B) and use --baked"
        )
    with process.stage("sample"):
        s = sample_clip(clip, fps)
    sidecar = {
        "clip": clip.name,
        "source": str(anim),
        "fps": s.fps,
        "frames": s.frames,
        "duration": s.duration,
        "loop": s.loop,
        "events": s.events,
        "curves": {k: v.tolist() for k, v in s.curves.items()},
        "unsupported": clip.unsupported,
        "warnings": clip.warnings + s.warnings,
    }
    job_clip = {"name": clip.name, "fps": s.fps, "frames": s.frames,
                "tracks": s.to_json()["tracks"]}
    return job_clip, sidecar


@process.staged("parse-baked")
def load_baked(path: Path):
    data = json.loads(path.read_text(encoding="utf-8"))
    if data.get("uab_baked") != 1:
        raise InputError(f"{path}: not a UnityAnimBridge baked.json (uab_baked != 1)")
    clips = []
    for c in data.get("clips", []):
        n = int(c["frames"])
        for p, tr in c["tracks"].items():
            for key, w in (("t", 3), ("r", 4), ("s", 3)):
                v = tr.get(key)
                if v is not None and (len(v) != n or any(len(row) != w for row in v)):
                    raise InputError(f"{path}: clip {c['name']!r} track {p!r}.{key} has wrong shape")
        sidecar = {
            "clip": c["name"], "source": str(path), "fps": c["fps"] if "fps" in c else data["fps"],
            "frames": n, "duration": c.get("duration"), "loop": c.get("loop", False),
            "events": c.get("events", []), "curves": {}, "unsupported": [], "warnings": c.get("warnings", []),
        }
        clips.append(({"name": c["name"], "fps": sidecar["fps"], "frames": n, "tracks": c["tracks"]}, sidecar))
    return clips, data.get("rest")


def skeleton_meta(skeleton: Path, meta_arg):
    mp = Path(meta_arg) if meta_arg else meta_path_for(skeleton)
    if mp.is_file():
        return read_model_meta(mp), []
    return None, [f"no .meta for {skeleton}; assuming Unity defaults (useFileScale=1, globalScale=1)"]


@process.staged("write-results")
def finish(results, sidecars, out: Path, extra_warnings) -> int:
    manifest = {"uab_version": 1, "tool_version": __version__, "clips": [], "warnings": extra_warnings}
    worst = EXIT_OK
    for res in results:
        if res.get("status") == "error" and not res.get("clips"):
            manifest["warnings"].append(f"job failed: {res.get('error')} (log {res.get('log')})")
            worst = EXIT_BLENDER
        for c in res.get("clips", []):
            sc = sidecars.get(c["out_fbx"], {})
            sc = dict(sc)
            for key in ("fps", "frames", "duration", "events"):
                if key in c and (key not in sc or sc[key] in (None, [])):
                    sc[key] = c[key]
            sc.update(
                uab_version=1,
                unity_units_per_fbx_unit=res.get("k"),
                skeleton_fbx=res.get("skeleton_fbx"),
                dropped=c.get("dropped", []),
                warnings=sc.get("warnings", []) + c.get("warnings", []),
                status=c["status"],
            )
            # Surface omitted tracks to desktop callers that consume warnings,
            # while retaining the structured diagnostics in the sidecar.
            sc["warnings"] += [
                f"Omitted track {item.get('path', '')!r}: {item.get('reason', 'unknown')}"
                for item in sc["dropped"]
            ]
            if c.get("error"):
                sc["error"] = c["error"]
                worst = max(worst, EXIT_PARTIAL)
            if sc["warnings"] or sc["dropped"]:
                sc["status"] = "warning" if c["status"] == "ok" else c["status"]
            sc_path = Path(c["out_fbx"]).with_suffix(".uab.json")
            sc_path.write_text(json.dumps(sc, indent=2, ensure_ascii=False), encoding="utf-8")
            manifest["clips"].append(
                {"clip": sc.get("clip", c["name"]), "fbx": c["out_fbx"], "sidecar": str(sc_path),
                 "status": sc["status"], "source": sc.get("source"), "fps": sc.get("fps"),
                 "frames": sc.get("frames"), "duration": sc.get("duration")}
            )
            print(f"[{sc['status']}] {sc.get('clip', c['name'])} -> {c['out_fbx']}"
                  + (f"  ({c['error']})" if c.get("error") else ""))
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False), encoding="utf-8")
    report = [f"{m['status']:8} {m['clip']}  {m['fbx']}" for m in manifest["clips"]] + manifest["warnings"]
    (out / "report.txt").write_text("\n".join(report) + "\n", encoding="utf-8")
    return worst


@process.staged("prepare-samples")
def samples_job(args, skeleton: Path, clips, out: Path, unity_rest, used=None):
    meta, warns = skeleton_meta(skeleton, args.meta)
    if meta and meta.bake_axis_conversion:
        raise InputError(f"{meta.path}: bakeAxisConversion is on; not supported (DESIGN 8.3)")
    used = set() if used is None else used
    job_clips, sidecars = [], {}
    for job_clip, sidecar in clips:
        fbx = str((out / (safe_name(job_clip["name"], used) + ".fbx")).resolve())
        job_clips.append(dict(job_clip, out_fbx=fbx))
        sidecars[fbx] = dict(sidecar, skeleton_fbx=str(skeleton))
    job = {
        "mode": "samples",
        "skeleton_fbx": str(skeleton.resolve()),
        "armature": args.armature,
        "use_file_scale": meta.use_file_scale if meta else True,
        "global_scale": meta.global_scale if meta else 1.0,
        "unity_rest": unity_rest,
        "bone_map": json.loads(Path(args.bone_map).read_text(encoding="utf-8")) if args.bone_map else None,
        "root_to_bone": args.root_to_bone,
        "strict": args.strict,
        "clips": job_clips,
    }
    # This FBX supplies the skeleton, not the animation. Its clip-level mirror
    # and raw-Humanoid warnings apply to path C, not Unity-evaluated samples.
    # Unsupported skeleton axis conversion is still rejected above.
    return job, sidecars, warns


@process.staged("parse-model-meta")
def split_job(args, fbx: Path, out: Path):
    mp = Path(args.meta) if args.meta else meta_path_for(fbx)
    warns, split = [], None
    used: set[str] = set()
    if mp.is_file():
        meta = read_model_meta(mp)
        warns += meta.warnings
        if meta.clips:
            split = [
                {"name": c.name, "take": c.take_name, "first": c.first_frame, "last": c.last_frame,
                 "loop": c.loop, "events": c.events,
                 "out_fbx": str((out / (safe_name(c.name, used) + ".fbx")).resolve())}
                for c in meta.clips
            ]
    else:
        warns.append(f"no .meta for {fbx}; one clip per take")
    job = {"mode": "split", "source_fbx": str(fbx.resolve()), "armature": args.armature,
           "fps": args.fps, "strict": args.strict, "split_clips": split}
    sidecars = {}
    if split:
        for c in split:
            sidecars[c["out_fbx"]] = {"clip": c["name"], "source": str(fbx), "loop": c["loop"]}
    return job, sidecars, warns, used


def run_split(args, fbx, out, blender):
    with process.job(args.job_timeout, stage_seconds=args.timeout):
        return _run_split(args, fbx, out, blender)


def run_one(job, blender, args):
    """One clip or single request: its own absolute job limit, with `--timeout` as the stage budget."""
    with process.job(args.job_timeout):
        return run_job(job, blender, args.timeout)


def _run_split(args, fbx, out, blender):
    job, sidecars, warns, used = split_job(args, fbx, out)
    if job["split_clips"] is None:
        probe = run_job(dict(job, split_clips=[], probe=True), blender, args.timeout)
        if probe.get("status") == "error":
            raise BlenderError(f"FBX inspection failed: {probe.get('error', 'unknown error')} (log {probe.get('log')})")
        stacks = probe.get("stacks") or []
        if not stacks:
            raise InputError(f"{fbx}: no animation takes found")
        job["split_clips"] = [
            {"name": s, "take": s, "first": None, "last": None, "loop": False, "events": [],
             "out_fbx": str((out / (safe_name(s, used) + ".fbx")).resolve())}
            for s in stacks
        ]
        for c in job["split_clips"]:
            sidecars[c["out_fbx"]] = {"clip": c["name"], "source": str(fbx), "loop": False}
    job["split_clips"] = select_clips(job["split_clips"], getattr(args, "clip", None), lambda c: c["name"])
    res = run_job(job, blender, args.timeout)
    res["skeleton_fbx"] = str(fbx)
    return res, sidecars, warns


def cmd_convert(args) -> int:
    with process.job(args.job_timeout, stage_seconds=args.timeout):
        return _cmd_convert(args)


def _cmd_convert(args) -> int:
    out = Path(args.output)
    out.mkdir(parents=True, exist_ok=True)
    blender = find_blender(args.blender)
    unity_rest = None
    if args.unity_rest:
        _, unity_rest = load_baked(Path(args.unity_rest))
    if args.baked:
        if not args.skeleton:
            raise InputError("--baked needs --skeleton <model.fbx>")
        clips, rest = load_baked(Path(args.baked))
        clips = select_clips(clips, args.clip, lambda c: c[0]["name"])
        unity_rest = unity_rest or rest
        job, sidecars, warns = samples_job(args, Path(args.skeleton), clips, out, unity_rest)
        res = run_one(job, blender, args)
        res["skeleton_fbx"] = args.skeleton
        return finish([res], sidecars, out, warns)
    if not args.input:
        raise InputError("give an input .anim/.fbx or --baked")
    p = Path(args.input)
    if not p.is_file():
        raise InputError(f"{p} not found")
    if p.suffix.lower() == ".fbx":
        res, sidecars, warns = run_split(args, p, out, blender)
        return finish([res], sidecars, out, warns)
    if not args.skeleton:
        raise InputError(".anim conversion needs --skeleton <model.fbx>")
    clips = [load_clip_samples(p, args.fps)]
    clips = select_clips(clips, args.clip, lambda c: c[0]["name"])
    job, sidecars, warns = samples_job(args, Path(args.skeleton), clips, out, unity_rest)
    res = run_one(job, blender, args)
    res["skeleton_fbx"] = args.skeleton
    return finish([res], sidecars, out, warns)


def cmd_batch(args) -> int:
    root = Path(args.folder)
    out = Path(args.output)
    out.mkdir(parents=True, exist_ok=True)
    blender = find_blender(args.blender)
    pattern = "**/*" if args.recursive else "*"
    anims = sorted(q for q in root.glob(pattern + ".anim"))
    fbxs = sorted(q for q in root.glob(pattern + ".fbx")) if args.include_fbx else []
    results, sidecars, warns = [], {}, []
    if anims and not args.skeleton:
        raise InputError("batch with .anim files needs --skeleton <model.fbx>")
    used = set()
    for a in anims:
        try:
            with process.job(args.job_timeout, stage_seconds=args.timeout):
                clip = load_clip_samples(a, args.fps)
                if not args.skeleton:
                    raise InputError("batch with .anim files needs --skeleton <model.fbx>")
                unity_rest = load_baked(Path(args.unity_rest))[1] if args.unity_rest else None
                job, sc, warnings = samples_job(args, Path(args.skeleton), [clip], out, unity_rest, used)
                result = run_one(job, blender, args)
                result["skeleton_fbx"] = args.skeleton
                results.append(result)
                sidecars.update(sc)
                warns += warnings
        except InputError as e:
            warns.append(f"skipped: {e}")
    for f in fbxs:
        sub = out / f.stem
        sub.mkdir(exist_ok=True)
        res, sc, w = run_split(args, f, sub, blender)
        results.append(res)
        sidecars.update(sc)
        warns += w
    return finish(results, sidecars, out, warns)


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(prog="uab", description=__doc__)
    ap.add_argument("--version", action="version", version=__version__)
    sub = ap.add_subparsers(dest="cmd", required=True)

    def common(p):
        p.add_argument("-o", "--output", required=True)
        p.add_argument("--skeleton", help="model .fbx whose skeleton the clip animates")
        p.add_argument("--meta", help=".meta of the skeleton/source FBX (default <fbx>.meta)")
        p.add_argument("--fps", type=float)
        p.add_argument("--armature")
        p.add_argument("--bone-map")
        p.add_argument("--root-to-bone")
        p.add_argument("--unity-rest", help="baked.json whose rest pose calibrates the conversion")
        p.add_argument("--blender")
        p.add_argument("--timeout", type=float, default=process.DEFAULT_STAGE_SECONDS,
                       help="per-stage budget in seconds (default 600)")
        p.add_argument("--job-timeout", type=float, default=process.DEFAULT_JOB_SECONDS,
                       help="limit for one clip or single request in seconds (default 3600; not a whole-batch cap)")
        p.add_argument("--strict", action="store_true")

    p = sub.add_parser("inspect")
    p.add_argument("file")
    p.add_argument("--meta")
    p.set_defaults(func=cmd_inspect)

    p = sub.add_parser("convert")
    p.add_argument("input", nargs="?")
    p.add_argument("--baked")
    p.add_argument("--clip", action="append", help="convert only this exact clip name; repeat to select multiple clips")
    common(p)
    p.set_defaults(func=cmd_convert)

    p = sub.add_parser("batch")
    p.add_argument("folder")
    p.add_argument("--recursive", action="store_true")
    p.add_argument("--include-fbx", action="store_true")
    common(p)
    p.set_defaults(func=cmd_batch)
    return ap


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)
    try:
        runtime.current_context()
        code = args.func(args)
    except process.ProcessTimeout as e:
        print(f"timeout ({e.layer}): {e}", file=sys.stderr)
        return EXIT_BLENDER
    except InputError as e:
        print(f"error: {e}", file=sys.stderr)
        return EXIT_INPUT
    except BlenderError as e:
        layer = f" [{e.layer} timeout]" if getattr(e, "layer", None) else ""
        print(f"blender error{layer}: {e}", file=sys.stderr)
        return EXIT_BLENDER
    except (OSError, ValueError) as e:
        print(f"error: {e}", file=sys.stderr)
        return EXIT_INPUT
    if getattr(args, "strict", False) and code == EXIT_OK:
        pass
    return code


if __name__ == "__main__":
    sys.exit(main())
