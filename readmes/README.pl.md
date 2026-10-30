<p align="center">
  <img src="../assets/readme/animbridge-readme-banner.png" alt="AnimBridge: move animation between engines" width="100%">
</p>

<h1 align="center">AnimBridge</h1>

<p align="center">
  <strong>Przenoś animacje między Unity a Unreal Engine.</strong><br>
  Upuść paczkę, obejrzyj podgląd potrzebnych klipów i wyeksportuj tylko je.
</p>

<div align="center">

[![Version](https://img.shields.io/github/v/tag/AidenKim923/AnimBridge?sort=semver&style=flat-square&color=6d5dfc)](https://github.com/AidenKim923/AnimBridge/tags)
![Windows x64](https://img.shields.io/badge/platform-Windows%20x64-0078D6?style=flat-square)
[![MIT License](https://img.shields.io/badge/license-MIT-34a853?style=flat-square)](../LICENSE)
![15 languages](https://img.shields.io/badge/languages-15-e69e34?style=flat-square)

[Funkcje](#funkcje) · [Jak to działa](#jak-to-działa) · [Wymagania](#wymagania) · [Współtworzenie](#współtworzenie) · [Licencja](#licencja)

</div>

<!-- readme-languages -->
<div align="center">

[English](../README.md) · [Español](README.es.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [简体中文](README.zh-CN.md) · [繁體中文](README.zh-TW.md) · **Polski** · [हिन्दी](README.hi.md) · [Русский](README.ru.md) · [العربية](README.ar.md) · [Português (Brasil)](README.pt-BR.md) · [Português (Portugal)](README.pt-PT.md) · [Français](README.fr.md) · [ไทย](README.th.md) · [Türkçe](README.tr.md)

</div>
<!-- /readme-languages -->

---

## Funkcje

- **Upuść i sprawdź.** Otwórz plik `.unitypackage`, folder, plik `.anim` lub `.fbx`; wyszukuj, filtruj i wybieraj potrzebne klipy.
- **Podgląd przed eksportem.** Przed eksportem odtwórz klipy na dołączonej domyślnej postaci, bez instalowania Unreal. Klipy humanoidalne i binarne wymagają Unity Editor.
- **Wybierz sposób eksportu.** Wyodrębnij oryginalne pliki lub wypal obliczoną animację do formatu FBX z odpowiadającym jej szkieletem.
- **Połącz oba silniki.** Korzystaj z narzędzi pomocniczych do importu w Unreal oraz z eksportu plików FBX i manifestów z powrotem do Unity.
- **Pracuj lokalnie.** Natywna aplikacja hosta w C++ i interfejs WebView2 koordynują procesy konwersji w Pythonie i Blenderze.
- **Korzystaj ze swojego języka.** Aplikacja obsługuje 15 języków, w tym układ od prawej do lewej dla języka arabskiego.

## Jak to działa

1. Upuść paczkę Unity, folder, animację lub plik FBX w AnimBridge.
2. Wyszukaj i wybierz klipy, a następnie obejrzyj ich podgląd w aplikacji.
3. Wyodrębnij oryginały lub wypal wybrane animacje do formatu FBX.
4. Włącz pliki wynikowe do docelowego procesu pracy za pomocą dołączonych narzędzi pomocniczych do silników.

## Wymagania

- **Windows x64.** Windows 10 1809 lub Windows 11, z kartą graficzną i sterownikami spełniającymi wymagania dołączonej wersji Blendera.
- **Unity Editor** jest potrzebny do obliczania animacji z klipów humanoidalnych i binarnych. W systemie Windows AnimBridge może połączyć się z Unity Editor zainstalowanym za pośrednictwem Unity Hub.
- **Unreal Editor** jest potrzebny do importowania zasobów do Unreal; podczas korzystania z narzędzi pomocniczych w Pythonie włącz w nim wtyczkę Python Editor Script Plugin.

W [uwagach dotyczących instalacji](../packaging/INSTALL.txt) znajdziesz szczegóły dotyczące dostarczanego instalatora i konfiguracji wersji przenośnej, zweryfikowane wersje edytorów oraz informacje o komponentach zewnętrznych. Edytory ani ich licencje nie są dołączone.

## Współtworzenie

Zapraszamy do współtworzenia. Zobacz [CONTRIBUTING.md](../CONTRIBUTING.md) i [politykę bezpieczeństwa](../SECURITY.md).

## Licencja

AnimBridge jest udostępniany na [licencji MIT](../LICENSE). Komponenty zewnętrzne, w tym Blender i WebView2, zachowują własne licencje; zobacz [informacje o komponentach zewnętrznych](../packaging/THIRD_PARTY_NOTICES.txt).

