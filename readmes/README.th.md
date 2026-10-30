<p align="center">
  <img src="../assets/readme/animbridge-readme-banner.png" alt="AnimBridge: move animation between engines" width="100%">
</p>

<h1 align="center">AnimBridge</h1>

<p align="center">
  <strong>ย้ายแอนิเมชันระหว่าง Unity และ Unreal Engine</strong><br>
  ลากแพ็กเกจมาวาง ดูตัวอย่างคลิปที่ต้องการ แล้วส่งออกเฉพาะคลิปเหล่านั้น
</p>

<div align="center">

[![Version](https://img.shields.io/github/v/tag/AidenKim923/AnimBridge?sort=semver&style=flat-square&color=6d5dfc)](https://github.com/AidenKim923/AnimBridge/tags)
![Windows x64](https://img.shields.io/badge/platform-Windows%20x64-0078D6?style=flat-square)
[![MIT License](https://img.shields.io/badge/license-MIT-34a853?style=flat-square)](../LICENSE)
![15 languages](https://img.shields.io/badge/languages-15-e69e34?style=flat-square)

[ความสามารถ](#ความสามารถ) · [วิธีการทำงาน](#วิธีการทำงาน) · [ข้อกำหนด](#ข้อกำหนด) · [การร่วมพัฒนา](#การร่วมพัฒนา) · [สัญญาอนุญาต](#สัญญาอนุญาต)

</div>

<!-- readme-languages -->
<div align="center">

[English](../README.md) · [Español](README.es.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [简体中文](README.zh-CN.md) · [繁體中文](README.zh-TW.md) · [Polski](README.pl.md) · [हिन्दी](README.hi.md) · [Русский](README.ru.md) · [العربية](README.ar.md) · [Português (Brasil)](README.pt-BR.md) · [Português (Portugal)](README.pt-PT.md) · [Français](README.fr.md) · **ไทย** · [Türkçe](README.tr.md)

</div>
<!-- /readme-languages -->

---

## ความสามารถ

- **วางแล้วตรวจสอบ** เปิดไฟล์ `.unitypackage` โฟลเดอร์ ไฟล์ `.anim` หรือ `.fbx` แล้วค้นหา กรอง และเลือกคลิปที่ต้องการ
- **ดูตัวอย่างก่อนส่งออก** เล่นคลิปบนตัวละครเริ่มต้นที่มาพร้อมแอปก่อนส่งออก โดยไม่ต้องติดตั้ง Unreal คลิปแบบมนุษย์และคลิปแบบไบนารีต้องใช้ Unity Editor
- **เลือกรูปแบบผลลัพธ์** แยกไฟล์ต้นฉบับ หรือเบกแอนิเมชันที่ผ่านการประเมินเป็น FBX พร้อมโครงกระดูกที่ตรงกัน
- **เชื่อมต่อทั้งสองเอนจิน** ใช้ตัวช่วยนำเข้า Unreal รวมถึงการส่งออก FBX และไฟล์รายการกำกับกลับไปยัง Unity
- **ทำงานภายในเครื่อง** โปรแกรมโฮสต์แบบเนทีฟที่เขียนด้วย C++ และส่วนติดต่อผู้ใช้ WebView2 ประสานการทำงานของโปรเซสแปลงไฟล์ด้วย Python และ Blender
- **ใช้ภาษาของคุณ** แอปรองรับ 15 ภาษา รวมถึงการจัดวางจากขวาไปซ้ายสำหรับภาษาอาหรับ

## วิธีการทำงาน

1. ลากแพ็กเกจ Unity โฟลเดอร์ ไฟล์แอนิเมชัน หรือ FBX มาวางใน AnimBridge
2. ค้นหาและเลือกคลิป แล้วดูตัวอย่างภายในแอป
3. แยกไฟล์ต้นฉบับ หรือเบกแอนิเมชันที่เลือกเป็น FBX
4. นำผลลัพธ์ไปใช้ในขั้นตอนการทำงานปลายทางด้วยตัวช่วยสำหรับเอนจินที่ให้มาด้วย

## ข้อกำหนด

- **Windows x64** Windows 10 1809 หรือ Windows 11 พร้อมหน่วยประมวลผลกราฟิกและไดรเวอร์ที่ตรงตามข้อกำหนดของ Blender รุ่นที่ให้มาด้วย
- **Unity Editor** จำเป็นสำหรับการประเมินคลิปแบบมนุษย์และคลิปแบบไบนารี บน Windows นั้น AnimBridge สามารถเชื่อมต่อกับ Unity Editor ที่ติดตั้งผ่าน Unity Hub ได้
- **Unreal Editor** จำเป็นสำหรับขั้นตอนการนำเข้า Unreal เมื่อใช้ตัวช่วย Python ให้เปิดใช้งาน Python Editor Script Plugin ใน Unreal Editor

ดู[หมายเหตุการติดตั้ง](../packaging/INSTALL.txt) สำหรับรายละเอียดการตั้งค่าด้วยตัวติดตั้งและแบบพกพา รุ่นของเอดิเตอร์ที่ผ่านการตรวจสอบ และประกาศเกี่ยวกับส่วนประกอบของบุคคลที่สาม เอดิเตอร์และสิทธิ์การใช้งานไม่ได้รวมอยู่ในชุดนี้

## การร่วมพัฒนา

ยินดีต้อนรับผู้ที่ต้องการร่วมพัฒนา ดู [CONTRIBUTING.md](../CONTRIBUTING.md) และ[นโยบายความปลอดภัย](../SECURITY.md)

## สัญญาอนุญาต

AnimBridge ใช้[สัญญาอนุญาต MIT](../LICENSE) ส่วนประกอบของบุคคลที่สาม รวมถึง Blender และ WebView2 ยังคงใช้สัญญาอนุญาตของตนเอง ดู[ประกาศเกี่ยวกับส่วนประกอบของบุคคลที่สาม](../packaging/THIRD_PARTY_NOTICES.txt)

