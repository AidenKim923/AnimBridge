<p align="center">
  <img src="assets/readme/animbridge-readme-banner.png" alt="AnimBridge: move animation between engines" width="100%">
</p>

<h1 align="center">AnimBridge</h1>

<p align="center">
  <strong>Move animations between Unity and Unreal Engine.</strong><br>
  Drop in a package, preview the clips you need, and export only those.
</p>

<div align="center">

[![Version](https://img.shields.io/github/v/tag/AidenKim923/AnimBridge?sort=semver&style=flat-square&color=6d5dfc)](https://github.com/AidenKim923/AnimBridge/tags)
![Windows x64](https://img.shields.io/badge/platform-Windows%20x64-0078D6?style=flat-square)
[![MIT License](https://img.shields.io/badge/license-MIT-34a853?style=flat-square)](LICENSE)
![15 languages](https://img.shields.io/badge/languages-15-e69e34?style=flat-square)

[Features](#features) · [How it works](#how-it-works) · [Requirements](#requirements) · [Contributing](#contributing) · [License](#license)

</div>

<!-- readme-languages -->
<div align="center">

**English** · [Español](readmes/README.es.md) · [日本語](readmes/README.ja.md) · [한국어](readmes/README.ko.md) · [简体中文](readmes/README.zh-CN.md) · [繁體中文](readmes/README.zh-TW.md) · [Polski](readmes/README.pl.md) · [हिन्दी](readmes/README.hi.md) · [Русский](readmes/README.ru.md) · [العربية](readmes/README.ar.md) · [Português (Brasil)](readmes/README.pt-BR.md) · [Português (Portugal)](readmes/README.pt-PT.md) · [Français](readmes/README.fr.md) · [ไทย](readmes/README.th.md) · [Türkçe](readmes/README.tr.md)

</div>
<!-- /readme-languages -->

---

## Features

- **Drop and inspect.** Open a `.unitypackage`, folder, `.anim`, or `.fbx`; search, filter, and select the clips you want.
- **Preview before export.** Play clips on a bundled default character before exporting, without an Unreal installation. Humanoid and binary clips require Unity Editor.
- **Choose your output.** Extract original files or bake evaluated animation to FBX with a matching skeleton.
- **Bridge both engines.** Use Unreal import helpers, plus FBX and manifest export back to Unity.
- **Work locally.** A native C++ host and WebView2 UI coordinate Python and Blender conversion workers.
- **Use your language.** The app supports 15 languages, including a right-to-left layout for Arabic.

## How it works

1. Drop a Unity package, folder, animation, or FBX into AnimBridge.
2. Search and select clips, then preview them in the app.
3. Extract the originals or bake the selected animations to FBX.
4. Bring the output into your target workflow with the supplied engine helpers.
   Baked animation FBX files contain bones but no mesh. When importing them into Unreal by hand, import the character model in the output's `Character` folder first, then import the animations onto the Skeleton it creates.

## Requirements

- **Windows x64.** Windows 10 1809 or Windows 11, with a GPU and drivers meeting the bundled Blender requirements.
- **Unity Editor** is needed to evaluate humanoid and binary clips. On Windows, AnimBridge can connect to a Unity Editor installed through Unity Hub.
- **Unreal Editor** is needed for Unreal import workflows; enable its Python Editor Script Plugin when using the Python helpers.

See [installation notes](packaging/INSTALL.txt) for packaged installer and portable setup details, verified editor versions, and third-party notices. Editors and their licenses are not bundled.

## Contributing

Contributions are welcome. See [CONTRIBUTING.md](CONTRIBUTING.md) and the [security policy](SECURITY.md).

## License

Copyright © 2026 Minsung Kim (aurenixs) and AnimBridge contributors.

AnimBridge is [MIT licensed](LICENSE). Third-party components, including Blender and WebView2, retain their own licenses; see the [third-party notices](packaging/THIRD_PARTY_NOTICES.txt).
