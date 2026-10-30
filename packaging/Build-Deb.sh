#!/usr/bin/env bash
# Native Ubuntu assembly. This creates an unsigned candidate; it does not pass G4.
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
target=""; work=""; build=""; cache=""; python_dir=""; blender_dir=""; receipt=""
offline=0; skip_build=0; stage_only=0
while (($#)); do
  case "$1" in
    --target|--work|--build-dir|--cache|--python-dir|--blender-dir|--runtime-receipt)
      (($# >= 2)) || { echo "Missing value for $1" >&2; exit 2; }
      case "$1" in
        --target) target=$2;; --work) work=$2;; --build-dir) build=$2;; --cache) cache=$2;;
        --python-dir) python_dir=$2;; --blender-dir) blender_dir=$2;; --runtime-receipt) receipt=$2;;
      esac
      shift 2;;
    --offline) offline=1; shift;; --skip-build) skip_build=1; shift;; --stage-only) stage_only=1; shift;;
    *) echo "Unknown option: $1" >&2; exit 2;;
  esac
done
[[ $(uname -s) == Linux ]] || { echo 'Build-Deb.sh requires Linux.' >&2; exit 2; }
[[ $(dpkg --print-architecture) == amd64 ]] || { echo 'The initial Linux release requires amd64.' >&2; exit 2; }
if [[ -z $target ]]; then
  # shellcheck source=/dev/null
  . /etc/os-release
  [[ $ID == ubuntu ]] || { echo 'Select a supported Ubuntu target explicitly.' >&2; exit 2; }
  target="ubuntu${VERSION_ID}"
fi
case "$target" in ubuntu22.04|ubuntu24.04) ;; *) echo 'Supported targets: ubuntu22.04, ubuntu24.04' >&2; exit 2;; esac
for tool in python3 cmake dpkg-deb tar; do command -v "$tool" >/dev/null || { echo "Missing build tool: $tool" >&2; exit 2; }; done
work=${work:-"$repo/_verify/distribution-$target"}
mkdir -p -- "$work"
run=$(mktemp -d "$work/run.XXXXXXXX")
cache=${cache:-"$work/downloads"}
build=${build:-"$run/build"}
if (( ! skip_build )); then
  cmake -S "$repo" -B "$build" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
    -DANIMBRIDGE_BUILD_DESKTOP=ON -DANIMBRIDGE_ENABLE_SOURCE_FALLBACK=OFF -DANIMBRIDGE_ENABLE_UI_TEST_HOOKS=OFF \
    | tee "$run/configure.log"
  cmake --build "$build" --parallel | tee "$run/build.log"
fi
if [[ -z $python_dir && -z $blender_dir && -z $receipt ]]; then
  command -v zstd >/dev/null || { echo 'Preparing the private Python license bundle requires the build-only zstd tool.' >&2; exit 2; }
  prepare=(python3 "$repo/packaging/stage.py" prepare-runtimes --target "$target" --cache "$cache" --out "$run/runtimes")
  (( ! offline )) || prepare+=(--offline)
  "${prepare[@]}" | tee "$run/runtime-preparation.json"
  python_dir="$run/runtimes/python"; blender_dir="$run/runtimes/blender"; receipt="$run/runtimes/runtime-receipt.json"
elif [[ -z $python_dir || -z $blender_dir || -z $receipt ]]; then
  echo 'Supply --python-dir, --blender-dir and --runtime-receipt together.' >&2; exit 2
fi
prefix="$run/prefix"
python3 "$repo/packaging/stage.py" stage --target "$target" --repo "$repo" --build-dir "$build" \
  --python-dir "$python_dir" --blender-dir "$blender_dir" --runtime-receipt "$receipt" --download-cache "$cache" \
  --profile production --out "$prefix" | tee "$run/staging.json"
python3 "$repo/packaging/stage.py" manifest --target "$target" --root "$prefix" --repo "$repo" | tee "$run/manifest.json"
python3 "$repo/packaging/stage.py" verify --target "$target" --root "$prefix" | tee "$run/payload-verification.json"
if (( ! stage_only )); then
  version=$(tr -d '\r\n' < "$repo/VERSION")
  python3 "$repo/packaging/stage.py" deb --target "$target" --root "$prefix" --repo "$repo" \
    --output "$run/AnimBridge-$version-$target.deb" | tee "$run/deb-result.json"
fi
printf 'Candidate assembled in %s\nNative installation, offline first run and G2/G4 release gates remain pending.\n' "$run"
