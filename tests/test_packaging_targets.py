"""Unit tests for packaging/stage.py: layout, manifest, path escape, missing files, arch fail-closed.

These run on any OS with synthetic binary headers; they do NOT prove real Mac/Linux hardware behaviour.
"""
from __future__ import annotations

import importlib.util
import io
import json
import copy
import os
import plistlib
import re
from pathlib import Path
import struct
import tarfile
import types
import zipfile

import pytest

STAGE_PATH = Path(__file__).resolve().parents[1] / "packaging" / "stage.py"
spec = importlib.util.spec_from_file_location("animbridge_stage", STAGE_PATH)
stage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stage)

ALL_TARGETS = ["windows", "macarm64", "ubuntu22.04", "ubuntu24.04"]
UNIX_TARGETS = ["macarm64", "ubuntu22.04", "ubuntu24.04"]


def pe(machine):
    data = bytearray(0x100)
    data[0:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<H", data, 0x84, machine)
    return bytes(data)


def elf(machine):
    data = bytearray(64)
    data[0:4] = b"\x7fELF"
    data[4], data[5], data[6] = 2, 1, 1
    struct.pack_into("<H", data, 18, machine)
    return bytes(data)


def macho(cpu):
    return b"\xcf\xfa\xed\xfe" + struct.pack("<I", cpu) + bytes(24)


def fat(*cpus):
    data = b"\xca\xfe\xba\xbe" + struct.pack(">I", len(cpus))
    for cpu in cpus:
        data += struct.pack(">I", cpu) + bytes(16)
    return data + bytes(64)


ARM_MACHO, X64_MACHO = 0x0100000C, 0x01000007


def native_bytes(target):
    return {"windows": pe(0x8664), "macarm64": macho(ARM_MACHO)}.get(target, elf(0x3E))


def write(path, data=b"x", executable=False):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data if isinstance(data, bytes) else data.encode())
    if executable:
        path.chmod(0o755)
    return path


def make_repo(root):
    write(root / "src/unityanimbridge/bootstrap.py", "# trusted\n")
    write(root / "src/unityanimbridge/__init__.py", "")
    write(root / "src/unityanimbridge/stale.pyc", b"pyc")
    write(root / "src/unityanimbridge/__pycache__/x.pyc", b"pyc")
    write(root / "native/ui/index.html", "<html/>")
    write(root / "blender/addon.py", "")
    write(root / "unity/Editor/Importer.cs", "")
    write(root / "unreal/tool.py", "")
    write(root / "assets/characters/Default.fbx", b"fbx")
    write(root / "pyproject.toml", "[project]\n")
    write(root / "LICENSE", "MIT\n")
    write(root / "VERSION", "0.2.3\n")
    return root


def make_tree(target, root, repo=None, arch_bytes=None):
    paths = stage.layout(target)
    blob = arch_bytes or native_bytes(target)
    for name in ("main", "cli", "supervisor", "python", "blender"):
        write(root / paths[name], blob, executable=True)
    for asset in stage.UI_ASSETS:
        write(root / paths["ui"] / asset, "<html/>" if asset.endswith(".html") else "// fixture")
    write(root / paths["backend"] / "src/unityanimbridge/__init__.py", "")
    write(root / paths["bootstrap"], "# trusted\n")
    if stage.TARGETS[target]["os"] == "windows":
        write(root / paths["webview2"] / "msedgewebview2.exe", pe(0x8664), executable=True)
    return root


def finalize(target, root, repo):
    return stage.finalize(target, root, repo)


@pytest.fixture
def repo(tmp_path):
    return make_repo(tmp_path / "repo")


# ------------------------------------------------------------------ layout
def test_aliases_resolve_to_canonical_targets():
    assert stage.canonical("macos-arm64") == "macarm64"
    assert stage.canonical("ubuntu-22.04") == "ubuntu22.04"
    with pytest.raises(stage.StageError, match="Unknown target"):
        stage.canonical("freebsd")


def test_layouts_follow_the_design_contract():
    assert stage.layout("windows")["manifest"] == "runtime-manifest.json"
    mac = stage.layout("macarm64")
    assert mac["main"] == "MacOS/AnimBridge" and mac["manifest"] == "Resources/runtime-manifest.json"
    assert mac["ui"] == "Resources/ui" and mac["python_dir"] == "Resources/runtime/python"
    for target in ("ubuntu22.04", "ubuntu24.04"):
        linux = stage.layout(target)
        assert linux["main"] == "bin/AnimBridge" and linux["manifest"] == "share/runtime-manifest.json"
        assert linux["backend"] == "share/backend" and linux["blender_dir"] == "runtime/blender"


# ------------------------------------------------------------------ architecture
@pytest.mark.parametrize("data,expected", [
    (pe(0x8664), {"x64"}), (pe(0xAA64), {"arm64"}), (pe(0x14C), {"x86"}),
    (elf(0x3E), {"x64"}), (elf(0xB7), {"arm64"}), (elf(0x03), {"x86"}),
    (macho(ARM_MACHO), {"arm64"}), (macho(X64_MACHO), {"x64"}),
    (fat(ARM_MACHO), {"arm64"}), (fat(ARM_MACHO, X64_MACHO), {"arm64", "x64"}),
])
def test_binary_architectures(tmp_path, data, expected):
    assert stage.binary_architectures(write(tmp_path / "bin", data)) == expected


@pytest.mark.parametrize("data", [b"#!/bin/sh\n", b"", b"MZ", b"\x7fELF"])
def test_unrecognised_or_truncated_binaries_fail_closed(tmp_path, data):
    with pytest.raises(stage.StageError):
        stage.binary_architectures(write(tmp_path / "bin", data))


def test_require_architecture_rejects_mismatch_and_mixed_fat(tmp_path):
    with pytest.raises(stage.StageError, match="does not match"):
        stage.require_architecture(write(tmp_path / "a", macho(X64_MACHO)), "arm64")
    with pytest.raises(stage.StageError, match="does not match"):
        stage.require_architecture(write(tmp_path / "b", fat(ARM_MACHO, X64_MACHO)), "arm64")
    stage.require_architecture(write(tmp_path / "c", fat(ARM_MACHO)), "arm64")


# ------------------------------------------------------------------ path escape
@pytest.mark.parametrize("bad", ["../x", "a/../../x", "/etc/passwd", "C:\\x", "C:/x", "a\\..\\..\\x"])
def test_safe_join_rejects_escapes(tmp_path, bad):
    with pytest.raises(stage.StageError, match="escapes"):
        stage.safe_join(tmp_path, bad)


def test_safe_join_accepts_nested(tmp_path):
    assert stage.safe_join(tmp_path, "a/b/c") == (tmp_path / "a/b/c").resolve()


def make_tar(path, members):
    with tarfile.open(path, "w:gz") as tarred:
        for name, kind, payload in members:
            info = tarfile.TarInfo(name)
            if kind == "sym":
                info.type, info.linkname = tarfile.SYMTYPE, payload
                tarred.addfile(info)
            else:
                info.size = len(payload)
                tarred.addfile(info, io.BytesIO(payload))
    return path


def test_safe_extract_rejects_member_escape(tmp_path):
    archive = make_tar(tmp_path / "a.tar.gz", [("ok.txt", "file", b"1"), ("../evil", "file", b"2")])
    with pytest.raises(stage.StageError, match="escapes"):
        stage.safe_extract(archive, tmp_path / "out")
    assert not (tmp_path / "evil").exists()
    assert not (tmp_path / "out" / "ok.txt").exists()


def test_safe_extract_rejects_escaping_symlink(tmp_path):
    archive = make_tar(tmp_path / "a.tar.gz", [("lib/link", "sym", "../../outside")])
    with pytest.raises(stage.StageError, match="escapes"):
        stage.safe_extract(archive, tmp_path / "out")
    absolute = make_tar(tmp_path / "b.tar.gz", [("link", "sym", "/etc/passwd")])
    with pytest.raises(stage.StageError, match="Link target"):
        stage.safe_extract(absolute, tmp_path / "out2")


def test_safe_extract_zip_escape_and_sha_mismatch(tmp_path):
    archive = tmp_path / "a.zip"
    with zipfile.ZipFile(archive, "w") as zipped:
        zipped.writestr("../evil", b"x")
    with pytest.raises(stage.StageError, match="escapes"):
        stage.safe_extract(archive, tmp_path / "out")
    with pytest.raises(stage.StageError, match="SHA-256 mismatch"):
        stage.safe_extract(archive, tmp_path / "out", "0" * 64)


def test_safe_extract_good_archive_with_expected_hash(tmp_path):
    archive = make_tar(tmp_path / "a.tar.gz", [("dir/file.txt", "file", b"hello")])
    stage.safe_extract(archive, tmp_path / "out", stage.sha256_file(archive))
    assert (tmp_path / "out/dir/file.txt").read_bytes() == b"hello"


# ------------------------------------------------------------------ locks
@pytest.mark.parametrize("target", ALL_TARGETS)
def test_shipped_locks_are_structurally_valid(target):
    assert stage.check_lock(target, require_resolved=False) == []


@pytest.mark.parametrize("target", UNIX_TARGETS)
def test_unix_lock_resolved_entries_are_real_hashes_and_unresolved_are_null(target):
    for asset in stage.load_lock(target)["assets"]:
        if asset["status"] == "resolved":
            assert stage.SHA256.fullmatch(asset["sha256"]) and asset["url"].startswith("https://")
        else:
            assert asset["sha256"] is None and asset["url"] is None and asset["resolve_via"]
        assert asset["license"]


def test_lock_problems_rules():
    unresolved = {"schema_version": 2, "assets": [{"name": "py", "filename": "py.tar.gz", "version": "3.12.10", "status": "unresolved", "sha256": None,
                                                    "license": "PSF-2.0", "resolve_via": "somewhere"}]}
    assert stage.lock_problems(unresolved, False) == []
    assert any("unresolved" in p for p in stage.lock_problems(unresolved, True))
    bad = {"schema_version": 2, "assets": [
        {"name": "a", "url": "http://x", "sha256": "zz", "license": ""},
        {"name": "a", "status": "unresolved", "sha256": "0" * 64}]}
    text = "\n".join(stage.lock_problems(bad, False))
    assert "64-hex" in text and "https" in text and "license is required" in text
    assert "duplicate" in text and "must not carry a sha256" in text


@pytest.mark.parametrize("target", UNIX_TARGETS)
def test_unix_runtime_locks_have_verified_pins_but_no_hardware_claim(target):
    assert stage.check_lock(target, require_resolved=True) == []
    lock = stage.load_lock(target)
    assert all(row["status"] == "resolved" for row in lock["assets"])
    assert "execution" in lock["note"]


# ------------------------------------------------------------------ stage refusals
def stage_args(tmp_path, repo, target, **overrides):
    native = tmp_path / "native-out"
    for name in ("main", "cli", "supervisor"):
        write(native / Path(stage.layout(target)[name]).name, native_bytes(target), executable=True)
    python = tmp_path / "py"
    blender = tmp_path / "bl"
    write(python / "bin/python3", native_bytes(target), executable=True)
    write(python / "python.exe", native_bytes(target), executable=True)
    write(blender / "blender", native_bytes(target), executable=True)
    write(blender / "blender.exe", native_bytes(target), executable=True)
    write(blender / "Blender.app/Contents/MacOS/Blender", native_bytes(target), executable=True)
    values = dict(target=target, repo=str(repo), out=str(tmp_path / "out"), build_dir=str(native),
                  native_dir=None, python_dir=str(python), blender_dir=str(blender),
                  webview2_dir=None, allow_unresolved=True, profile="development")
    values.update(overrides)
    return types.SimpleNamespace(**values)


@pytest.mark.parametrize("target", UNIX_TARGETS)
def test_stage_refuses_unresolved_lock_without_override(tmp_path, repo, target, monkeypatch):
    monkeypatch.setattr(stage, "check_lock", lambda target, require_resolved: ["runtime SHA-256 is unresolved"])
    args = stage_args(tmp_path, repo, target, allow_unresolved=False, profile="production")
    with pytest.raises(stage.StageError, match="Lock is not releasable"):
        stage.stage(args)
    assert not (tmp_path / "out").exists()


def test_stage_fails_on_missing_built_executable(tmp_path, repo):
    args = stage_args(tmp_path, repo, "ubuntu22.04")
    (Path(args.build_dir) / "AnimBridgeProcessSupervisor").unlink()
    with pytest.raises(stage.StageError, match="Missing built AnimBridgeProcessSupervisor"):
        stage.stage(args)


def test_stage_fails_on_wrong_architecture_native_binary(tmp_path, repo):
    args = stage_args(tmp_path, repo, "macarm64")
    write(Path(args.build_dir) / "AnimBridge", macho(X64_MACHO), executable=True)
    with pytest.raises(stage.StageError, match="does not match"):
        stage.stage(args)


def test_stage_fails_on_wrong_architecture_runtime(tmp_path, repo):
    args = stage_args(tmp_path, repo, "ubuntu24.04")
    write(Path(args.python_dir) / "bin/python3", elf(0xB7), executable=True)
    with pytest.raises(stage.StageError, match="does not match"):
        stage.stage(args)


def test_stage_fails_on_missing_runtime_directory(tmp_path, repo):
    args = stage_args(tmp_path, repo, "ubuntu24.04", blender_dir=str(tmp_path / "absent"))
    with pytest.raises(stage.StageError, match="Missing extracted runtime directory"):
        stage.stage(args)


def test_stage_requires_bootstrap(tmp_path, repo):
    (repo / "src/unityanimbridge/bootstrap.py").unlink()
    with pytest.raises(stage.StageError, match="trusted bootstrap"):
        stage.stage(stage_args(tmp_path, repo, "ubuntu22.04"))


def test_stage_refuses_non_empty_output(tmp_path, repo):
    args = stage_args(tmp_path, repo, "ubuntu22.04")
    write(Path(args.out) / "leftover", b"x")
    with pytest.raises(stage.StageError, match="not empty"):
        stage.stage(args)


def test_windows_stage_requires_webview2(tmp_path, repo):
    with pytest.raises(stage.StageError, match="webview2"):
        stage.stage(stage_args(tmp_path, repo, "windows"))


@pytest.mark.parametrize("target", UNIX_TARGETS)
def test_dev_stage_produces_verifiable_tree_without_pyc(tmp_path, repo, target):
    result = stage.stage(stage_args(tmp_path, repo, target))
    out = tmp_path / "out"
    assert result["target"] == target
    assert not list(out.rglob("*.pyc"))
    assert (out / stage.layout(target)["bootstrap"]).read_text() == "# trusted\n"
    assert stage.verify(target, out)["hardware_verified"] is False


# ------------------------------------------------------------------ finalize / runtime manifest
@pytest.mark.parametrize("target", ALL_TARGETS)
def test_finalize_writes_contract_manifest(tmp_path, repo, target):
    root = make_tree(target, tmp_path / "root")
    finalize(target, root, repo)
    manifest = json.loads((root / stage.layout(target)["manifest"]).read_text(encoding="utf-8"))
    arch = "arm64" if target == "macarm64" else "x64"
    assert manifest["schemaVersion"] == 1 and manifest["appVersion"] == "0.2.3"
    assert manifest["targetId"] == stage.TARGETS[target]["id"] and manifest["architecture"] == arch
    components, paths = manifest["components"], stage.layout(target)
    assert components["backend"]["resourcePath"] == paths["backend"]
    assert components["ui"]["resourcePath"] == paths["ui"]
    assert components["python"]["executablePath"] == paths["python"]
    assert components["blender"]["executablePath"] == paths["blender"]
    assert components["supervisor"]["executablePath"] == paths["supervisor"]
    assert components["bootstrap"]["resourcePath"] == paths["bootstrap"]
    assert components["backend"]["architecture"] == arch
    assert ("webview2" in components) == (target == "windows")
    assert stage.validate_runtime_manifest(manifest, target, root) == []


@pytest.mark.parametrize("target", ALL_TARGETS)
def test_finalize_fails_closed_on_missing_component(tmp_path, repo, target):
    root = make_tree(target, tmp_path / "root")
    (root / stage.layout(target)["supervisor"]).unlink()
    with pytest.raises(stage.StageError, match="Missing executable"):
        finalize(target, root, repo)
    assert not (root / stage.layout(target)["manifest"]).exists()


def test_finalize_fails_closed_on_wrong_arch_runtime(tmp_path, repo):
    root = make_tree("macarm64", tmp_path / "root")
    write(root / stage.layout("macarm64")["blender"], macho(X64_MACHO), executable=True)
    with pytest.raises(stage.StageError, match="does not match"):
        finalize("macarm64", root, repo)


def test_validate_runtime_manifest_detects_problems(tmp_path, repo):
    root = make_tree("ubuntu22.04", tmp_path / "root")
    finalize("ubuntu22.04", root, repo)
    manifest = json.loads((root / "share/runtime-manifest.json").read_text())
    manifest["schemaVersion"] = 2
    manifest["architecture"] = "arm64"
    manifest["components"]["python"]["executablePath"] = "../outside"
    manifest["components"]["ui"]["resourcePath"] = "share/missing"
    manifest["components"]["webview2"] = {"resourcePath": "runtime/webview2"}
    del manifest["components"]["bootstrap"]
    text = "\n".join(stage.validate_runtime_manifest(manifest, "ubuntu22.04", root))
    for fragment in ("schemaVersion", "architecture must be x64", "portable relative paths", "does not exist",
                     "webview2 is Windows-only", "missing component bootstrap"):
        assert fragment in text


def test_copy_backend_excludes_pyc_and_copies_bootstrap(tmp_path, repo):
    backend = tmp_path / "backend"
    stage.copy_backend(repo, backend)
    assert (backend / "bootstrap.py").is_file()
    assert (backend / "src/unityanimbridge/__init__.py").is_file()
    assert (backend / "unity/Editor/Importer.cs").is_file()
    assert (backend / "assets/characters/Default.fbx").is_file()
    assert not list(backend.rglob("*.pyc"))


def test_app_version_rejects_invalid(tmp_path, repo):
    (repo / "VERSION").write_text("not-a-version", encoding="utf-8")
    with pytest.raises(stage.StageError, match="VERSION"):
        stage.app_version(repo)


# ------------------------------------------------------------------ distribution manifest / verify
def test_distribution_manifest_detects_tampering_and_is_excluded_from_itself(tmp_path, repo):
    root = make_tree("ubuntu24.04", tmp_path / "root")
    finalize("ubuntu24.04", root, repo)
    path = stage.write_distribution_manifest("ubuntu24.04", root, repo, signed=False)
    manifest = json.loads(path.read_text(encoding="utf-8"))
    paths = {entry["path"] for entry in manifest["files"]}
    assert "distribution-manifest.json" not in paths and "share/runtime-manifest.json" in paths
    assert manifest["source_fallback"] is None and manifest["hardware_verified"] is False
    assert manifest["releaseInputsVerified"] is False and manifest["releaseGatesPassed"] is False
    assert manifest["signed"] is False and manifest["notarized"] is False
    assert all(re.fullmatch(r"0[0-7]{3}", entry["mode"]) for entry in manifest["files"] if "mode" in entry)
    if os.name != "nt":
        assert any(entry["mode"] == "0755" for entry in manifest["files"] if "mode" in entry)
    assert stage.verify("ubuntu24.04", root)["status"] == "layout-verified"
    (root / "share/ui/index.html").write_text("tampered", encoding="utf-8")
    with pytest.raises(stage.StageError, match="payload changed"):
        stage.verify("ubuntu24.04", root)


# ------------------------------------------------------------------ CLI
def test_cli_check_lock_exit_codes(capsys, monkeypatch):
    assert stage.main(["check-lock", "--target", "ubuntu22.04"]) == 0
    assert json.loads(capsys.readouterr().out)["problems"] == []
    assert stage.main(["check-lock", "--target", "ubuntu22.04", "--require-resolved"]) == 0
    capsys.readouterr()
    monkeypatch.setattr(stage, "check_lock", lambda target, require_resolved: ["unresolved"])
    assert stage.main(["check-lock", "--target", "ubuntu22.04", "--require-resolved"]) == 1
    assert json.loads(capsys.readouterr().out)["problems"] == ["unresolved"]


def test_cli_unknown_target_is_an_error(capsys):
    assert stage.main(["verify", "--target", "freebsd", "--root", "."]) == 2
    assert "Unknown target" in capsys.readouterr().err


# Regression coverage for the native manifest and release boundaries.
@pytest.mark.parametrize("target,expected", [
    ("windows", "windows-x64"), ("macarm64", "macos-arm64"),
    ("ubuntu22.04", "linux-x64-ubuntu22.04"), ("ubuntu24.04", "linux-x64-ubuntu24.04")])
def test_native_runtime_target_ids_are_not_lock_aliases(target, expected):
    manifest = stage.build_runtime_manifest(target, "0.2.3")
    assert manifest["targetId"] == expected
    assert stage.canonical(expected) == target
    assert all(value["version"] for value in manifest["components"].values())


@pytest.mark.parametrize("target", ALL_TARGETS)
def test_release_and_harness_cannot_enable_unresolved_override(tmp_path, repo, target):
    for profile in ("production", "harness"):
        args = stage_args(tmp_path, repo, target, profile=profile)
        with pytest.raises(stage.StageError, match="forbidden"):
            stage.stage(args)
    assert not Path(args.out).exists()


def test_development_staging_cannot_claim_release_verification(tmp_path, repo):
    result = stage.stage(stage_args(tmp_path, repo, "ubuntu22.04"))
    assert result["provenance"]["profile"] == "development"
    assert result["provenance"]["releaseInputsVerified"] is False
    manifest = stage.write_distribution_manifest("ubuntu22.04", tmp_path / "out", repo, False)
    value = json.loads(manifest.read_text())
    assert value["source_fallback"] is None and value["releaseGatesPassed"] is False


def make_build_proof(root, repo, profile="production", fallback="OFF"):
    hooks = "ON" if profile == "harness" else "OFF"
    write(root / "CMakeCache.txt", f"ANIMBRIDGE_ENABLE_SOURCE_FALLBACK:BOOL={fallback}\nANIMBRIDGE_ENABLE_UI_TEST_HOOKS:BOOL={hooks}\nANIMBRIDGE_BUILD_DESKTOP:BOOL=ON\nCMAKE_PROJECT_VERSION:STATIC=0.2.3\nCMAKE_GENERATOR:INTERNAL=Ninja\nCMAKE_CXX_COMPILER:FILEPATH=/usr/bin/c++\nCMAKE_BUILD_TYPE:STRING=Release\n")
    write(root / "generated/animbridge/app_config.hpp", "// frozen generated application settings")
    write(root / "build.ninja", 'DEFINES = -DANIMBRIDGE_SOURCE_SNAPSHOT_SHA=\\"' + stage.source_snapshot(repo) + '\\"\n')
    return root


def test_build_provenance_rejects_source_fallback_and_hook_mismatch(tmp_path, repo):
    build = make_build_proof(tmp_path / "build", repo)
    proof = stage.build_provenance(build, repo, "production")
    assert proof["validated"] and proof["sourceFallback"] is False and proof["testHooks"] is False
    with pytest.raises(stage.StageError, match="UI_TEST_HOOKS"):
        stage.build_provenance(build, repo, "harness")
    make_build_proof(build, repo, fallback="ON")
    with pytest.raises(stage.StageError, match="SOURCE_FALLBACK"):
        stage.build_provenance(build, repo, "production")


def test_build_provenance_requires_frozen_source(tmp_path, repo):
    build = make_build_proof(tmp_path / "build", repo)
    write(repo / "native/ui/late.js", "export const late = true;")
    with pytest.raises(stage.StageError, match="freeze sources"):
        stage.build_provenance(build, repo, "production")


def test_production_harness_comparison_requires_same_source_runtime_and_flags():
    production = {"validated": True, "sourceSnapshot": "a" * 64, "runtimeLockSha256": "b" * 64,
                  "targetId": "linux-x64-ubuntu24.04", "appConfigSha256": "e" * 64, "sourceFallback": False, "profile": "production", "testHooks": False}
    harness = {**production, "profile": "harness", "testHooks": True}
    stage.compare_provenance(production, harness)
    for key, changed in (("sourceSnapshot", "c" * 64), ("runtimeLockSha256", "d" * 64),
                         ("sourceFallback", True), ("testHooks", False), ("compiler", "different")):
        with pytest.raises(stage.StageError):
            stage.compare_provenance(production, {**harness, key: changed})


def test_runtime_receipt_detects_post_extraction_mutation(tmp_path):
    target = "ubuntu24.04"
    python, blender = tmp_path / "python", tmp_path / "blender"
    write(python / "bin/python3", elf(0x3E), executable=True)
    write(blender / "blender", elf(0x3E), executable=True)
    receipt = {"schemaVersion": 1, "targetId": stage.TARGETS[target]["id"],
               "runtimeLockSha256": stage.sha256_file(stage.lock_path(target)),
               "pythonInventory": stage.distribution_files(python), "blenderInventory": stage.distribution_files(blender), "licenseInventory": []}
    path = write(tmp_path / "runtime-receipt.json", json.dumps(receipt))
    stage._validate_receipt(target, path, python, blender)
    write(python / "bin/python3", b"modified")
    with pytest.raises(stage.StageError, match="changed after verified"):
        stage._validate_receipt(target, path, python, blender)


@pytest.mark.parametrize("change", ["added", "deleted", "vendor-bytecode"])
def test_distribution_integrity_has_no_bytecode_or_added_file_exemption(tmp_path, repo, change):
    root = make_tree("ubuntu24.04", tmp_path / "root")
    finalize("ubuntu24.04", root, repo)
    stage.write_distribution_manifest("ubuntu24.04", root, repo, False)
    if change == "added":
        write(root / "runtime/unexpected.bin", b"added")
    elif change == "deleted":
        (root / "share/backend/runtime_probe.py").unlink()
    else:
        write(root / "runtime/blender/vendor/__pycache__/startup.pyc", b"generated")
    with pytest.raises(stage.StageError, match="payload changed"):
        stage.verify("ubuntu24.04", root)


def test_finalization_removes_loose_bytecode_and_records_transform(tmp_path, repo):
    root = make_tree("ubuntu22.04", tmp_path / "root")
    write(root / "runtime/python/lib/__pycache__/original.pyc", b"old cache")
    result = finalize("ubuntu22.04", root, repo)
    assert not list(root.rglob("*.pyc"))
    assert result["provenance"]["removedLooseBytecode"] == ["runtime/python/lib/__pycache__/original.pyc"]


def test_macos_manifest_stays_outside_app_and_signed_claims_require_native_evidence(tmp_path, repo, monkeypatch):
    root = make_tree("macarm64", tmp_path / "AnimBridge.app/Contents")
    finalize("macarm64", root, repo)
    manifest = stage.write_distribution_manifest("macarm64", root, repo, False)
    assert not manifest.is_relative_to(root.parent)
    with pytest.raises(stage.StageError, match="outside"):
        stage.write_distribution_manifest("macarm64", root, repo, False, output=root / "manifest.json")
    monkeypatch.setattr(stage.sys, "platform", "linux")
    with pytest.raises(stage.StageError, match="actual .app on macOS"):
        stage.write_distribution_manifest("macarm64", root, repo, True, True)
    with pytest.raises(stage.StageError, match="without signing"):
        stage.write_distribution_manifest("macarm64", root, repo, False, True)


def test_deb_control_has_distro_specific_t64_and_offline_dependency_disclosure():
    old = stage.deb_control("ubuntu22.04", "0.2.3", 100)
    modern = stage.deb_control("ubuntu24.04", "0.2.3", 100)
    assert "0.2.3-1~ubuntu22.04" in old and "libgtk-3-0," in old and "libssl3," in old
    assert "0.2.3-1~ubuntu24.04" in modern and "libgtk-3-0t64" in modern and "libssl3t64" in modern
    assert "libwebkit2gtk-4.1-0 (>= 2.40)" in modern and "package manager" in modern


def test_archive_duplicate_and_existing_destinations_are_rejected_before_writes(tmp_path):
    archive = make_tar(tmp_path / "duplicate.tar.gz", [("same", "file", b"first"), ("same", "file", b"second")])
    with pytest.raises(stage.StageError, match="Duplicate"):
        stage.safe_extract(archive, tmp_path / "out")
    assert not (tmp_path / "out/same").exists()
    archive = make_tar(tmp_path / "new.tar.gz", [("same", "file", b"new")])
    write(tmp_path / "existing/same", b"preserve")
    with pytest.raises(stage.StageError, match="destination exists"):
        stage.safe_extract(archive, tmp_path / "existing")
    assert (tmp_path / "existing/same").read_bytes() == b"preserve"


def test_wheel_installation_keeps_native_payload_and_refuses_import_hooks(tmp_path):
    wheel = tmp_path / "numpy.whl"
    with zipfile.ZipFile(wheel, "w") as archive:
        archive.writestr("numpy/__init__.py", "version='pinned'")
        archive.writestr("numpy.libs/lib.so", elf(0x3E))
        archive.writestr("numpy-2.2.6.dist-info/METADATA", "Version: 2.2.6")
    stage.install_wheel(wheel, tmp_path / "site")
    assert (tmp_path / "site/numpy.libs/lib.so").read_bytes() == elf(0x3E)
    with zipfile.ZipFile(tmp_path / "bad.whl", "w") as archive:
        archive.writestr("startup.pth", "import external")
    with pytest.raises(stage.StageError, match="import hooks"):
        stage.install_wheel(tmp_path / "bad.whl", tmp_path / "bad-site")


def test_ci_prepares_exact_eight_variants_and_keeps_execution_paused():
    root = Path(__file__).resolve().parents[1]
    workflow = (root / ".github/workflows/cross-platform.yml").read_text()
    assert "os: [windows-latest, macos-15, ubuntu-22.04, ubuntu-24.04]" in workflow
    assert "profile: [production, harness]" in workflow
    assert "ANIMBRIDGE_ENABLE_NATIVE_CI == 'true'" in workflow and "default: false" in workflow
    assert "--no-build-isolation -e \".[test]\"" in workflow
    assert '-m "not blender and not unity and not unreal"' in workflow
    assert "ci-development-test" in workflow and "-DANIMBRIDGE_ENABLE_SOURCE_FALLBACK=ON" in workflow
    uses = re.findall(r"uses:\s+([^\s]+)", workflow)
    assert all(re.fullmatch(r"[\w.-]+/[\w.-]+@[0-9a-f]{40}", action) for action in uses)


def test_verifier_does_not_reintroduce_vendor_cache_exceptions():
    source = (STAGE_PATH.parent / "Verify-Distribution.py").read_text()
    assert "regenerated_blender_vendor_caches" not in source and "if path not in vendor_caches" not in source
    assert "before == stage.distribution_files(bundle)" in source


def test_missing_host_transport_is_a_release_layout_failure(tmp_path, repo):
    root = make_tree("ubuntu24.04", tmp_path / "root")
    (root / "share/ui/host-transport.js").unlink()
    with pytest.raises(stage.StageError, match="host-transport"):
        finalize("ubuntu24.04", root, repo)


def test_codesign_and_gatekeeper_failure_cannot_create_signed_manifest(tmp_path, repo, monkeypatch):
    root = make_tree("macarm64", tmp_path / "AnimBridge.app/Contents")
    finalize("macarm64", root, repo)
    monkeypatch.setattr(stage.sys, "platform", "darwin")
    calls = []
    def reject(command, **kwargs):
        calls.append(command)
        if command[0] == "spctl":
            raise stage.subprocess.CalledProcessError(1, command)
    monkeypatch.setattr(stage.subprocess, "run", reject)
    with pytest.raises(stage.subprocess.CalledProcessError):
        stage.write_distribution_manifest("macarm64", root, repo, True, True)
    assert [command[0] for command in calls] == ["codesign", "spctl"]
    assert not stage.default_distribution_manifest("macarm64", root).exists()


def test_notary_rejection_does_not_emit_success_evidence(tmp_path, repo, monkeypatch):
    root = make_tree("macarm64", tmp_path / "AnimBridge.app/Contents")
    entitlements = {}
    for component in ("desktop", "cli", "supervisor", "python", "blender"):
        path = tmp_path / (component + ".plist")
        path.write_bytes(plistlib.dumps({}))
        entitlements[component] = path.name
    config = write(tmp_path / "signing.json", json.dumps({"identity": "Test identity", "notaryProfile": "Test profile", "entitlements": entitlements}))
    monkeypatch.setattr(stage.sys, "platform", "darwin")
    calls = []
    def reject_notary(command, **kwargs):
        calls.append(command)
        return types.SimpleNamespace(stdout='{"id":"test","status":"Invalid"}')
    monkeypatch.setattr(stage.subprocess, "run", reject_notary)
    evidence = tmp_path / "signing-result.json"
    with pytest.raises(stage.StageError, match="Accepted"):
        stage.sign_macos(root, config, evidence)
    assert any("notarytool" in command for command in calls)
    assert not evidence.exists() and not any("stapler" in command for command in calls)


def test_configured_but_stale_native_executable_is_not_a_release_input(tmp_path):
    proof = {"sourceSnapshot": "a" * 64, "profile": "production"}
    binary = write(tmp_path / "AnimBridge", elf(0x3E) + b"old object")
    with pytest.raises(stage.StageError, match="predates"):
        stage.verify_embedded_build(binary, proof)
    write(binary, elf(0x3E) + b"a" * 64)
    stage.verify_embedded_build(binary, proof)
    for encoding in ("ascii", "utf-16-le"):
        write(binary, elf(0x3E) + b"a" * 64 + "--ui-test".encode(encoding))
        with pytest.raises(stage.StageError, match="automation/debug"):
            stage.verify_embedded_build(binary, proof)


def test_windows_online_configure_clears_a_previously_disconnected_cache():
    source = (STAGE_PATH.parent / "Build-Distribution.ps1").read_text(encoding="utf-8")
    branch = re.search(r"if \(\$Offline\) \{ \$configureArgs \+= '([^']+)' \}\s*else \{ \$configureArgs \+= '([^']+)' \}", source)
    assert branch is not None, "Online builds must explicitly reset the persistent CMake cache option"
    assert branch.groups() == ("-DFETCHCONTENT_FULLY_DISCONNECTED=ON", "-DFETCHCONTENT_FULLY_DISCONNECTED=OFF")
