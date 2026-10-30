"""Exercise the asset write boundary without needing an Unreal installation."""
import importlib.util
from pathlib import Path
import sys
import types

import pytest


@pytest.fixture
def importer(monkeypatch):
    monkeypatch.setitem(sys.modules, "unreal", types.ModuleType("unreal"))
    path = Path(__file__).parents[1] / "unreal" / "uab_import.py"
    spec = importlib.util.spec_from_file_location("animbridge_destination_test", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@pytest.mark.parametrize("destination", ["/Game/UserApproved", "/Game/Characters/Hero_01"])
def test_profile_is_created_under_approved_destination(importer, destination):
    calls = []
    settings = types.SimpleNamespace()
    settings.set_editor_property = lambda *args: None
    importer.unreal.load_asset = lambda path: calls.append(("load", path))
    importer.unreal.load_class = lambda *args: object()
    importer.unreal.AnimBoneCompressionSettings = type("Settings", (), {})
    importer.unreal.AnimBoneCompressionSettingsFactory = lambda: object()
    importer.unreal.new_object = lambda *args, **kwargs: object()
    importer.unreal.AssetToolsHelpers = types.SimpleNamespace(get_asset_tools=lambda: types.SimpleNamespace(
        create_asset=lambda name, folder, *args: calls.append(("create", folder)) or settings))
    importer.unreal.EditorAssetLibrary = types.SimpleNamespace(set_metadata_tag=lambda *args: None,
        save_loaded_asset=lambda *args, **kwargs: True)
    assert importer.sample_preserving_compression(destination) is settings
    assert ("create", destination + "/AnimBridgeGenerated") in calls
    assert all(path.startswith(destination + "/") for _, path in calls)


@pytest.mark.parametrize("destination", ["/Game", "/Game/../Outside", "/Plugin/Outside", "/Game//A", "/Game/A/", "/Game/A.B", "C:/Content", None])
def test_invalid_destination_rejected_before_engine_write(importer, destination):
    importer.unreal.load_asset = lambda _: pytest.fail("Invalid path reached the engine")
    with pytest.raises(ValueError):
        importer.sample_preserving_compression(destination)
