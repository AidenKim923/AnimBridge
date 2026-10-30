"""Unreal integration tests; run ``python -m pytest -m unreal`` after ``-m unity``.

All projects, logs and jobs stay in _verify/unreal. UAB_UNREAL selects another
UnrealEditor-Cmd.exe; the default is UE 5.8. No existing game project is touched.
"""
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys

import numpy as np
import pytest

from unityanimbridge import coords

pytestmark = pytest.mark.unreal
ROOT = Path(__file__).resolve().parents[1]
DEFAULT_EDITOR = Path(r"C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe")


def run_editor(editor, project, job, work):
    job_path = work / (job["phase"] + "_job.json")
    job_path.write_text(json.dumps(job, indent=2), encoding="utf-8")
    temp = work / "temp"
    temp.mkdir(exist_ok=True)
    env = dict(os.environ, UAB_UNREAL_JOB=str(job_path), TEMP=str(temp), TMP=str(temp))
    cmd = [str(editor), str(project), "-run=pythonscript", "-script=" + str(ROOT / "tests" / "unreal_helpers.py"),
           "-unattended", "-nop4", "-nosplash", "-NullRHI", "-nosound", "-NoAsyncLoadingThread",
           "-stdout", "-FullStdOutLogOutput", "-abslog=" + str(work / (job["phase"] + "_engine.log"))]
    (work / (job["phase"] + "_command.json")).write_text(json.dumps(cmd, indent=2), encoding="utf-8")
    completed = subprocess.run(cmd, cwd=ROOT, env=env, capture_output=True, timeout=900)
    log = work / (job["phase"] + "_process.log")
    log.write_bytes(completed.stdout + completed.stderr)
    result_path = Path(job["out"])
    assert result_path.is_file(), f"Unreal exit {completed.returncode}; see {log}"
    result = json.loads(result_path.read_text())
    assert completed.returncode == 0 and result["status"] == "ok", f"{result.get('error')}; see {log}"
    return result


@pytest.mark.parametrize("adapter", ["interchange", "legacy-fbx"])
def test_unreal_import_pose_markers_and_reverse_export(adapter):
    editor = Path(os.environ.get("UAB_UNREAL", str(DEFAULT_EDITOR)))
    if not editor.is_file():
        pytest.skip("Unreal editor unavailable; set UAB_UNREAL")
    unity_work = ROOT / "_verify" / "unity" / "v3"
    for label in ("zup_cm_default", "yup_m_default"):
        if not (unity_work / (label + "_baked.json")).is_file():
            pytest.skip("V3 fixture missing: run python -m pytest -m unity first")
    engine_label = editor.parents[3].name
    work = ROOT / "_verify" / "unreal" / engine_label / adapter
    work.mkdir(parents=True, exist_ok=True)
    project_dir = work / "project"
    (project_dir / "Config").mkdir(parents=True, exist_ok=True)
    project = project_dir / "AnimBridgeV4.uproject"
    project.write_text(json.dumps({"FileVersion": 3, "EngineAssociation": "",
        "Plugins": [{"Name": "PythonScriptPlugin", "Enabled": True},
                    {"Name": "EditorScriptingUtilities", "Enabled": True}]}, indent=2), encoding="utf-8")
    (project_dir / "Config" / "DefaultEngine.ini").write_text(
        "[/Script/Engine.RendererSettings]\nr.RayTracing=False\nr.DynamicGlobalIlluminationMethod=0\nr.ReflectionMethod=0\n",
        encoding="utf-8")
    cases = []
    for kind in ("zup_cm", "yup_m"):
        label = kind + "_default"
        source_manifest = json.loads((unity_work / f"converted_{label}" / "manifest.json").read_text())
        side = json.loads(Path(source_manifest["clips"][0]["sidecar"]).read_text())
        side["events"] = [{"name": "FootL", "time": 0.25}, {"name": "FootR", "time": 0.75}]
        side_path = work / (label + ".uab.json")
        side_path.write_text(json.dumps(side, indent=2), encoding="utf-8")
        source_manifest["clips"][0]["sidecar"] = str(side_path)
        manifest_path = work / (label + "_manifest.json")
        manifest_path.write_text(json.dumps(source_manifest, indent=2), encoding="utf-8")
        cases.append({"label": label, "source": str(unity_work / "project" / "Assets" / (kind + ".fbx")),
                      "manifest": str(manifest_path), "dest": "/Game/Verify/" + kind})
    import_path = work / "import_result.json"
    imported = run_editor(editor, project, {"phase": "import", "adapter": adapter, "cases": cases,
                                         "out": str(import_path)}, work)
    for case in cases:
        case["import_result"] = str(import_path)
    reloaded = run_editor(editor, project, {"phase": "reload", "adapter": adapter, "cases": cases,
                                         "out": str(work / "reload_result.json")}, work)
    assert len(imported["cases"]) == len(reloaded["cases"]) == 2
    maxima = {}
    scales = {}
    for first, loaded in zip(imported["cases"], reloaded["cases"], strict=True):
        hierarchy = {"Hips": "None", "Spine": "Hips", "Hand": "Spine"}
        assert first["hierarchy_before"] == first["hierarchy_after"] == loaded["hierarchy_reloaded"] == hierarchy
        assert loaded["markers"] == [["FootL", 0.25], ["FootR", 0.75]]
        assert loaded["frames"] == 11 and loaded["duration"] == pytest.approx(1.0, abs=1e-5)
        baked = json.loads((unity_work / (first["label"] + "_baked.json")).read_text())
        tracks = baked["clips"][0]["tracks"]
        # Unity reflects FBX X: q=(x,-y,-z,w). Unreal's FFbxDataConverter
        # reflects Y: q=(x,-y,z,-w). Combined, up to quaternion sign:
        # Unity -> UE non-root q=(-x,-y,z,w). FBX axis conversion affects roots.
        for bone in ("Spine", "Hand"):
            path = next(p for p in tracks if p.endswith("/" + bone))
            expected = np.asarray(tracks[path]["r"]) * [-1, -1, 1, 1]
            actual = np.asarray([f[bone]["r"] for f in loaded["poses"]])
            expected /= np.linalg.norm(expected, axis=1)[:, None]
            actual /= np.linalg.norm(actual, axis=1)[:, None]
            dot = np.abs(np.sum(expected * actual, axis=1))
            degrees = np.degrees(2 * np.arccos(np.clip(dot, -1, 1)))
            maxima[first["label"] + "/" + bone] = float(np.max(degrees))
            assert np.max(degrees) <= 0.1, maxima
            expected_scale = np.asarray(tracks[path]["s"])
            actual_scale = np.asarray([f[bone]["s"] for f in loaded["poses"]])
            scales[first["label"] + "/" + bone] = float(np.max(np.abs(expected_scale - actual_scale)))
            np.testing.assert_allclose(actual_scale, expected_scale, atol=1e-5, rtol=0)
        reverse = loaded["export"]
        assert reverse["animbridge_version"] == 1 and reverse["source_engine"] == "unreal"
        assert len(reverse["clips"]) == 1 and not reverse["errors"]
        assert Path(reverse["clips"][0]["fbx"]).stat().st_size > 0
    (work / "rotation_errors_degrees.json").write_text(json.dumps(maxima, indent=2), encoding="utf-8")
    (work / "scale_errors.json").write_text(json.dumps(scales, indent=2), encoding="utf-8")


@pytest.mark.unity
def test_unreal_export_imports_in_unity():
    editor = Path(os.environ.get("UAB_UNITY", r"C:\Program Files\Unity\Hub\Editor\6000.4.7f1\Editor\Unity.exe"))
    if not editor.is_file():
        pytest.skip("Unity editor unavailable; set UAB_UNITY")
    source = ROOT / "_verify" / "unreal" / "UE_5.8" / "interchange"
    manifests = [source / (label + "_reverse") / "animbridge-manifest.json"
                 for label in ("zup_cm_default", "yup_m_default")]
    if not all(path.is_file() for path in manifests):
        pytest.skip("Run UE 5.8 V4 first to produce Unreal FBX exports")
    work = ROOT / "_verify" / "unreal" / "reverse_unity"
    project = work / "project"
    assets = project / "Assets"
    (assets / "Editor").mkdir(parents=True, exist_ok=True)
    (project / "Packages").mkdir(exist_ok=True)
    (project / "ProjectSettings").mkdir(exist_ok=True)
    (work / "temp").mkdir(exist_ok=True)
    (project / "Packages" / "manifest.json").write_text('{"dependencies":{}}', encoding="utf-8")
    (project / "ProjectSettings" / "ProjectVersion.txt").write_text("m_EditorVersion: 6000.4.7f1\n", encoding="utf-8")
    shutil.copy2(ROOT / "unity" / "Editor" / "AnimBridgeImporter.cs", assets / "Editor")
    manifest = {"animbridge_version": 1, "source_engine": "unreal", "clips": []}
    for path in manifests:
        manifest["clips"].extend(json.loads(path.read_text())["clips"])
    manifest_path = work / "animbridge-manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    report_path = work / "animbridge-unity-import-report.json"
    command = [str(editor), "-batchmode", "-nographics", "-quit", "-projectPath", str(project),
               "-executeMethod", "AnimBridgeImporter.ImportFromCommandLine", "-animBridgeManifest", str(manifest_path),
               "-animBridgeDest", "Assets/ImportedUnreal", "-animBridgeReport", str(report_path),
               "-logFile", str(work / "unity.log")]
    (work / "command.json").write_text(json.dumps(command, indent=2), encoding="utf-8")
    env = dict(os.environ, TEMP=str(work / "temp"), TMP=str(work / "temp"))
    completed = subprocess.run(command, cwd=ROOT, env=env, capture_output=True, timeout=600)
    (work / "process.log").write_bytes(completed.stdout + completed.stderr)
    assert completed.returncode == 0, f"Unity exit {completed.returncode}; see {work / 'unity.log'}"
    report = json.loads(report_path.read_text())
    assert len(report["clips"]) == 2
    for expected, actual in zip(manifest["clips"], report["clips"], strict=True):
        assert actual["status"] == "ok", actual
        assert len(actual["clips"]) == 1
        assert actual["clips"][0]["name"] == expected["name"]
        assert actual["clips"][0]["length"] == pytest.approx(expected["duration"], abs=1e-5)
        assert actual["clips"][0]["fps"] == pytest.approx(expected["fps"], abs=1e-5)


@pytest.mark.unity
def test_unreal_reverse_nonunit_duration_events_and_pose():
    """A 2.5 s clip exposes normalized-importer-time bugs hidden by 1 s clips."""
    editor = Path(os.environ.get("UAB_UNREAL", str(DEFAULT_EDITOR)))
    unity = Path(os.environ.get("UAB_UNITY", r"C:\Program Files\Unity\Hub\Editor\6000.4.7f1\Editor\Unity.exe"))
    source_work = ROOT / "_verify" / "unity" / "v3"
    if not editor.is_file() or not unity.is_file():
        pytest.skip("Unreal and Unity editors required")
    if not (source_work / "zup_cm_default_baked.json").is_file():
        pytest.skip("Run V3 first")
    work = ROOT / "_verify" / "unreal" / "v6_nonunit"
    (work / "temp").mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, TEMP=str(work / "temp"), TMP=str(work / "temp"), UAB_WORK_DIR=str(work / "temp"))
    baked = json.loads((source_work / "zup_cm_default_baked.json").read_text())
    baked["fps"] = 4
    clip = baked["clips"][0]
    clip.update(name="long_nonunit", fps=4, frames=11, duration=2.5,
                events=[{"name": "StepEarly", "time": 0.5}, {"name": "StepLate", "time": 1.75}])
    baked_path = work / "long_baked.json"
    baked_path.write_text(json.dumps(baked), encoding="utf-8")
    output = work / "converted"
    command = [sys.executable, "-m", "unityanimbridge.cli", "convert", "--baked", str(baked_path),
               "--skeleton", str(source_work / "project" / "Assets" / "zup_cm.fbx"), "-o", str(output)]
    completed = subprocess.run(command, cwd=ROOT, env=env, capture_output=True, timeout=600)
    (work / "convert_command.json").write_text(json.dumps(command, indent=2), encoding="utf-8")
    (work / "convert.log").write_bytes(completed.stdout + completed.stderr)
    assert completed.returncode == 0, str(work / "convert.log")
    project_dir = work / "project"
    project_dir.mkdir(exist_ok=True)
    project = project_dir / "AnimBridgeV6.uproject"
    project.write_text(json.dumps({"FileVersion": 3, "EngineAssociation": "", "Plugins": [
        {"Name": "PythonScriptPlugin", "Enabled": True}, {"Name": "EditorScriptingUtilities", "Enabled": True}]}), encoding="utf-8")
    case = {"label": "long_nonunit", "source": str(source_work / "project" / "Assets" / "zup_cm.fbx"),
            "manifest": str(output / "manifest.json"), "dest": "/Game/Verify/LongNonunit"}
    import_path = work / "import_result.json"
    run_editor(editor, project, {"phase": "import", "adapter": "interchange", "cases": [case], "out": str(import_path)}, work)
    case["import_result"] = str(import_path)
    ue = run_editor(editor, project, {"phase": "reload", "adapter": "interchange", "cases": [case],
                                    "out": str(work / "reload_result.json")}, work)
    assert len(ue["cases"]) == 1
    ue_case = ue["cases"][0]
    assert ue_case["frames"] == 11 and ue_case["duration"] == pytest.approx(2.5, abs=1e-5)
    manifest_path = work / "long_nonunit_reverse" / "animbridge-manifest.json"
    exported = json.loads(manifest_path.read_text())
    assert exported["clips"][0]["events"] == clip["events"]
    unity_project = work / "unity_project"
    assets = unity_project / "Assets"
    (assets / "Editor").mkdir(parents=True, exist_ok=True)
    (unity_project / "Packages").mkdir(exist_ok=True)
    (unity_project / "ProjectSettings").mkdir(exist_ok=True)
    (unity_project / "Packages" / "manifest.json").write_text('{"dependencies":{}}', encoding="utf-8")
    (unity_project / "ProjectSettings" / "ProjectVersion.txt").write_text("m_EditorVersion: 6000.4.7f1\n", encoding="utf-8")
    for source in (ROOT / "unity" / "Editor" / "AnimBridgeImporter.cs", ROOT / "unity" / "Editor" / "UnityAnimBridgeBaker.cs",
                   ROOT / "tests" / "UnityReverseValidation.cs"):
        shutil.copy2(source, assets / "Editor")
    report_path = work / "unity_import_report.json"
    evidence_path = work / "unity_evidence.json"
    command = [str(unity), "-batchmode", "-nographics", "-quit", "-projectPath", str(unity_project),
               "-executeMethod", "UnityReverseValidation.Run", "-animBridgeManifest", str(manifest_path),
               "-animBridgeDest", "Assets/ImportedUnreal", "-animBridgeReport", str(report_path),
               "-animBridgeValidation", str(evidence_path), "-logFile", str(work / "unity.log")]
    (work / "unity_command.json").write_text(json.dumps(command, indent=2), encoding="utf-8")
    completed = subprocess.run(command, cwd=ROOT, env=env, capture_output=True, timeout=600)
    (work / "unity_process.log").write_bytes(completed.stdout + completed.stderr)
    assert completed.returncode == 0, f"Unity exit {completed.returncode}; {work / 'unity.log'}"
    evidence = json.loads(evidence_path.read_text())
    assert len(evidence["clips"]) == 1
    item = evidence["clips"][0]
    for key in ("events", "embeddedEvents"):
        assert len(item[key]) == 2
        for actual, expected in zip(item[key], clip["events"], strict=True):
            assert actual["name"] == expected["name"]
            assert actual["function"] == "AnimBridgeSyncMarker"
            assert actual["time"] == pytest.approx(expected["time"], abs=1e-5)
            assert actual["messageOptions"] == "DontRequireReceiver"
    unity_pose = json.loads(Path(item["baked"]).read_text())["clips"][0]
    assert unity_pose["frames"] == 11 and unity_pose["duration"] == pytest.approx(2.5, abs=1e-5)
    errors = {}
    for bone in ("Hips", "Spine", "Hand"):
        path = next(path for path in unity_pose["tracks"] if path == bone or path.endswith("/" + bone))
        track = unity_pose["tracks"][path]
        expected_r = np.asarray([frame[bone]["r"] for frame in ue_case["poses"]]) * [-1, -1, 1, 1]
        expected_t = np.asarray([frame[bone]["t"] for frame in ue_case["poses"]]) * [-0.01, -0.01, 0.01]
        if bone == "Hips":
            # UE's exporter explicitly uses Z-up RH (force_front_x_axis=False).
            # Unity's Z-up -> Y-up axis conversion affects only this top node.
            c = np.array([[1, 0, 0], [0, 0, 1], [0, -1, 0]])
            mirror = np.diag([-1, 1, 1])
            fbx_r = coords.quat_to_mat3(np.asarray([frame[bone]["r"] for frame in ue_case["poses"]]) * [1, -1, 1, -1])
            expected_r = coords.mat3_to_quat(mirror @ c @ fbx_r @ mirror)
            expected_t = np.asarray([frame[bone]["t"] for frame in ue_case["poses"]])[:, [0, 2, 1]] * [-0.01, 0.01, 0.01]
        actual_r = np.asarray(track["r"])
        expected_r /= np.linalg.norm(expected_r, axis=1)[:, None]
        actual_r /= np.linalg.norm(actual_r, axis=1)[:, None]
        degrees = np.degrees(2 * np.arccos(np.clip(np.abs(np.sum(expected_r * actual_r, axis=1)), -1, 1)))
        expected_s = np.asarray([frame[bone]["s"] for frame in ue_case["poses"]])
        errors[bone] = {"rotation_degrees": float(np.max(degrees)),
                        "translation_m": float(np.max(np.abs(expected_t - np.asarray(track["t"])))),
                        "scale": float(np.max(np.abs(expected_s - np.asarray(track["s"]))))}
        (work / "pose_errors.json").write_text(json.dumps(errors, indent=2), encoding="utf-8")
        assert np.max(degrees) <= 0.1, errors
        np.testing.assert_allclose(track["t"], expected_t, atol=1e-4, rtol=0)
        np.testing.assert_allclose(track["s"], expected_s, atol=1e-4, rtol=0)
