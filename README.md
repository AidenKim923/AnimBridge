![AnimBridge: move animation between engines](assets/readme/animbridge-readme-banner.png)

# AnimBridge

<!-- readme-languages -->
**English** · [Español](README.es.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [简体中文](README.zh-CN.md) · [繁體中文](README.zh-TW.md) · [Polski](README.pl.md) · [हिन्दी](README.hi.md) · [Русский](README.ru.md) · [العربية](README.ar.md) · [Português (Brasil)](README.pt-BR.md) · [Português (Portugal)](README.pt-PT.md) · [Français](README.fr.md) · [ไทย](README.th.md) · [Türkçe](README.tr.md)
<!-- /readme-languages -->

Move animations between Unity and Unreal Engine.
Drop in a package, pick the clips you want, and export only those.

Free and open source · Windows x64

## Features

- **Drop and inspect.** Drop a `.unitypackage`, folder, `.anim`, or `.fbx`, then search, filter, and select clips.
- **Preview before export.** Play any clip in the app first, on a bundled default character (no Unreal install needed; humanoid and binary clips need Unity Editor; the Windows app connects the Unity Editor installed through Unity Hub automatically).
- **Original or baked.** Extract the original files, or bake evaluated animation to FBX with a matching skeleton.
- **Humanoid and binary clips.** Unity evaluates them for you.
- **Unity to Unreal and back.** Unreal import helpers, plus FBX and manifest export back to Unity.
- **Local and native.** A C++20 host with a local WebView2 UI and Python/Blender conversion workers.
- **15 languages.** Saved language setting, with right-to-left layout for Arabic.
- **Free forever.** MIT licensed.

## Contributing

[CONTRIBUTING.md](CONTRIBUTING.md)

## License

[MIT](LICENSE). Third-party components such as Blender and WebView2 keep their own licenses; see [third-party notices](packaging/THIRD_PARTY_NOTICES.txt).
