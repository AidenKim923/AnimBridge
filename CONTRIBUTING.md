# Contributing

Use the [issue forms](https://github.com/AidenKim923/AnimBridge/issues/new/choose)
for bugs, feature ideas, and animation format or engine requests. Include exact
engine versions and a minimal shareable reproduction. English and Korean are
welcome. Pull requests include a checklist for validation and license notices.

Keep changes focused and preserve conversion behavior unless a reproducible
fixture demonstrates a bug. Use English comments to explain non-obvious
constraints; avoid comments that repeat the code.

For vulnerability reports, follow the [security policy](SECURITY.md).

## Development

```powershell
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -e ".[test]"
$env:PATH = "$PWD\.venv\Scripts;$env:PATH"
cmake -S . -B _verify/native/build -A x64
cmake --build _verify/native/build --config Release --parallel
ctest --test-dir _verify/native/build -C Release --output-on-failure
python -m pytest -m "not blender and not unity and not unreal"
```

These commands use the default development setting,
`ANIMBRIDGE_ENABLE_SOURCE_FALLBACK=ON`, to read backend files from the checkout.
For binary distribution, use the `windows-release` preset in
[CMakePresets.json](CMakePresets.json) or the scripts in `packaging/`,
which disable source fallback.

CI (`.github/workflows/ci.yml`) runs the Python tests and the native build with `ctest` on Windows and Linux for every push to `main` and every pull request.

Optional code signing: `packaging/Build-Distribution.ps1` signs `AnimBridge.exe`, `AnimBridgeCli.exe`, `AnimBridgeProcessSupervisor.exe` and the Setup executable with `signtool` (Windows SDK) when a certificate is configured. Set `ANIMBRIDGE_SIGN_CERT_THUMBPRINT` (certificate in the Windows store) or `ANIMBRIDGE_SIGN_PFX` plus `ANIMBRIDGE_SIGN_PFX_PASSWORD`. `ANIMBRIDGE_SIGN_TIMESTAMP_URL` overrides the default RFC 3161 server (`http://timestamp.digicert.com`). Without these variables the build is unsigned.

Engine integration tests require the corresponding local editor. Check the
README for supported platforms and known limitations.
UI translation checks use Node.js 22 or later. Changes to UI text should keep
all 15 locale catalogs and interpolation variables in sync.
Put generated projects, logs, and fixtures in `_verify/`. Do not commit
commercial animation packages, editor installations, credentials, or user data.

The commands above cover the verified Windows x64 development workflow.
macOS/Linux porting is in progress; report the platform and checks actually run
without treating Windows results as verification of another platform.

Describe the problem, the resulting behavior, and the checks actually run in
pull requests. Changes to coordinate conversion need a non-rest-pose fixture
and a note in the pull request explaining the equations that changed.

The desktop and package version comes from `VERSION`. Header destinations are
configured in `native/app-config.json`; leave them empty until public URLs exist.

Use `python packaging/Export-Source.py --label github` for a separately named
source archive. Existing exports are preserved; the optional label adds a
filename suffix for a new review copy.

AnimBridge's own source is licensed under [MIT](LICENSE). Third-party code and
bundled runtimes keep their own licenses and notices. Keep runtime distribution
changes consistent with the
[third-party notices](packaging/THIRD_PARTY_NOTICES.txt).
