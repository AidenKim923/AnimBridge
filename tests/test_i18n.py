"""Catalog and runtime checks without a browser or network connection."""
import json
from pathlib import Path
import re
import shutil
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[1]
UI = ROOT / "native/ui"
LANGUAGES = {"en", "es", "ja", "ko", "zh-CN", "zh-TW", "pl", "hi", "ru",
             "ar", "pt-BR", "pt-PT", "fr", "th", "tr"}


def run_js(script):
    node = shutil.which("node")
    if not node:
        pytest.skip("Node.js is required for JavaScript runtime checks")
    result = subprocess.run([node, "--input-type=module", "-e", script],
                            cwd=ROOT, capture_output=True, text=True, encoding="utf-8")
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout)


def test_catalogs_match_readme_and_preserve_interpolation():
    readme = (ROOT / "README.md").read_text(encoding="utf-8")
    advertised = {"en", *re.findall(r"\]\(readmes/README\.([\w-]+)\.md\)", readme)}
    assert advertised == LANGUAGES
    catalogs = run_js("""
      const codes = %s;
      const catalogs = {};
      for (const code of codes) {
        catalogs[code] = {...(await import(`./native/ui/locales/${code}.js`)).default};
        if (code === 'en' || code === 'ko') Object.assign(catalogs[code], (await import(`./native/ui/locales/native-${code}.js`)).default);
      }
      console.log(JSON.stringify(catalogs));
    """ % json.dumps(sorted(LANGUAGES)))
    baseline = catalogs["en"]
    assert len(baseline) > 100
    referenced = set(re.findall(r"\bt\(['\"]([^'\"]+)['\"]", (UI / "app.js").read_text(encoding="utf-8")))
    referenced.update(re.findall(r'data-i18n="([^"]+)"', (UI / "index.html").read_text(encoding="utf-8")))
    for path in (ROOT / "native/src").glob("*.cpp"):
        referenced.update(re.findall(r'"(native\.[A-Za-z]+)"', path.read_text(encoding="utf-8")))
    assert referenced <= baseline.keys(), referenced - baseline.keys()
    for code, catalog in catalogs.items():
        assert catalog.keys() == baseline.keys(), code
        if code != "en":
            unchanged = [key for key in baseline if catalog[key] == baseline[key]]
            assert len(unchanged) < len(baseline) // 10, (code, unchanged)
        for key, value in catalog.items():
            assert isinstance(value, str) and value.strip(), (code, key)
            assert sorted(re.findall(r"\{\w+\}", value)) == sorted(re.findall(r"\{\w+\}", baseline[key])), (code, key)


def test_locale_runtime_and_fallback():
    result = run_js("""
      globalThis.document = { documentElement: {lang: '', dir: '', setAttribute(name, value) {this[name] = value;}} };
      const i = await import('./native/ui/i18n.js');
      const en = (await import('./native/ui/locales/en.js')).default;
      const ar = (await import('./native/ui/locales/ar.js')).default;
      const key = Object.keys(en).find(k => !en[k].includes('{') && en[k] !== ar[k]);
      i.setLocale('ar');
      const arabic = i.t(key);
      const templateKey = Object.keys(en).find(k => en[k].includes('{name}'));
      const interpolated = i.t(templateKey, {name: '<img src=x>'});
      const savedTranslation = ar[key];
      delete ar[key];
      const missingTranslation = i.t(key);
      ar[key] = savedTranslation;
      i.setLocale('not-a-language');
      const fallback = i.t(key);
      console.log(JSON.stringify({codes: i.SUPPORTED_LOCALES.map(x => x.code),
        arabic, expectedArabic: ar[key], fallback, expectedFallback: en[key], locale: i.getLocale(),
        interpolated, missingTranslation, unknownKey: i.t('missing.test.key'), prototypeKey: i.t('__proto__'), number: i.formatNumber(1234.5), invalidDate: i.formatDate('invalid')}));
    """)
    assert set(result["codes"]) == LANGUAGES
    assert result["arabic"] == result["expectedArabic"]
    assert result["fallback"] == result["expectedFallback"]
    assert result["missingTranslation"] == result["expectedFallback"]
    assert result["locale"] == "en"
    assert "<img src=x>" in result["interpolated"]
    assert result["unknownKey"] == "missing.test.key"
    assert result["prototypeKey"] == "__proto__"
    assert result["number"] == "1,234.5"
    assert result["invalidDate"] == ""


def test_saving_settings_preserves_changes_made_while_waiting():
    result = run_js("""
      import fs from 'node:fs';
      import vm from 'node:vm';
      const source = fs.readFileSync('./native/ui/app.js', 'utf8');
      const start = source.indexOf('async function saveSettings()');
      const end = source.indexOf('\\nasync function action(', start);
      if (start < 0 || end < 0) throw new Error('Settings function not found');
      const requests = [], errors = [];
      const context = {
        request: (action, payload) => new Promise((resolve, reject) => requests.push({payload, resolve, reject})),
        native: {settings: {language: 'en'}}, settingsDraft: {language: 'ja'},
        settingsDirty: true, settingsSaving: false, draft: {},
        normalizeLocale: value => value, setLocale: () => {}, t: key => key,
        toast: () => {}, render: () => {}, showError: error => errors.push(error.message),
      };
      vm.createContext(context);
      vm.runInContext(source.slice(start, end), context);

      const saving = context.saveSettings();
      context.settingsDraft.language = 'fr';
      const duplicate = await context.saveSettings();
      const requestCount = requests.length;
      const first = requests.shift();
      first.resolve({language: 'ja'});
      const firstReturn = await saving;
      const changed = {draft: context.settingsDraft.language, dirty: context.settingsDirty,
        persisted: context.native.settings.language, firstReturn, duplicate, requestCount,
        submitted: first.payload.settings.language};

      const savingNewChoice = context.saveSettings();
      const second = requests.shift();
      second.resolve({language: second.payload.settings.language});
      const nextReturn = await savingNewChoice;
      const saved = {draft: context.settingsDraft.language, dirty: context.settingsDirty, nextReturn};

      context.settingsDraft.language = 'ja'; context.settingsDirty = true;
      const savingBeforeDiscard = context.saveSettings();
      context.settingsDraft = {...context.native.settings}; context.settingsDirty = false;
      requests.shift().resolve({language: 'ja'});
      await savingBeforeDiscard;
      const discarded = {draft: context.settingsDraft.language, dirty: context.settingsDirty};

      context.settingsDraft.language = 'ar'; context.settingsDirty = true;
      const failing = context.saveSettings();
      context.settingsDraft.language = 'pl';
      requests.shift().reject(new Error('write failed'));
      const failureReturn = await failing;
      const failed = {draft: context.settingsDraft.language, dirty: context.settingsDirty,
        saving: context.settingsSaving, failureReturn, errors};
      console.log(JSON.stringify({changed, saved, discarded, failed}));
    """)
    assert result["changed"] == {"draft": "fr", "dirty": True, "persisted": "ja",
                                 "firstReturn": False, "duplicate": False,
                                 "requestCount": 1, "submitted": "ja"}
    assert result["saved"] == {"draft": "fr", "dirty": False, "nextReturn": True}
    assert result["discarded"] == {"draft": "ja", "dirty": False}
    assert result["failed"] == {"draft": "pl", "dirty": True, "saving": False,
                                "failureReturn": False, "errors": ["write failed"]}
