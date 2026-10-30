<p align="center">
  <img src="../assets/readme/animbridge-readme-banner.png" alt="AnimBridge: move animation between engines" width="100%">
</p>

<h1 align="center">AnimBridge</h1>

<p align="center">
  <strong>Transfira animações entre o Unity e o Unreal Engine.</strong><br>
  Solte um pacote, visualize os clipes de que precisa e exporte apenas esses.
</p>

<div align="center">

[![Version](https://img.shields.io/github/v/tag/AidenKim923/AnimBridge?sort=semver&style=flat-square&color=6d5dfc)](https://github.com/AidenKim923/AnimBridge/tags)
![Windows x64](https://img.shields.io/badge/platform-Windows%20x64-0078D6?style=flat-square)
[![MIT License](https://img.shields.io/badge/license-MIT-34a853?style=flat-square)](../LICENSE)
![15 languages](https://img.shields.io/badge/languages-15-e69e34?style=flat-square)

[Recursos](#recursos) · [Como funciona](#como-funciona) · [Requisitos](#requisitos) · [Como contribuir](#como-contribuir) · [Licença](#licença)

</div>

<!-- readme-languages -->
<div align="center">

[English](../README.md) · [Español](README.es.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [简体中文](README.zh-CN.md) · [繁體中文](README.zh-TW.md) · [Polski](README.pl.md) · [हिन्दी](README.hi.md) · [Русский](README.ru.md) · [العربية](README.ar.md) · **Português (Brasil)** · [Português (Portugal)](README.pt-PT.md) · [Français](README.fr.md) · [ไทย](README.th.md) · [Türkçe](README.tr.md)

</div>
<!-- /readme-languages -->

---

## Recursos

- **Solte e inspecione.** Abra um `.unitypackage`, uma pasta, um `.anim` ou um `.fbx`; pesquise, filtre e selecione os clipes desejados.
- **Visualize antes de exportar.** Reproduza os clipes no personagem padrão incluído antes de exportá-los, sem precisar instalar o Unreal. Clipes humanoides e binários exigem o Unity Editor.
- **Escolha a saída.** Extraia os arquivos originais ou faça o bake da animação avaliada para FBX com um esqueleto correspondente.
- **Conecte os dois motores.** Use as ferramentas auxiliares de importação para o Unreal e a exportação de FBX e manifestos de volta para o Unity.
- **Trabalhe localmente.** Um aplicativo host nativo em C++ e uma interface WebView2 coordenam os processos de conversão em Python e Blender.
- **Use seu idioma.** O aplicativo oferece suporte a 15 idiomas, incluindo um layout da direita para a esquerda para o árabe.

## Como funciona

1. Solte um pacote do Unity, uma pasta, uma animação ou um FBX no AnimBridge.
2. Pesquise e selecione clipes e, em seguida, visualize-os no aplicativo.
3. Extraia os originais ou faça o bake das animações selecionadas para FBX.
4. Incorpore os arquivos resultantes ao seu fluxo de trabalho de destino com as ferramentas auxiliares fornecidas para os motores.

## Requisitos

- **Windows x64.** Windows 10 1809 ou Windows 11, com uma GPU e drivers que atendam aos requisitos da versão do Blender incluída.
- **Unity Editor** é necessário para avaliar clipes humanoides e binários. No Windows, o AnimBridge pode se conectar a um Unity Editor instalado pelo Unity Hub.
- **Unreal Editor** é necessário para os fluxos de trabalho de importação para o Unreal; ative o Python Editor Script Plugin ao usar as ferramentas auxiliares em Python.

Consulte as [notas de instalação](../packaging/INSTALL.txt) para obter detalhes sobre o instalador e a configuração da versão portátil, as versões verificadas dos editores e os avisos de terceiros. Os editores e suas licenças não estão incluídos.

## Como contribuir

Contribuições são bem-vindas. Consulte [CONTRIBUTING.md](../CONTRIBUTING.md) e a [política de segurança](../SECURITY.md).

## Licença

O AnimBridge é distribuído sob a [licença MIT](../LICENSE). Os componentes de terceiros, incluindo Blender e WebView2, mantêm suas próprias licenças; consulte os [avisos de terceiros](../packaging/THIRD_PARTY_NOTICES.txt).

