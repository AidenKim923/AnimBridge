from importlib import metadata
from pathlib import Path
import runpy

import unityanimbridge
from unityanimbridge import blender_runner, desktop, runtime


def test_helper_default_storage_is_separate_from_backend(tmp_path, monkeypatch):
    backend = tmp_path / "read only installation" / "backend"
    writable = tmp_path / "user data"
    monkeypatch.setenv("UAB_DATA_DIR", str(writable))
    monkeypatch.setattr(desktop, "PROJECT_ROOT", backend)
    work = desktop._work({}, "preview")
    assert work.is_relative_to(writable / "helper" / "preview")
    assert (work / ".animbridge-owned.json").is_file()
    assert not backend.exists()


def test_default_user_data_root(tmp_path, monkeypatch):
    for name in ("UAB_DATA_DIR", "UAB_RUNTIME_MODE", "UAB_RUNTIME_MANIFEST", "UAB_RUNTIME_ROOT"):
        monkeypatch.delenv(name, raising=False)
    monkeypatch.setenv("LOCALAPPDATA", str(tmp_path / "Local"))
    monkeypatch.setenv("XDG_DATA_HOME", str(tmp_path / "xdg"))
    expected = runtime.default_dirs()["data_dir"]
    assert blender_runner.default_data_root() == expected
    if runtime.host_platform() == "windows":
        assert expected == tmp_path / "Local" / "AnimBridge"
    elif runtime.host_platform() == "linux":
        assert expected == tmp_path / "xdg" / "AnimBridge"


def test_bundled_blender_precedes_system_discovery(tmp_path, monkeypatch):
    backend = tmp_path / "portable" / "backend"
    bundled = backend.parent / "runtime" / "blender" / runtime.blender_binary_name(runtime.host_platform())
    bundled.parent.mkdir(parents=True)
    bundled.write_bytes(b"path fixture")
    monkeypatch.setattr(blender_runner, "PROJECT_ROOT", backend)
    for name in ("UAB_BLENDER", "UAB_RUNTIME_MODE", "UAB_RUNTIME_MANIFEST", "UAB_RUNTIME_ROOT"):
        monkeypatch.delenv(name, raising=False)
    monkeypatch.setattr(runtime, "check_executable", lambda *_a, **_k: None)
    monkeypatch.setattr(blender_runner.runtime, "system_blender_candidates",
                        lambda *_: (_ for _ in ()).throw(AssertionError("System discovery must not run")))
    monkeypatch.setattr(blender_runner.glob, "glob", lambda _: (_ for _ in ()).throw(AssertionError("System discovery must not run")))
    assert blender_runner.find_blender() == bundled
    explicit = tmp_path / "custom.exe"
    explicit.write_bytes(b"path fixture")
    assert blender_runner.find_blender(str(explicit)) == explicit


def test_version_uses_installed_metadata(monkeypatch):
    monkeypatch.setattr(metadata, "version", lambda _: "1.2.3")
    assert runpy.run_path(unityanimbridge.__file__)["__version__"] == "1.2.3"


def test_version_reads_uninstalled_bundle_version(tmp_path, monkeypatch):
    def missing_distribution(_):
        raise metadata.PackageNotFoundError("unityanimbridge")

    monkeypatch.setattr(metadata, "version", missing_distribution)
    package = tmp_path / "backend" / "src" / "unityanimbridge" / "__init__.py"
    package.parent.mkdir(parents=True)
    package.write_text(Path(unityanimbridge.__file__).read_text(encoding="utf-8"), encoding="utf-8")
    (tmp_path / "backend" / "VERSION").write_text("2.3.4\n", encoding="utf-8")
    assert runpy.run_path(str(package))["__version__"] == "2.3.4"


def test_version_is_unknown_without_metadata_or_version_file(tmp_path, monkeypatch):
    def missing_distribution(_):
        raise metadata.PackageNotFoundError("unityanimbridge")

    monkeypatch.setattr(metadata, "version", missing_distribution)
    package = tmp_path / "src" / "unityanimbridge" / "__init__.py"
    package.parent.mkdir(parents=True)
    package.write_text(Path(unityanimbridge.__file__).read_text(encoding="utf-8"), encoding="utf-8")
    assert runpy.run_path(str(package))["__version__"] == "0+unknown"
