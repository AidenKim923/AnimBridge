<p align="center">
  <img src="../assets/readme/animbridge-readme-banner.png" alt="AnimBridge: move animation between engines" width="100%">
</p>

<h1 align="center">AnimBridge</h1>

<p align="center">
  <strong>Unity와 Unreal Engine 사이에서 애니메이션을 옮기세요.</strong><br>
  패키지를 끌어다 놓고, 필요한 클립을 미리 본 다음 해당 클립만 내보내세요.
</p>

<div align="center">

[![Version](https://img.shields.io/github/v/tag/AidenKim923/AnimBridge?sort=semver&style=flat-square&color=6d5dfc)](https://github.com/AidenKim923/AnimBridge/tags)
![Windows x64](https://img.shields.io/badge/platform-Windows%20x64-0078D6?style=flat-square)
[![MIT License](https://img.shields.io/badge/license-MIT-34a853?style=flat-square)](../LICENSE)
![15 languages](https://img.shields.io/badge/languages-15-e69e34?style=flat-square)

[주요 기능](#주요-기능) · [사용 방법](#사용-방법) · [요구 사항](#요구-사항) · [기여 안내](#기여-안내) · [라이선스](#라이선스)

</div>

<!-- readme-languages -->
<div align="center">

[English](../README.md) · [Español](README.es.md) · [日本語](README.ja.md) · **한국어** · [简体中文](README.zh-CN.md) · [繁體中文](README.zh-TW.md) · [Polski](README.pl.md) · [हिन्दी](README.hi.md) · [Русский](README.ru.md) · [العربية](README.ar.md) · [Português (Brasil)](README.pt-BR.md) · [Português (Portugal)](README.pt-PT.md) · [Français](README.fr.md) · [ไทย](README.th.md) · [Türkçe](README.tr.md)

</div>
<!-- /readme-languages -->

---

## 주요 기능

- **끌어다 놓고 확인하세요.** `.unitypackage`, 폴더, `.anim`, `.fbx`를 열고 원하는 클립을 검색하고 필터링하고 선택할 수 있습니다.
- **내보내기 전에 미리 보세요.** Unreal을 설치하지 않아도 함께 제공되는 기본 캐릭터로 클립을 재생하여 내보내기 전에 확인할 수 있습니다. Humanoid 및 바이너리 클립에는 Unity Editor가 필요합니다.
- **출력 방식을 선택하세요.** 원본 파일을 추출하거나, 평가된 애니메이션을 일치하는 스켈레톤과 함께 FBX로 베이크할 수 있습니다.
- **두 엔진을 연결하세요.** Unreal 가져오기 도우미와 함께, Unity로 되돌리기 위한 FBX 및 매니페스트 내보내기를 사용할 수 있습니다.
- **로컬에서 작업하세요.** 네이티브 C++ 호스트와 WebView2 UI가 Python 및 Blender 변환 워커를 조율합니다.
- **원하는 언어로 사용하세요.** 앱은 15개 언어를 지원하며, 아랍어용 오른쪽에서 왼쪽으로의 레이아웃도 제공합니다.

## 사용 방법

1. Unity 패키지, 폴더, 애니메이션 또는 FBX를 AnimBridge에 끌어다 놓습니다.
2. 클립을 검색하고 선택한 다음 앱에서 미리 봅니다.
3. 원본을 추출하거나 선택한 애니메이션을 FBX로 베이크합니다.
4. 제공되는 엔진 도우미를 사용해 출력물을 원하는 작업 흐름으로 가져옵니다.

## 요구 사항

- **Windows x64.** Windows 10 1809 또는 Windows 11과 함께, 포함된 Blender의 요구 사항을 충족하는 GPU 및 드라이버가 필요합니다.
- Humanoid 및 바이너리 클립을 평가하려면 **Unity Editor**가 필요합니다. Windows에서는 AnimBridge를 Unity Hub로 설치한 Unity Editor에 연결할 수 있습니다.
- Unreal 가져오기 작업 흐름에는 **Unreal Editor**가 필요합니다. Python 도우미를 사용할 때는 Unreal Editor의 Python Editor Script Plugin을 활성화하세요.

설치 프로그램 및 포터블 버전의 설정 세부 사항, 검증된 에디터 버전, 타사 고지는 [설치 안내](../packaging/INSTALL.txt)를 확인하세요. 에디터와 해당 라이선스는 포함되어 있지 않습니다.

## 기여 안내

기여를 환영합니다. [CONTRIBUTING.md](../CONTRIBUTING.md)와 [보안 정책](../SECURITY.md)을 확인하세요.

## 라이선스

Copyright © 2026 Minsung Kim (aurenixs) and AnimBridge contributors.

AnimBridge는 [MIT 라이선스](../LICENSE)로 제공됩니다. Blender와 WebView2를 비롯한 타사 구성 요소에는 각자의 라이선스가 그대로 적용됩니다. [타사 고지](../packaging/THIRD_PARTY_NOTICES.txt)를 확인하세요.

