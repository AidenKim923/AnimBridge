<p align="center">
  <img src="../assets/readme/animbridge-readme-banner.png" alt="AnimBridge: move animation between engines" width="100%">
</p>

<h1 align="center">AnimBridge</h1>

<p align="center">
  <strong>在 Unity 和 Unreal Engine 之间迁移动画。</strong><br>
  拖入资源包，预览所需片段，并仅导出这些片段。
</p>

<div align="center">

[![Version](https://img.shields.io/github/v/tag/AidenKim923/AnimBridge?sort=semver&style=flat-square&color=6d5dfc)](https://github.com/AidenKim923/AnimBridge/tags)
![Windows x64](https://img.shields.io/badge/platform-Windows%20x64-0078D6?style=flat-square)
[![MIT License](https://img.shields.io/badge/license-MIT-34a853?style=flat-square)](../LICENSE)
![15 languages](https://img.shields.io/badge/languages-15-e69e34?style=flat-square)

[功能特点](#功能特点) · [使用方法](#使用方法) · [运行要求](#运行要求) · [参与贡献](#参与贡献) · [许可证](#许可证)

</div>

<!-- readme-languages -->
<div align="center">

[English](../README.md) · [Español](README.es.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · **简体中文** · [繁體中文](README.zh-TW.md) · [Polski](README.pl.md) · [हिन्दी](README.hi.md) · [Русский](README.ru.md) · [العربية](README.ar.md) · [Português (Brasil)](README.pt-BR.md) · [Português (Portugal)](README.pt-PT.md) · [Français](README.fr.md) · [ไทย](README.th.md) · [Türkçe](README.tr.md)

</div>
<!-- /readme-languages -->

---

## 功能特点

- **拖入并查看。** 打开 `.unitypackage`、文件夹、`.anim` 或 `.fbx`，搜索、筛选并选择所需片段。
- **导出前先预览。** 无需安装 Unreal，即可在导出前使用随附的默认角色播放片段。人形（Humanoid）片段和二进制片段需要 Unity Editor。
- **选择输出方式。** 提取原始文件，或将求值后的动画连同匹配的骨架一起烘焙为 FBX。
- **连接两大引擎。** 使用 Unreal 导入辅助工具，以及用于传回 Unity 的 FBX 和清单文件导出功能。
- **在本地运行。** 原生 C++ 主程序和 WebView2 界面协调 Python 与 Blender 转换工作进程。
- **使用你的语言。** 应用支持 15 种语言，包括阿拉伯语的从右到左布局。

## 使用方法

1. 将 Unity 资源包、文件夹、动画或 FBX 拖入 AnimBridge。
2. 搜索并选择片段，然后在应用中预览。
3. 提取原始文件，或将所选动画烘焙为 FBX。
4. 使用随附的引擎辅助工具，将输出内容导入目标工作流程。

## 运行要求

- **Windows x64。** 需要 Windows 10 1809 或 Windows 11，以及满足随附 Blender 要求的 GPU 和驱动程序。
- 对人形（Humanoid）片段和二进制片段求值需要 **Unity Editor**。在 Windows 上，AnimBridge 可以连接通过 Unity Hub 安装的 Unity Editor。
- Unreal 导入工作流程需要 **Unreal Editor**；使用 Python 辅助工具时，请启用其 Python Editor Script Plugin 插件。

安装程序版和便携版的设置详情、已验证的编辑器版本及第三方声明，请参阅[安装说明](../packaging/INSTALL.txt)。编辑器及其许可证不随附提供。

## 参与贡献

欢迎参与贡献。请参阅 [CONTRIBUTING.md](../CONTRIBUTING.md) 和[安全政策](../SECURITY.md)。

## 许可证

Copyright © 2026 Minsung Kim (aurenixs) and AnimBridge contributors.

AnimBridge 采用 [MIT 许可证](../LICENSE)。包括 Blender 和 WebView2 在内的第三方组件仍适用各自的许可证；请参阅[第三方声明](../packaging/THIRD_PARTY_NOTICES.txt)。

