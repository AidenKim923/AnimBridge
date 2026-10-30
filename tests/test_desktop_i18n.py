"""Opt-in language acceptance against the real C++ WebView2 host."""
import os
from pathlib import Path
import re

import pytest


pytestmark = pytest.mark.skipif(os.environ.get("ANIMBRIDGE_DESKTOP_UI") != "1",
                              reason="Requires the local AnimBridge --ui-test host")


def test_all_languages_save_reload_and_render():
    playwright = pytest.importorskip("playwright.sync_api")
    root = Path(__file__).resolve().parents[1]
    evidence = root / "_verify/i18n/screenshots"
    evidence.mkdir(parents=True, exist_ok=True)
    with playwright.sync_playwright() as p:
        browser = p.chromium.connect_over_cdp("http://127.0.0.1:9223")
        page = browser.contexts[0].pages[0]
        errors = []
        page.on("pageerror", lambda error: errors.append(str(error)))
        page.reload()
        page.wait_for_selector("#app .shell")
        original = page.evaluate("async () => (await animbridgeUI.request('getState')).settings.language")
        try:
            page.evaluate("animbridgeUI.navigate('settings')")
            codes = page.locator("#setting-language option").evaluate_all("els => els.map(e => e.value)")
            assert len(codes) == 15
            for code in codes:
                page.locator("#setting-language").select_option(code)
                page.locator('[data-action="saveSettings"]').click()
                page.wait_for_function("code => document.documentElement.lang === code", arg=code)
                assert page.evaluate("document.documentElement.dir") == ("rtl" if code == "ar" else "ltr")
                page.reload()
                page.wait_for_function("code => window.animbridgeUI?.getState().native.settings.language === code", arg=code)
                assert page.locator("html").get_attribute("lang") == code
                page.evaluate("animbridgeUI.navigate('settings')")
                assert page.locator("#setting-language").input_value() == code
                invalid = page.evaluate("async () => { try { await animbridgeUI.request('saveSettings', {settings: {language: 'invalid'}}); return null; } catch(e) { return e.message; } }")
                expected = page.evaluate("async () => (await import('./i18n.js')).t('native.unsupportedLanguage')")
                assert invalid == expected
                assert page.locator(".page").inner_text().strip()
                if code != "ko":
                    text = page.locator(".page").evaluate("el => {const copy = el.cloneNode(true); copy.querySelector('#setting-language')?.remove(); return copy.textContent;}")
                    assert not any('\uac00' <= c <= '\ud7a3' for c in text)
                if code in {"en", "ar", "ja", "hi", "th", "pt-PT"}:
                    page.screenshot(path=str(evidence / f"settings-{code}.png"))
                for destination in ("home", "library", "avatar", "unity", "export", "queue", "complete", "errors", "history"):
                    page.evaluate("destination => animbridgeUI.navigate(destination)", destination)
                    body = page.locator("#app").inner_text()
                    assert not re.search(r"\b(?:ui\.\d{3}|native\.[A-Za-z]+)\b", body), (code, destination)
                page.evaluate("animbridgeUI.navigate('settings')")
            saved = codes[-1]
            page.locator("#setting-language").select_option("ja")
            assert page.locator("html").get_attribute("lang") == saved
            page.evaluate("animbridgeUI.navigate('home')")
            page.locator('dialog [data-answer="discard"]').click()
            page.evaluate("animbridgeUI.navigate('settings')")
            assert page.locator("#setting-language").input_value() == saved
            assert not errors
        finally:
            page.evaluate("code => animbridgeUI.request('saveSettings', {settings: {language: code}})", original)
            page.reload()
