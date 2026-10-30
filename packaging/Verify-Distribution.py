"""Verify a real installed candidate using only its manifest-selected runtimes.

This records local acceptance, never a clean-machine or full G4 pass. Every
payload addition/change/deletion (including vendor bytecode) is a failure.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import time

_SPEC = importlib.util.spec_from_file_location("animbridge_stage", Path(__file__).with_name("stage.py"))
stage = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(stage)


def selected_bundle(bundle, requested=None):
    root = bundle.resolve()
    if root.suffix == ".app":
        root /= "Contents"
    candidates = [root / "runtime-manifest.json", root / "Resources/runtime-manifest.json", root / "share/runtime-manifest.json"]
    manifests = [path for path in candidates if path.is_file()]
    if len(manifests) != 1:
        raise ValueError("The bundle must have exactly one runtime manifest")
    manifest = json.loads(manifests[0].read_text(encoding="utf-8"))
    target = stage.canonical(manifest["targetId"])
    if requested and stage.canonical(requested) != target:
        raise ValueError("Requested target differs from the runtime manifest")
    return root, target, manifest


def isolated_environment(work):
    env = {key: value for key, value in os.environ.items() if not key.upper().startswith(
        ("PYTHON", "UAB_", "ANIMBRIDGE_", "LD_", "DYLD_", "WEBKIT_", "WEBVIEW2_", "BLENDER_"))}
    if os.name == "nt":
        windows = next((value for key, value in env.items() if key.upper() == "SYSTEMROOT"), None)
        if not windows:
            raise ValueError("Windows SYSTEMROOT is unavailable")
        env["PATH"] = str(Path(windows) / "System32")
    else:
        env["PATH"] = "/usr/bin:/bin:/usr/sbin:/sbin"
    for name in ("home", "tmp", "blender-config", "blender-scripts", "blender-data"):
        (work / name).mkdir()
    env.update({"HOME": str(work / "home"), "TMPDIR": str(work / "tmp"), "TMP": str(work / "tmp"), "TEMP": str(work / "tmp"),
                "PYTHONHOME": str(work / "external-python"), "PYTHONPATH": str(work / "external-modules"),
                "PYTHONDONTWRITEBYTECODE": "1", "UAB_BLENDER": str(work / "external-blender"),
                "UAB_DATA_DIR": str(work / "external-data"), "UAB_UNITY": str(work / "external-unity"),
                "WEBKIT_INSPECTOR_SERVER": "127.0.0.1:9223", "WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS": "--remote-debugging-port=9223",
                "BLENDER_USER_CONFIG": str(work / "blender-config"), "BLENDER_USER_SCRIPTS": str(work / "blender-scripts"),
                "BLENDER_USER_DATAFILES": str(work / "blender-data")})
    return env


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", type=Path, required=True)
    parser.add_argument("--target")
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--package", type=Path)
    parser.add_argument("--fbx", type=Path)
    parser.add_argument("--expected-clips", type=int, default=2)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--evidence-root", type=Path, default=Path(__file__).resolve().parents[1] / "_verify")
    parser.add_argument("--layout-only", action="store_true")
    parser.add_argument("--require-network-disabled", action="store_true")
    parser.add_argument("--profile", choices=("production", "harness"), default="production")
    args = parser.parse_args(argv)
    bundle, target, runtime = selected_bundle(args.bundle, args.target)
    work, allowed = args.work.resolve(), args.evidence_root.resolve()
    if not work.is_relative_to(allowed) or work == allowed or work.is_relative_to(bundle):
        raise ValueError("Verification work must be a new directory below the named evidence root, outside the installation")
    if bool(args.package) != bool(args.fbx):
        raise ValueError("Supply --package and --fbx together")
    work.mkdir(parents=True, exist_ok=False)
    cwd = work / "unrelated cwd"; cwd.mkdir()
    report = {"bundle": str(bundle), "targetId": runtime["targetId"], "work": str(work), "scope": "local-bundle-acceptance",
              "second_pc_tested": False, "releaseGatesPassed": False, "network_disabled": False, "steps": [], "status": "running"}
    inputs = {str(path.resolve()): stage.sha256_file(path) for path in (args.package, args.fbx) if path}
    env = isolated_environment(work)

    def run(label, executable, arguments, *, environment=None, expected_exit=0):
        start = time.monotonic()
        result = subprocess.run([str(executable), *map(str, arguments)], cwd=cwd, env=environment or env,
                                capture_output=True, text=True, encoding="utf-8", errors="strict", timeout=3900)
        (work / (label + ".stdout.txt")).write_text(result.stdout, encoding="utf-8")
        (work / (label + ".stderr.txt")).write_text(result.stderr, encoding="utf-8")
        report["steps"].append({"name": label, "exit_code": result.returncode, "seconds": round(time.monotonic() - start, 3)})
        if (expected_exit == 0 and result.returncode != 0) or (expected_exit != 0 and result.returncode == 0):
            raise AssertionError(f"{label}: exit {result.returncode}: {result.stderr[-2000:]}")
        return result.stdout, result.stderr

    try:
        report["layout"] = stage.verify(target, bundle, args.manifest, require_manifest=True)
        distribution_path = args.manifest or stage.default_distribution_manifest(target, bundle)
        distribution = json.loads(distribution_path.read_text(encoding="utf-8-sig"))
        if distribution.get("source_fallback") is not False or distribution.get("test_hooks") != (args.profile == "harness"):
            raise AssertionError("The candidate lacks the requested distribution-mode build provenance")
        report["build_provenance"] = distribution.get("provenance", {})
        report["manifest_files_verified"] = len(distribution["files"])
        report["runtime_manifest_sha256"] = stage.sha256_file(bundle / stage.layout(target)["manifest"])
        before = stage.distribution_files(bundle)
        if args.layout_only:
            report["status"] = "layout-verified"
            return report
        host = "windows" if os.name == "nt" else "macos" if sys.platform == "darwin" else "linux"
        if stage.TARGETS[target]["os"] != host or (target == "macarm64" and platform.machine() != "arm64"):
            raise AssertionError("Runtime verification requires the target OS/architecture")
        if sys.platform.startswith("linux"):
            interfaces = sorted(path.name for path in Path("/sys/class/net").iterdir())
            report["network_interfaces"] = interfaces
            report["network_disabled"] = interfaces == ["lo"]
        if args.require_network_disabled and not report["network_disabled"]:
            raise AssertionError("An isolated network namespace was not observed; network blocking is not established")
        paths = stage.layout(target, args.profile)
        components = runtime["components"]
        py = stage.safe_join(bundle, components["python"]["executablePath"])
        blender = stage.safe_join(bundle, components["blender"]["executablePath"])
        backend = stage.safe_join(bundle, components["backend"]["resourcePath"])
        cli = bundle / paths["cli"]
        stdout, _ = run("isolated-python", py, ["-I", "-B", "-X", "utf8", backend / "runtime_probe.py", "--backend", backend / "src"])
        imports = json.loads(stdout)
        assert imports["isolated"] == 1 and imports["dont_write_bytecode"] is True
        for key in ("executable", "backend", "numpy_path", "yaml_path"):
            assert Path(imports[key]).resolve().is_relative_to(bundle), (key, imports[key])
        assert all(Path(path).resolve().is_relative_to(bundle) for path in imports["sys_path"] if path), imports["sys_path"]
        assert ".".join(map(str, imports["version"])) == components["python"]["version"]
        report["imports"] = imports
        blender_env = {key: value for key, value in env.items() if not key.upper().startswith(("PYTHONHOME", "PYTHONPATH"))}
        stdout, _ = run("blender-startup", blender, ["--background", "--factory-startup", "--python-use-system-env", "--python-expr",
            'import sys,bpy,json; assert sys.dont_write_bytecode; print("ANIMBRIDGE_PROBE:"+json.dumps({"version":bpy.app.version_string,"bytecode_disabled":sys.dont_write_bytecode}))'], environment=blender_env)
        observed = next(json.loads(line.partition(":")[2]) for line in stdout.splitlines() if line.startswith("ANIMBRIDGE_PROBE:"))
        assert observed["version"].startswith(components["blender"]["version"].split()[0])
        report["blender"] = observed
        source = args.package.resolve() if args.package else work / "native-probe.anim"
        if not args.package:
            source.write_text("%YAML 1.1\n--- !u!74 &7400000\nAnimationClip:\n  m_Name: LocalProbe\n  m_SampleRate: 30\n", encoding="utf-8")
        stdout, _ = run("package-scan", cli, ["scan", source, "--data-dir", work / "data"])
        scan = json.loads(stdout)
        assert scan["clips"], scan
        if args.package:
            assert len(scan["clips"]) == args.expected_clips, scan
        report["clips"] = [{key: row.get(key) for key in ("name", "kind", "duration", "fps", "sourceHash")} for row in scan["clips"]]
        _, rejected = run("external-runtime-rejected", cli, ["plan", source, "--all", "--out", work / "override-output", "--python", py.parent / "external-python", "--data-dir", work / "override-data"], expected_exit=1)
        assert "override" in rejected.lower(), rejected
        if args.package:
            animation = next(row for row in scan["clips"] if row["kind"] == "anim")
            common = [source, "--select", animation["id"], "--skeleton", args.fbx.resolve(), "--out", work / "converted", "--data-dir", work / "data"]
            stdout, _ = run("conversion-plan", cli, ["plan", *common])
            plan = json.loads(stdout)
            for key in ("python", "blender", "repository"):
                assert Path(plan["options"][key]).resolve().is_relative_to(bundle), plan["options"]
            report["resolved_paths"] = {key: plan["options"][key] for key in ("python", "blender", "repository")}
            stdout, _ = run("conversion-run", cli, ["run", *common])
            converted = json.loads(stdout)
            assert converted["results"] and all(row["status"] not in ("error", "blocked", "failed") for row in converted["results"]), converted
            outputs = list((work / "converted").rglob("*.fbx")); assert outputs, converted
            report["results"] = converted["results"]
            report["output_fbx"] = [{"path": str(path), "sha256": stage.sha256_file(path), "bytes": path.stat().st_size} for path in outputs]
        assert all(stage.sha256_file(Path(path)) == digest for path, digest in inputs.items())
        report["source_hashes_unchanged"] = inputs
        assert before == stage.distribution_files(bundle), "Installation payload changed; no vendor bytecode exceptions are allowed"
        assert not list(bundle.rglob("*.pyc")), "Loose Python bytecode appeared in the installation"
        report["installation_payload_unchanged"] = True
        report["status"] = "passed"
        return report
    except BaseException as error:
        report.update({"status": "failed", "error": str(error)})
        raise
    finally:
        (work / "evidence.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        print(json.dumps({key: value for key, value in report.items() if key in ("status", "scope", "targetId", "steps", "error", "manifest_files_verified", "releaseGatesPassed")}, ensure_ascii=False))


if __name__ == "__main__":
    main()
