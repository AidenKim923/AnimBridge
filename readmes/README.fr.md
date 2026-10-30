<p align="center">
  <img src="../assets/readme/animbridge-readme-banner.png" alt="AnimBridge: move animation between engines" width="100%">
</p>

<h1 align="center">AnimBridge</h1>

<p align="center">
  <strong>Transférez des animations entre Unity et Unreal Engine.</strong><br>
  Déposez un paquet, prévisualisez les clips dont vous avez besoin et exportez uniquement ceux-ci.
</p>

<div align="center">

[![Version](https://img.shields.io/github/v/tag/AidenKim923/AnimBridge?sort=semver&style=flat-square&color=6d5dfc)](https://github.com/AidenKim923/AnimBridge/tags)
![Windows x64](https://img.shields.io/badge/platform-Windows%20x64-0078D6?style=flat-square)
[![MIT License](https://img.shields.io/badge/license-MIT-34a853?style=flat-square)](../LICENSE)
![15 languages](https://img.shields.io/badge/languages-15-e69e34?style=flat-square)

[Fonctionnalités](#fonctionnalités) · [Fonctionnement](#fonctionnement) · [Configuration requise](#configuration-requise) · [Contribuer](#contribuer) · [Licence](#licence)

</div>

<!-- readme-languages -->
<div align="center">

[English](../README.md) · [Español](README.es.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [简体中文](README.zh-CN.md) · [繁體中文](README.zh-TW.md) · [Polski](README.pl.md) · [हिन्दी](README.hi.md) · [Русский](README.ru.md) · [العربية](README.ar.md) · [Português (Brasil)](README.pt-BR.md) · [Português (Portugal)](README.pt-PT.md) · **Français** · [ไทย](README.th.md) · [Türkçe](README.tr.md)

</div>
<!-- /readme-languages -->

---

## Fonctionnalités

- **Déposez et inspectez.** Ouvrez un `.unitypackage`, un dossier, un `.anim` ou un `.fbx` ; recherchez, filtrez et sélectionnez les clips souhaités.
- **Prévisualisez avant d’exporter.** Lisez les clips sur le personnage par défaut fourni avant de les exporter, sans avoir à installer Unreal. Les clips humanoïdes et binaires nécessitent Unity Editor.
- **Choisissez votre sortie.** Extrayez les fichiers d’origine ou effectuez un bake de l’animation évaluée au format FBX avec un squelette correspondant.
- **Reliez les deux moteurs.** Utilisez les outils d’aide à l’importation dans Unreal ainsi que l’exportation FBX et de manifestes pour revenir vers Unity.
- **Travaillez en local.** Une application hôte native en C++ et une interface WebView2 coordonnent les processus de conversion Python et Blender.
- **Utilisez votre langue.** L’application prend en charge 15 langues, avec une mise en page de droite à gauche pour l’arabe.

## Fonctionnement

1. Déposez un paquet Unity, un dossier, une animation ou un FBX dans AnimBridge.
2. Recherchez et sélectionnez des clips, puis prévisualisez-les dans l’application.
3. Extrayez les originaux ou effectuez un bake des animations sélectionnées au format FBX.
4. Intégrez les fichiers obtenus à votre flux de travail cible à l’aide des outils fournis pour les moteurs.

## Configuration requise

- **Windows x64.** Windows 10 1809 ou Windows 11, avec un GPU et des pilotes répondant aux exigences de la version de Blender fournie.
- **Unity Editor** est nécessaire pour évaluer les clips humanoïdes et binaires. Sous Windows, AnimBridge peut se connecter à un Unity Editor installé via Unity Hub.
- **Unreal Editor** est nécessaire pour les flux de travail d’importation dans Unreal ; activez son Python Editor Script Plugin lorsque vous utilisez les outils d’aide en Python.

Consultez les [notes d’installation](../packaging/INSTALL.txt) pour les détails concernant l’installateur et la configuration de la version portable, les versions vérifiées des éditeurs et les mentions relatives aux composants tiers. Les éditeurs et leurs licences ne sont pas fournis.

## Contribuer

Les contributions sont les bienvenues. Consultez [CONTRIBUTING.md](../CONTRIBUTING.md) et la [politique de sécurité](../SECURITY.md).

## Licence

Copyright © 2026 Minsung Kim (aurenixs) and AnimBridge contributors.

AnimBridge est distribué sous [licence MIT](../LICENSE). Les composants tiers, dont Blender et WebView2, conservent leurs propres licences ; consultez les [mentions relatives aux composants tiers](../packaging/THIRD_PARTY_NOTICES.txt).

