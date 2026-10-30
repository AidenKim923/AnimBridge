"""Opt-in acceptance against the real C++ WebView2 host, never a mock page.

Start AnimBridge.exe --ui-test, then set ANIMBRIDGE_DESKTOP_UI=1.
The Unity ExportPackage fixture must already exist under _verify/unity.
"""
import hashlib
import json
import os
from pathlib import Path
import time
import uuid

import pytest

pytestmark = pytest.mark.skipif(
    os.environ.get("ANIMBRIDGE_DESKTOP_UI") != "1",
    reason="Requires explicit opt-in and the local AnimBridge --ui-test host",
)


@pytest.fixture(autouse=True)
def korean_interface():
    playwright = pytest.importorskip("playwright.sync_api")
    with playwright.sync_playwright() as p:
        browser = p.chromium.connect_over_cdp("http://127.0.0.1:9223")
        page = browser.contexts[0].pages[0]
        page.wait_for_function("() => window.animbridgeUI !== undefined")
        previous = page.evaluate("async () => (await animbridgeUI.request('getState')).settings.language")
        page.evaluate("animbridgeUI.request('saveSettings', {settings: {language: 'ko'}})")
        page.reload()
        page.wait_for_selector("#app .shell")
    yield
    with playwright.sync_playwright() as p:
        browser = p.chromium.connect_over_cdp("http://127.0.0.1:9223")
        page = browser.contexts[0].pages[0]
        page.evaluate("code => animbridgeUI.request('saveSettings', {settings: {language: code}})", previous)
        page.reload()


def test_header_version_and_support_placeholders():
    playwright = pytest.importorskip("playwright.sync_api")
    version = (Path(__file__).resolve().parents[1] / "VERSION").read_text().strip()
    with playwright.sync_playwright() as p:
        browser = p.chromium.connect_over_cdp("http://127.0.0.1:9223")
        page = browser.contexts[0].pages[0]
        page.reload()
        page.wait_for_selector("#app .shell")
        playwright.expect(page.locator(".topbar")).to_contain_text(version)
        assert "LOCAL WORKSPACE" not in page.locator(".topbar").inner_text()
        page.get_by_role("button", name="후원하기", exact=True).click()
        dialog = page.locator("dialog[open]")
        playwright.expect(dialog).to_contain_text("평생 무료")
        playwright.expect(dialog).to_contain_text("GitHub Stars도 많은 도움이 됩니다.")
        for name in ("Fairy", "GitHub Sponsors"):
            dialog.get_by_role("button", name=name, exact=False).click()
            playwright.expect(dialog).to_be_visible()
            playwright.expect(page.locator("#support-status")).to_contain_text("준비 중")
        page.keyboard.press("Escape")
        playwright.expect(page.locator("dialog")).not_to_be_visible()
        playwright.expect(page.get_by_role("button", name="후원하기", exact=True)).to_be_focused()
        info = page.evaluate("animbridgeUI.request('getAppInfo')")
        assert info["links"]["github"] is True
        playwright.expect(page.get_by_role("button", name="GitHub", exact=True)).to_be_enabled()


def test_real_ui_selection_export_and_failure():
    playwright = pytest.importorskip("playwright.sync_api")
    root = Path(__file__).resolve().parents[1]
    package = root / "_verify/unity/actual_export.unitypackage"
    if not package.is_file():
        pytest.skip("Create the actual Unity ExportPackage verification fixture first")
    output = root / "_verify/desktop/ui_tests" / uuid.uuid4().hex
    output.mkdir(parents=True)
    evidence = {}
    with playwright.sync_playwright() as p:
        browser = p.chromium.connect_over_cdp("http://127.0.0.1:9223")
        page = browser.contexts[0].pages[0]
        errors = []
        page.on("pageerror", lambda error: errors.append(str(error)))
        page.reload()
        page.wait_for_selector("#app .shell")
        page.evaluate("path => animbridgeUI.request('scan',{paths:[path]})", str(package))

        def idle():
            for _ in range(600):
                state = page.evaluate("animbridgeUI.getState()")
                if not state["native"]["busy"]:
                    return state
                time.sleep(.1)
            raise AssertionError("Native operation did not finish")

        idle()
        page.evaluate("animbridgeUI.navigate('library')")
        page.locator("#clip-search").fill("zup_cm")
        page.locator('[data-action="selectNone"]').click()
        page.locator('[data-action="selectVisible"]').click()
        page.wait_for_timeout(100)
        page.locator("#clip-search").fill("default")
        assert page.locator("[data-clip]").count() == 1
        assert len(page.evaluate("animbridgeUI.getState().selected")) == 2
        page.locator("[data-clip]").first.click()
        assert not page.evaluate("animbridgeUI.getState().native.busy")
        page.locator('[data-action="toExport"]').first.click()
        page.locator('[data-action="exportMode"][data-value="bake"]').click()
        page.locator("#export-fps").select_option("30")
        page.locator('[data-action="exportMode"][data-value="extractOriginal"]').click()
        page.locator("#export-output").fill(str(output / "extracted"))
        page.locator('[data-action="planExport"]').click()
        page.wait_for_selector("dialog[open]")
        plan = page.evaluate("animbridgeUI.getState().view.plan")
        assert plan["options"]["sampleRate"] is None
        page.get_by_role("button", name="내보내기 시작", exact=True).click()
        page.wait_for_timeout(100)
        state = idle()
        job = next(j for j in state["native"]["jobs"] if j["attemptId"] == plan["attemptId"])
        assert len(job["results"]) == 2
        for row in job["results"]:
            assert row["status"] == "ok"
            assert hashlib.sha256(Path(row["output"]).read_bytes()).hexdigest() == row["sourceHash"]
        evidence["successfulAttempt"] = job
        page.screenshot(path=str(output / "result.png"))

        # Make the approved output directory unavailable before execution.
        page.evaluate("animbridgeUI.navigate('export')")
        obstruction = output / "obstructed"
        page.locator("#export-output").fill(str(obstruction))
        page.locator('[data-action="planExport"]').click()
        page.wait_for_selector("dialog[open]")
        obstruction.write_text("test-owned output obstruction", encoding="utf-8")
        page.get_by_role("button", name="내보내기 시작", exact=True).click()
        page.wait_for_timeout(100)
        state = idle()
        assert state["native"]["jobs"][-1]["state"] == "failed"
        assert "작업이 중단되었습니다" in page.locator("body").inner_text()
        assert "내보내기가 완료되었습니다" not in page.locator("body").inner_text()
        evidence["failedAttempt"] = state["native"]["jobs"][-1]
        evidence["pageErrors"] = errors
        (output / "evidence.json").write_text(json.dumps(evidence, indent=2), encoding="utf-8")
        assert not errors


def test_every_clip_prepares_automatically_and_hover_plays_only_its_card():
    playwright = pytest.importorskip("playwright.sync_api")
    root = Path(__file__).resolve().parents[1] / "_verify" / "preview-autoload" / uuid.uuid4().hex
    root.mkdir(parents=True)
    source = root / "automatic.json"
    names = [f"Automatic{i}-{root.name}" for i in range(5)]
    source.write_text(json.dumps({"uab_baked": 1, "fps": 30, "clips": [
        {"name": name, "fps": 30, "frames": 61, "duration": 2,
         "tracks": {"Hips": {"t": [[frame * .01, 0, 0] for frame in range(61)]},
                    "Hips/Spine": {"t": [[0, 1, 0] for _ in range(61)]}}}
        for name in names]}), encoding="utf-8")
    with playwright.sync_playwright() as p:
        browser = p.chromium.connect_over_cdp("http://127.0.0.1:9223")
        page = browser.contexts[0].pages[0]
        errors = []
        page.on("pageerror", lambda error: errors.append(str(error)))
        page.evaluate("path => animbridgeUI.request('scan', {paths: [path]})", str(source))
        page.evaluate("animbridgeUI.navigate('library')")
        page.locator("#clip-search").fill("Automatic0-" + root.name)
        page.wait_for_function("""names => names.every(name => {
            const state = animbridgeUI.getState();
            return state.native.session.clips.some(clip => clip.name === name);
        })""", arg=names)
        # Remove the search: all five must be ready without opening any preview.
        page.locator("#clip-search").fill(root.name)
        page.wait_for_function("() => document.querySelectorAll('canvas[data-thumbnail]').length === 5")
        card = page.locator("[data-clip]").first
        card.hover()
        page.wait_for_function("() => document.querySelector('canvas[data-thumbnail]').poseViewer.frame > 0")
        assert page.evaluate("document.querySelectorAll('canvas[data-thumbnail]')[1].poseViewer.frame") == 0
        page.locator("#main-title").hover()
        page.wait_for_function("() => document.querySelector('canvas[data-thumbnail]').poseViewer.frame === 0")
        card.dblclick()
        page.wait_for_function("() => document.querySelector('#main-player')?.poseViewer.frame > 0")
        assert not errors
