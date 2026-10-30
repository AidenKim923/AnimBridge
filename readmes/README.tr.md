<p align="center">
  <img src="../assets/readme/animbridge-readme-banner.png" alt="AnimBridge: move animation between engines" width="100%">
</p>

<h1 align="center">AnimBridge</h1>

<p align="center">
  <strong>Animasyonları Unity ile Unreal Engine arasında taşıyın.</strong><br>
  Bir paketi sürükleyip bırakın, ihtiyacınız olan klipleri önizleyin ve yalnızca bunları dışa aktarın.
</p>

<div align="center">

[![Version](https://img.shields.io/github/v/tag/AidenKim923/AnimBridge?sort=semver&style=flat-square&color=6d5dfc)](https://github.com/AidenKim923/AnimBridge/tags)
![Windows x64](https://img.shields.io/badge/platform-Windows%20x64-0078D6?style=flat-square)
[![MIT License](https://img.shields.io/badge/license-MIT-34a853?style=flat-square)](../LICENSE)
![15 languages](https://img.shields.io/badge/languages-15-e69e34?style=flat-square)

[Özellikler](#özellikler) · [Nasıl çalışır](#nasıl-çalışır) · [Gereksinimler](#gereksinimler) · [Katkıda bulunma](#katkıda-bulunma) · [Lisans](#lisans)

</div>

<!-- readme-languages -->
<div align="center">

[English](../README.md) · [Español](README.es.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [简体中文](README.zh-CN.md) · [繁體中文](README.zh-TW.md) · [Polski](README.pl.md) · [हिन्दी](README.hi.md) · [Русский](README.ru.md) · [العربية](README.ar.md) · [Português (Brasil)](README.pt-BR.md) · [Português (Portugal)](README.pt-PT.md) · [Français](README.fr.md) · [ไทย](README.th.md) · **Türkçe**

</div>
<!-- /readme-languages -->

---

## Özellikler

- **Bırakın ve inceleyin.** Bir `.unitypackage`, klasör, `.anim` veya `.fbx` açın; istediğiniz klipleri arayın, filtreleyin ve seçin.
- **Dışa aktarmadan önce önizleyin.** Unreal kurulumu gerektirmeden, klipleri dışa aktarmadan önce uygulamayla birlikte gelen varsayılan karakter üzerinde oynatın. İnsansı ve ikili biçimdeki klipler için Unity Editor gerekir.
- **Çıktınızı seçin.** Orijinal dosyaları çıkarın veya değerlendirilmiş animasyonu eşleşen bir iskeletle FBX biçimine pişirin.
- **İki motor arasında köprü kurun.** Unreal içe aktarma yardımcılarının yanı sıra Unity'ye geri aktarım için FBX ve bildirim dosyası dışa aktarımını kullanın.
- **Yerel olarak çalışın.** Yerel C++ ana uygulaması ve WebView2 kullanıcı arayüzü, Python ve Blender dönüştürme işçi süreçlerini koordine eder.
- **Kendi dilinizi kullanın.** Uygulama, Arapça için sağdan sola düzen dâhil olmak üzere 15 dili destekler.

## Nasıl çalışır

1. Bir Unity paketini, klasörü, animasyon dosyasını veya FBX dosyasını AnimBridge'e sürükleyip bırakın.
2. Klipleri arayıp seçin, ardından uygulamada önizleyin.
3. Orijinal dosyaları çıkarın veya seçilen animasyonları FBX biçimine pişirin.
4. Sağlanan motor yardımcılarıyla çıktıyı hedef iş akışınıza aktarın.

## Gereksinimler

- **Windows x64.** Birlikte gelen Blender sürümünün gereksinimlerini karşılayan bir GPU ve sürücülerle Windows 10 1809 veya Windows 11.
- **Unity Editor**, insansı ve ikili biçimdeki klipleri değerlendirmek için gereklidir. Windows'ta AnimBridge, Unity Hub üzerinden yüklenmiş bir Unity Editor'a bağlanabilir.
- **Unreal Editor**, Unreal içe aktarma iş akışları için gereklidir; Python yardımcılarını kullanırken Python Editor Script Plugin eklentisini etkinleştirin.

Paketle sunulan yükleyici ve taşınabilir sürüm kurulum ayrıntıları, doğrulanmış editör sürümleri ve üçüncü taraf bildirimleri için [kurulum notlarına](../packaging/INSTALL.txt) bakın. Editörler ve lisansları pakete dâhil değildir.

## Katkıda bulunma

Katkılarınızı bekliyoruz. [CONTRIBUTING.md](../CONTRIBUTING.md) dosyasına ve [güvenlik politikasına](../SECURITY.md) bakın.

## Lisans

Copyright © 2026 Minsung Kim (aurenixs) and AnimBridge contributors.

AnimBridge, [MIT lisansı](../LICENSE) ile sunulur. Blender ve WebView2 dâhil üçüncü taraf bileşenler kendi lisanslarını korur; [üçüncü taraf bildirimlerine](../packaging/THIRD_PARTY_NOTICES.txt) bakın.

