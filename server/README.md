# 归家中枢本机服务与手机网页

Python 3.11+ 标准库实现，无需 pip、Node 构建、云账号或额外数据库。默认仅监听 `127.0.0.1:8765`，没有公网部署、没有修改防火墙。

## 启动与凭证

在项目根目录运行：

```powershell
python server/app.py
# 或
& .\server\run.ps1
```

电脑浏览器打开 `http://127.0.0.1:8765`。第一次运行生成 `server/.runtime/tokens.json`，其中 `phone_token` 用于网页，`device_token` 仅用于设备请求。启动日志只显示文件位置，不打印值。用本机文本编辑器打开该文件，把 `phone_token` 粘贴到网页登录框。网页只在当前浏览器会话的 `sessionStorage` 保存凭证；“断开本次会话”会清除它。

不要把 token 文件、SQLite 数据库、原始家庭录音或云端凭证提交到版本库。`.runtime/` 已由本目录 `.gitignore` 忽略。Windows 下此文件沿用所在目录的访问权限；请将项目放在自己的用户目录或只允许本人访问的位置。

本机 `localhost` / `127.0.0.1` 可以请求麦克风。手机上的 `localhost` 指手机自己，不是这台电脑。当前版本未开通手机远程访问；之后若需手机访问，需单独配置同网服务地址及 HTTPS。普通 LAN HTTP 页面通常无法录音，届时仍可选择已有音频上传。不要把未部署的本机页面当作公网地址。

关闭运行窗口或按 Ctrl+C 即可停止。数据库与音频保存在同一 SQLite 数据库中，重启保留留言、回执、语音库、日期留言及幂等记录。默认数据目录为 `server/.runtime/`，可用 `--data-dir <目录>` 或 `HOMEHUB_DATA_DIR` 改位置。备份时先停止服务，再备份整个数据目录（包括可能存在的 WAL 文件）。

## 网页可用功能

- **留一句话**：选我/TA，选钥匙放回/取走时播放；按住录音、松手生成预览；也可上传音频。最长30秒。
- **真实状态**：已发送、设备已收到、设备已播放。网页试听不会标记设备播放，回执也不代表人听见。
- **声音小屋**：上传、试听、删除；区分真人录音、测试语音、合成语音，标注语言/方言。也可以保存刚才录好的声音。没有自动声称已有真人方言素材。
- **日期留言**：指定公历日期一次投递，或每年公历生日；从语音库复制音频，因此删掉语音库原件不影响已创建日期留言。取消日期规则不删除已经投进信箱的消息。
- **钥匙记录**：区分取走/放回，显示服务器接收时间；设备报告的时间单独标注，不保证已校时，不推断人行踪。
- **节气日历**：附2026年24节气日期表，依据[香港天文台2026对照表](https://www.hko.gov.hk/tc/gts/time/calendar/text/files/T2026c.txt)。按北京时间的整日粒度显示，不声称精确交节时刻。其他年份需要增加对应年度数据文件，缺表显示未配置，不跨年重复使用固定月日。
- **AI与天气**：可查看配置状态并试调用；当前没有服务配置时，天气为 `null`，AI为 `source: unconfigured`，不会生成假天气或假AI结果。

设备未接时，网页可发送、管理、试听，但不会自行出现“设备已收到/已播放”。设备侧仍应每30秒轮询，在后台下载并校验，按键/钥匙触发的播放时机由设备仲裁。

## 音频

网页录音使用 Web Audio，导出16kHz、16bit、单声道 PCM WAV。服务端也会检查并规范化 WAV；内置支持8/16/24/32bit、单/双声道 PCM、8–96kHz采样率，采用线性重采样供原型使用。

其他浏览器录音/上传可能是 WebM/Opus、AAC/MP4 等。若 PATH 中有 `ffmpeg`，或配置 `HOMEHUB_FFMPEG` 为已有可执行文件路径，服务端可转成相同 WAV 格式。没有 ffmpeg 时，非受支持 PCM WAV 会返回 `415 audio_format`，提示使用网页录音或有效 WAV，绝不改扩展名冒充转码。最长30秒、原文件最大6MiB、JSON请求最大9MiB。

导入明确提供的本机样例（不上传云端）：

```powershell
python server/import_voice.py .\sdcard\homehub\voices\sample.wav --title "普通话测试" --language "普通话" --source test
```

请用真实存在的文件替换示例路径。`--source human` 仅用于真人录音；`tts` 用于合成素材。

## HTTP契约

设备接口严格沿用 `../docs/api-contract.md`。所有 `/api/device/*` 使用设备 Bearer token；其他业务 API 使用手机 Bearer token，角色互不通用。静态页面和 `/api/health` 无需凭证，但不返回凭证或家庭记录。无 Cookie 登录，无跨域开放。

| 手机接口 | 用途 |
|---|---|
| `POST /api/session`，JSON `{}` | 检查手机凭证 |
| `GET /api/messages` | 最近200条留言与真实回执 |
| `POST /api/messages` | 原声留言；字段见下方 |
| `GET /api/messages/ID/audio` | 手机鉴权试听WAV |
| `DELETE /api/messages/ID` | 删除服务器留言；已下载的设备副本不保证撤回 |
| `GET /api/library` | 最近200条声音，返回 `voices` |
| `POST /api/library` | 音频与 `title, language, voice_source`（human/test/tts） |
| `GET /api/library/ID/audio` | 手机鉴权试听 |
| `DELETE /api/library/ID` | 删除库中声音 |
| `GET /api/schedules` | 日期规则，返回 `schedules` |
| `POST /api/schedules` | `recipient, trigger, title, kind, date, library_id, client_id` |
| `DELETE /api/schedules/ID` | 取消之后的投递；已入队留言保留 |
| `POST /api/schedules/dispatch`，JSON `{}` | 手动检查到期规则，通常无需调用 |
| `GET /api/events` | 最近200条钥匙事件，服务器接收时间排序 |
| `GET /api/context` | 日期、节气表、天气、AI配置状态、音频能力 |
| `GET /api/greeting?recipient=1&event=arrive` | 手机测试真实AI网关 |

留言请求示例（不含实际凭证/声音）：

```json
{
  "client_id": "本次提交唯一ASCII编号",
  "recipient": 1,
  "trigger": "arrive",
  "title": "回家记得吃饭",
  "filename": "voice.wav",
  "audio_base64": "不带dataURL前缀的Base64音频"
}
```

所有ID（包括 `client_id` / `event_id`）仅接受 `A-Za-z0-9_-`，最长48字符。相同提交编号及相同规范化内容重试返回原ID；同编号不同内容返回409。接口只按受限ID访问SQLite音频，不把上传文件名当磁盘路径。

日期 `kind=once` 时 `date=YYYY-MM-DD`；日期到达后投递一次，离线错过后会在服务重启时补入信箱。`kind=birthday` 时 `date=MM-DD`，仅在当天投递一次/年；2月29日只在闰年投递，不自动改成28日，不补投过去生日。使用北京时间 UTC+8，不含农历生日。

到期任务在后台每2秒检查，且列表/设备轮询也会检查；`schedule_id + occurrence_date` 在数据库中唯一，即使重复调用或重启也只入队一次。留言可以在日期当天入队，真正播放仍等待指定钥匙取放事件。

设备 `poll` 会优先发送尚未下载的消息，同状态内按上次轮询轮转；已下载但未播放的消息仍会重复出现，以便重启后重拉，超过8条也不会永久被最老8条挡住。`downloaded`/`played` 及事件回执按 `event_id` 幂等；`played` 之后的迟到下载回执不会回退状态。只有设备确认播放完成后，设备才应上报 `played`。

手机删除消息会删除服务器音频与正文，保留最小消息编号/提交编号/指纹墓碑。已下载设备的迟到回执仍返回成功，避免设备永久重试；不会重新出现在信箱。删除后旧上传编号重试返回410，不能因网络重试复活已删除留言。

## 涂鸦AI/受控网关接入

当前没有云服务配置。代码不会读取 OpenAI key、Codex凭证或任何其他本机模型密钥。接入代码通过**显式配置的服务端HTTP网关**调用；涂鸦原始IoT签名、Agent激活等由网关处理，本服务没有假定任意涂鸦原始端点都兼容该契约，也未做真实涂鸦云端实调。

后续准备好用户控制的涂鸦AI网关后，在启动服务的 PowerShell 会话设置（以下均为占位示意，不是现有配置）：

```powershell
$env:HOMEHUB_TUYA_AI_URL = 'https://your-controlled-gateway.example/homehub/greeting'
# HOMEHUB_TUYA_AI_TOKEN 仅通过受控本机环境配置，不要写入网页/设备/公开仓库。
python server/app.py
```

通用受控网关使用 `HOMEHUB_AI_GATEWAY_URL` / `HOMEHUB_AI_GATEWAY_TOKEN`，但涂鸦配置优先；来源分别标为 `tuya` / `gateway`。没有配置或首次请求失败时 `text=""`、`audio_url=null`、`source=unconfigured`。默认网络超时3秒、成功缓存5分钟；缓存过期且服务故障可返回上次真实结果并标记 `stale=true`。

服务端向网关发送 `POST application/json`：

```json
{"recipient":1,"event":"arrive","weather":null,"calendar":{"date":"2026-11-05","timezone":"Asia/Shanghai"},"language":"zh-CN","max_characters":80}
```

网关返回 `{"text":"真实生成的短欢迎语","audio_base64":"可选音频"}`。音频若提供，会校验/转成WAV、存入本机，设备拿到同源 `/api/device/audio/g_ID.wav`，并附 `sha256` 和 `bytes` 供下载校验。只有文字时 `audio_url=null`，不能假称已经有TTS语音。外部音频URL不会直接交给设备，当前网关需内联音频。LLM、ASR、TTS的权限与额度需分别验证。

目标方言需要明确并试听确认；当前没有真人方言素材，不承诺嘉兴话、声纹复刻或任意方言TTS。

## 天气接入

二选一：

1. 显式配置 `HOMEHUB_WEATHER_LATITUDE` 与 `HOMEHUB_WEATHER_LONGITUDE`，调用真实 Open-Meteo forecast/current 服务；不自动定位。
2. 配置用户控制的 `HOMEHUB_WEATHER_URL`（可配 `HOMEHUB_WEATHER_TOKEN`），GET返回 `{"temperature":18.5,"condition":"小雨","city":"城市名"}`。

默认缓存10分钟。获取失败时有旧缓存返回 `stale=true`、原 `fetched_at`；没有缓存则 `null`。未配置也返回 `null`。远端网关要求HTTPS；明确的loopback HTTP用于本机集成测试。公网真实服务尚未配置或实调。

## 验证

```powershell
python -m unittest discover -s server/tests -p "test_*.py" -v
node --check server/static/app.js
node --test server/tests/ui-state.test.js
```

测试通过真实127.0.0.1 HTTP端口，不只直接调用处理函数。覆盖非法鉴权、角色边界、上传/下载/摘要、幂等重试、状态不回退、持久化重启、路径穿越、格式错误、重采样、30秒限制、钥匙记录、语音库删除、日期幂等与重启、poll公平性、年度节气数据、真实HTTP测试网关及缓存/故障。测试网关只用合成测试信号，不代替真实云服务验收。

未连接硬件；实体按键、麦克风/扬声器、干簧管与磁场、TF存储、设备实际回执需要接板验收。浏览器麦克风权限与手机浏览器实录需分别实测；自动化HTTP测试不能证明真人录音效果。
