"""Graphical Unreal animation preview with an ordered asset-editor shutdown.

Run inside a graphical editor (never -Cmd):
  UnrealEditor.exe Project.uproject -ExecCmds="py <this file> <request.json>"

The launcher is non-blocking: it opens the owned animation asset editor(s) and
then drives a state machine from the Slate post-tick callback. When the stop
request appears it closes ONLY the owned assets' editors while editor
subsystems are still alive, drains Slate ticks, confirms the editors are gone,
and only then lowers the Python keepalive and requests a graceful editor quit.

Close confirmation relies on UAssetEditorSubsystem::CloseAllEditorsForAsset,
the only UFUNCTION-exposed close API (UE 5.8 header). It returns the number of
editors FOUND for the asset and its subobjects before the close attempt, so a
repeat call after the drain returning 0 for every owned asset proves none are
still tracked; a positive count means the repeat call was itself a retry. When
get_all_edited_assets happens to be exposed it must agree. A close that cannot
be confirmed holds the editor open instead of quitting, unless the request
explicitly opts into a differential quit.

This sequencing is a QA-side mitigation. It does not fix the engine defect in
FAnimationEditor::~FAnimationEditor, which touches UImportSubsystem during
global Slate shutdown.
"""
import json
import math
import os
import sys
import time
import traceback
from datetime import datetime, timezone
from pathlib import Path

import unreal

REQUEST_SCHEMA = "animbridge.unreal.preview-request/1"
EVIDENCE_SCHEMA = "animbridge.unreal.preview-evidence/1"
REQUEST_ENV = "ANIMBRIDGE_PREVIEW_REQUEST"
PREFERRED_CLIPS = ["idle", "move_front", "move_Flying_front", "attack01", "attack_01"]

DEFAULTS = {
    "schema": REQUEST_SCHEMA,
    "work_dir": None,
    "selection": "preview_selection.json",
    "source_result": "source_result.json",
    "assets": None,
    "mesh": None,
    "skeleton": None,
    "clip": None,
    "evidence_path": "preview_launch.json",
    "stop_request": "preview-stop.request",
    "stop_after_seconds": None,
    "close_method": "asset",
    "drain_ticks": 10,
    "min_drain_seconds": 0.5,
    "max_close_attempts": 3,
    "quit_after_close": True,
    "quit_command": "QUIT_EDITOR",
    "on_unconfirmed": "hold",
    "on_close_failure": "hold",
    "save_assets": True,
    "keepalive": True,
}
PATH_KEYS = ("selection", "source_result", "evidence_path", "stop_request")
CHOICES = {
    "close_method": ("asset", "none"),
    "on_unconfirmed": ("quit", "hold"),
    "on_close_failure": ("quit", "hold"),
}


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def write_json_atomic(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_text(json.dumps(data, indent=2, ensure_ascii=False), encoding="utf-8")
    os.replace(tmp, path)


def resolve_request_path(argv, environ, script_dir):
    """argv[1] .json > env var > preview-request.json next to the script > None (legacy)."""
    for arg in list(argv)[1:]:
        if str(arg).lower().endswith(".json"):
            return Path(arg)
    if environ.get(REQUEST_ENV):
        return Path(environ[REQUEST_ENV])
    candidate = Path(script_dir) / "preview-request.json"
    return candidate if candidate.exists() else None


def normalize_request(raw, request_dir):
    raw = dict(raw or {})
    unknown = sorted(set(raw) - set(DEFAULTS))
    if unknown:
        raise ValueError("unknown preview request keys: " + ", ".join(unknown))
    if raw.get("schema", REQUEST_SCHEMA) != REQUEST_SCHEMA:
        raise ValueError("unsupported preview request schema: %r" % raw.get("schema"))
    req = {**DEFAULTS, **raw}
    for key, allowed in CHOICES.items():
        if req[key] not in allowed:
            raise ValueError("%s must be one of %s" % (key, allowed))
    if req["assets"] is not None and (
            not isinstance(req["assets"], list) or not req["assets"]
            or not all(isinstance(a, str) and a.startswith("/") for a in req["assets"])):
        raise ValueError("assets must be a non-empty list of /Game object paths")
    for key in ("drain_ticks", "max_close_attempts"):
        if not isinstance(req[key], int) or req[key] < 1:
            raise ValueError("%s must be a positive integer" % key)
    after = req["stop_after_seconds"]
    if after is not None and (type(after) not in (int, float) or not math.isfinite(after) or after < 0):
        raise ValueError("stop_after_seconds must be null or a finite number >= 0")
    work = Path(req["work_dir"]) if req["work_dir"] else Path(request_dir)
    req["work_dir"] = str(work)
    for key in PATH_KEYS:
        path = Path(req[key])
        req[key] = str(path if path.is_absolute() else work / path)
    return req


def _imported_result(req, selection):
    work = Path(req["work_dir"])
    candidates = []
    if selection.get("result"):
        candidates.append(Path(selection["result"]))
    candidates += [work / "reload_result.json", work / "import_result.json"]
    return next((p for p in candidates if p.exists()), None)


def choose_assets(req, selection, imported):
    """Return [(clip_label, asset_path)] and the ranked clip list for evidence."""
    clips = (imported or {}).get("clips", [])
    ranked = sorted(clips, key=lambda c: PREFERRED_CLIPS.index(c["clip"]) if c["clip"] in PREFERRED_CLIPS else len(PREFERRED_CLIPS))
    if req["assets"]:
        by_asset = {c["asset"]: c["clip"] for c in clips}
        return [(by_asset.get(a, a.rsplit(".", 1)[-1]), a) for a in req["assets"]], ranked
    if not ranked:
        raise RuntimeError("no clips available and request.assets is empty")
    wanted_clip = req["clip"] or selection.get("clip")
    chosen = next((c for c in ranked if c["asset"] == selection.get("asset")), None)
    if chosen is None:
        chosen = next((c for c in ranked if c["clip"] == wanted_clip), ranked[0])
    return [(chosen["clip"], chosen["asset"])], ranked


def _utc():
    return datetime.now(timezone.utc).isoformat()


class PreviewSession:
    """Owns the opened asset editors and the ordered close/quit state machine."""

    def __init__(self, req, owned_assets, evidence):
        self.req = req
        self.assets = list(owned_assets)
        self.owned_paths = [a.get_path_name() for a in self.assets]
        self.evidence = evidence
        self.evidence_path = Path(req["evidence_path"])
        self.stop_path = Path(req["stop_request"])
        self.subsystem = unreal.get_editor_subsystem(unreal.AssetEditorSubsystem)
        self.state = "running"
        self.handle = None
        self.start = time.monotonic()
        self.stop_baseline = self._stop_stamp()
        self.first_tick = True
        self.calls = 0
        self.last_counts = {}
        self.drained = 0
        self.close_time = 0.0
        evidence["state"] = self.state
        evidence["stop_request_preexisting"] = self.stop_baseline is not None

    def _stop_stamp(self):
        try:
            return self.stop_path.stat().st_mtime_ns
        except OSError:
            return None

    def flush(self):
        self.evidence["state"] = self.state
        write_json_atomic(self.evidence_path, self.evidence)

    def stage(self, name, **detail):
        entry = {"stage": name, "elapsed_s": round(time.monotonic() - self.start, 3), "utc": _utc(), **detail}
        self.evidence.setdefault("stages", []).append(entry)
        unreal.log("AnimBridge preview stage: %s %s" % (name, json.dumps(detail, default=str)))
        self.flush()

    def register(self):
        self.handle = unreal.register_slate_post_tick_callback(self._on_tick)
        self.stage("tick_registered")

    def _unregister(self):
        if self.handle is not None:
            unreal.unregister_slate_post_tick_callback(self.handle)
            self.handle = None

    def _on_tick(self, _delta):
        try:
            self.step(time.monotonic())
        except Exception:
            self.state = "error"
            self._unregister()
            self.stage("error", traceback=traceback.format_exc())

    def stop_requested(self, now):
        stamp = self._stop_stamp()
        if stamp is not None and stamp != self.stop_baseline:
            return "stop_request_file"
        after = self.req["stop_after_seconds"]
        if after is not None and now - self.start >= after:
            return "stop_after_seconds"
        return None

    def open_owned(self):
        """Owned paths listed as edited, or None when get_all_edited_assets is not exposed or fails."""
        getter = getattr(self.subsystem, "get_all_edited_assets", None)
        if not callable(getter):
            return None
        try:
            edited = {a.get_path_name() for a in getter() if a is not None}
        except Exception:
            return None
        return [p for p in self.owned_paths if p in edited]

    def _close(self, now, kind):
        """Close every owned asset's editors; the per-asset int is the count found before the attempt."""
        self.calls += 1
        counts = {}
        for asset in self.assets:
            counts[asset.get_path_name()] = self.subsystem.close_all_editors_for_asset(asset)
        self.last_counts = counts
        self.drained = 0
        self.close_time = now
        self.state = "drain"
        self.stage("close_called", kind=kind, call=self.calls, editors_found=counts)

    def step(self, now):
        state = self.state
        if state == "running":
            if self.first_tick:
                self.first_tick = False
                self.stage("open_observed", open_owned=self.open_owned())
            reason = self.stop_requested(now)
            if reason:
                self.stage("stop_requested", reason=reason, open_owned=self.open_owned())
                self.state = "close"
                self.step(now)
        elif state == "close":
            if self.req["close_method"] == "none":
                self.stage("close_skipped", note="differential baseline: quitting without closing editors")
                self.state = "quit"
                self.step(now)
            else:
                self._close(now, "close")
                counts = self.last_counts
                if not all(type(n) is int and n > 0 for n in counts.values()):
                    self.stage("close_unconfirmed", editors_found=counts,
                               reason="first close call did not observe a tracked editor (count > 0) for every owned asset; "
                                      "the editor may have switched to another asset")
                    self._conclude("unconfirmed", "hold")
        elif state == "drain":
            self.drained += 1
            if self.drained >= self.req["drain_ticks"] and now - self.close_time >= self.req["min_drain_seconds"]:
                self.stage("drained", ticks=self.drained)
                self.state = "verify"
        elif state == "verify":
            self._verify(now)
        elif state == "quit":
            self._quit()

    def _verify(self, now):
        """Repeat the close call: 0 for every owned asset proves nothing is tracked; >0 means it retried."""
        self._close(now, "verify")
        counts = self.last_counts
        listing = self.open_owned()
        if not all(type(n) is int for n in counts.values()):
            outcome, policy = "unconfirmed", self.req["on_unconfirmed"]
            self.stage("close_unconfirmed", reason="close_all_editors_for_asset did not return an integer count", editors_found=counts)
        elif any(counts.values()) or listing:
            if self.calls - 1 < self.req["max_close_attempts"]:
                self.stage("close_retry", editors_found=counts, still_listed=listing)
                return
            outcome, policy = "failed", self.req["on_close_failure"]
            self.stage("close_failed", editors_found=counts, still_listed=listing, calls=self.calls)
        else:
            self.evidence["close_outcome"] = "confirmed"
            self.state = "quit"
            self.stage("close_confirmed", calls=self.calls, listing_checked=listing is not None)
            return
        self._conclude(outcome, policy)

    def _conclude(self, outcome, policy):
        self.evidence["close_outcome"] = outcome
        self.state = "quit" if policy == "quit" else "hold"
        if self.state == "hold":
            self._unregister()
            self.stage("held", note="close not confirmed; editor left running, quit manually after inspecting evidence")

    def _quit(self):
        self._unregister()
        if not self.req["quit_after_close"]:
            self.state = "closed_idle"
            self.stage("closed_idle", note="quit_after_close=false; editor and keepalive left unchanged")
            return
        unreal.EditorPythonScripting.set_keep_python_script_alive(False)
        self.stage("keepalive_lowered")
        world = None
        for getter in (lambda: unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world(),
                       lambda: unreal.EditorLevelLibrary.get_editor_world()):
            try:
                world = getter()
                break
            except Exception:
                continue
        unreal.SystemLibrary.execute_console_command(world, self.req["quit_command"])
        self.state = "quit_requested"
        self.stage("quit_requested", command=self.req["quit_command"], world_found=world is not None)


def probe_api():
    sub = unreal.AssetEditorSubsystem
    return {
        "AssetEditorSubsystem." + n: hasattr(sub, n)
        for n in ("open_editor_for_assets", "close_all_editors_for_asset", "get_all_edited_assets")
    } | {
        "register_slate_post_tick_callback": hasattr(unreal, "register_slate_post_tick_callback"),
        "EditorPythonScripting.set_keep_python_script_alive": hasattr(unreal.EditorPythonScripting, "set_keep_python_script_alive"),
        "SystemLibrary.execute_console_command": hasattr(unreal.SystemLibrary, "execute_console_command"),
    }


def launch(req):
    """Open the owned asset editor(s), write evidence, and start the non-blocking stop watcher."""
    work = Path(req["work_dir"])
    evidence = {"schema": EVIDENCE_SCHEMA, "launch_status": "starting", "request": req, "api": probe_api(), "stages": [],
                "engine_version": unreal.SystemLibrary.get_engine_version(), "visual_playback_confirmed": False,
                "note": "Visible editor playback must be checked separately; this script only opens the real asset editor and orders its shutdown."}
    write_json_atomic(req["evidence_path"], evidence)
    try:
        for name in ("AssetEditorSubsystem.open_editor_for_assets", "register_slate_post_tick_callback"):
            if not evidence["api"][name]:
                raise RuntimeError("required Unreal Python API is missing: " + name)
        if req["close_method"] == "asset" and not evidence["api"]["AssetEditorSubsystem.close_all_editors_for_asset"]:
            raise RuntimeError("close_all_editors_for_asset is not exposed; refusing to open a preview that cannot be closed safely")
        if req["keepalive"]:
            unreal.EditorPythonScripting.set_keep_python_script_alive(True)
        selection_path = Path(req["selection"])
        selection = read_json(selection_path) if selection_path.exists() else {}
        source_path = Path(req["source_result"])
        source = read_json(source_path) if source_path.exists() else {}
        imported_path = _imported_result(req, selection)
        imported = read_json(imported_path) if imported_path else None
        picked, ranked = choose_assets(req, selection, imported)
        mesh_path = req["mesh"] or source.get("mesh")
        skeleton_path = req["skeleton"] or source.get("skeleton")
        mesh = unreal.load_asset(mesh_path) if mesh_path else None
        if mesh_path and mesh is None:
            raise RuntimeError("mesh not found: " + mesh_path)
        if mesh is not None and skeleton_path:
            skeleton = unreal.load_asset(skeleton_path)
            if skeleton is None:
                raise RuntimeError("skeleton not found: " + skeleton_path)
            unreal.AnimationLibrary.set_skeleton_preview_mesh(skeleton, mesh, True)
            if req["save_assets"]:
                unreal.EditorAssetLibrary.save_loaded_asset(skeleton, only_if_is_dirty=False)
        animations = []
        for _clip, path in picked:
            animation = unreal.load_asset(path)
            if animation is None:
                raise RuntimeError("animation not found: " + path)
            if mesh is not None:
                animation.set_preview_skeletal_mesh(mesh)
            if req["save_assets"]:
                unreal.EditorAssetLibrary.save_loaded_asset(animation, only_if_is_dirty=False)
            animations.append(animation)
        opened = unreal.get_editor_subsystem(unreal.AssetEditorSubsystem).open_editor_for_assets(animations)
    except Exception:
        evidence["launch_status"] = "error"
        evidence["stages"].append({"stage": "launch_error", "utc": _utc(), "traceback": traceback.format_exc()})
        write_json_atomic(req["evidence_path"], evidence)
        raise
    evidence.update({
        "launch_status": "asset_editor_requested", "status": "asset_editor_requested",
        "clip": picked[0][0], "asset": animations[0].get_path_name(),
        "assets": [a.get_path_name() for a in animations],
        "mesh": mesh.get_path_name() if mesh is not None else None,
        "open_editor_for_assets_result": repr(opened),
        "available_clips": [{"clip": c["clip"], "asset": c["asset"], "manifest": c.get("manifest")} for c in ranked],
    })
    session = PreviewSession(req, animations, evidence)
    session.stage("editor_open_requested", assets=session.owned_paths, work_dir=str(work))
    session.register()
    unreal.log("AnimBridge real animation preview opened: " + ", ".join(session.owned_paths))
    global SESSION
    SESSION = session
    return session


SESSION = None


def main(argv=None, environ=None, script_dir=None):
    argv = sys.argv if argv is None else argv
    environ = os.environ if environ is None else environ
    script_dir = Path(script_dir or Path(globals().get("__file__") or argv[0]).resolve().parent)
    request_path = resolve_request_path(argv, environ, script_dir)
    raw = read_json(request_path) if request_path else {}
    return launch(normalize_request(raw, Path(request_path).resolve().parent if request_path else script_dir))


if __name__ == "__main__":
    main()
