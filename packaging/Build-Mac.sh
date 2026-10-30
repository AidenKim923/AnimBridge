#!/usr/bin/env bash
# Script preparation only until the user resumes macOS work. Signing is opt-in.
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
work=""; build=""; cache=""; config=""; offline=0; skip_build=0; sign=0; mounted=0
while (($#)); do
  case "$1" in
    --work|--build-dir|--cache|--signing-config)
      (($# >= 2)) || { echo "Missing value for $1" >&2; exit 2; }
      case "$1" in --work) work=$2;; --build-dir) build=$2;; --cache) cache=$2;; --signing-config) config=$2;; esac
      shift 2;;
    --offline) offline=1; shift;; --skip-build) skip_build=1; shift;; --sign) sign=1; shift;;
    *) echo "Unknown option: $1" >&2; exit 2;;
  esac
done
[[ $(uname -s) == Darwin && $(uname -m) == arm64 ]] || { echo 'Build-Mac.sh requires an Apple Silicon Mac.' >&2; exit 2; }
if (( sign )); then
  [[ -n $config && -f $config ]] || { echo '--sign requires a reviewed --signing-config; no credentials are read otherwise.' >&2; exit 2; }
elif [[ -n $config ]]; then
  echo 'A signing config does not authorize signing by itself. Add --sign explicitly.' >&2; exit 2
fi
for tool in python3 cmake hdiutil ditto zstd; do command -v "$tool" >/dev/null || { echo "Missing build tool: $tool" >&2; exit 2; }; done
work=${work:-"$repo/_verify/distribution-macos-arm64"}
mkdir -p -- "$work"
run=$(mktemp -d "$work/run.XXXXXXXX")
cache=${cache:-"$work/downloads"}; build=${build:-"$run/build"}; mount="$run/blender-dmg"
cleanup() { status=$?; trap - EXIT; if (( mounted )); then hdiutil detach "$mount" -quiet || true; fi; exit "$status"; }
trap cleanup EXIT
if (( ! skip_build )); then
  cmake -S "$repo" -B "$build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=14.5 \
    -DBUILD_TESTING=OFF -DANIMBRIDGE_BUILD_DESKTOP=ON -DANIMBRIDGE_ENABLE_SOURCE_FALLBACK=OFF -DANIMBRIDGE_ENABLE_UI_TEST_HOOKS=OFF \
    | tee "$run/configure.log"
  cmake --build "$build" --config Release --parallel | tee "$run/build.log"
fi
fetch=(python3 "$repo/packaging/stage.py" fetch --target macarm64 --cache "$cache")
(( ! offline )) || fetch+=(--offline)
"${fetch[@]}" | tee "$run/downloads.json"
dmg=$(python3 - "$repo/packaging/targets/macarm64.lock.json" "$cache" <<'PY'
import json, pathlib, sys
lock = json.loads(pathlib.Path(sys.argv[1]).read_text())
print(pathlib.Path(sys.argv[2]) / next(row['filename'] for row in lock['assets'] if row['name'] == 'blender'))
PY
)
mkdir -p -- "$mount"
hdiutil attach "$dmg" -nobrowse -readonly -mountpoint "$mount" -plist > "$run/blender-mount.plist"
mounted=1
python3 "$repo/packaging/stage.py" prepare-runtimes --target macarm64 --cache "$cache" --offline \
  --out "$run/runtimes" --mac-blender-app "$mount/Blender.app" | tee "$run/runtime-preparation.json"
hdiutil detach "$mount" -quiet; mounted=0
app="$run/AnimBridge.app"
python3 "$repo/packaging/stage.py" stage --target macarm64 --repo "$repo" --build-dir "$build" \
  --python-dir "$run/runtimes/python" --blender-dir "$run/runtimes/blender" \
  --runtime-receipt "$run/runtimes/runtime-receipt.json" --download-cache "$cache" \
  --profile production --out "$app/Contents" | tee "$run/staging.json"
if (( ! sign )); then
  python3 "$repo/packaging/stage.py" manifest --target macarm64 --root "$app/Contents" --repo "$repo"
  python3 "$repo/packaging/stage.py" verify --target macarm64 --root "$app/Contents"
  printf 'Unsigned candidate only: %s\nSigning, notarization, DMG assembly and macOS execution have not been performed.\n' "$app"
  exit 0
fi
python3 "$repo/packaging/stage.py" sign-macos --root "$app/Contents" --config "$config" --evidence "$run/app-signing.json"
# The payload manifest is written OUTSIDE the app after its final staple.
python3 "$repo/packaging/stage.py" manifest --target macarm64 --root "$app/Contents" --repo "$repo" --signed --notarized
python3 "$repo/packaging/stage.py" verify --target macarm64 --root "$app/Contents"
mkdir -p -- "$run/dmg-source"
ditto "$app" "$run/dmg-source/AnimBridge.app"
cp -- "$run/AnimBridge-payload-manifest.json" "$run/dmg-source/AnimBridge-payload-manifest.json"
version=$(tr -d '\r\n' < "$repo/VERSION")
result="$run/AnimBridge-$version-macos-arm64.dmg"
hdiutil create -volname AnimBridge -srcfolder "$run/dmg-source" -format UDZO "$result"
python3 "$repo/packaging/stage.py" sign-macos --root "$result" --config "$config" --kind dmg --evidence "$run/dmg-signing.json"
printf 'Signed and stapled artifacts: %s\nG1/G2/G3/G4 macOS release gates still require separate execution evidence.\n' "$run"
