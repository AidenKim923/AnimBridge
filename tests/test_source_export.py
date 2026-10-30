from pathlib import Path
import runpy
from zipfile import ZipFile

import pytest


EXPORTER = runpy.run_path(str(Path(__file__).resolve().parents[1] / "packaging" / "Export-Source.py"))


def make_source(root):
    for name in EXPORTER["ROOT_FILES"]:
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("0.2.3\n" if name == "VERSION" else "source fixture\n", encoding="utf-8")
    for name in ("native/src/main.cpp", "native/include/animbridge/config.hpp.in",
                 "README.ko.md", "README.ja.md", "README.pt-BR.md", "native/ui/locales/en.js",
                 "src/unityanimbridge/__init__.py", "tests/test_example.py", "tests/viewer_test.mjs",
                 ".github/ISSUE_TEMPLATE/bug_report.yml", ".github/PULL_REQUEST_TEMPLATE.md", ".github/CODEOWNERS",
                 "unity/Editor/Importer.cs", "packaging/licenses/Dependency-LICENSE.txt"):
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("source fixture\n", encoding="utf-8")


def test_source_export_is_allowlisted_and_reproducible(tmp_path):
    archives = []
    for name in ("first", "second"):
        root = tmp_path / name
        make_source(root)
        for excluded in ("_verify/secret.log", ".git/config", "docs/REMAINING.md",
                         "docs/DESIGN.md", "docs/DISTRIBUTION_VERIFICATION.md", "docs/OPEN_SOURCE_PREPARATION.md",
                         "src/unityanimbridge/__pycache__/cache.pyc", "src/unityanimbridge.egg-info/PKG-INFO",
                         "native/third_party/runtime.exe", "tests/commercial.fbx", "packaging/runtime.dll"):
            path = root / excluded
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"excluded fixture")
        result = EXPORTER["export_source"](root)
        folder, archive = Path(result["folder"]), Path(result["zip"])
        exported = {path.relative_to(folder).as_posix() for path in folder.rglob("*") if path.is_file()}
        assert "native/include/animbridge/config.hpp.in" in exported
        assert "native/ui/locales/en.js" in exported
        assert "tests/viewer_test.mjs" in exported
        assert {".github/ISSUE_TEMPLATE/bug_report.yml", ".github/PULL_REQUEST_TEMPLATE.md", ".github/CODEOWNERS"} <= exported
        assert {"LICENSE", "README.md", "assets/readme/animbridge-readme-banner.png"} <= exported
        assert not any(path.startswith("docs/") for path in exported)
        assert {"README.ko.md", "README.ja.md", "README.pt-BR.md"} <= exported
        assert not any(path.endswith((".fbx", ".exe", ".dll", ".pyc")) for path in exported)
        assert not any("_verify" in path or ".git/" in path or "egg-info" in path for path in exported)
        assert not any("VERIFICATION" in path or "REMAINING" in path or "OPEN_SOURCE_PREPARATION" in path for path in exported)
        with ZipFile(archive) as zipped:
            assert zipped.namelist() == [f"{folder.name}/{path}" for path in sorted(exported)]
            for path in exported:
                assert zipped.read(f"{folder.name}/{path}") == (folder / path).read_bytes()
        archives.append(archive.read_bytes())
        with pytest.raises(FileExistsError):
            EXPORTER["export_source"](root)
    assert archives[0] == archives[1]


def test_source_export_rejects_escaping_links(tmp_path):
    root = tmp_path / "repository"
    make_source(root)
    external = tmp_path / "external.py"
    external.write_text("private source\n", encoding="utf-8")
    link = root / "src" / "unityanimbridge" / "linked.py"
    try:
        link.symlink_to(external)
    except OSError:
        pytest.skip("Creating symlinks requires Windows developer mode or privileges")
    with pytest.raises(ValueError, match="repository|Linked paths"):
        EXPORTER["export_source"](root)
    assert not (root / "_verify").exists()


def test_source_export_rejects_output_links(tmp_path):
    root = tmp_path / "repository"
    make_source(root)
    external = tmp_path / "external"
    external.mkdir()
    try:
        (root / "_verify").symlink_to(external, target_is_directory=True)
    except OSError:
        pytest.skip("Creating symlinks requires Windows developer mode or privileges")
    with pytest.raises(ValueError, match="repository|Linked paths"):
        EXPORTER["export_source"](root)
    assert not list(external.iterdir())


@pytest.mark.parametrize("existing", ["folder", "zip"])
def test_source_export_preserves_existing_artifacts(tmp_path, existing):
    make_source(tmp_path)
    release = tmp_path / "_verify" / "source-release"
    release.mkdir(parents=True)
    artifact = release / ("AnimBridge-0.2.3-source.zip" if existing == "zip" else "AnimBridge-0.2.3-source")
    if existing == "folder":
        artifact.mkdir()
        artifact /= "keep.txt"
    artifact.write_bytes(b"existing release")
    with pytest.raises(FileExistsError):
        EXPORTER["export_source"](tmp_path)
    assert artifact.read_bytes() == b"existing release"
    assert len(list(release.iterdir())) == 1


def test_source_export_rejects_invalid_version_before_writing(tmp_path):
    make_source(tmp_path)
    (tmp_path / "VERSION").write_text("../../outside", encoding="utf-8")
    with pytest.raises(ValueError, match="VERSION"):
        EXPORTER["export_source"](tmp_path)
    assert not (tmp_path / "_verify").exists()


def add_platform_sources(root):
    files = {
        "native/src/platform/macos/window.mm": "// objc++\n",
        "native/src/platform/linux/window.cpp": "// gtk\n",
        "native/src/platform/windows/deep/nested/impl.cpp": "// win\n",
        "native/include/animbridge/platform/deep/api.h": "// header\n",
        "native/platform/macos/Info.plist": "<plist/>\n",
        "native/app-macos.plist.in": "<plist>@PROJECT_VERSION@</plist>\n",
        "native/platform/macos/AnimBridge.entitlements": "<plist/>\n",
        "native/platform/linux/animbridge.desktop": "[Desktop Entry]\n",
        "native/platform/linux/postinst.sh": "#!/bin/sh\n",
        "native/platform/CMakeLists.txt": "# cmake\n",
        "packaging/Build-Deb.sh": "#!/bin/sh\n",
        "packaging/targets/macarm64.lock.json": "{}\n",
        "packaging/INSTALL-macOS.txt": "install\n",
    }
    for name, text in files.items():
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
    return set(files)


def exported_files(folder):
    folder = Path(folder)
    return {path.relative_to(folder).as_posix() for path in folder.rglob("*") if path.is_file()}


def test_source_export_includes_recursive_platform_sources(tmp_path):
    make_source(tmp_path)
    expected = add_platform_sources(tmp_path)
    exported = exported_files(EXPORTER["export_source"](tmp_path)["folder"])
    assert expected <= exported
    assert {"CMakePresets.json", "packaging/stage.py",
            ".github/workflows/cross-platform.yml"} <= exported
    assert not any(path.startswith("docs/") for path in exported)


def test_source_export_preserves_unix_modes(tmp_path):
    make_source(tmp_path)
    add_platform_sources(tmp_path)
    result = EXPORTER["export_source"](tmp_path)
    prefix = Path(result["folder"]).name
    with ZipFile(result["zip"]) as zipped:
        modes = {info.filename: (info.external_attr >> 16) & 0o777 for info in zipped.infolist()}
    assert modes[f"{prefix}/packaging/Build-Deb.sh"] == 0o755
    assert modes[f"{prefix}/native/platform/linux/postinst.sh"] == 0o755
    assert modes[f"{prefix}/LICENSE"] == 0o644
    assert modes[f"{prefix}/native/src/platform/macos/window.mm"] == 0o644


@pytest.mark.parametrize("excluded", [
    "native/runtime/python/python.cpp", "native/verify/log.cpp", "native/third_party/x/impl.cpp",
    "native/build/gen.h", "native/assets/demo.cpp", "native/src/__pycache__/x.cpp",
])
def test_source_export_excludes_runtime_verify_and_asset_directories(tmp_path, excluded):
    make_source(tmp_path)
    path = tmp_path / excluded
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("// excluded\n", encoding="utf-8")
    assert excluded not in exported_files(EXPORTER["export_source"](tmp_path)["folder"])


@pytest.mark.parametrize("leak", ["C:\\Users\\someone\\project", "/" + "Users/someone/project/x", "/" + "home/someone/x",
                                  "D:\\osk-vault\\note", "D:\\LLMWIKI\\raw"])
def test_source_export_rejects_local_paths_before_writing(tmp_path, leak):
    make_source(tmp_path)
    (tmp_path / "packaging" / "INSTALL.txt").write_text(f"see {leak}\n", encoding="utf-8")
    with pytest.raises(ValueError, match="Local paths"):
        EXPORTER["export_source"](tmp_path)
    assert not (tmp_path / "_verify").exists()


def test_source_export_allows_macos_shared_installation_but_not_user_homes(tmp_path):
    make_source(tmp_path)
    shared = "/" + "Users/Shared/Epic Games/UE_5.8"
    (tmp_path / "packaging" / "INSTALL.txt").write_text(shared, encoding="utf-8")
    assert Path(EXPORTER["export_source"](tmp_path)["zip"]).is_file()
    for home in ("someone", "Shared-personal"):
        assert EXPORTER["leaked_path"](("/" + f"Users/{home}/private").encode(), Path("README.md"))


def test_source_export_rebuilds_from_its_own_output(tmp_path):
    first = tmp_path / "first"
    make_source(first)
    add_platform_sources(first)
    folder = Path(EXPORTER["export_source"](first)["folder"])
    original = {name: (folder / name).read_bytes() for name in exported_files(folder)}
    rebuilt = Path(EXPORTER["export_source"](folder)["folder"])
    assert {name: (rebuilt / name).read_bytes() for name in exported_files(rebuilt)} == original


def test_source_export_requires_license_before_writing(tmp_path):
    make_source(tmp_path)
    (tmp_path / "LICENSE").unlink()
    with pytest.raises(FileNotFoundError, match="LICENSE"):
        EXPORTER["export_source"](tmp_path)
    assert not (tmp_path / "_verify").exists()


def test_source_export_label_preserves_previous_archive(tmp_path):
    make_source(tmp_path)
    first = EXPORTER["export_source"](tmp_path)
    original = Path(first["zip"]).read_bytes()
    updated = EXPORTER["export_source"](tmp_path, "github")
    assert Path(updated["zip"]).name == "AnimBridge-0.2.3-source-github.zip"
    assert Path(first["zip"]).read_bytes() == original
    with pytest.raises(ValueError, match="label"):
        EXPORTER["export_source"](tmp_path, "../outside")
