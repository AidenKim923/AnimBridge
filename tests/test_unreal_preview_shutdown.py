"""Behavioral tests of the ordered preview shutdown against a fake Unreal module (no engine)."""
import importlib.util
import json
import os
from pathlib import Path
import sys
import types

import pytest

ASSET = "/Game/Anims/A_attack01.A_attack01"
MESH = "/Game/Succubus/SK_Succubus.SK_Succubus"
SKELETON = "/Game/Succubus/SK_Succubus_Skeleton.SK_Succubus_Skeleton"


class FakeAsset:
    def __init__(self, path):
        self.path = path

    def get_path_name(self):
        return self.path

    def set_preview_skeletal_mesh(self, mesh):
        pass


class FakeSubsystem:
    """close_all_editors_for_asset returns the editor count found BEFORE closing, like UE 5.8."""

    def __init__(self, log, closes_on_call=1, listing="live", bad_return=False):
        self.log = log
        self.open = {}
        self.calls = 0
        self.closes_on_call = closes_on_call
        self.bad_return = bad_return
        self.assets = {}
        if listing == "live":
            self.get_all_edited_assets = lambda: [self.assets[p] for p, n in self.open.items() if n]
        elif listing == "stuck":
            self.get_all_edited_assets = lambda: list(self.assets.values())

    def open_editor_for_assets(self, assets):
        self.log.append("open")
        for asset in assets:
            self.assets[asset.get_path_name()] = asset
            self.open[asset.get_path_name()] = 1
        return True

    def close_all_editors_for_asset(self, asset):
        self.calls += 1
        path = asset.get_path_name()
        found = self.open.get(path, 0)
        self.log.append("close:%s:found=%d" % (path, found))
        if self.calls >= self.closes_on_call:
            self.open[path] = 0
        return None if self.bad_return else found


@pytest.fixture
def env(monkeypatch, tmp_path):
    log = []
    state = {"callbacks": [], "subsystem": FakeSubsystem(log)}
    fake = types.ModuleType("unreal")
    fake.log = lambda message: None
    fake.load_asset = lambda path: FakeAsset(path)
    fake.get_editor_subsystem = lambda cls: state["subsystem"]
    fake.AssetEditorSubsystem = type("AssetEditorSubsystem", (), {
        "open_editor_for_assets": None, "close_all_editors_for_asset": None, "get_all_edited_assets": None})
    fake.UnrealEditorSubsystem = type("UnrealEditorSubsystem", (), {})
    fake.register_slate_post_tick_callback = lambda cb: state["callbacks"].append(cb) or cb
    fake.unregister_slate_post_tick_callback = lambda handle: state["callbacks"].remove(handle)
    fake.AnimationLibrary = types.SimpleNamespace(set_skeleton_preview_mesh=lambda *a: log.append("preview_mesh"))
    fake.EditorAssetLibrary = types.SimpleNamespace(save_loaded_asset=lambda *a, **k: True)
    fake.EditorPythonScripting = types.SimpleNamespace(
        set_keep_python_script_alive=lambda flag: log.append("keepalive:%s" % flag))
    fake.SystemLibrary = types.SimpleNamespace(
        get_engine_version=lambda: "5.8.2",
        execute_console_command=lambda world, command: log.append("console:" + command))
    fake.EditorLevelLibrary = types.SimpleNamespace(get_editor_world=lambda: object())
    monkeypatch.setitem(sys.modules, "unreal", fake)
    path = Path(__file__).parents[1] / "unreal" / "animbridge_preview_shutdown.py"
    spec = importlib.util.spec_from_file_location("animbridge_preview_shutdown_test", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    (tmp_path / "source_result.json").write_text(json.dumps({"mesh": MESH, "skeleton": SKELETON}), encoding="utf-8")
    (tmp_path / "reload_result.json").write_text(
        json.dumps({"clips": [{"clip": "attack01", "asset": ASSET, "manifest": "m.json"}]}), encoding="utf-8")
    return types.SimpleNamespace(module=module, log=log, state=state, work=tmp_path, fake=fake)


def start(env, **overrides):
    raw = {"drain_ticks": 3, "min_drain_seconds": 0, "evidence_path": "evidence.json", **overrides}
    return env.module.launch(env.module.normalize_request(raw, env.work))


def tick(env, count=1):
    for _ in range(count):
        for callback in list(env.state["callbacks"]):
            callback(0.0)


def evidence(env):
    return json.loads((env.work / "evidence.json").read_text(encoding="utf-8"))


def stages(env):
    return [s["stage"] for s in evidence(env)["stages"]]


def tail(env):
    return env.log[env.log.index("open") + 1:]


def test_request_validation_and_path_resolution(env, tmp_path):
    for bad in ({"stop_requestt": "x"}, {"close_method": "all"}, {"assets": []}, {"on_unconfirmed": "maybe"}):
        with pytest.raises(ValueError):
            env.module.normalize_request(bad, env.work)
    for bad in ("5", True, False, float("nan"), float("inf"), float("-inf"), -0.5, [1]):
        with pytest.raises(ValueError, match="stop_after_seconds"):
            env.module.normalize_request({"stop_after_seconds": bad}, env.work)
    for good in (None, 0, 2, 0.25):
        assert env.module.normalize_request({"stop_after_seconds": good}, env.work)["stop_after_seconds"] == good
    req = env.module.normalize_request({"work_dir": str(tmp_path / "r5"), "stop_request": "stop.request"}, tmp_path)
    assert Path(req["stop_request"]) == tmp_path / "r5" / "stop.request"
    assert Path(req["evidence_path"]) == tmp_path / "r5" / "preview_launch.json"
    assert req["on_unconfirmed"] == "hold" and req["on_close_failure"] == "hold"


def test_nothing_closes_until_stop_then_drains_before_verify_keepalive_and_quit(env):
    session = start(env)
    tick(env, 4)
    assert session.state == "running" and tail(env) == []
    Path(session.req["stop_request"]).write_text("stop", encoding="utf-8")
    tick(env)
    assert tail(env) == ["close:%s:found=1" % ASSET]
    tick(env, 2)
    assert len(tail(env)) == 1 and session.state == "drain"
    tick(env, 2)
    assert tail(env) == ["close:%s:found=1" % ASSET, "close:%s:found=0" % ASSET]
    tick(env)
    assert tail(env)[-2:] == ["keepalive:False", "console:QUIT_EDITOR"]
    names = stages(env)
    assert names.index("close_called") < names.index("drained") < names.index("close_confirmed") < names.index("keepalive_lowered") < names.index("quit_requested")
    data = evidence(env)
    assert data["close_outcome"] == "confirmed" and data["stages"][names.index("close_called")]["editors_found"] == {ASSET: 1}
    assert not env.state["callbacks"]


def test_verify_call_that_still_finds_editors_retries_after_another_drain(env):
    env.state["subsystem"] = FakeSubsystem(env.log, closes_on_call=2)
    start(env, stop_after_seconds=0)
    tick(env, 12)
    found = [s["editors_found"][ASSET] for s in evidence(env)["stages"] if s["stage"] == "close_called"]
    assert found == [1, 1, 0]
    assert "close_retry" in stages(env) and evidence(env)["close_outcome"] == "confirmed"
    assert tail(env)[-2:] == ["keepalive:False", "console:QUIT_EDITOR"]


def test_persistent_failure_holds_without_keepalive_or_quit(env):
    env.state["subsystem"] = FakeSubsystem(env.log, closes_on_call=999)
    session = start(env, stop_after_seconds=0, max_close_attempts=2)
    tick(env, 20)
    assert session.state == "hold" and not env.state["callbacks"]
    assert sum(e.startswith("close:") for e in env.log) == 3
    assert "keepalive:False" not in env.log and not any(e.startswith("console:") for e in env.log)
    assert evidence(env)["close_outcome"] == "failed" and stages(env)[-2:] == ["close_failed", "held"]


def test_explicit_differential_opt_in_quits_after_failed_close(env):
    env.state["subsystem"] = FakeSubsystem(env.log, closes_on_call=999)
    start(env, stop_after_seconds=0, max_close_attempts=1, on_close_failure="quit")
    tick(env, 12)
    assert evidence(env)["close_outcome"] == "failed" and env.log[-2:] == ["keepalive:False", "console:QUIT_EDITOR"]


def test_non_integer_close_result_is_unconfirmed_and_holds_by_default(env):
    env.state["subsystem"] = FakeSubsystem(env.log, bad_return=True)
    session = start(env, stop_after_seconds=0)
    tick(env, 12)
    assert session.state == "hold" and evidence(env)["close_outcome"] == "unconfirmed"
    assert "console:QUIT_EDITOR" not in env.log and "keepalive:False" not in env.log


@pytest.mark.parametrize("first", [0, -1, True, None])
def test_first_close_call_without_a_positive_count_holds_even_if_later_calls_return_zero(env, first):
    other = "/Game/Anims/Other.Other"
    session = start(env, stop_after_seconds=0)
    sub = env.state["subsystem"]
    sub.assets[other] = FakeAsset(other)
    sub.open.update({ASSET: 0, other: 1})
    original = sub.close_all_editors_for_asset
    sub.close_all_editors_for_asset = lambda asset: (original(asset), first)[1]
    tick(env, 12)
    data = evidence(env)
    assert session.state == "hold" and data["close_outcome"] == "unconfirmed"
    assert [e for e in env.log if e.startswith("close:")] == ["close:%s:found=0" % ASSET]
    assert "close_confirmed" not in stages(env) and stages(env)[-2:] == ["close_unconfirmed", "held"]
    close_stage = next(s for s in data["stages"] if s["stage"] == "close_called")
    assert close_stage["editors_found"] == {ASSET: first}
    assert "keepalive:False" not in env.log and not any(e.startswith("console:") for e in env.log)
    assert sub.open[other] == 1


def test_every_owned_asset_needs_a_positive_first_count(env):
    other = "/Game/Anims/Other.Other"
    session = start(env, assets=[ASSET, other], stop_after_seconds=0)
    env.state["subsystem"].open[other] = 0
    tick(env, 12)
    assert session.state == "hold" and evidence(env)["close_outcome"] == "unconfirmed"
    assert next(s for s in evidence(env)["stages"] if s["stage"] == "close_called")["editors_found"] == {ASSET: 1, other: 0}
    assert "console:QUIT_EDITOR" not in env.log


def test_first_count_zero_holds_even_with_on_unconfirmed_quit(env):
    other = "/Game/Anims/Other.Other"
    session = start(env, stop_after_seconds=0, on_unconfirmed="quit")
    sub = env.state["subsystem"]
    sub.assets[other] = FakeAsset(other)
    sub.open.update({ASSET: 0, other: 1})
    tick(env, 12)
    assert session.state == "hold" and evidence(env)["close_outcome"] == "unconfirmed"
    assert "keepalive:False" not in env.log and not any(e.startswith("console:") for e in env.log)
    assert sub.open[other] == 1 and stages(env)[-2:] == ["close_unconfirmed", "held"]


def test_exposed_listing_that_disagrees_with_zero_count_blocks_quit(env):
    env.state["subsystem"] = FakeSubsystem(env.log, listing="stuck")
    session = start(env, stop_after_seconds=0, max_close_attempts=1)
    tick(env, 12)
    assert session.state == "hold" and evidence(env)["close_outcome"] == "failed"


def test_works_when_get_all_edited_assets_is_not_exposed(env):
    env.state["subsystem"] = FakeSubsystem(env.log, listing=None)
    session = start(env, stop_after_seconds=0)
    tick(env, 12)
    assert session.state == "quit_requested"
    assert next(s for s in evidence(env)["stages"] if s["stage"] == "close_confirmed")["listing_checked"] is False


def test_missing_close_api_refuses_to_open_unless_baseline_close_method_none(env):
    del env.fake.AssetEditorSubsystem.close_all_editors_for_asset
    with pytest.raises(RuntimeError, match="close_all_editors_for_asset"):
        start(env)
    assert "open" not in env.log and evidence(env)["launch_status"] == "error"
    start(env, close_method="none", stop_after_seconds=0)
    tick(env, 3)
    assert not any(e.startswith("close:") for e in env.log) and env.log[-2:] == ["keepalive:False", "console:QUIT_EDITOR"]
    assert "close_skipped" in stages(env)


def test_only_owned_assets_are_closed(env):
    other = "/Game/Anims/Other.Other"
    session = start(env, assets=[other], stop_after_seconds=0)
    tick(env, 12)
    assert session.owned_paths == [other]
    assert all(other in e for e in env.log if e.startswith("close:"))


def test_quit_after_close_false_leaves_keepalive_and_editor_alone(env):
    session = start(env, stop_after_seconds=0, quit_after_close=False)
    tick(env, 12)
    assert session.state == "closed_idle" and "keepalive:False" not in env.log


def test_preexisting_stop_request_is_ignored_until_rewritten(env):
    stop = env.work / "preview-stop.request"
    stop.write_text("old", encoding="utf-8")
    session = start(env)
    tick(env, 6)
    assert session.state == "running" and evidence(env)["stop_request_preexisting"] is True
    old_stamp = stop.stat().st_mtime_ns
    stop.unlink()
    stop.write_text("new", encoding="utf-8")
    os.utime(stop, ns=(old_stamp + 10**9, old_stamp + 10**9))
    tick(env, 12)
    assert session.state == "quit_requested"


def test_main_reads_request_from_argv_or_env_and_keeps_global_session(env, tmp_path):
    request = tmp_path / "req.json"
    request.write_text(json.dumps({"work_dir": str(env.work), "evidence_path": "evidence.json"}), encoding="utf-8")
    session = env.module.main(["x.py", str(request)], {}, tmp_path)
    assert env.module.SESSION is session and Path(session.req["work_dir"]) == env.work
    other = env.module.main(["x.py"], {env.module.REQUEST_ENV: str(request)}, tmp_path)
    assert env.module.SESSION is other
