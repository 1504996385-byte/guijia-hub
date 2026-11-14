# 归巢 HomeHub · 会应声的玄关留言盒

> 一个挂在玄关的小房子，两只小鸟各挂一把钥匙。取下钥匙，鸟飞走，它叮嘱一句；放回钥匙，鸟归巢，它应一声。补上进门出门那两秒，家里没人应声的空缺。

## 它是什么

为「生活节奏不同步的家庭」做的情感硬件：异地老人与子女、早出晚归的夫妻、住校的孩子——想跟家人说句话，却常常碰不上面。我们把「等门、应声、留话」这三件正在消失的小事，做成一个不用学、不用喊的物件。

- **老人不用会**：交互就是拿钥匙、放钥匙这两个每天必做的动作，没有屏幕、没有菜单、没有学习成本。
- **子女能确认**：手机写下「少喝点酒」，父母放回钥匙时真的听到了——每条留言都带回执（已发送 / 设备已收到 / 设备已播放）。

一句话定位：**一个会应声的玄关——补上家人之间那两声「哎」。**

## 系统架构 · 三条链路

| 链路 | 作用 | 依赖网络 |
| --- | --- | --- |
| ① 离线主链（命门） | 干簧管 ×2 识别钥匙身份 → 磁铁靠近/远离触发 → 本地喇叭播放。拔掉网线照样出声 | ❌ 不依赖 |
| ② 联网副链（AI 语音） | 唤醒「你好涂鸦」问天气、控灯、记生日，基于 TuyaOpen 的 ESP32/T5 模组 | ✅ |
| ③ 云端内容链（留言） | 手机留言 → AI 校准文案 + TTS 合成 → 设备 30 秒轮询拉取 → 下载 WAV → 校验 → 回标已送达 → 播放 | ✅ |

核心判断：**联网是增强，不是依赖**——玄关那两秒，不该赌一次网络握手。

## 目录结构

| 目录 | 内容 |
| --- | --- |
| `firmware/` | 微雪 T5-E1-Touch-AMOLED-1.75 的固件源码和板型配置 |
| `server/` | 本机 Python 服务及手机网页源码 |
| `sdcard/homehub/` | TF 卡配置示例及提示音 WAV |
| `assets/` | 字体及许可证 |
| `scripts/`、`tests/` | 构建、打包和测试脚本 |
| `docs/` | 接线、接口、人体感应验收与提示音说明 |

## 本机运行网页

在解压后的 `HomeHub` 目录中使用 Python 3.11 或更新版本。

```powershell
python server/app.py
```

浏览器打开 `http://127.0.0.1:8765`。首次启动会在本地生成令牌，登录与音频格式说明见 `server/README.md`。

## 构建固件

准备 [TuyaOpen SDK](https://github.com/tuya/TuyaOpen) 和 [T5AI 平台](https://github.com/tuya/TuyaOpen-T5AI)。本项目使用以下版本。

- TuyaOpen `fff7da5a7d60250fc75b4d4469090fa682506713`
- T5AI `69793c66bebf58ef4f886282626c2ea96a28f9a2`

```powershell
& .\scripts\build-firmware.ps1 -SdkRoot 'D:\Tuya\TuyaOpen'
```

把路径换成实际 SDK 路径。编译后的 QIO 镜像位于 `firmware/dist/firmware_1.0.0/`。如需生成烧录发行包，运行 `python scripts/package-release.py --sdk 'D:\Tuya\TuyaOpen'`。

TF 卡的目录和示例在 `sdcard/homehub/`。实际使用时在 TF 卡上从 `config.example.json` 复制出 `config.json`，再按部署环境填写。使用自有板卡授权及联网参数配置云功能。当前交付提供源码和示例配置，设备授权、实际联网配置及烧录镜像留在本机。

接线见 `docs/wiring-and-first-run.md`，当前触发设置以 `docs/pir-responsive-20261107.md` 为准。

## 常用测试

```powershell
python tests/core/run_tests.py
python -m unittest discover -s server/tests -p 'test_*.py'
node --test server/tests/ui-state.test.js
```

`MANIFEST.sha256` 记录包内文件校验值。

> 打包说明：本仓库源码来自当前项目工作区（2026-11-14），含人体感应恢复、持续感应重触发和空闲 30 秒表情轮换；最近一次实机回读版本为 `18a9c2c7`，记录见 `docs/pir-responsive-20261107.md`。设备授权、实际联网配置与烧录镜像不在仓库内，请按上文自行配置。
