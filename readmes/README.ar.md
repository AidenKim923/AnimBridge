<p align="center">
  <img src="../assets/readme/animbridge-readme-banner.png" alt="AnimBridge: move animation between engines" width="100%">
</p>

<h1 align="center">AnimBridge</h1>

<p align="center">
  <strong>انقل الرسوم المتحركة بين Unity وUnreal Engine.</strong><br>
  أسقط حزمة، وعاين المقاطع التي تحتاجها، ثم صدّر تلك المقاطع فقط.
</p>

<div align="center">

[![Version](https://img.shields.io/github/v/tag/AidenKim923/AnimBridge?sort=semver&style=flat-square&color=6d5dfc)](https://github.com/AidenKim923/AnimBridge/tags)
![Windows x64](https://img.shields.io/badge/platform-Windows%20x64-0078D6?style=flat-square)
[![MIT License](https://img.shields.io/badge/license-MIT-34a853?style=flat-square)](../LICENSE)
![15 languages](https://img.shields.io/badge/languages-15-e69e34?style=flat-square)

[الميزات](#الميزات) · [آلية العمل](#آلية-العمل) · [المتطلبات](#المتطلبات) · [المساهمة](#المساهمة) · [الترخيص](#الترخيص)

</div>

<!-- readme-languages -->
<div align="center">

[English](../README.md) · [Español](README.es.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [简体中文](README.zh-CN.md) · [繁體中文](README.zh-TW.md) · [Polski](README.pl.md) · [हिन्दी](README.hi.md) · [Русский](README.ru.md) · **العربية** · [Português (Brasil)](README.pt-BR.md) · [Português (Portugal)](README.pt-PT.md) · [Français](README.fr.md) · [ไทย](README.th.md) · [Türkçe](README.tr.md)

</div>
<!-- /readme-languages -->

---

## الميزات

- **أسقط وافحص.** افتح ملف `.unitypackage` أو مجلدًا أو ملف `.anim` أو `.fbx`؛ ثم ابحث عن المقاطع التي تريدها وصفّها واخترها.
- **عاين قبل التصدير.** شغّل المقاطع على شخصية افتراضية مرفقة قبل تصديرها، دون الحاجة إلى تثبيت Unreal. تتطلب مقاطع الشخصيات البشرية والمقاطع الثنائية Unity Editor.
- **اختر مخرجاتك.** استخرج الملفات الأصلية أو اخبز الرسوم المتحركة الناتجة عن التقييم إلى FBX مع هيكل عظمي مطابق.
- **اربط بين المحركين.** استخدم أدوات المساعدة على الاستيراد إلى Unreal، إلى جانب تصدير FBX وملف بيان للعودة إلى Unity.
- **اعمل محليًا.** ينسّق مضيف أصلي مكتوب بلغة C++ وواجهة مستخدم WebView2 عمل عمليات التحويل التي تستخدم Python وBlender.
- **استخدم لغتك.** يدعم التطبيق 15 لغة، بما في ذلك تخطيط من اليمين إلى اليسار للعربية.

## آلية العمل

1. أسقط حزمة Unity أو مجلدًا أو ملف رسوم متحركة أو ملف FBX في AnimBridge.
2. ابحث عن المقاطع واخترها، ثم عاينها داخل التطبيق.
3. استخرج الملفات الأصلية أو اخبز الرسوم المتحركة المحددة إلى FBX.
4. أدخل المخرجات في سير العمل المستهدف باستخدام أدوات المساعدة المرفقة الخاصة بالمحركات.

## المتطلبات

- **Windows x64.** نظام Windows 10 1809 أو Windows 11، مع وحدة معالجة رسومات وبرامج تشغيل تستوفي متطلبات إصدار Blender المرفق.
- **Unity Editor** مطلوب لتقييم مقاطع الشخصيات البشرية والمقاطع الثنائية. على Windows، يمكن لـ AnimBridge الاتصال بنسخة من Unity Editor مثبّتة عبر Unity Hub.
- **Unreal Editor** مطلوب لسير عمل الاستيراد إلى Unreal؛ فعّل إضافة Python Editor Script Plugin فيه عند استخدام أدوات المساعدة المكتوبة بلغة Python.

راجع [ملاحظات التثبيت](../packaging/INSTALL.txt) للاطلاع على تفاصيل الإعداد باستخدام برنامج التثبيت أو النسخة المحمولة، وإصدارات المحررات التي جرى التحقق منها، وإشعارات الأطراف الثالثة. المحررات وتراخيصها غير مرفقة.

## المساهمة

نرحّب بالمساهمات. راجع [CONTRIBUTING.md](../CONTRIBUTING.md) و[سياسة الأمان](../SECURITY.md).

## الترخيص

يخضع AnimBridge لـ[ترخيص MIT](../LICENSE). تحتفظ مكوّنات الأطراف الثالثة، بما فيها Blender وWebView2، بتراخيصها الخاصة؛ راجع [إشعارات الأطراف الثالثة](../packaging/THIRD_PARTY_NOTICES.txt).
