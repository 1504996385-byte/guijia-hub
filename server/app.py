"""HomeHub local HTTP service. Python 3.11+, standard library only."""
from __future__ import annotations

import argparse
import base64
import binascii
from contextlib import contextmanager
from dataclasses import dataclass
from datetime import date, datetime, timedelta, timezone
import hashlib
import io
import json
import math
import os
from pathlib import Path
import re
import secrets
import shutil
import sqlite3
import struct
import subprocess
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, unquote, urlencode, urlsplit
from urllib.error import HTTPError
from urllib.request import Request, urlopen
import uuid
import wave

ROOT = Path(__file__).resolve().parent
ID_RE = re.compile(r"[A-Za-z0-9_-]{1,48}\Z")
SHANGHAI = timezone(timedelta(hours=8))
MAX_BODY = 9 * 1024 * 1024
MAX_AUDIO = 6 * 1024 * 1024
MAX_SECONDS = 30
STATUS_LABELS = {"sent": "已发送", "downloaded": "设备已收到", "played": "设备已播放"}


class APIError(Exception):
    def __init__(self, status: int, message: str, code: str = "invalid_request"):
        super().__init__(message)
        self.status, self.code = status, code


def utcnow() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")


def valid_id(value, name="ID") -> str:
    if not isinstance(value, str) or not ID_RE.fullmatch(value):
        raise APIError(400, f"{name}只能包含1至48个英文字母、数字、下划线或连字符")
    return value


def recipient_trigger(data):
    recipient, trigger = data.get("recipient"), data.get("trigger")
    if type(recipient) is not int or recipient not in (0, 1):
        raise APIError(400, "接收人必须为0（我）或1（TA）")
    if trigger not in ("arrive", "leave"):
        raise APIError(400, "请选择钥匙放回或取走时播放")
    return recipient, trigger


def short_text(value, default="", limit=120):
    if value is None:
        return default
    if not isinstance(value, str) or len(value) > limit:
        raise APIError(400, f"文字须不超过{limit}字")
    return value.strip()


@dataclass
class Config:
    data_dir: Path = ROOT / ".runtime"
    host: str = "127.0.0.1"
    port: int = 8765
    phone_token: str | None = None
    device_token: str | None = None
    ffmpeg_path: str | None = None
    weather_url: str = ""
    weather_token: str = ""
    weather_latitude: str = ""
    weather_longitude: str = ""
    weather_cache_seconds: float = 600
    ai_gateway_url: str = ""
    ai_gateway_token: str = ""
    ai_source: str = "gateway"
    ai_cache_seconds: float = 300
    network_timeout: float = 3

    @classmethod
    def from_env(cls):
        # Deliberately no OpenAI variables, credential stores or auto-discovery.
        tuya_url = os.environ.get("HOMEHUB_TUYA_AI_URL", "")
        return cls(data_dir=Path(os.environ.get("HOMEHUB_DATA_DIR", str(ROOT / ".runtime"))),
                   ffmpeg_path=os.environ.get("HOMEHUB_FFMPEG"),
                   weather_url=os.environ.get("HOMEHUB_WEATHER_URL", ""),
                   weather_token=os.environ.get("HOMEHUB_WEATHER_TOKEN", ""),
                   weather_latitude=os.environ.get("HOMEHUB_WEATHER_LATITUDE", ""),
                   weather_longitude=os.environ.get("HOMEHUB_WEATHER_LONGITUDE", ""),
                   ai_gateway_url=tuya_url or os.environ.get("HOMEHUB_AI_GATEWAY_URL", ""),
                   ai_gateway_token=os.environ.get("HOMEHUB_TUYA_AI_TOKEN", "") if tuya_url else os.environ.get("HOMEHUB_AI_GATEWAY_TOKEN", ""),
                   ai_source="tuya" if tuya_url else "gateway")


def pcm_normalize(raw: bytes) -> tuple[bytes, float]:
    """Decode PCM WAV only; normalize channel count/rate without optional packages."""
    try:
        with wave.open(io.BytesIO(raw), "rb") as src:
            channels, width, rate, frames = src.getnchannels(), src.getsampwidth(), src.getframerate(), src.getnframes()
            if src.getcomptype() != "NONE" or channels not in (1, 2) or width not in (1, 2, 3, 4) or not 8000 <= rate <= 96000:
                raise ValueError("unsupported PCM layout")
            duration = frames / rate
            if not 0 < duration <= MAX_SECONDS:
                raise APIError(400, "录音须大于0秒且不超过30秒", "audio_duration")
            samples = src.readframes(frames)
            if len(samples) != frames * channels * width:
                raise APIError(400, "WAV文件未完整上传，请重新录音", "audio_truncated")
    except APIError:
        raise
    except (wave.Error, EOFError, ValueError, struct.error) as exc:
        raise ValueError("not supported PCM WAV") from exc
    if (channels, width, rate) == (1, 2, 16000):
        mono = samples
    else:
        def sample_at(frame):
            offset = frame * width * channels
            total = 0
            for channel in range(channels):
                start = offset + channel * width
                part = samples[start:start + width]
                value = (part[0] - 128) << 8 if width == 1 else int.from_bytes(part, "little", signed=True) >> ((width - 2) * 8) if width >= 2 else 0
                total += value
            return total / channels
        count = max(1, round(frames * 16000 / rate))
        output = bytearray(count * 2)
        for index in range(count):
            position = min(frames - 1, index * rate / 16000)
            left = int(position)
            fraction = position - left
            value = round(sample_at(left) * (1 - fraction) + sample_at(min(frames - 1, left + 1)) * fraction)
            struct.pack_into("<h", output, index * 2, max(-32768, min(32767, value)))
        mono = bytes(output)
    result = io.BytesIO()
    with wave.open(result, "wb") as dst:
        dst.setnchannels(1)
        dst.setsampwidth(2)
        dst.setframerate(16000)
        dst.writeframes(mono)
    return result.getvalue(), len(mono) / 32000


def normalize_audio(raw: bytes, config: Config) -> tuple[bytes, float]:
    if not raw or len(raw) > MAX_AUDIO:
        raise APIError(413, "音频为空或超过6MiB，请选择30秒以内的短录音")
    try:
        return pcm_normalize(raw)
    except ValueError:
        executable = config.ffmpeg_path if config.ffmpeg_path is not None else shutil.which("ffmpeg")
        if not executable:
            raise APIError(415, "该格式不能直接播放。请使用网页按住录音（WAV），或安装ffmpeg后再上传此文件。", "audio_format")
        try:
            with tempfile.TemporaryDirectory(prefix="audio-", dir=config.data_dir) as work:
                source, target = Path(work) / "input.audio", Path(work) / "output.wav"
                source.write_bytes(raw)
                subprocess.run([executable, "-nostdin", "-v", "error", "-y", "-i", str(source), "-vn", "-t", "31",
                                "-ac", "1", "-ar", "16000", "-c:a", "pcm_s16le", str(target)],
                               stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=15, check=True)
                return pcm_normalize(target.read_bytes())
        except APIError:
            raise
        except (OSError, subprocess.SubprocessError, ValueError) as exc:
            raise APIError(415, "音频转码失败，请使用网页录音或有效的短WAV文件", "audio_conversion") from exc


class App:
    def __init__(self, config: Config):
        self.config = config
        config.data_dir = Path(config.data_dir)
        config.data_dir.mkdir(parents=True, exist_ok=True)
        self.db_path = config.data_dir / "homehub.sqlite3"
        self.lock = threading.RLock()
        self.network_lock = threading.RLock()
        self.weather_cache = None
        self.weather_cached_at = 0.0
        self.ai_cache = {}
        self.load_tokens()
        with self.db() as db:
            db.executescript("""
                CREATE TABLE IF NOT EXISTS messages(
                    id TEXT PRIMARY KEY, client_id TEXT UNIQUE, fingerprint TEXT NOT NULL,
                    recipient INTEGER NOT NULL, trigger TEXT NOT NULL, title TEXT NOT NULL,
                    audio BLOB NOT NULL, sha256 TEXT NOT NULL, bytes INTEGER NOT NULL, duration REAL NOT NULL,
                    created_at TEXT NOT NULL, status TEXT NOT NULL DEFAULT 'sent',
                    downloaded_at TEXT, played_at TEXT, schedule_id TEXT, last_polled_at INTEGER NOT NULL DEFAULT 0);
                CREATE TABLE IF NOT EXISTS receipts(event_id TEXT PRIMARY KEY, fingerprint TEXT NOT NULL, received_at TEXT NOT NULL);
                CREATE TABLE IF NOT EXISTS deleted_messages(id TEXT PRIMARY KEY, client_id TEXT UNIQUE, fingerprint TEXT NOT NULL, deleted_at TEXT NOT NULL);
                CREATE TABLE IF NOT EXISTS events(event_id TEXT PRIMARY KEY, fingerprint TEXT NOT NULL, recipient INTEGER NOT NULL,
                    event TEXT NOT NULL, device_time TEXT, received_at TEXT NOT NULL);
                CREATE TABLE IF NOT EXISTS voices(id TEXT PRIMARY KEY, client_id TEXT UNIQUE, fingerprint TEXT NOT NULL,
                    title TEXT NOT NULL, language TEXT NOT NULL, voice_source TEXT NOT NULL, audio BLOB NOT NULL,
                    sha256 TEXT NOT NULL, bytes INTEGER NOT NULL, duration REAL NOT NULL, created_at TEXT NOT NULL);
                CREATE TABLE IF NOT EXISTS schedules(id TEXT PRIMARY KEY, client_id TEXT UNIQUE, fingerprint TEXT NOT NULL,
                    recipient INTEGER NOT NULL, trigger TEXT NOT NULL, title TEXT NOT NULL, kind TEXT NOT NULL, date TEXT NOT NULL,
                    audio BLOB NOT NULL, sha256 TEXT NOT NULL, bytes INTEGER NOT NULL, duration REAL NOT NULL, created_at TEXT NOT NULL);
                CREATE TABLE IF NOT EXISTS deliveries(schedule_id TEXT NOT NULL, occurrence TEXT NOT NULL, message_id TEXT NOT NULL,
                    PRIMARY KEY(schedule_id,occurrence));
                CREATE TABLE IF NOT EXISTS assets(id TEXT PRIMARY KEY, audio BLOB NOT NULL, created_at TEXT NOT NULL);
            """)
            if "last_polled_at" not in {row["name"] for row in db.execute("PRAGMA table_info(messages)")}:
                db.execute("ALTER TABLE messages ADD COLUMN last_polled_at INTEGER NOT NULL DEFAULT 0")

    def load_tokens(self):
        path = self.config.data_dir / "tokens.json"
        if self.config.phone_token is not None and self.config.device_token is not None:
            if not self.config.phone_token or not self.config.device_token or self.config.phone_token == self.config.device_token:
                raise ValueError("phone/device tokens must be nonempty and different")
            return
        if path.exists():
            stored = json.loads(path.read_text(encoding="utf-8"))
        else:
            stored = {"phone_token": secrets.token_urlsafe(36), "device_token": secrets.token_urlsafe(36)}
            try:
                with path.open("x", encoding="utf-8") as output:
                    json.dump(stored, output, indent=2)
                os.chmod(path, 0o600)
            except FileExistsError:
                stored = json.loads(path.read_text(encoding="utf-8"))
        self.config.phone_token = self.config.phone_token or stored["phone_token"]
        self.config.device_token = self.config.device_token or stored["device_token"]
        if not self.config.phone_token or not self.config.device_token or self.config.phone_token == self.config.device_token:
            raise ValueError("invalid local token file")

    @contextmanager
    def db(self):
        with self.lock:
            connection = sqlite3.connect(self.db_path, timeout=10)
            connection.row_factory = sqlite3.Row
            try:
                connection.execute("PRAGMA journal_mode=WAL")
                with connection:
                    yield connection
            finally:
                connection.close()

    def parse_audio(self, data):
        encoded = data.get("audio_base64")
        if not isinstance(encoded, str):
            raise APIError(400, "请先录音或选择音频文件")
        try:
            raw = base64.b64decode(encoded, validate=True)
        except (ValueError, binascii.Error) as exc:
            raise APIError(400, "音频上传内容损坏，请重试") from exc
        return normalize_audio(raw, self.config)

    @staticmethod
    def fingerprint(value):
        return hashlib.sha256(json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode()).hexdigest()

    @staticmethod
    def public_row(row, kind="messages"):
        result = {key: row[key] for key in row.keys() if key not in ("audio", "fingerprint", "client_id", "last_polled_at")}
        if kind == "messages":
            result["status_label"] = STATUS_LABELS[result["status"]]
            result["audio_url"] = "/api/messages/" + result["id"] + "/audio"
        elif kind == "voices":
            result["audio_url"] = "/api/library/" + result["id"] + "/audio"
        return result

    def add_message(self, data):
        recipient, trigger = recipient_trigger(data)
        title = short_text(data.get("title"), "一条原声留言") or "一条原声留言"
        client_id = valid_id(data.get("client_id", uuid.uuid4().hex), "client_id")
        audio, duration = self.parse_audio(data)
        digest = hashlib.sha256(audio).hexdigest()
        fingerprint = self.fingerprint([recipient, trigger, title, digest])
        with self.db() as db:
            if db.execute("SELECT 1 FROM deleted_messages WHERE client_id=?", (client_id,)).fetchone():
                raise APIError(410, "这次提交已被删除，请重新录音或选择文件后创建新留言", "message_deleted")
            existing = db.execute("SELECT * FROM messages WHERE client_id=?", (client_id,)).fetchone()
            if existing:
                if existing["fingerprint"] != fingerprint:
                    raise APIError(409, "这个提交编号已用于另一条留言，请刷新后重试", "id_conflict")
                return self.public_row(existing), False
            message_id = uuid.uuid4().hex
            db.execute("INSERT INTO messages(id,client_id,fingerprint,recipient,trigger,title,audio,sha256,bytes,duration,created_at) VALUES(?,?,?,?,?,?,?,?,?,?,?)",
                       (message_id, client_id, fingerprint, recipient, trigger, title, audio, digest, len(audio), duration, utcnow()))
            return self.public_row(db.execute("SELECT * FROM messages WHERE id=?", (message_id,)).fetchone()), True

    def add_voice(self, data):
        client_id = valid_id(data.get("client_id", uuid.uuid4().hex), "client_id")
        title = short_text(data.get("title"), "一段声音") or "一段声音"
        language = short_text(data.get("language"), "未标注", 40)
        source = data.get("voice_source", "human")
        if source not in ("human", "test", "tts"):
            raise APIError(400, "请选择真人录音、测试语音或合成语音")
        audio, duration = self.parse_audio(data)
        digest = hashlib.sha256(audio).hexdigest()
        fingerprint = self.fingerprint([title, language, source, digest])
        with self.db() as db:
            existing = db.execute("SELECT * FROM voices WHERE client_id=?", (client_id,)).fetchone()
            if existing:
                if existing["fingerprint"] != fingerprint:
                    raise APIError(409, "语音库提交编号重复且内容不同", "id_conflict")
                return self.public_row(existing, "voices"), False
            voice_id = uuid.uuid4().hex
            db.execute("INSERT INTO voices VALUES(?,?,?,?,?,?,?,?,?,?,?)", (voice_id, client_id, fingerprint, title, language, source,
                       audio, digest, len(audio), duration, utcnow()))
            return self.public_row(db.execute("SELECT * FROM voices WHERE id=?", (voice_id,)).fetchone(), "voices"), True

    def add_schedule(self, data):
        recipient, trigger = recipient_trigger(data)
        kind, when = data.get("kind"), data.get("date")
        try:
            if kind == "once":
                if date.fromisoformat(when).isoformat() != when:
                    raise ValueError()
            elif kind == "birthday":
                if not re.fullmatch(r"\d{2}-\d{2}", when) or date.fromisoformat("2000-" + when).strftime("%m-%d") != when:
                    raise ValueError()
            else:
                raise ValueError()
        except (TypeError, ValueError):
            raise APIError(400, "指定日期格式为YYYY-MM-DD，公历生日为MM-DD；请填写真实日期")
        title = short_text(data.get("title"), "日期留言") or "日期留言"
        client_id = valid_id(data.get("client_id", uuid.uuid4().hex), "client_id")
        with self.db() as db:
            if data.get("library_id"):
                voice = db.execute("SELECT * FROM voices WHERE id=?", (valid_id(data["library_id"]),)).fetchone()
                if not voice:
                    raise APIError(404, "语音库中的这段声音已不存在")
                audio, duration, digest = voice["audio"], voice["duration"], voice["sha256"]
            else:
                audio, duration = self.parse_audio(data)
                digest = hashlib.sha256(audio).hexdigest()
            fingerprint = self.fingerprint([recipient, trigger, kind, when, title, digest])
            existing = db.execute("SELECT * FROM schedules WHERE client_id=?", (client_id,)).fetchone()
            if existing:
                if existing["fingerprint"] != fingerprint:
                    raise APIError(409, "日期留言编号重复且内容不同", "id_conflict")
                return self.public_row(existing, "schedules"), False
            schedule_id = uuid.uuid4().hex
            db.execute("INSERT INTO schedules VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)", (schedule_id, client_id, fingerprint, recipient,
                       trigger, title, kind, when, audio, digest, len(audio), duration, utcnow()))
            return self.public_row(db.execute("SELECT * FROM schedules WHERE id=?", (schedule_id,)).fetchone(), "schedules"), True

    def dispatch_due(self, today=None):
        today = today or datetime.now(SHANGHAI).date()
        dispatched = 0
        with self.db() as db:
            for row in db.execute("SELECT * FROM schedules").fetchall():
                if row["kind"] == "once":
                    occurrence = row["date"]
                    if occurrence > today.isoformat():
                        continue
                else:
                    if row["date"] != today.strftime("%m-%d"):
                        continue
                    occurrence = today.isoformat()
                message_id = uuid.uuid5(uuid.NAMESPACE_URL, f"homehub:{row['id']}:{occurrence}").hex
                cursor = db.execute("INSERT OR IGNORE INTO deliveries VALUES(?,?,?)", (row["id"], occurrence, message_id))
                if not cursor.rowcount:
                    continue
                db.execute("INSERT INTO messages(id,client_id,fingerprint,recipient,trigger,title,audio,sha256,bytes,duration,created_at,schedule_id) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",
                           (message_id, message_id, row["fingerprint"], row["recipient"], row["trigger"], row["title"], row["audio"],
                            row["sha256"], row["bytes"], row["duration"], utcnow(), row["id"]))
                dispatched += 1
        return dispatched

    def receipt(self, data):
        message_id = valid_id(data.get("id"))
        event_id = valid_id(data.get("event_id"), "event_id")
        status = data.get("status")
        if status not in ("downloaded", "played"):
            raise APIError(400, "回执只能为downloaded或played")
        fingerprint = self.fingerprint([message_id, status])
        with self.db() as db:
            old = db.execute("SELECT fingerprint FROM receipts WHERE event_id=?", (event_id,)).fetchone()
            if old:
                if old["fingerprint"] != fingerprint:
                    raise APIError(409, "回执event_id重复且内容不同", "id_conflict")
                return
            row = db.execute("SELECT status FROM messages WHERE id=?", (message_id,)).fetchone()
            if not row:
                if db.execute("SELECT 1 FROM deleted_messages WHERE id=?", (message_id,)).fetchone():
                    db.execute("INSERT INTO receipts VALUES(?,?,?)", (event_id, fingerprint, utcnow()))
                    return
                raise APIError(404, "留言不存在")
            now = utcnow()
            if status == "played":
                db.execute("UPDATE messages SET status='played',downloaded_at=COALESCE(downloaded_at,?),played_at=COALESCE(played_at,?) WHERE id=?", (now, now, message_id))
            elif row["status"] != "played":
                db.execute("UPDATE messages SET status='downloaded',downloaded_at=COALESCE(downloaded_at,?) WHERE id=?", (now, message_id))
            db.execute("INSERT INTO receipts VALUES(?,?,?)", (event_id, fingerprint, now))

    def event(self, data):
        event_id = valid_id(data.get("event_id"), "event_id")
        recipient = data.get("recipient")
        event = data.get("event")
        recipient_trigger({"recipient": recipient, "trigger": event})
        device_time = data.get("device_time")
        if device_time is not None:
            try:
                parsed = datetime.fromisoformat(device_time.replace("Z", "+00:00"))
                if parsed.tzinfo is None:
                    raise ValueError()
            except (AttributeError, TypeError, ValueError):
                raise APIError(400, "设备时间须为带时区的ISO8601时间，也可留空")
        fingerprint = self.fingerprint([recipient, event, device_time])
        with self.db() as db:
            old = db.execute("SELECT fingerprint FROM events WHERE event_id=?", (event_id,)).fetchone()
            if old:
                if old["fingerprint"] != fingerprint:
                    raise APIError(409, "钥匙事件编号重复且内容不同", "id_conflict")
                return
            db.execute("INSERT INTO events VALUES(?,?,?,?,?,?)", (event_id, fingerprint, recipient, event, device_time, utcnow()))

    def calendar(self):
        today = datetime.now(SHANGHAI).date()
        path = ROOT / "data" / f"solar-terms-{today.year}.json"
        terms = json.loads(path.read_text(encoding="utf-8")) if path.exists() else None
        entries = terms.get("terms", []) if terms else []
        previous = [term for term in entries if term["date"] <= today.isoformat()]
        upcoming = next((term for term in entries if term["date"] > today.isoformat()), None)
        return {"date": today.isoformat(), "timezone": "Asia/Shanghai", "solar_term": previous[-1]["name"] if previous else None,
                "next_solar_term": upcoming, "solar_terms": entries, "solar_source": terms.get("source") if terms else None,
                "solar_status": "date_table" if terms else "unconfigured", "birthday_calendar": "gregorian"}

    def request_gateway(self, url, token="", data=None):
        if urlsplit(url).scheme not in ("https", "http") or not urlsplit(url).hostname:
            raise ValueError("gateway must use HTTP(S)")
        # Plain HTTP is allowed for explicit loopback integration tests/local Tuya gateways only.
        if urlsplit(url).scheme == "http" and urlsplit(url).hostname not in ("127.0.0.1", "localhost", "::1"):
            raise ValueError("remote gateway requires HTTPS")
        headers = {"Accept": "application/json", "User-Agent": "HomeHub/1.0"}
        if token:
            headers["Authorization"] = "Bearer " + token
        payload = None
        if data is not None:
            headers["Content-Type"] = "application/json"
            payload = json.dumps(data, ensure_ascii=False).encode()
        try:
            with urlopen(Request(url, data=payload, headers=headers), timeout=self.config.network_timeout) as result:
                raw = result.read(MAX_BODY + 1)
                if len(raw) > MAX_BODY:
                    raise ValueError("gateway response too large")
                body = json.loads(raw)
                if not isinstance(body, dict):
                    raise ValueError("gateway response must be an object")
                return body
        except HTTPError as error:
            error.close()
            raise

    def weather(self):
        cfg = self.config
        url, source = cfg.weather_url, "gateway"
        if not url and cfg.weather_latitude and cfg.weather_longitude:
            try:
                lat, lon = float(cfg.weather_latitude), float(cfg.weather_longitude)
                if not (-90 <= lat <= 90 and -180 <= lon <= 180):
                    return None
            except ValueError:
                return None
            url = "https://api.open-meteo.com/v1/forecast?" + urlencode({"latitude": lat, "longitude": lon,
                    "current": "temperature_2m,weather_code", "timezone": "Asia/Shanghai"})
            source = "open-meteo"
        if not url:
            return None
        with self.network_lock:
            now = time.monotonic()
            if self.weather_cache and now - self.weather_cached_at < cfg.weather_cache_seconds:
                return dict(self.weather_cache)
            try:
                result = self.request_gateway(url, cfg.weather_token)
                if source == "open-meteo":
                    current = result["current"]
                    code = current.get("weather_code")
                    condition = "晴" if code == 0 else "多云" if code in (1, 2, 3) else "雾" if code in (45, 48) else "雨" if code in (51, 53, 55, 56, 57, 61, 63, 65, 66, 67, 80, 81, 82) else "雪" if code in (71, 73, 75, 77, 85, 86) else "雷雨" if code in (95, 96, 99) else "天气代码" + str(code)
                    result = {"temperature": current["temperature_2m"], "condition": condition, "observed_at": current.get("time")}
                temperature = result.get("temperature")
                if type(temperature) not in (int, float) or not math.isfinite(temperature):
                    raise ValueError("temperature is required")
                output = {"temperature": temperature, "condition": short_text(result.get("condition"), "", 80),
                          "city": short_text(result.get("city"), "", 80), "source": source, "fetched_at": utcnow(),
                          "observed_at": result.get("observed_at"), "stale": False}
                self.weather_cache, self.weather_cached_at = output, now
                return dict(output)
            except Exception:
                return dict(self.weather_cache, stale=True) if self.weather_cache else None

    def greeting(self, recipient, event):
        weather = self.weather()
        empty = {"text": "", "audio_url": None, "source": "unconfigured", "weather": weather}
        if not self.config.ai_gateway_url:
            return empty
        key = (recipient, event, datetime.now(SHANGHAI).date().isoformat())
        with self.network_lock:
            cached = self.ai_cache.get(key)
            if cached and time.monotonic() - cached[0] < self.config.ai_cache_seconds:
                return dict(cached[1], weather=weather)
            try:
                body = self.request_gateway(self.config.ai_gateway_url, self.config.ai_gateway_token,
                                            {"recipient": recipient, "event": event, "weather": weather, "calendar": self.calendar(),
                                             "language": "zh-CN", "max_characters": 80})
                text = short_text(body.get("text"), "", 500)
                if not text:
                    raise ValueError("no greeting text")
                output = {"text": text, "audio_url": None, "source": self.config.ai_source, "weather": weather}
                if body.get("audio_base64"):
                    audio, _ = self.parse_audio(body)
                    asset_id = "g_" + hashlib.sha256(audio).hexdigest()[:40]
                    with self.db() as db:
                        db.execute("INSERT OR IGNORE INTO assets VALUES(?,?,?)", (asset_id, audio, utcnow()))
                    output["audio_url"] = "/api/device/audio/" + asset_id + ".wav"
                    output["sha256"] = hashlib.sha256(audio).hexdigest()
                    output["bytes"] = len(audio)
                # External gateway audio URLs are intentionally not passed to board HTTP clients.
                self.ai_cache[key] = (time.monotonic(), output)
                return output
            except Exception:
                if cached:
                    return dict(cached[1], weather=weather, stale=True)
                return dict(empty, error="AI服务暂不可用，请使用本地欢迎语")

    def list_rows(self, table, limit=200):
        with self.db() as db:
            return [self.public_row(row, table) for row in db.execute(f"SELECT * FROM {table} ORDER BY created_at DESC LIMIT ?", (limit,))]


class Handler(BaseHTTPRequestHandler):
    server_version = "HomeHub/1.0"

    @property
    def app(self):
        return self.server.app

    def log_message(self, *_):
        # No request URLs, tokens, family text or credentials enter terminal logs.
        pass

    def auth(self, role):
        expected = self.app.config.device_token if role == "device" else self.app.config.phone_token
        received = self.headers.get("Authorization", "")
        if not secrets.compare_digest(received.encode("utf-8"), ("Bearer " + expected).encode("utf-8")):
            raise APIError(401, "访问凭证无效，请填写手机访问凭证" if role == "phone" else "device authentication required", "unauthorized")

    def send_bytes(self, status, body, content_type):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("Content-Security-Policy", "default-src 'self'; script-src 'self'; style-src 'self'; media-src 'self' blob:; connect-src 'self'; object-src 'none'; frame-ancestors 'none'")
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def send_json(self, body, status=200):
        self.send_bytes(status, json.dumps(body, ensure_ascii=False, allow_nan=False).encode(), "application/json; charset=utf-8")

    def read_json(self):
        if self.headers.get("Transfer-Encoding"):
            raise APIError(400, "不支持分块请求，请一次上传短音频")
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            raise APIError(400, "Content-Length无效")
        if not 0 < length <= MAX_BODY:
            raise APIError(413, "请求为空或超过9MiB")
        if self.headers.get_content_type() != "application/json":
            raise APIError(415, "请使用JSON上传")
        try:
            body = json.loads(self.rfile.read(length))
        except (UnicodeDecodeError, json.JSONDecodeError):
            raise APIError(400, "JSON无效")
        if not isinstance(body, dict):
            raise APIError(400, "请求须为JSON对象")
        return body

    def do_GET(self):
        self.handle_api("GET")

    def do_POST(self):
        self.handle_api("POST")

    def do_DELETE(self):
        self.handle_api("DELETE")

    def handle_api(self, method):
        try:
            self.route(method)
        except APIError as exc:
            self.send_json({"error": str(exc), "code": exc.code}, exc.status)
        except Exception:
            self.send_json({"error": "服务端处理失败，请重试；原有留言仍保存在本机", "code": "internal_error"}, 500)

    def route(self, method):
        parsed = urlsplit(self.path)
        path, query = unquote(parsed.path), parse_qs(parsed.query)
        static = {"/": ("index.html", "text/html; charset=utf-8"), "/app.js": ("app.js", "text/javascript; charset=utf-8"),
                  "/styles.css": ("styles.css", "text/css; charset=utf-8")}
        if method == "GET" and path in static:
            filename, mime = static[path]
            self.send_bytes(200, (ROOT / "static" / filename).read_bytes(), mime)
            return
        if path == "/api/health" and method == "GET":
            self.send_json({"ok": True, "version": "1.0", "mode": "local", "server_time": utcnow()})
            return
        if not path.startswith("/api/"):
            raise APIError(404, "页面不存在", "not_found")
        self.auth("device" if path.startswith("/api/device/") else "phone")
        if method == "GET" and path == "/api/device/poll":
            try:
                limit = int(query.get("limit", ["8"])[0])
                if not 1 <= limit <= 64:
                    raise ValueError()
            except ValueError:
                raise APIError(400, "limit须为1至64")
            self.app.dispatch_due()
            with self.app.db() as db:
                messages = [{"id": r["id"], "recipient": r["recipient"], "trigger": r["trigger"],
                             "audio_url": "/api/device/audio/" + r["id"] + ".wav", "sha256": r["sha256"], "bytes": r["bytes"]}
                            for r in db.execute("SELECT id,recipient,trigger,sha256,bytes FROM messages WHERE status!='played' ORDER BY CASE status WHEN 'sent' THEN 0 ELSE 1 END,last_polled_at,created_at,id LIMIT ?", (limit,))]
                db.executemany("UPDATE messages SET last_polled_at=? WHERE id=?", [(time.time_ns(), message["id"]) for message in messages])
            self.send_json({"messages": messages, "weather": self.app.weather(), "greeting": None, "server_time": utcnow()})
            return
        audio_match = re.fullmatch(r"/api/device/audio/([A-Za-z0-9_-]{1,48})\.wav", path)
        phone_audio = re.fullmatch(r"/api/(messages|library)/([A-Za-z0-9_-]{1,48})/audio", path)
        if method == "GET" and (audio_match or phone_audio):
            with self.app.db() as db:
                if audio_match:
                    key = audio_match.group(1)
                    row = db.execute("SELECT audio FROM messages WHERE id=? UNION ALL SELECT audio FROM assets WHERE id=? LIMIT 1", (key, key)).fetchone()
                else:
                    table = "messages" if phone_audio.group(1) == "messages" else "voices"
                    row = db.execute(f"SELECT audio FROM {table} WHERE id=?", (phone_audio.group(2),)).fetchone()
            if not row:
                raise APIError(404, "音频不存在", "not_found")
            self.send_bytes(200, row["audio"], "audio/wav")
            return
        if method == "POST" and path in ("/api/device/receipt", "/api/device/events"):
            body = self.read_json()
            self.app.receipt(body) if path.endswith("receipt") else self.app.event(body)
            self.send_json({"ok": True})
            return
        if method == "GET" and path in ("/api/device/greeting", "/api/greeting"):
            try:
                recipient = int(query.get("recipient", ["0"])[0])
            except ValueError:
                raise APIError(400, "接收人无效")
            event = query.get("event", ["arrive"])[0]
            recipient_trigger({"recipient": recipient, "trigger": event})
            self.send_json(self.app.greeting(recipient, event))
            return
        if method == "POST" and path == "/api/session":
            self.read_json()
            self.send_json({"ok": True})
            return
        if method == "GET" and path == "/api/context":
            cfg = self.app.config
            self.send_json({"calendar": self.app.calendar(), "weather": self.app.weather(),
                            "ai": {"source": cfg.ai_source if cfg.ai_gateway_url else "unconfigured", "configured": bool(cfg.ai_gateway_url)},
                            "audio": {"sample_rate": 16000, "channels": 1, "bits": 16, "max_seconds": 30,
                                      "ffmpeg_available": bool(cfg.ffmpeg_path if cfg.ffmpeg_path is not None else shutil.which("ffmpeg"))}})
            return
        lists = {"/api/messages": ("messages", "messages"), "/api/library": ("voices", "voices"), "/api/schedules": ("schedules", "schedules")}
        if path in lists:
            table, key = lists[path]
            if method == "GET":
                self.app.dispatch_due()
                self.send_json({key: self.app.list_rows(table)})
                return
            if method == "POST":
                body = self.read_json()
                add = {"messages": self.app.add_message, "voices": self.app.add_voice, "schedules": self.app.add_schedule}[table]
                result, created = add(body)
                self.send_json(result, 201 if created else 200)
                return
        if method == "POST" and path == "/api/schedules/dispatch":
            self.read_json()
            self.send_json({"ok": True, "enqueued": self.app.dispatch_due()})
            return
        deletion = re.fullmatch(r"/api/(messages|library|schedules)/([A-Za-z0-9_-]{1,48})", path)
        if method == "DELETE" and deletion:
            table = {"messages": "messages", "library": "voices", "schedules": "schedules"}[deletion.group(1)]
            with self.app.db() as db:
                if table == "messages":
                    row = db.execute("SELECT id,client_id,fingerprint FROM messages WHERE id=?", (deletion.group(2),)).fetchone()
                    if row:
                        db.execute("INSERT OR IGNORE INTO deleted_messages VALUES(?,?,?,?)", (row["id"], row["client_id"], row["fingerprint"], utcnow()))
                cursor = db.execute(f"DELETE FROM {table} WHERE id=?", (deletion.group(2),))
            if not cursor.rowcount:
                raise APIError(404, "记录已不存在", "not_found")
            self.send_json({"ok": True})
            return
        if method == "GET" and path == "/api/events":
            with self.app.db() as db:
                rows = [dict(row) for row in db.execute("SELECT event_id,recipient,event,device_time,received_at FROM events ORDER BY received_at DESC LIMIT 200")]
            self.send_json({"events": rows})
            return
        raise APIError(404, "接口不存在", "not_found")


class HomeHubServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, config):
        self.app = App(config)
        self.scheduler_stop = threading.Event()
        super().__init__((config.host, config.port), Handler)

    def serve_forever(self, poll_interval=0.1):
        def scheduler():
            while not self.scheduler_stop.wait(2):
                try:
                    self.app.dispatch_due()
                except Exception:
                    # Request-side dispatch also retries; a transient lock never drops a schedule.
                    pass
        worker = threading.Thread(target=scheduler, name="homehub-schedules", daemon=True)
        worker.start()
        try:
            super().serve_forever(poll_interval)
        finally:
            self.scheduler_stop.set()
            worker.join(3)

    def server_close(self):
        self.scheduler_stop.set()
        super().server_close()


def create_server(config=None):
    return HomeHubServer(config or Config.from_env())


def main():
    parser = argparse.ArgumentParser(description="归家中枢本机服务（默认仅本机可访问）")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--data-dir", type=Path)
    args = parser.parse_args()
    config = Config.from_env()
    config.host, config.port = args.host, args.port
    if args.data_dir:
        config.data_dir = args.data_dir
    server = create_server(config)
    print(f"HomeHub local URL: http://{config.host}:{server.server_port}")
    print(f"Local credentials file: {config.data_dir / 'tokens.json'} (values are not printed)")
    print("AI/weather remain unconfigured until you explicitly configure them. Ctrl+C stops the server.")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
