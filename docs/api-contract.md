# HomeHub HTTP v1

本地服务默认127.0.0.1:8765。设备与手机分别使用不同Bearer token。仅受限设备数据流；手机密钥不得写固件。文本/日期UTC ISO8601；接收人0=我、1=TA；trigger=arrive或leave。显示语义为钥匙取放、设备已播放，不推断本人到家或听见。

## Device

GET /api/device/poll?limit=8

Authorization: Bearer DEVICE_TOKEN

返回JSON：{"messages":[{"id":"唯一ASCII消息ID","recipient":0,"trigger":"arrive","audio_url":"/api/device/audio/ID.wav","sha256":"hex","bytes":1234}],"weather":null,"greeting":null,"server_time":"ISO8601"}。

ID最多48字符且仅A-Za-z0-9_-。服务端输出WAV为16kHz/16bit/mono PCM；URL是同源相对地址。设备需每30秒后台轮询，保存音频并校验长度/摘要后报告downloaded。poll应重复包含未played消息以支持丢失本地文件/设备重启；按ID去重，播放完成后才回played。

GET /api/device/audio/ID.wav 同一设备token，返回WAV。

POST /api/device/receipt JSON {"id":"ID","status":"downloaded或played","event_id":"唯一ID"}。幂等；played不可退回downloaded；返回{"ok":true}。

POST /api/device/events JSON {"event_id":"唯一ID","recipient":0,"event":"arrive或leave","device_time":"可选ISO8601"}；服务端另记录接收时间，幂等，不相信未校时的板端时间。

GET /api/device/greeting?recipient=0&event=arrive 返回{"text":"...","audio_url":null,"source":"unconfigured或tuya或gateway","weather":null}。没有真实AI时source必须为unconfigured，不返回假AI结果。是否下载与何时播放由板端音频仲裁，绝不阻塞按键。

## Phone

服务端代理自行设计/api/messages、/api/library、/api/schedules、/api/events等手机API并文档化。实现原声短留言上传和可选ffmpeg转码；没有ffmpeg时WAV仍可用，其他格式返回可操作错误，不改后缀冒充转码成功。MediaRecorder需HTTPS或localhost并处理权限。管理包含接收人、触发时机、音频、已发送/已收到/已播放的真实状态。

## 启用配置

默认生成本地开发token到忽略文件，输出只说明文件位置。云服务无需公网部署即可本机验收；手机远程访问需要同网或正式HTTPS部署，不能宣称未部署的公网可用。AI优先Tuya/用户指定服务，不擅自读取或使用OpenAI Key。支持受控服务端AI网关URL和token配置，服务未开通时明确未配置。天气使用有超时/缓存的真实服务适配，未配置返回null。
