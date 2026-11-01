"""Default character preparation and content-bound Unity preview caching."""
import json
import struct
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import time
import threading

import pytest

from unityanimbridge import desktop, default_humanoid


def human_clip():
    class Clip:
        is_humanoid = True
        channels = {}
    return Clip()


@pytest.fixture
def preview_request(tmp_path, monkeypatch):
    source, model = tmp_path / "Human.anim", tmp_path / "Manny.fbx"
    source.write_text("%YAML 1.1\n", encoding="utf-8")
    model.write_bytes(b"actual-model-supplied-to-mocked-engine")
    monkeypatch.setattr(desktop, "parse_anim", lambda source: human_clip())
    monkeypatch.setattr(default_humanoid, "resolve_default", lambda request: {
        "model": str(model), "name": "UE5 Mannequin (Manny)", "human_mapping": {"Hips": "pelvis"}})
    return {"source": str(source), "cache": str(tmp_path / "cache"), "include_mesh": True}, model


def fake_engine(calls):
    def run(request, operation, work):
        calls.append((dict(request), operation))
        output = work / "sample.json"
        output.write_text(json.dumps({"status": "ready", "fps": 30, "duration": 1, "frames": [[[0, 1, 0]]],
            "bones": [{"name": "pelvis", "parent": -1}], "mesh": {"kind": "evaluated-skinned-mesh"}}), encoding="utf-8")
        return {"status": "ok", "preview_file": str(output), "avatar": {"is_human": True, "is_valid": True}}
    return run


def test_humanoid_uses_manny_and_one_validating_engine_call(preview_request, monkeypatch):
    request, model = preview_request
    calls = []
    monkeypatch.setattr(desktop, "_unity", fake_engine(calls))
    monkeypatch.setattr(desktop, "validate", lambda request: pytest.fail("preview must not run a second validation process"))
    result = desktop.preview(request)
    assert result["status"] == "ready" and result["default_character"]["name"] == "UE5 Mannequin (Manny)"
    assert result["mesh"]["kind"] == "evaluated-skinned-mesh"
    assert len(calls) == 1 and calls[0][1] == "preview"
    assert calls[0][0]["skeleton"] == str(model) and calls[0][0]["human_mapping"] == {"Hips": "pelvis"}
    again = desktop.preview(request)
    assert again["cache_hit"] and len(calls) == 1


def test_explicit_character_wins_and_mapping_model_meta_invalidate_cache(preview_request, monkeypatch):
    request, model = preview_request
    calls = []
    monkeypatch.setattr(desktop, "_unity", fake_engine(calls))
    monkeypatch.setattr(default_humanoid, "resolve_default", lambda request: pytest.fail("explicit model must win"))
    request.update(skeleton=str(model), human_mapping={"Hips": "ChosenHips"})
    assert "default_character" not in desktop.preview(request)
    assert desktop.preview(request)["cache_hit"]
    request["human_mapping"] = {"Hips": "AnotherHips"}
    assert not desktop.preview(request).get("cache_hit")
    model.write_bytes(b"changed-model-content")
    assert not desktop.preview(request).get("cache_hit")
    Path(str(model) + ".meta").write_text("changed model importer", encoding="utf-8")
    assert not desktop.preview(request).get("cache_hit")
    request["max_frames"] = 12
    assert not desktop.preview(request).get("cache_hit")
    assert len(calls) == 5


def test_failed_avatar_result_is_not_cached(preview_request, monkeypatch):
    request, model = preview_request
    calls = []
    def fail(request, operation, work):
        calls.append(operation)
        return {"status": "needsAvatar", "reason": "invalid actual Avatar"}
    monkeypatch.setattr(desktop, "_unity", fail)
    assert desktop.preview(request)["status"] == "blocked"
    assert desktop.preview(request)["reason"] == "invalid actual Avatar"
    assert calls == ["preview", "preview"]


def test_concurrent_identical_requests_share_one_evaluation(preview_request, monkeypatch):
    request, model = preview_request
    calls = []
    evaluate = fake_engine(calls)
    def delayed(*args):
        time.sleep(.05)
        return evaluate(*args)
    monkeypatch.setattr(desktop, "_unity", delayed)
    with ThreadPoolExecutor(max_workers=2) as pool:
        results = list(pool.map(desktop.preview, [request, request]))
    assert all(result["status"] == "ready" for result in results)
    assert len(calls) == 1 and sum(bool(result.get("cache_hit")) for result in results) == 1


def test_changed_model_during_sampling_is_not_cached(preview_request, monkeypatch):
    request, model = preview_request
    evaluate = fake_engine([])
    def changed(*args):
        result = evaluate(*args)
        model.write_bytes(b"model replaced during engine evaluation")
        return result
    monkeypatch.setattr(desktop, "_unity", changed)
    result = desktop.preview(request)
    assert result["status"] == "blocked" and result["code"] == "sourceChanged"
    assert not list(Path(request["cache"]).glob("preview-results/*.json"))


def test_bundled_character_is_the_default_without_unreal(monkeypatch):
    monkeypatch.delenv("UAB_DEFAULT_HUMANOID_MODEL", raising=False)
    monkeypatch.setattr(default_humanoid, "_engines", lambda request: pytest.fail("no engine discovery"))
    for request in ({}, {"_native_editor_policy": "explicit-only"}):
        result = default_humanoid.resolve_default(request)
        assert result["bundled"] and result["name"] == "Mocapture Humanoid"
        assert Path(result["model"]).is_file() and Path(result["model"]).suffix == ".fbx"
        assert result["human_mapping"]["Hips"] == "Hips" and result["human_mapping"]["LeftToes"] == "LeftToes"


def test_missing_local_ue_template_explains_requirement(tmp_path, monkeypatch):
    monkeypatch.delenv("UAB_DEFAULT_HUMANOID_MODEL", raising=False)
    monkeypatch.setattr(default_humanoid, "BUNDLED_CHARACTER", tmp_path / "missing.fbx")
    monkeypatch.setattr(default_humanoid, "_engines", lambda request: iter(()))
    with pytest.raises(ValueError, match="installed Unreal Engine 5 template"):
        default_humanoid.resolve_default({"cache": str(tmp_path)})


def test_engine_discovery_reads_custom_launcher_installation(tmp_path, monkeypatch):
    for key in ("UAB_UNREAL_ENGINE", "UAB_UNREAL_EDITOR"):
        monkeypatch.delenv(key, raising=False)
    monkeypatch.setenv("ProgramData", str(tmp_path / "ProgramData"))
    monkeypatch.setenv("ProgramFiles", str(tmp_path / "ProgramFiles"))
    manifest = tmp_path / "ProgramData/Epic/UnrealEngineLauncher/LauncherInstalled.dat"
    manifest.parent.mkdir(parents=True)
    engine = tmp_path / "CustomDrive/UE_5.8"
    manifest.write_text(json.dumps({"InstallationList": [{"AppName": "UE_5.8", "InstallLocation": str(engine)}]}), encoding="utf-8")
    assert list(default_humanoid._engines({})) == [engine]


def test_default_preparation_refuses_existing_unity_project(preview_request, monkeypatch, tmp_path):
    request, model = preview_request
    (tmp_path / "Assets").mkdir()
    (tmp_path / "ProjectSettings").mkdir()
    request["cache"] = str(tmp_path / "cache")
    monkeypatch.setattr(default_humanoid, "resolve_default", lambda request: pytest.fail("cache must be checked first"))
    with pytest.raises(ValueError, match="existing Unity project"):
        desktop.preview(request)


def test_humanoid_fbx_uses_target_avatar_instead_of_source_bones(preview_request, monkeypatch):
    request, model = preview_request
    source = Path(request["source"]).with_suffix(".fbx")
    source.write_bytes(b"FBX source fixture")
    Path(str(source) + ".meta").write_text("ModelImporter:\n  animationType: 3\n", encoding="utf-8")
    request["source"] = str(source)
    calls = []
    monkeypatch.setattr(desktop, "_unity", fake_engine(calls))
    result = desktop.preview(request)
    assert result["status"] == "ready" and calls[0][0]["humanoid"]
    assert calls[0][0]["skeleton"] == str(model)


def test_copied_meta_removal_does_not_remove_generated_importer(tmp_path):
    source = tmp_path / "source/Manny.fbx"
    source.parent.mkdir()
    source.write_bytes(b"same FBX bytes")
    meta = Path(str(source) + ".meta")
    meta.write_text("source importer", encoding="utf-8")
    target = tmp_path / "project/Assets/Data/Model"
    provenance = tmp_path / "project/.animbridge-copies"
    copied = desktop._copy_data(source, target, provenance)
    copied_meta = Path(str(copied) + ".meta")
    assert copied_meta.read_text() == "source importer"
    meta.unlink()
    desktop._copy_data(source, target, provenance)
    assert not copied_meta.exists()
    copied_meta.write_text("generated Unity Avatar importer", encoding="utf-8")
    desktop._copy_data(source, target, provenance)
    assert copied_meta.read_text() == "generated Unity Avatar importer"
    source.write_bytes(b"changed FBX bytes")
    desktop._copy_data(source, target, provenance)
    assert not copied_meta.exists()


def test_two_cold_clips_acquire_different_model_project_slots(tmp_path, monkeypatch):
    model = tmp_path / "Manny.fbx"
    model.write_bytes(b"model fixture")
    barrier = threading.Barrier(2)
    projects = []
    def run(request, operation, work):
        projects.append(request["_preview_project"])
        barrier.wait(timeout=3)
        return {"status": "ok"}
    monkeypatch.setattr(desktop, "_unity_run", run)
    request = {"skeleton": str(model), "cache": str(tmp_path / "cache")}
    with ThreadPoolExecutor(max_workers=2) as pool:
        results = list(pool.map(lambda clip: desktop._unity(dict(request, clip=clip), "preview", tmp_path), ["Walk", "Run"]))
    assert all(result["status"] == "ok" for result in results)
    assert len(set(projects)) == 2 and {Path(project).parent.name for project in projects} == {"slot-0", "slot-1"}


def test_project_slots_are_separated_by_editor_version(tmp_path, monkeypatch):
    model, editor = tmp_path / "Manny.fbx", tmp_path / "Unity.exe"
    model.write_bytes(b"model fixture")
    editor.write_bytes(b"editor version one")
    projects = []
    monkeypatch.setattr(desktop, "_unity_run", lambda request, operation, work: projects.append(request["_preview_project"]))
    request = {"skeleton": str(model), "cache": str(tmp_path / "cache"), "unity_editor": str(editor)}
    desktop._unity(request, "preview", tmp_path)
    editor.write_bytes(b"newer editor version two")
    desktop._unity(request, "preview", tmp_path)
    assert len(set(projects)) == 2


def test_reused_slot_removes_past_clip_guids_only_inside_owned_folder(tmp_path):
    project = tmp_path / "project"
    clips = project / "Assets/Data/Clips"
    clips.mkdir(parents=True)
    for name in ("Previous.fbx", "Previous.fbx.meta", "Current.fbx", "Current.fbx.meta"):
        (clips / name).write_text(name, encoding="utf-8")
    model = project / "Assets/Data/Model/Manny.fbx"
    model.parent.mkdir()
    model.write_bytes(b"model")
    desktop._clear_old_clips(project, "Current.fbx")
    assert {path.name for path in clips.iterdir()} == {"Current.fbx", "Current.fbx.meta"}
    assert model.read_bytes() == b"model"
    desktop._clear_old_clips(project, None)
    assert list(clips.iterdir()) == []


def _fbx_prop(value):
    if isinstance(value, int):
        return b"L" + struct.pack("<q", value)
    data = value.encode()
    return b"S" + struct.pack("<I", len(data)) + data


def _fbx_node(name, props, children, base):
    body = b"".join(_fbx_prop(value) for value in props)
    head = 25 + len(name)
    blob, position = b"", base + head + len(body)
    for child in children:
        piece = _fbx_node(*child, position)
        blob, position = blob + piece, position + len(piece)
    if children:
        blob, position = blob + b"\0" * 25, position + 25
    return struct.pack("<QQQB", position, len(props), len(body), len(name)) + name.encode() + body + blob


def write_fbx(path, skin, setup=None, guid=None, source=None):
    """Minimal binary FBX 7.5 with an optional Skin deformer; `setup` also writes an importer .meta."""
    objects = [("Model", [100, "Hips\0\x01Model", "LimbNode"], [])]
    if skin:
        objects.append(("Deformer", [900, "Skin\0\x01Deformer", "Skin"], []))
    header = b"Kaydara FBX Binary  \0\x1a\0" + struct.pack("<I", 7500)
    settings = _fbx_node("GlobalSettings", [], [], len(header))
    path.write_bytes(header + settings + _fbx_node("Objects", [], objects, len(header) + len(settings)))
    if setup is not None:
        meta = f"fileFormatVersion: 2\nguid: {guid}\nModelImporter:\n  animationType: 3\n  avatarSetup: {setup}\n"
        if source:
            meta += "  lastHumanDescriptionAvatarSource: {fileID: 9000000, guid: " + source + ", type: 3}\n"
        Path(str(path) + ".meta").write_text(meta, encoding="utf-8")
    return path


def test_fbx_has_skinned_mesh_reads_the_skin_deformer(tmp_path):
    skinned = write_fbx(tmp_path / "skinned.fbx", True)
    assert default_humanoid.fbx_has_skinned_mesh(skinned)
    assert not default_humanoid.fbx_has_skinned_mesh(write_fbx(tmp_path / "bare.fbx", False))
    (tmp_path / "ascii.fbx").write_text("; FBX 7.4.0 project file", encoding="utf-8")
    assert not default_humanoid.fbx_has_skinned_mesh(tmp_path / "ascii.fbx")
    skinned.write_bytes(skinned.read_bytes()[:40])
    assert not default_humanoid.fbx_has_skinned_mesh(skinned)
    assert not default_humanoid.fbx_has_skinned_mesh(tmp_path / "missing.fbx")


def test_embedded_default_follows_the_clips_avatar_source(tmp_path):
    packages = tmp_path / "cache/packages"
    folder = packages / "package-1/assets"
    folder.mkdir(parents=True)
    owner_guid = "a" * 32
    model = write_fbx(folder / "model.fbx", True, 1, owner_guid)
    write_fbx(folder / "decoy.fbx", True, 1, "b" * 32)
    copied = write_fbx(folder / "idle.fbx", False, 2, "c" * 32, owner_guid)
    own = write_fbx(folder / "own.fbx", True, 1, "d" * 32)
    request = {"humanoid": True}

    def choose(clip):
        return default_humanoid.embedded_default(dict(request, source=str(clip)), packages_root=packages)

    # A clip that builds its avatar from its own skinned model uses itself.
    chosen = choose(own)
    assert chosen["model"] == str(own.resolve()) and chosen["embedded"] and chosen["human_mapping"] == {}
    # A clip that copies an avatar uses the asset owning that GUID, not a lookalike.
    assert choose(copied)["model"] == str(model.resolve())
    # An owner without a skin, or one that itself copies an avatar, is not a usable character.
    write_fbx(folder / "model.fbx", False, 1, owner_guid)
    assert choose(copied) is None
    write_fbx(folder / "model.fbx", True, 2, owner_guid, "b" * 32)
    assert choose(copied) is None
    # Outside the app's package cache nothing is looked up, because the host granted no access there.
    loose = tmp_path / "loose"
    loose.mkdir()
    write_fbx(loose / "model.fbx", True, 1, owner_guid)
    assert choose(write_fbx(loose / "idle.fbx", False, 2, "c" * 32, owner_guid)) is None
    # Without importer metadata, for a non-Humanoid request or a non-FBX clip the regular default applies.
    assert choose(write_fbx(folder / "nometa.fbx", True)) is None
    assert default_humanoid.embedded_default({"source": str(own), "humanoid": False}, packages_root=packages) is None
    assert choose(tmp_path / "clip.anim") is None


def test_preview_uses_the_embedded_character_before_any_default(tmp_path, monkeypatch):
    clip = write_fbx(tmp_path / "clip.fbx", True, 1, "e" * 32)
    calls = []
    monkeypatch.setattr(desktop, "_unity", fake_engine(calls))
    monkeypatch.setattr(default_humanoid, "resolve_default", lambda request: pytest.fail("the clip's own character must win"))
    result = desktop.preview({"source": str(clip), "humanoid": True, "cache": str(tmp_path / "cache")})
    assert result["status"] == "ready" and result["default_character"]["embedded"] is True
    assert len(calls) == 1 and calls[0][0]["skeleton"] == str(clip.resolve()) and calls[0][0]["human_mapping"] == {}
    # A cached result is shared with an explicit run of the same character, yet still names the character.
    again = desktop.preview({"source": str(clip), "humanoid": True, "cache": str(tmp_path / "cache")})
    assert again["cache_hit"] and again["default_character"]["embedded"] is True and len(calls) == 1


def test_failed_embedded_evaluation_retries_with_the_regular_default(tmp_path, monkeypatch):
    clip, manny = write_fbx(tmp_path / "clip.fbx", True, 1, "e" * 32), tmp_path / "Manny.fbx"
    manny.write_bytes(b"actual-model-supplied-to-mocked-engine")
    tried, real = [], fake_engine([])

    def engine(request, operation, work):
        tried.append(request["skeleton"])
        if request["skeleton"] == str(clip.resolve()):
            return {"status": "needsAvatar", "connected": True, "reason": "avatar invalid"}
        return real(request, operation, work)

    monkeypatch.setattr(desktop, "_unity", engine)
    monkeypatch.setattr(default_humanoid, "resolve_default", lambda request: {
        "model": str(manny), "name": "UE5 Mannequin (Manny)", "human_mapping": {"Hips": "pelvis"}})
    result = desktop.preview({"source": str(clip), "humanoid": True, "cache": str(tmp_path / "cache")})
    assert result["status"] == "ready" and result["default_character"]["name"] == "UE5 Mannequin (Manny)"
    assert tried == [str(clip.resolve()), str(manny)]


def test_unity_preview_slots_follow_ram_within_two_to_four():
    gb = 1024 ** 3
    assert default_humanoid.unity_preview_slots(32 * gb) == 4
    assert default_humanoid.unity_preview_slots(12 * gb) == 3
    assert default_humanoid.unity_preview_slots(6 * gb) == 2
    assert default_humanoid.unity_preview_slots(0) == 2  # unknown RAM keeps the old default
