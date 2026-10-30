<p align="center">
  <img src="../assets/readme/animbridge-readme-banner.png" alt="AnimBridge: move animation between engines" width="100%">
</p>

<h1 align="center">AnimBridge</h1>

<p align="center">
  <strong>在 Unity 與 Unreal Engine 之間移轉動畫。</strong><br>
  拖入資源套件，預覽所需片段，並僅匯出這些片段。
</p>

<div align="center">

[![Version](https://img.shields.io/github/v/tag/AidenKim923/AnimBridge?sort=semver&style=flat-square&color=6d5dfc)](https://github.com/AidenKim923/AnimBridge/tags)
![Windows x64](https://img.shields.io/badge/platform-Windows%20x64-0078D6?style=flat-square)
[![MIT License](https://img.shields.io/badge/license-MIT-34a853?style=flat-square)](../LICENSE)
![15 languages](https://img.shields.io/badge/languages-15-e69e34?style=flat-square)

[功能特色](#功能特色) · [使用方式](#使用方式) · [系統需求](#系統需求) · [參與貢獻](#參與貢獻) · [授權](#授權)

</div>

<!-- readme-languages -->
<div align="center">

[English](../README.md) · [Español](README.es.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [简体中文](README.zh-CN.md) · **繁體中文** · [Polski](README.pl.md) · [हिन्दी](README.hi.md) · [Русский](README.ru.md) · [العربية](README.ar.md) · [Português (Brasil)](README.pt-BR.md) · [Português (Portugal)](README.pt-PT.md) · [Français](README.fr.md) · [ไทย](README.th.md) · [Türkçe](README.tr.md)

</div>
<!-- /readme-languages -->

---

## 功能特色

- **拖入並檢視。** 開啟 `.unitypackage`、資料夾、`.anim` 或 `.fbx`，搜尋、篩選並選取所需片段。
- **匯出前先預覽。** 無須安裝 Unreal，即可在匯出前使用隨附的預設角色播放片段。人形（Humanoid）片段和二進位片段需要 Unity Editor。
- **選擇輸出方式。** 擷取原始檔案，或將求值後的動畫連同相符的骨架一起烘焙為 FBX。
- **串接兩大引擎。** 使用 Unreal 匯入輔助工具，以及用於傳回 Unity 的 FBX 與資訊清單匯出功能。
- **在本機運作。** 原生 C++ 主程式與 WebView2 使用者介面協調 Python 和 Blender 轉換工作程序。
- **使用你的語言。** 應用程式支援 15 種語言，包括阿拉伯文的由右至左版面配置。

## 使用方式

1. 將 Unity 資源套件、資料夾、動畫或 FBX 拖入 AnimBridge。
2. 搜尋並選取片段，接著在應用程式中預覽。
3. 擷取原始檔案，或將選取的動畫烘焙為 FBX。
4. 使用隨附的引擎輔助工具，將輸出內容匯入目標工作流程。

## 系統需求

- **Windows x64。** 需要 Windows 10 1809 或 Windows 11，以及符合隨附 Blender 需求的 GPU 和驅動程式。
- 對人形（Humanoid）片段和二進位片段求值需要 **Unity Editor**。在 Windows 上，AnimBridge 可以連線至透過 Unity Hub 安裝的 Unity Editor。
- Unreal 匯入工作流程需要 **Unreal Editor**；使用 Python 輔助工具時，請啟用其 Python Editor Script Plugin 外掛程式。

安裝程式版與可攜版的設定詳情、已驗證的編輯器版本及第三方聲明，請參閱[安裝說明](../packaging/INSTALL.txt)。編輯器及其授權並未隨附提供。

## 參與貢獻

歡迎參與貢獻。請參閱 [CONTRIBUTING.md](../CONTRIBUTING.md) 與[安全政策](../SECURITY.md)。

## 授權

AnimBridge 採用 [MIT 授權](../LICENSE)。包括 Blender 和 WebView2 在內的第三方元件仍適用各自的授權；請參閱[第三方聲明](../packaging/THIRD_PARTY_NOTICES.txt)。

