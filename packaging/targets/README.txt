AnimBridge target dependency locks — 2026-10-30

macarm64, ubuntu22.04, ubuntu24.04 lock의 unresolved 항목을 공식 배포
metadata와 다운로드 SHA-256으로 확정했다. 세 lock은 stage.py check-lock
--require-resolved를 통과한다. status=resolved는 URL/체크섬/출처 확정을 뜻하며
실기 호환성, 서명, 공증, 설치, 오프라인 실행 또는 출시 gate 통과를 뜻하지 않는다.

Python은 기존 Windows lock과 같은 CPython 3.12.10이다. Astral의 공식
python-build-standalone 20250529 release에서 install_only_stripped archive를
선택했고 공식 SHA256SUMS와 실제 다운로드 SHA-256이 일치했다.
https://github.com/astral-sh/python-build-standalone/releases/tag/20250529
https://github.com/astral-sh/python-build-standalone/releases/download/20250529/SHA256SUMS

동일 release의 full archive도 검증해 python/PYTHON.json을 확인했다.
Linux target triple은 x86_64-unknown-linux-gnu이며 upstream은 최대 요구
glibc symbol 2.17을 기록한다. macOS는 aarch64-apple-darwin이고 Python의
deployment target은 11.0이다. 앱의 macOS 지원 하한 14.5는 별도 계약이다.
full archive의 metadata에는 Python-2.0 및 CNRI-Python license ID가 들어 있어
그 값을 사용했다. 단순한 Python 라이선스 이름만으로 bundled native library의
별도 고지를 대신하지 않는다.

install_only_stripped에는 full archive의 python/licenses 묶음이 없다.
각 python asset의 license_bundle은 같은 release full archive의 URL/SHA-256,
python/licenses 경로, PYTHON.json SHA-256, 고지 파일 19개의 개별 SHA-256을
보존한다. staging은 이 묶음을 함께 넣고 inventory를 확인해야 한다.
CPython source와 build recipe source는 python.source/build_recipe_source에
각각 URL/SHA-256으로 연결했다. SHA-256을 직접 계산한 공식 source download는
digest_origin에 그 사실을 적어 upstream이 별도 SHA를 발표한 것처럼 표시하지 않았다.

NumPy 2.2.6 및 PyYAML 6.0.2 wheel/source의 URL과 digest는 공식 PyPI release
JSON의 urls[].digests.sha256에서 읽었고 실제 다운로드와 대조했다.
https://pypi.org/pypi/numpy/2.2.6/json
https://pypi.org/pypi/PyYAML/6.0.2/json

Linux 두 target은 CPython cp312/cp312 manylinux_2_17_x86_64 (manylinux2014)
wheel을 공유한다. Mac NumPy는 macosx_14_0_arm64, PyYAML은
macosx_11_0_arm64 wheel이다. NumPy wheel의 LICENSE.txt에는 본체 외 bundled
구성요소 고지도 있으므로 원본 dist-info 고지를 제거하지 않는다. lock의
bundled_notices는 wheel 내부 고지 파일의 경로/SHA-256을 보존한다.

Blender 5.2.3은 공식 binary SHA256 목록과 source download를 기준으로 한다.
https://download.blender.org/release/Blender5.2/blender-5.2.3.sha256
https://download.blender.org/source/blender-5.2.3.tar.xz
Linux binary archive와 source archive는 실제 다운로드 SHA-256을 대조했다.
Mac arm64 DMG는 공식 checksum만 확정했다. 사용자 요청대로 이 작업에서는
DMG 전체 다운로드, 마운트, 실행, 재서명 및 공증을 수행하지 않았다.
Blender source의 COPYING과 v5.2.3 공식 repository의 COPYING이 byte 단위로
일치함을 확인했다. bundled third-party source/notice의 최종 배포 완결성은
source archive/SBOM/최종 package gate에서 별도로 확인해야 한다.

새 native 의존성 utf8proc 2.10.0 / Unicode 16.0.0의 source URL/SHA-256과
정확한 LICENSE.md URL/SHA-256을 추가했다. LICENSE.md에는 MIT 코드 고지와
Unicode 데이터 고지가 모두 있다. 데이터 고지는 SPDX Unicode-DFS-2015의
조건 (a)/(b)/(c)와 대응하므로 MIT AND Unicode-DFS-2015로 기록했다.
https://github.com/JuliaStrings/utf8proc/releases/tag/v2.10.0
https://spdx.org/licenses/Unicode-DFS-2015.html

lock의 target 값은 stage CLI 이름(macarm64/ubuntu22.04/ubuntu24.04)을 유지한다.
platform은 macos-arm64 또는 linux-x64이다. native runtime-manifest의 정확한
targetId(macos-arm64/linux-x64-ubuntu22.04/linux-x64-ubuntu24.04) 생성은
stage.py의 별도 mapping 책임이며 lock 이름을 임의로 바꾸지 않는다.

확인 명령:
  python -X utf8 packaging/stage.py check-lock --target macarm64 --require-resolved
  python -X utf8 packaging/stage.py check-lock --target ubuntu22.04 --require-resolved
  python -X utf8 packaging/stage.py check-lock --target ubuntu24.04 --require-resolved

실제 다운로드, 공식 JSON/checksum 원문, full-archive metadata, 고지 inventory 및
추출/실행 진단은 _verify/cross-platform/runtime-downloads 아래에 보존한다.
이 경로의 검증 자료를 signed release 또는 실기 G0~G4 근거로 확대 해석하지 않는다.

검증 중 Linux Python archive를 Windows NTFS Docker bind에 직접 추출하면
share/terminfo/H 및 h 등의 대소문자 구분 경로가 합쳐져 symlink loop가 생김을
확인했다. 해당 부분 추출 디렉터리는 staging 입력으로 사용하지 않는다.
Linux runtime 조립은 대소문자를 보존하는 native Linux filesystem에서 수행한다.

이후 task 전용 native Docker volume animbridge-runtime-payload-linux-r1에서
Python/Blender를 원형 추출하고 Linux wheel 두 개와 Python 고지 묶음을 설치했다.
원형 symlink는 Python 1048개, Blender 79개를 보존했다. 원본 Python archive의
bytecode 파일 3개는 최종 staging에서 제거해야 한다.

Ubuntu 22.04(glibc 2.35) 및 24.04(glibc 2.39) container userland에서 네트워크를
끊고 runtime volume과 root filesystem을 읽기 전용으로 둔 실행을 확인했다.
private Python 3.12.10의 -I -B 실행, NumPy 2.2.6 행렬 연산, PyYAML 6.0.2
파싱, Blender 5.2.3의 --version 및 background Python 실행이 모두 exit 0이었다.
Python/Blender 전체 tree hash는 실행 전후 같았다. ELF header는 둘 다 AMD64이고
readelf로 확인한 executable의 최대 GLIBC 요구 symbol은 Python 2.17,
Blender 2.28이다. 이는 GUI, GPU, 엔진 import, DEB 설치 또는 실기 OS gate의
대체 근거가 아니다. 실행 receipt는 runtime-downloads/probe-ubuntu22.04.json,
probe-ubuntu24.04.json, userland-identities.json에 있다.

Mac Python executable의 Mach-O header는 arm64/minimum OS 11.0.0으로 확인했다.
선택한 NumPy wheel의 native extension 19개와 PyYAML extension 1개도 arm64였다.
이 정적 ABI 검사는 macOS 실행이나 DMG 검증을 뜻하지 않는다.
