<p align="center">
  <img src="../assets/readme/animbridge-readme-banner.png" alt="AnimBridge: move animation between engines" width="100%">
</p>

<h1 align="center">AnimBridge</h1>

<p align="center">
  <strong>UnityとUnreal Engineの間でアニメーションを移行。</strong><br>
  パッケージをドロップし、必要なクリップをプレビューして、そのクリップだけを書き出せます。
</p>

<div align="center">

[![Version](https://img.shields.io/github/v/tag/AidenKim923/AnimBridge?sort=semver&style=flat-square&color=6d5dfc)](https://github.com/AidenKim923/AnimBridge/tags)
![Windows x64](https://img.shields.io/badge/platform-Windows%20x64-0078D6?style=flat-square)
[![MIT License](https://img.shields.io/badge/license-MIT-34a853?style=flat-square)](../LICENSE)
![15 languages](https://img.shields.io/badge/languages-15-e69e34?style=flat-square)

[主な機能](#主な機能) · [使い方](#使い方) · [動作要件](#動作要件) · [貢献](#貢献) · [ライセンス](#ライセンス)

</div>

<!-- readme-languages -->
<div align="center">

[English](../README.md) · [Español](README.es.md) · **日本語** · [한국어](README.ko.md) · [简体中文](README.zh-CN.md) · [繁體中文](README.zh-TW.md) · [Polski](README.pl.md) · [हिन्दी](README.hi.md) · [Русский](README.ru.md) · [العربية](README.ar.md) · [Português (Brasil)](README.pt-BR.md) · [Português (Portugal)](README.pt-PT.md) · [Français](README.fr.md) · [ไทย](README.th.md) · [Türkçe](README.tr.md)

</div>
<!-- /readme-languages -->

---

## 主な機能

- **ドロップして確認。** `.unitypackage`、フォルダー、`.anim`、`.fbx`を開き、必要なクリップを検索・絞り込み・選択できます。
- **書き出す前にプレビュー。** Unrealをインストールしなくても、同梱のデフォルトキャラクターでクリップを再生し、書き出す前に確認できます。HumanoidクリップとバイナリクリップにはUnity Editorが必要です。
- **出力を選択。** 元のファイルを抽出するか、評価済みのアニメーションを対応するスケルトンとともにFBXにベイクできます。
- **両エンジンを橋渡し。** Unrealへのインポート支援ツールに加え、Unityに戻すためのFBXとマニフェストの書き出しを利用できます。
- **ローカルで動作。** ネイティブC++ホストとWebView2 UIが、PythonとBlenderの変換ワーカーを連携させます。
- **自分の言語で利用。** アプリは15言語に対応し、アラビア語用の右から左へのレイアウトも備えています。

## 使い方

1. Unityパッケージ、フォルダー、アニメーション、またはFBXをAnimBridgeにドロップします。
2. クリップを検索・選択し、アプリ内でプレビューします。
3. 元のファイルを抽出するか、選択したアニメーションをFBXにベイクします。
4. 付属のエンジン支援ツールを使って、出力を目的のワークフローに取り込みます。

## 動作要件

- **Windows x64。** Windows 10 1809またはWindows 11、および同梱のBlenderの要件を満たすGPUとドライバーが必要です。
- Humanoidクリップとバイナリクリップの評価には**Unity Editor**が必要です。Windowsでは、Unity Hub経由でインストールしたUnity EditorにAnimBridgeを接続できます。
- Unrealへのインポートワークフローには**Unreal Editor**が必要です。Python支援ツールを使う場合は、Unreal EditorのPython Editor Script Pluginを有効にしてください。

インストーラー版とポータブル版のセットアップの詳細、動作確認済みのエディターバージョン、サードパーティに関する通知については、[インストールに関する注意事項](../packaging/INSTALL.txt)をご覧ください。エディターおよびそのライセンスは同梱されていません。

## 貢献

貢献を歓迎します。[CONTRIBUTING.md](../CONTRIBUTING.md)と[セキュリティポリシー](../SECURITY.md)をご覧ください。

## ライセンス

Copyright © 2026 Minsung Kim (aurenixs) and AnimBridge contributors.

AnimBridgeは[MITライセンス](../LICENSE)で提供されています。BlenderやWebView2を含むサードパーティ製コンポーネントには、それぞれ独自のライセンスが引き続き適用されます。[サードパーティに関する通知](../packaging/THIRD_PARTY_NOTICES.txt)をご覧ください。

