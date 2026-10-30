<p align="center">
  <img src="../assets/readme/animbridge-readme-banner.png" alt="AnimBridge: move animation between engines" width="100%">
</p>

<h1 align="center">AnimBridge</h1>

<p align="center">
  <strong>Transfiere animaciones entre Unity y Unreal Engine.</strong><br>
  Arrastra un paquete, previsualiza los clips que necesitas y exporta solo esos.
</p>

<div align="center">

[![Version](https://img.shields.io/github/v/tag/AidenKim923/AnimBridge?sort=semver&style=flat-square&color=6d5dfc)](https://github.com/AidenKim923/AnimBridge/tags)
![Windows x64](https://img.shields.io/badge/platform-Windows%20x64-0078D6?style=flat-square)
[![MIT License](https://img.shields.io/badge/license-MIT-34a853?style=flat-square)](../LICENSE)
![15 languages](https://img.shields.io/badge/languages-15-e69e34?style=flat-square)

[Características](#características) · [Cómo funciona](#cómo-funciona) · [Requisitos](#requisitos) · [Contribuir](#contribuir) · [Licencia](#licencia)

</div>

<!-- readme-languages -->
<div align="center">

[English](../README.md) · **Español** · [日本語](README.ja.md) · [한국어](README.ko.md) · [简体中文](README.zh-CN.md) · [繁體中文](README.zh-TW.md) · [Polski](README.pl.md) · [हिन्दी](README.hi.md) · [Русский](README.ru.md) · [العربية](README.ar.md) · [Português (Brasil)](README.pt-BR.md) · [Português (Portugal)](README.pt-PT.md) · [Français](README.fr.md) · [ไทย](README.th.md) · [Türkçe](README.tr.md)

</div>
<!-- /readme-languages -->

---

## Características

- **Arrastra e inspecciona.** Abre un `.unitypackage`, una carpeta, un `.anim` o un `.fbx`; busca, filtra y selecciona los clips que quieras.
- **Previsualiza antes de exportar.** Reproduce los clips en el personaje predeterminado incluido antes de exportarlos, sin tener Unreal instalado. Los clips humanoides y binarios requieren Unity Editor.
- **Elige el formato de salida.** Extrae los archivos originales u hornea la animación evaluada a FBX con un esqueleto correspondiente.
- **Conecta ambos motores.** Utiliza las herramientas auxiliares de importación para Unreal y la exportación de FBX y manifiestos de vuelta a Unity.
- **Trabaja en local.** Una aplicación anfitriona nativa en C++ y una interfaz WebView2 coordinan los procesos de conversión de Python y Blender.
- **Usa tu idioma.** La aplicación admite 15 idiomas, incluido un diseño de derecha a izquierda para el árabe.

## Cómo funciona

1. Arrastra un paquete de Unity, una carpeta, una animación o un FBX a AnimBridge.
2. Busca y selecciona clips y, después, previsualízalos en la aplicación.
3. Extrae los originales u hornea las animaciones seleccionadas a FBX.
4. Incorpora los archivos resultantes a tu flujo de trabajo de destino con las herramientas auxiliares para los motores que se incluyen.

## Requisitos

- **Windows x64.** Windows 10 1809 o Windows 11, con una GPU y controladores que cumplan los requisitos de la versión de Blender incluida.
- **Unity Editor** es necesario para evaluar los clips humanoides y binarios. En Windows, AnimBridge puede conectarse a un Unity Editor instalado mediante Unity Hub.
- **Unreal Editor** es necesario para los flujos de trabajo de importación a Unreal; activa su Python Editor Script Plugin cuando utilices las herramientas auxiliares de Python.

Consulta las [notas de instalación](../packaging/INSTALL.txt) para conocer los detalles del instalador y de la configuración de la versión portátil, las versiones de los editores verificadas y los avisos de terceros. Los editores y sus licencias no se incluyen.

## Contribuir

Las contribuciones son bienvenidas. Consulta [CONTRIBUTING.md](../CONTRIBUTING.md) y la [política de seguridad](../SECURITY.md).

## Licencia

AnimBridge se distribuye bajo la [licencia MIT](../LICENSE). Los componentes de terceros, incluidos Blender y WebView2, conservan sus propias licencias; consulta los [avisos de terceros](../packaging/THIRD_PARTY_NOTICES.txt).

