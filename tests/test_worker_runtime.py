"""Worker authority, isolation, leases and engine probes without launching an Editor."""
import concurrent.futures
import json
import os
from pathlib import Path, PurePosixPath, PureWindowsPath
import subprocess
import sys
import threading
import time

import pytest

from unityanimbridge import blender_runner, default_humanoid, desktop, process, runtime


@pytest.fixture(autouse=True)
def clean_host_environment(monkeypatch):
    for name in list(os.environ):
        if name.upper().startswith("UAB_"):
            monkeypatch.delenv(name)
    process._STATE.__dict__.clear()
    yield
    process._STATE.__dict__.clear()


@pytest.fixture
def release(tmp_path):
    root = tmp_path / "private bundle"
    root.mkdir()
    arch = runtime.host_architecture()
    platform = runtime.host_platform()
    target = {"windows": "windows-x64", "macos": "macos-arm64", "linux": "linux-x64-ubuntu22.04"}[platform]
    entries = {"backend": ("resourcePath", "backend"), "ui": ("resourcePath", "ui"),
               "python": ("executablePath", "runtime/python/python"),
               "blender": ("executablePath", "runtime/blender/blender"),
               "supervisor": ("executablePath", "bin/supervisor"),
               "bootstrap": ("resourcePath", "backend/bootstrap.py")}
    components = {}
    for name, (key, value) in entries.items():
        path = root / value
        if name in ("backend", "ui"):
            path.mkdir(parents=True, exist_ok=True)
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"non-executed component fixture")
        components[name] = {key: value, "version": "1.0", "architecture": arch}
    (root / "backend/src/unityanimbridge").mkdir(parents=True)
    (root / "backend/src/unityanimbridge/__init__.py").write_text("", encoding="utf-8")
    manifest = {"schemaVersion": 1, "appVersion": "1.0", "targetId": target,
                "architecture": arch, "components": components}
    path = root / "runtime-manifest.json"
    path.write_text(json.dumps(manifest), encoding="utf-8")
    work = tmp_path / "approved work"
    env = {"UAB_RUNTIME_MODE": "bundled", "UAB_RUNTIME_ROOT": str(root), "UAB_RUNTIME_MANIFEST": str(path),
           "UAB_BACKEND_DIR": str(root / "backend"), "UAB_DATA_DIR": str(tmp_path / "data"),
           "UAB_CACHE_DIR": str(tmp_path / "cache"), "UAB_STATE_DIR": str(tmp_path / "state"),
           "UAB_LOG_DIR": str(tmp_path / "logs"), "UAB_WORK_DIR": str(work), "TMPDIR": str(work)}
    return root, path, manifest, env


def test_native_context_environment_and_deadline_fields_are_preserved(release):
    _, _, _, env = release
    env.update(UAB_STAGE_TIMEOUT_SECONDS="12", UAB_JOB_DEADLINE_MONOTONIC_MS="1234567", UAB_ATTEMPT_NONCE="a" * 32)
    context = runtime.RuntimeContext.from_environ(env)
    child = runtime.safe_env(context, environ=env, include_context=True)
    for key in ("UAB_DATA_DIR", "UAB_CACHE_DIR", "UAB_STATE_DIR", "UAB_LOG_DIR", "UAB_WORK_DIR",
                "UAB_STAGE_TIMEOUT_SECONDS", "UAB_JOB_DEADLINE_MONOTONIC_MS", "UAB_ATTEMPT_NONCE"):
        assert key in runtime.HOST_VARIABLES and child[key] == env[key]
    assert child["TMPDIR"] == child["TMP"] == child["TEMP"] == env["TMPDIR"]
    assert runtime.RuntimeContext.from_environ(child).temp_dir == context.temp_dir


@pytest.mark.parametrize("defect", ["escape", "architecture", "version", "target", "missing", "override"])
def test_release_manifest_and_override_validation_fail_closed(release, defect):
    root, path, manifest, env = release
    if defect == "escape":
        manifest["components"]["blender"]["executablePath"] = "../outside"
    elif defect == "architecture":
        manifest["components"]["blender"]["architecture"] = "wrong"
    elif defect == "version":
        manifest["components"]["python"].pop("version")
    elif defect == "target":
        manifest["targetId"] = "foreign-platform"
    elif defect == "missing":
        (root / "runtime/blender/blender").unlink()
    else:
        env["UAB_BLENDER"] = str(root.parent / "unapproved.exe")
    path.write_text(json.dumps(manifest), encoding="utf-8")
    with pytest.raises(runtime.RuntimeContextError):
        runtime.RuntimeContext.from_environ(env)


def test_manifest_outside_bundle_is_rejected_before_loading(release, tmp_path):
    _, _, _, env = release
    outside = tmp_path / "outside.json"
    outside.write_text("not JSON", encoding="utf-8")
    env["UAB_RUNTIME_MANIFEST"] = str(outside)
    with pytest.raises(runtime.RuntimeContextError, match="outside the bundle"):
        runtime.RuntimeContext.from_environ(env)


def test_child_environment_drops_loader_browser_and_python_overrides(release):
    _, _, _, env = release
    context = runtime.RuntimeContext.from_environ(env)
    poisoned = {**env, **{name: "hostile" for name in ("PYTHONHOME", "PYTHONPATH", "LD_PRELOAD",
                "DYLD_INSERT_LIBRARIES", "WEBKIT_INSPECTOR_SERVER", "WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", "UAB_BLENDER")}}
    original = dict(poisoned)
    child = runtime.safe_env(context, environ=poisoned)
    assert poisoned == original
    assert all(name not in child for name in poisoned if name.startswith(("PYTHON", "LD_", "DYLD_", "WEBKIT_", "WEBVIEW2_", "UAB_")))
    with pytest.raises(ValueError):
        runtime.safe_env(context, extra={"PYTHONPATH": "unapproved"})


@pytest.mark.parametrize("key", ["ALLUSERSPROFILE", "AllUsersProfile"])
def test_windows_child_environment_preserves_unity_package_manager_profile(release, key):
    _, _, _, env = release
    context = runtime.RuntimeContext.from_environ(env)
    inherited = {**env, key: r"C:\ProgramData", "SystemRoot": r"C:\Windows",
                 "PATH": r"C:\unapproved", "PYTHONPATH": "unapproved", "UAB_BLENDER": "unapproved"}
    child = runtime.safe_env(context, environ=inherited, platform="windows")
    assert child[key] == r"C:\ProgramData"
    assert child["PATH"] == r"C:\Windows\System32;C:\Windows;C:\Windows\System32\Wbem"
    assert "PYTHONPATH" not in child and "UAB_BLENDER" not in child


def test_blender_environment_honors_fixed_python_flags_without_inheriting_overrides(tmp_path):
    trusted, hostile = tmp_path / "trusted 한글", tmp_path / "hostile"
    trusted.mkdir()
    hostile.mkdir()
    (trusted / "uab_env_probe.py").write_text("VALUE = 'trusted'", encoding="utf-8")
    (hostile / "uab_env_probe.py").write_text("raise RuntimeError('PYTHONPATH was inherited')", encoding="utf-8")
    poisoned = {**os.environ, "PYTHONPATH": str(hostile), "PYTHONHOME": str(hostile),
                "PYTHONUSERBASE": str(hostile), "PYTHONDONTWRITEBYTECODE": "0", "PYTHONNOUSERSITE": "0",
                "PYTHONUTF8": "0", "PYTHONSTARTUP": str(hostile / "startup.py"), "pYtHoNpAtH": str(hostile),
                "LD_PRELOAD": str(hostile / "bad.so"), "DYLD_INSERT_LIBRARIES": str(hostile / "bad.dylib"),
                "UAB_BACKEND_DIR": str(hostile), "UAB_JOB_TIMEOUT_SECONDS": "1", "LC_CTYPE": "C.UTF-8"}
    context = runtime.RuntimeContext.from_environ({})
    original = dict(poisoned)
    child = runtime.blender_env(context, temp=trusted, environ=poisoned)
    assert poisoned == original
    assert {key: value for key, value in child.items() if key.upper().startswith("PYTHON")} == {
        "PYTHONDONTWRITEBYTECODE": "1", "PYTHONNOUSERSITE": "1", "PYTHONUTF8": "1"}
    assert not any(key.upper().startswith(("LD_", "DYLD_", "UAB_")) for key in child)
    assert child["TMPDIR"] == child["TMP"] == child["TEMP"] == str(trusted)
    if context.platform != "windows":
        assert child["LC_CTYPE"] == "C.UTF-8"
    command = [sys.executable, "-c", "import json,os,sys,uab_env_probe; print(json.dumps([uab_env_probe.VALUE,sys.dont_write_bytecode,sys.flags.no_user_site,sys.flags.utf8_mode,os.environ['TMPDIR']]))"]
    result = subprocess.run(command, cwd=trusted, env=child, capture_output=True, text=True, encoding="utf-8", check=True)
    assert json.loads(result.stdout) == ["trusted", True, 1, 1, str(trusted)]
    assert not list(trusted.rglob("*.pyc"))


@pytest.mark.parametrize("launcher", ["preview", "convert"])
def test_blender_launchers_opt_in_to_sanitized_python_environment(tmp_path, monkeypatch, launcher):
    for name in ("PYTHONHOME", "PYTHONPATH", "PYTHONUSERBASE", "PYTHONSTARTUP", "LD_PRELOAD", "DYLD_INSERT_LIBRARIES"):
        monkeypatch.setenv(name, str(tmp_path / "hostile"))
    for name in ("PYTHONDONTWRITEBYTECODE", "PYTHONNOUSERSITE", "PYTHONUTF8"):
        monkeypatch.setenv(name, "0")
    monkeypatch.setenv("UAB_DATA_DIR", str(tmp_path / "data"))
    monkeypatch.setenv("UAB_BACKEND_DIR", str(tmp_path / "backend"))
    monkeypatch.setenv("UAB_BLENDER", str(tmp_path / "hostile-blender"))
    original = dict(os.environ)
    selected = tmp_path / "approved 한글 Blender"
    work = tmp_path / "work 한글"
    work.mkdir()
    calls = []
    def run(command, **kwargs):
        assert command[0] == str(selected)
        assert command.count("--python-use-system-env") == 1
        assert command.index("--python-use-system-env") < command.index("--python")
        assert "--factory-startup" in command
        env = kwargs["env"]
        assert {key: value for key, value in env.items() if key.upper().startswith("PYTHON")} == {
            "PYTHONDONTWRITEBYTECODE": "1", "PYTHONNOUSERSITE": "1", "PYTHONUTF8": "1"}
        assert not any(key.upper().startswith(("UAB_", "LD_", "DYLD_")) for key in env)
        assert Path(env["TMPDIR"]).is_relative_to(work)
        assert env["TMPDIR"] == env["TMP"] == env["TEMP"]
        if launcher == "preview":
            result = Path(command[-1])
        else:
            job = json.loads(Path(command[-1]).read_text(encoding="utf-8"))
            result = Path(job["result"])
        result.write_text(json.dumps({"status": "ok", "value": "한글"}), encoding="utf-8")
        calls.append(command)
        return subprocess.CompletedProcess(command, 0)
    monkeypatch.setattr(process, "run_process", run)
    monkeypatch.setattr(desktop, "find_blender", lambda _: selected)
    if launcher == "preview":
        result = desktop._blender(tmp_path / "source 한글.fbx", "preview", work, {})
    else:
        result = blender_runner.run_job({"source": str(tmp_path / "source 한글.anim")}, selected, log_dir=work)
    assert result["value"] == "한글" and len(calls) == 1 and dict(os.environ) == original


def test_os_directory_contract_matches_native_roots():
    linux = runtime.default_dirs("linux", {"XDG_DATA_HOME": "relative", "XDG_CACHE_HOME": "/cache"}, "/" + "home/user")
    assert linux["data_dir"] == PurePosixPath("/" + "home/user/.local/share/AnimBridge")
    assert linux["cache_dir"] == PurePosixPath("/cache/AnimBridge")
    assert linux["work_dir"] == linux["cache_dir"] / "jobs"
    mac = runtime.default_dirs("macos", {}, "/" + "Users/user")
    assert mac["state_dir"] == mac["data_dir"] == PurePosixPath("/" + "Users/user/Library/Application Support/AnimBridge")
    windows = runtime.default_dirs("windows", {"LOCALAPPDATA": "C:/Local"}, "C:/Users/user")
    assert windows["state_dir"] == windows["data_dir"] == PureWindowsPath("C:/Local/AnimBridge")


def test_worker_argv_is_isolated_and_keeps_special_arguments(release):
    root, _, _, env = release
    context = runtime.RuntimeContext.from_environ(env)
    tail = ["-m", "unityanimbridge.cli", "convert", "space 한글 & $(literal)", "--clip=-Walk"]
    command = runtime.python_worker_command(context, tail)
    assert command[1:5] == ["-I", "-B", "-X", "utf8"]
    assert command[5:9] == [str(root / "backend/bootstrap.py"), "--backend", str(root / "backend/src"), "--"]
    assert command[9:] == tail


def test_nested_stages_pause_and_resume_without_extending_deadlines(monkeypatch):
    now = [1000.0]
    events = []
    monkeypatch.setattr(process.time, "monotonic", lambda: now[0])
    monkeypatch.setattr(process._LEASE, "send", lambda event: events.append(dict(event)))
    with process.job(100):
        with process.stage("outer", 30):
            now[0] += 3
            with process.stage("inner", 20):
                now[0] += 5
            assert process._STATE.stage.deadline == 1030
    assert [(event["event"], event.get("name")) for event in events] == [
        ("stage-start", "outer"), ("stage-end", None), ("stage-start", "inner"),
        ("stage-end", None), ("stage-start", "outer"), ("stage-end", None)]
    assert events[4]["budgetSeconds"] == 22


def test_expired_outer_stage_is_not_restarted_with_artificial_time(monkeypatch):
    now, events = [1000.0], []
    monkeypatch.setattr(process.time, "monotonic", lambda: now[0])
    monkeypatch.setattr(process._LEASE, "send", lambda event: events.append(dict(event)))
    with pytest.raises(process.ProcessTimeout):
        with process.stage("outer", 10):
            with process.stage("inner", 3):
                now[0] += 11
    assert [event["event"] for event in events] == ["stage-start", "stage-end", "stage-start", "stage-end"]


def test_host_deadline_survives_new_jobs_and_worker_restarts(monkeypatch):
    now = [1000.0]
    monkeypatch.setattr(process.time, "monotonic", lambda: now[0])
    monkeypatch.setenv("UAB_JOB_DEADLINE_MONOTONIC_MS", "1500000")
    with process.job(1000):
        assert process.job_remaining() == 500
    now[0] = 1100
    with process.job(1000):
        assert process.job_remaining() == 400
    now[0] = 1501
    with pytest.raises(process.ProcessTimeout) as error:
        with process.job(1000):
            pytest.fail("Expired host deadline must not start work")
    assert error.value.layer == "job"
    monkeypatch.setenv("UAB_JOB_DEADLINE_MONOTONIC_MS", "nan")
    with pytest.raises(ValueError, match="host item deadline"):
        process.host_deadline()


def test_cpu_only_stage_emits_heartbeats_without_extending_budget(monkeypatch):
    events = []
    monkeypatch.setattr(process, "HEARTBEAT_SECONDS", .01)
    monkeypatch.setattr(process._LEASE, "send", lambda event: events.append(dict(event)))
    with process.stage("parse", 1):
        original = process._STATE.stage.deadline
        time.sleep(.04)
        assert process._STATE.stage.deadline == original
    assert any(event["event"] == "heartbeat" for event in events)
    assert events[-1]["event"] == "stage-end"


def test_scratch_context_is_independent_in_concurrent_read_workers(tmp_path):
    barrier = threading.Barrier(2)
    original = dict(os.environ)
    def worker(name):
        path = tmp_path / name
        with runtime.scratch(path):
            barrier.wait(timeout=3)
            assert runtime.scratch_dir() == path
            with runtime.scratch(path / "inner"):
                assert runtime.scratch_dir() == path / "inner"
            assert runtime.scratch_dir() == path
        assert runtime.scratch_dir() is None
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        list(pool.map(worker, ["first", "second"]))
    assert runtime.scratch_dir() is None and dict(os.environ) == original


def test_process_launch_uses_argument_array_and_explicit_windows_stdio(monkeypatch):
    captured = {}
    class Child:
        def __init__(self, command, **kwargs):
            captured.update(command=command, **kwargs)
        def wait(self, timeout=None):
            return 0
        def poll(self):
            return 0
    monkeypatch.setattr(process.subprocess, "Popen", Child)
    command = [str(Path(sys.executable).resolve()), "literal & $(x)", "한글 space"]
    assert process.run_process(command, budget=1).returncode == 0
    assert captured["command"] == command and captured["shell"] is False and captured["close_fds"] is True
    if os.name == "nt":
        assert captured["creationflags"] == subprocess.CREATE_NO_WINDOW
        assert all(captured[name] == subprocess.DEVNULL for name in ("stdin", "stdout", "stderr"))


def test_editor_probe_does_not_trust_version_in_path(tmp_path, monkeypatch):
    editor = tmp_path / "6000.4.7f1/Editor/Unity.exe"
    editor.parent.mkdir(parents=True)
    editor.write_bytes(b"not a Unity binary")
    monkeypatch.setattr(runtime, "check_executable", lambda *_: [])
    monkeypatch.setattr(runtime, "_windows_product_version", lambda _: None)
    invalid = runtime.probe_unity(editor, "windows", probe_command=lambda _: "unrelated executable")
    assert invalid["state"] == "invalid" and invalid["version"] is None
    verified = runtime.probe_unity(editor, "windows", probe_command=lambda _: "6000.4.7f1")
    assert verified["state"] == "ready" and verified["version"] == runtime.UNITY_BASELINE_VERSION


def test_explicit_only_engine_policy_never_discovers_or_uses_overrides(release, monkeypatch):
    _, _, _, env = release
    context = runtime.RuntimeContext.from_environ(env)
    monkeypatch.setattr(runtime, "current_context", lambda: context)
    monkeypatch.setattr(runtime, "hub_editor_candidates", lambda *_: pytest.fail("Discovery cannot grant execution"))
    monkeypatch.setenv("UAB_UNITY", "/unapproved/Unity")
    monkeypatch.setenv("UAB_UNREAL_ENGINE", "/unapproved/Unreal")
    assert runtime.locate_unity({}, context=context) is None
    assert list(default_humanoid._engines({})) == []
    development = runtime.RuntimeContext("development", context.platform, context.data_dir, context.cache_dir,
                                         context.state_dir, context.log_dir, context.work_dir, context.backend_dir)
    monkeypatch.setattr(runtime, "current_context", lambda: development)
    request = {"_native_editor_policy": "explicit-only"}
    assert runtime.locate_unity(request, context=development) is None
    assert list(default_humanoid._engines(request)) == []


def test_unreal_version_comes_from_selected_installation(tmp_path):
    engine = tmp_path / "UE_5.8"
    editor = runtime.unreal_cmd_path(engine)
    editor.parent.mkdir(parents=True)
    editor.write_bytes(b"non-executed fixture")
    editor.chmod(0o755)
    version = engine / "Engine/Build/Build.version"
    version.parent.mkdir(parents=True)
    version.write_text(json.dumps({"MajorVersion": 5, "MinorVersion": 8, "PatchVersion": 2}), encoding="utf-8")
    assert runtime.probe_unreal(editor)["version"] == runtime.UNREAL_BASELINE_VERSION
    version.unlink()
    assert runtime.probe_unreal(editor)["state"] == "invalid"


def test_unity_helper_project_uses_verified_editor_version(tmp_path, monkeypatch):
    editor = tmp_path / "chosen-editor"
    editor.write_bytes(b"not executed")
    monkeypatch.setattr(runtime, "probe_unity", lambda _: {"state": "ready", "version": "6000.4.7f1"})
    def run(command, **kwargs):
        project = Path(command[command.index("-projectPath") + 1])
        assert "m_EditorVersion: 6000.4.7f1" in (project / "ProjectSettings/ProjectVersion.txt").read_text()
        request = json.loads(Path(command[command.index("-animBridgeDesktopRequest") + 1]).read_text())
        result = Path(command[command.index("-animBridgeDesktopResult") + 1])
        result.write_text(json.dumps({"status": "ok", "connected": True, "session": request["session"], "helper_version": desktop.HELPER_VERSION}))
        assert kwargs["env"]["TMPDIR"] == str(result.parent / "temp")
        return subprocess.CompletedProcess(command, 0)
    monkeypatch.setattr(process, "run_process", run)
    result = desktop.helper_test({"unity_editor": str(editor), "cache": str(tmp_path / "cache")})
    assert result["isolated"] and result["editor_version"] == "6000.4.7f1"


def test_bootstrap_isolates_cwd_and_pythonpath(tmp_path):
    source = tmp_path / "trusted/src/unityanimbridge"
    source.mkdir(parents=True)
    (source / "__init__.py").write_text("")
    (source / "cli.py").write_text("import json,sys; print(json.dumps({'file':__file__,'isolated':sys.flags.isolated,'no_pyc':sys.dont_write_bytecode,'argv':sys.argv[1:]}))")
    hostile = tmp_path / "hostile/unityanimbridge"
    hostile.mkdir(parents=True)
    marker = tmp_path / "executed"
    (hostile / "__init__.py").write_text(f"open({str(marker)!r},'w').write('bad')")
    bootstrap = Path(runtime.__file__).with_name("bootstrap.py")
    env = {**os.environ, "PYTHONPATH": str(hostile.parent), "PYTHONHOME": str(tmp_path / "poison")}
    command = [sys.executable, "-I", "-B", "-X", "utf8", str(bootstrap), "--backend", str(source.parent), "--", "-m", "unityanimbridge.cli", "literal 한글 & $(x)"]
    result = subprocess.run(command, cwd=hostile.parent, env=env, capture_output=True, text=True, encoding="utf-8", check=True)
    data = json.loads(result.stdout)
    assert Path(data["file"]) == source / "cli.py" and data["isolated"] == 1 and data["no_pyc"]
    assert data["argv"] == ["literal 한글 & $(x)"] and not marker.exists()
    assert not list(source.rglob("*.pyc"))


def test_release_bootstrap_rejects_backend_override_before_import(release, tmp_path):
    _, _, _, host = release
    hostile = tmp_path / "outside/unityanimbridge"
    hostile.mkdir(parents=True)
    marker = tmp_path / "executed"
    (hostile / "__init__.py").write_text(f"open({str(marker)!r},'w').write('bad')")
    bootstrap = Path(runtime.__file__).with_name("bootstrap.py")
    command = [sys.executable, "-I", "-B", str(bootstrap), "--backend", str(hostile.parent), "--", "-m", "unityanimbridge.cli"]
    result = subprocess.run(command, env={**os.environ, **host}, capture_output=True, text=True)
    assert result.returncode == 2 and "backend override" in result.stderr and not marker.exists()


@pytest.fixture
def approved_unreal(tmp_path, tmp_path_factory):
    engine = tmp_path / "approved UE"
    editor = runtime.unreal_cmd_path(engine)
    version = engine / "Engine/Build/Build.version"
    template = engine / "Templates/TemplateResources/High/Characters/Content/Mannequins/Meshes"
    editor.parent.mkdir(parents=True)
    editor.write_bytes(b"approved, non-executed editor")
    editor.chmod(0o755)
    version.parent.mkdir(parents=True)
    version.write_text(json.dumps({"MajorVersion": 5, "MinorVersion": 8, "PatchVersion": 2}))
    template.mkdir(parents=True)
    files = [template / "SKM_Manny_Simple.uasset", template / "SK_Mannequin.uasset"]
    for path in files:
        path.write_bytes(path.name.encode())
    def record(path):
        return {"path": str(path), "sha256": default_humanoid.file_digest(path)}
    cap = {"engineRoot": str(engine), "version": "5.8.2", "editor": record(editor),
           "versionFile": record(version), "templates": [record(path) for path in files]}
    return {"_native_editor_policy": "explicit-only", "unreal_editor": str(editor),
            "_native_unreal_capability": cap, "cache": str(tmp_path_factory.mktemp("mc") / "cache")}, files


def test_manny_requires_host_capability_and_rejects_changed_templates(approved_unreal, monkeypatch):
    request, files = approved_unreal
    context = runtime.current_context()
    monkeypatch.setattr(default_humanoid, "BUNDLED_CHARACTER", Path(request["cache"]).parent / "missing.fbx")
    with pytest.raises(default_humanoid.UnrealApprovalRequired):
        default_humanoid.resolve_default({"_native_editor_policy": "explicit-only"})
    assert default_humanoid._verified_native_unreal(request, context)[2] == Path(request["unreal_editor"])
    files[0].write_bytes(b"replaced template")
    monkeypatch.setattr(process, "run_process", lambda *_a, **_k: pytest.fail("Changed capability must not execute an engine"))
    with pytest.raises(default_humanoid.UnrealApprovalRequired, match="changed"):
        default_humanoid.resolve_default(request)
    assert not Path(request["cache"]).exists()


def test_manny_approved_template_workflow_preserves_isolated_project(approved_unreal, monkeypatch):
    request, files = approved_unreal
    calls = []
    def export(command, **kwargs):
        assert command[0] == request["_native_unreal_capability"]["editor"]["path"]
        project = Path(command[1])
        copied = list((project.parent / "Content").rglob("*.uasset"))
        assert {path.name for path in copied} == {path.name for path in files}
        assert all(default_humanoid.file_digest(path) == next(entry["sha256"] for entry in request["_native_unreal_capability"]["templates"] if Path(entry["path"]).name == path.name) for path in copied)
        argument = next(value for value in command if value.startswith("-animBridgeMannequin="))
        config = json.loads(Path(argument.split("=", 1)[1]).read_text())
        model = Path(config["output"])
        model.write_bytes(b"locally evaluated Manny FBX")
        Path(config["report"]).write_text(json.dumps({"status": "ok", "sha256": default_humanoid.file_digest(model)}))
        assert kwargs["env"]["TMPDIR"] == str(project.parent.parent / "temp")
        calls.append(command)
        return subprocess.CompletedProcess(command, 0)
    monkeypatch.setattr(process, "run_process", export)
    first = default_humanoid.resolve_default(request)
    again = default_humanoid.resolve_default(request)
    assert first["locally_prepared"] and again["model"] == first["model"] and len(calls) == 1
