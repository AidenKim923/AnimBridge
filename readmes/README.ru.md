<p align="center">
  <img src="../assets/readme/animbridge-readme-banner.png" alt="AnimBridge: move animation between engines" width="100%">
</p>

<h1 align="center">AnimBridge</h1>

<p align="center">
  <strong>Переносите анимации между Unity и Unreal Engine.</strong><br>
  Перетащите пакет, просмотрите нужные клипы и экспортируйте только их.
</p>

<div align="center">

[![Version](https://img.shields.io/github/v/tag/AidenKim923/AnimBridge?sort=semver&style=flat-square&color=6d5dfc)](https://github.com/AidenKim923/AnimBridge/tags)
![Windows x64](https://img.shields.io/badge/platform-Windows%20x64-0078D6?style=flat-square)
[![MIT License](https://img.shields.io/badge/license-MIT-34a853?style=flat-square)](../LICENSE)
![15 languages](https://img.shields.io/badge/languages-15-e69e34?style=flat-square)

[Возможности](#возможности) · [Как это работает](#как-это-работает) · [Требования](#требования) · [Участие в разработке](#участие-в-разработке) · [Лицензия](#лицензия)

</div>

<!-- readme-languages -->
<div align="center">

[English](../README.md) · [Español](README.es.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [简体中文](README.zh-CN.md) · [繁體中文](README.zh-TW.md) · [Polski](README.pl.md) · [हिन्दी](README.hi.md) · **Русский** · [العربية](README.ar.md) · [Português (Brasil)](README.pt-BR.md) · [Português (Portugal)](README.pt-PT.md) · [Français](README.fr.md) · [ไทย](README.th.md) · [Türkçe](README.tr.md)

</div>
<!-- /readme-languages -->

---

## Возможности

- **Перетащите и изучите.** Откройте `.unitypackage`, папку, `.anim` или `.fbx`; находите, фильтруйте и выбирайте нужные клипы.
- **Предпросмотр перед экспортом.** Перед экспортом воспроизводите клипы на стандартном персонаже из комплекта поставки без установки Unreal. Для гуманоидных и бинарных клипов требуется Unity Editor.
- **Выбирайте формат результата.** Извлекайте исходные файлы или запекайте вычисленную анимацию в FBX с соответствующим скелетом.
- **Связывайте оба движка.** Используйте вспомогательные средства импорта в Unreal, а также экспорт FBX и манифеста для переноса обратно в Unity.
- **Работайте локально.** Нативное хост-приложение на C++ и интерфейс WebView2 координируют рабочие процессы конвертации на Python и Blender.
- **Используйте свой язык.** Приложение поддерживает 15 языков, включая расположение элементов справа налево для арабского.

## Как это работает

1. Перетащите пакет Unity, папку, файл анимации или FBX в AnimBridge.
2. Найдите и выберите клипы, затем просмотрите их в приложении.
3. Извлеките исходные файлы или запеките выбранные анимации в FBX.
4. Включите результат в целевой рабочий процесс с помощью прилагаемых вспомогательных средств для движков.

## Требования

- **Windows x64.** Windows 10 1809 или Windows 11 с графическим процессором и драйверами, соответствующими требованиям Blender из комплекта поставки.
- **Unity Editor** необходим для вычисления гуманоидных и бинарных клипов. В Windows AnimBridge может подключаться к Unity Editor, установленному через Unity Hub.
- **Unreal Editor** необходим для рабочих процессов импорта в Unreal; при использовании вспомогательных средств на Python включите в нём Python Editor Script Plugin.

Подробности установки с помощью установщика и настройки портативной версии, проверенные версии редакторов и уведомления о сторонних компонентах приведены в [примечаниях по установке](../packaging/INSTALL.txt). Редакторы и лицензии на них не входят в комплект поставки.

## Участие в разработке

Мы приветствуем вклад в проект. См. [CONTRIBUTING.md](../CONTRIBUTING.md) и [политику безопасности](../SECURITY.md).

## Лицензия

AnimBridge распространяется по [лицензии MIT](../LICENSE). Сторонние компоненты, включая Blender и WebView2, сохраняют собственные лицензии; см. [уведомления о сторонних компонентах](../packaging/THIRD_PARTY_NOTICES.txt).

