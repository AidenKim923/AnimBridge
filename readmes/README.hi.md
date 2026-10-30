<p align="center">
  <img src="../assets/readme/animbridge-readme-banner.png" alt="AnimBridge: move animation between engines" width="100%">
</p>

<h1 align="center">AnimBridge</h1>

<p align="center">
  <strong>Unity और Unreal Engine के बीच एनिमेशन ले जाएँ।</strong><br>
  पैकेज खींचकर छोड़ें, ज़रूरी क्लिप का पूर्वावलोकन करें और केवल उन्हीं को निर्यात करें।
</p>

<div align="center">

[![Version](https://img.shields.io/github/v/tag/AidenKim923/AnimBridge?sort=semver&style=flat-square&color=6d5dfc)](https://github.com/AidenKim923/AnimBridge/tags)
![Windows x64](https://img.shields.io/badge/platform-Windows%20x64-0078D6?style=flat-square)
[![MIT License](https://img.shields.io/badge/license-MIT-34a853?style=flat-square)](../LICENSE)
![15 languages](https://img.shields.io/badge/languages-15-e69e34?style=flat-square)

[विशेषताएँ](#विशेषताएँ) · [इस्तेमाल का तरीका](#इस्तेमाल-का-तरीका) · [आवश्यकताएँ](#आवश्यकताएँ) · [योगदान](#योगदान) · [लाइसेंस](#लाइसेंस)

</div>

<!-- readme-languages -->
<div align="center">

[English](../README.md) · [Español](README.es.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [简体中文](README.zh-CN.md) · [繁體中文](README.zh-TW.md) · [Polski](README.pl.md) · **हिन्दी** · [Русский](README.ru.md) · [العربية](README.ar.md) · [Português (Brasil)](README.pt-BR.md) · [Português (Portugal)](README.pt-PT.md) · [Français](README.fr.md) · [ไทย](README.th.md) · [Türkçe](README.tr.md)

</div>
<!-- /readme-languages -->

---

## विशेषताएँ

- **खींचकर छोड़ें और जाँचें।** `.unitypackage`, फ़ोल्डर, `.anim` या `.fbx` खोलें; अपनी ज़रूरत की क्लिप खोजें, छाँटें और चुनें।
- **निर्यात से पहले पूर्वावलोकन करें।** Unreal स्थापित किए बिना, साथ दिए गए डिफ़ॉल्ट पात्र पर क्लिप चलाकर निर्यात से पहले देखें। ह्यूमनॉइड और बाइनरी क्लिप के लिए Unity Editor आवश्यक है।
- **आउटपुट चुनें।** मूल फ़ाइलें निकालें या मूल्यांकित एनिमेशन को उससे मेल खाते स्केलेटन के साथ FBX में बेक करें।
- **दोनों इंजन को जोड़ें।** Unreal में आयात के सहायक उपकरणों के साथ-साथ Unity में वापस ले जाने के लिए FBX और मैनिफ़ेस्ट निर्यात का उपयोग करें।
- **स्थानीय रूप से काम करें।** नेटिव C++ होस्ट और WebView2 उपयोगकर्ता इंटरफ़ेस, Python और Blender रूपांतरण वर्कर का समन्वय करते हैं।
- **अपनी भाषा में इस्तेमाल करें।** ऐप 15 भाषाओं का समर्थन करता है, जिसमें अरबी के लिए दाएँ से बाएँ लेआउट भी शामिल है।

## इस्तेमाल का तरीका

1. Unity पैकेज, फ़ोल्डर, एनिमेशन या FBX को AnimBridge में खींचकर छोड़ें।
2. क्लिप खोजें और चुनें, फिर ऐप में उनका पूर्वावलोकन करें।
3. मूल फ़ाइलें निकालें या चुने गए एनिमेशन को FBX में बेक करें।
4. साथ दिए गए इंजन सहायक उपकरणों से आउटपुट को अपनी लक्षित कार्यप्रणाली में लाएँ।

## आवश्यकताएँ

- **Windows x64।** Windows 10 1809 या Windows 11, और साथ दिए गए Blender की आवश्यकताओं को पूरा करने वाला GPU तथा ड्राइवर आवश्यक हैं।
- ह्यूमनॉइड और बाइनरी क्लिप का मूल्यांकन करने के लिए **Unity Editor** आवश्यक है। Windows पर AnimBridge, Unity Hub के ज़रिए स्थापित Unity Editor से जुड़ सकता है।
- Unreal में आयात की कार्यप्रणालियों के लिए **Unreal Editor** आवश्यक है; Python सहायक उपकरणों का उपयोग करते समय उसका Python Editor Script Plugin सक्षम करें।

इंस्टॉलर पैकेज और पोर्टेबल संस्करण की स्थापना के विवरण, सत्यापित एडिटर संस्करणों और तृतीय-पक्ष सूचनाओं के लिए [स्थापना संबंधी निर्देश](../packaging/INSTALL.txt) देखें। एडिटर और उनके लाइसेंस साथ नहीं दिए जाते हैं।

## योगदान

योगदान का स्वागत है। [CONTRIBUTING.md](../CONTRIBUTING.md) और [सुरक्षा नीति](../SECURITY.md) देखें।

## लाइसेंस

Copyright © 2026 Minsung Kim (aurenixs) and AnimBridge contributors.

AnimBridge [MIT लाइसेंस](../LICENSE) के अंतर्गत उपलब्ध है। Blender और WebView2 सहित तृतीय-पक्ष घटकों पर उनके अपने लाइसेंस लागू रहते हैं; [तृतीय-पक्ष सूचनाएँ](../packaging/THIRD_PARTY_NOTICES.txt) देखें।

