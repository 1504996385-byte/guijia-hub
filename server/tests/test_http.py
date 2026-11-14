"""Real loopback HTTP tests; no board, cloud account or production token needed."""
import base64
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
import wave
from datetime import datetime, timedelta, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

MODULE = Path(__file__).resolve().parents[1] / "app.py"


def wav_bytes(rate=16000, channels=1, seconds=0.08):
    out = io.BytesIO()
    with wave.open(out, "wb") as audio:
        audio.setnchannels(channels)
        audio.setsampwidth(2)
        audio.setframerate(rate)
        audio.writeframes(struct.pack("<h", 1200) * int(rate * seconds) * channels)
    return out.getvalue()


class HTTPTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # An explicit assertion makes the initial RED a missing-feature failure.
        if MODULE.exists():
            spec = importlib.util.spec_from_file_location("homehub_app", MODULE)
            cls.mod = importlib.util.module_from_spec(spec)
            sys.modules[spec.name] = cls.mod
            spec.loader.exec_module(cls.mod)
        else:
            cls.mod = None

    def setUp(self):
        self.assertIsNotNone(self.mod, "HomeHub HTTP service has not been implemented")
        self.tmp = tempfile.TemporaryDirectory()
        self.cfg = self.mod.Config(data_dir=Path(self.tmp.name), phone_token="test-phone-token",
                                   device_token="test-device-token", port=0, ffmpeg_path="")
        self.start()

    def start(self):
        self.server = self.mod.create_server(self.cfg)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.base = "http://127.0.0.1:%d" % self.server.server_port

    def stop(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(2)

    def tearDown(self):
        if hasattr(self, "server"):
            self.stop()
        if hasattr(self, "tmp"):
            self.tmp.cleanup()

    def request(self, path, data=None, method=None, role="phone", token=None, raw=False):
        headers = {}
        if role is not None:
            headers["Authorization"] = "Bearer " + (token or ("test-phone-token" if role == "phone" else "test-device-token"))
        if data is not None:
            headers["Content-Type"] = "application/json"
            data = json.dumps(data).encode()
        req = urllib.request.Request(self.base + path, data=data, headers=headers,
                                     method=method or ("POST" if data is not None else "GET"))
        try:
            response = urllib.request.urlopen(req, timeout=5)
        except urllib.error.HTTPError as err:
            response = err
        try:
            body, status = response.read(), response.status
            return status, body if raw else json.loads(body)
        finally:
            response.close()

    def upload(self, path="/api/messages", **overrides):
        body = {"client_id": "upload_1", "recipient": 1, "trigger": "arrive", "title": "回家记得吃饭",
                "filename": "hello.wav", "audio_base64": base64.b64encode(wav_bytes()).decode()}
        body.update(overrides)
        status, result = self.request(path, body)
        self.assertIn(status, (200, 201), result)
        return result

    def test_public_shell_and_role_boundaries(self):
        status, body = self.request("/", role=None, raw=True)
        self.assertEqual(status, 200)
        self.assertIn("归家中枢".encode(), body)
        self.assertNotIn(b"test-phone-token", body)
        self.assertEqual(self.request("/api/messages", role=None)[0], 401)
        self.assertEqual(self.request("/api/messages", role="device")[0], 401)
        self.assertEqual(self.request("/api/device/poll", token="wrong")[0], 401)
        self.assertEqual(self.request("/api/device/poll", role="phone")[0], 401)
        self.assertEqual(self.request("/api/messages", token="invalid-é")[0], 401)

    def test_upload_poll_download_and_checksum(self):
        message = self.upload()
        self.assertEqual(message["status"], "sent")
        status, poll = self.request("/api/device/poll?limit=8", role="device")
        self.assertEqual(status, 200)
        self.assertIsNone(poll["weather"])
        self.assertIsNone(poll["greeting"])
        self.assertEqual(len(poll["messages"]), 1)
        queued = poll["messages"][0]
        self.assertEqual(queued["id"], message["id"])
        self.assertEqual(queued["recipient"], 1)
        status, audio = self.request(queued["audio_url"], role="device", raw=True)
        self.assertEqual(status, 200)
        self.assertEqual(hashlib.sha256(audio).hexdigest(), queued["sha256"])
        self.assertEqual(len(audio), queued["bytes"])
        with wave.open(io.BytesIO(audio)) as result:
            self.assertEqual((result.getframerate(), result.getsampwidth(), result.getnchannels()), (16000, 2, 1))

    def test_upload_retry_is_idempotent_but_conflict_is_rejected(self):
        first = self.upload()
        second = self.upload()
        self.assertEqual(first["id"], second["id"])
        self.assertEqual(len(self.request("/api/messages")[1]["messages"]), 1)
        payload = {"client_id": "upload_1", "recipient": 0, "trigger": "leave", "filename": "x.wav",
                   "audio_base64": base64.b64encode(wav_bytes()).decode()}
        self.assertEqual(self.request("/api/messages", payload)[0], 409)

    def test_poll_prioritizes_new_audio_and_rotates_downloaded_backlog(self):
        older = [self.upload(client_id="older_" + str(i)) for i in range(10)]
        for i, message in enumerate(older):
            self.request("/api/device/receipt", {"id": message["id"], "status": "downloaded", "event_id": "dl_" + str(i)}, role="device")
        newest = self.upload(client_id="newest")
        first = self.request("/api/device/poll?limit=8", role="device")[1]["messages"]
        self.assertEqual(first[0]["id"], newest["id"], "downloaded backlog must not starve a new message")
        self.request("/api/device/receipt", {"id": newest["id"], "status": "downloaded", "event_id": "dl_new"}, role="device")
        second = self.request("/api/device/poll?limit=8", role="device")[1]["messages"]
        self.assertEqual(len({m["id"] for m in first + second}), 11, "later downloaded messages must remain recoverable")

    def test_receipts_monotonic_idempotent_and_not_human_read(self):
        msg = self.upload()
        payload = {"id": msg["id"], "status": "downloaded", "event_id": "receipt_1"}
        self.assertEqual(self.request("/api/device/receipt", payload, role="device")[0], 200)
        self.assertEqual(self.request("/api/device/receipt", payload, role="device")[0], 200)
        self.assertEqual(len(self.request("/api/device/poll", role="device")[1]["messages"]), 1)
        payload.update(status="played", event_id="receipt_2")
        self.assertEqual(self.request("/api/device/receipt", payload, role="device")[0], 200)
        payload.update(status="downloaded", event_id="receipt_3")
        self.assertEqual(self.request("/api/device/receipt", payload, role="device")[0], 200)
        message = self.request("/api/messages")[1]["messages"][0]
        self.assertEqual(message["status"], "played")
        self.assertEqual(message["status_label"], "设备已播放")
        self.assertNotIn("听见", message["status_label"])
        self.assertEqual(self.request("/api/device/poll", role="device")[1]["messages"], [])
        payload.update(status="played", event_id="receipt_1")
        self.assertEqual(self.request("/api/device/receipt", payload, role="device")[0], 409)

    def test_restart_persists_message_and_receipt(self):
        msg = self.upload()
        self.request("/api/device/receipt", {"id": msg["id"], "status": "downloaded", "event_id": "r1"}, role="device")
        self.stop()
        self.start()
        self.assertEqual(self.request("/api/messages")[1]["messages"][0]["status"], "downloaded")
        self.assertEqual(self.request("/api/device/poll", role="device")[1]["messages"][0]["id"], msg["id"])

    def test_deleted_message_accepts_late_device_receipts_without_resurrection(self):
        message = self.upload()
        self.request("/api/device/receipt", {"id": message["id"], "status": "downloaded", "event_id": "before_delete"}, role="device")
        self.assertEqual(self.request("/api/messages/" + message["id"], method="DELETE")[0], 200)
        late = {"id": message["id"], "status": "played", "event_id": "after_delete"}
        self.assertEqual(self.request("/api/device/receipt", late, role="device")[0], 200, "late real device receipts must not retry forever")
        self.assertEqual(self.request("/api/device/receipt", late, role="device")[0], 200)
        self.assertEqual(self.request("/api/messages")[1]["messages"], [])
        self.assertEqual(self.request("/api/device/poll", role="device")[1]["messages"], [])
        self.assertEqual(self.request("/api/device/audio/" + message["id"] + ".wav", role="device", raw=True)[0], 404)
        retry = {"client_id": "upload_1", "recipient": 1, "trigger": "arrive", "title": "回家记得吃饭",
                 "audio_base64": base64.b64encode(wav_bytes()).decode()}
        self.assertEqual(self.request("/api/messages", retry)[0], 410)
        self.assertEqual(self.request("/api/device/receipt", {"id": "never_existed", "status": "played", "event_id": "wrong_receipt"}, role="device")[0], 404)

    def test_pcm_wav_normalizes_rate_and_channels_without_ffmpeg(self):
        msg = self.upload(audio_base64=base64.b64encode(wav_bytes(48000, 2)).decode())
        status, audio = self.request(msg["audio_url"], raw=True)
        self.assertEqual(status, 200)
        with wave.open(io.BytesIO(audio)) as result:
            self.assertEqual((result.getframerate(), result.getsampwidth(), result.getnchannels()), (16000, 2, 1))
            self.assertAlmostEqual(result.getnframes() / 16000, 0.08, places=2)

    def test_unknown_format_and_overlong_recording_are_rejected(self):
        payload = {"recipient": 0, "trigger": "arrive", "filename": "pretend.wav",
                   "audio_base64": base64.b64encode(b"not a wave").decode()}
        status, result = self.request("/api/messages", payload)
        self.assertEqual(status, 415)
        self.assertIn("WAV", result["error"])
        payload["audio_base64"] = base64.b64encode(wav_bytes(seconds=30.1)).decode()
        self.assertEqual(self.request("/api/messages", payload)[0], 400)

    def test_invalid_recipient_trigger_id_and_path_traversal(self):
        good = {"recipient": 2, "trigger": "arrive", "audio_base64": base64.b64encode(wav_bytes()).decode()}
        self.assertEqual(self.request("/api/messages", good)[0], 400)
        good.update(recipient=0, trigger="automatic")
        self.assertEqual(self.request("/api/messages", good)[0], 400)
        good.update(trigger="arrive", client_id="../../secret")
        self.assertEqual(self.request("/api/messages", good)[0], 400)
        for path in ("/api/device/audio/../../tokens.json", "/api/device/audio/%2e%2e%2fsecret.wav", "/.runtime/tokens.json", "/../app.py"):
            self.assertIn(self.request(path, role="device")[0], (400, 404))

    def test_key_events_dedupe_and_keep_server_timestamp(self):
        payload = {"event_id": "key_1", "recipient": 0, "event": "leave", "device_time": "1970-01-01T00:00:00Z"}
        self.assertEqual(self.request("/api/device/events", payload, role="device")[0], 200)
        self.assertEqual(self.request("/api/device/events", payload, role="device")[0], 200)
        events = self.request("/api/events")[1]["events"]
        self.assertEqual(len(events), 1)
        self.assertEqual(events[0]["device_time"], payload["device_time"])
        self.assertNotEqual(events[0]["received_at"][:4], "1970")
        payload["event"] = "arrive"
        self.assertEqual(self.request("/api/device/events", payload, role="device")[0], 409)

    def test_voice_library_upload_preview_delete(self):
        voice = self.upload("/api/library", title="普通话测试", language="普通话", voice_source="test")
        self.assertEqual(len(self.request("/api/library")[1]["voices"]), 1)
        self.assertEqual(self.request(voice["audio_url"], raw=True)[0], 200)
        self.assertEqual(self.request("/api/library/" + voice["id"], method="DELETE")[0], 200)
        self.assertEqual(self.request("/api/library")[1]["voices"], [])

    def test_due_schedule_dispatch_is_idempotent_across_restart(self):
        voice = self.upload("/api/library")
        today = (datetime.now(timezone.utc) + timedelta(hours=8)).date().isoformat()
        payload = {"client_id": "schedule_1", "recipient": 0, "trigger": "arrive", "kind": "once", "date": today,
                   "title": "今天回家拿快递", "library_id": voice["id"]}
        self.assertEqual(self.request("/api/schedules", payload)[0], 201)
        for _ in range(3):
            self.request("/api/schedules/dispatch", {})
        messages = self.request("/api/device/poll", role="device")[1]["messages"]
        self.assertEqual(len(messages), 1)
        first_id = messages[0]["id"]
        self.stop()
        self.start()
        self.request("/api/schedules/dispatch", {})
        self.assertEqual([m["id"] for m in self.request("/api/device/poll", role="device")[1]["messages"]], [first_id])

    def test_future_schedule_and_invalid_date_do_not_enqueue(self):
        voice = self.upload("/api/library")
        tomorrow = (datetime.now(timezone.utc) + timedelta(hours=8, days=1)).date().isoformat()
        payload = {"recipient": 1, "trigger": "leave", "kind": "once", "date": tomorrow, "library_id": voice["id"]}
        self.assertEqual(self.request("/api/schedules", payload)[0], 201)
        self.assertEqual(self.request("/api/device/poll", role="device")[1]["messages"], [])
        payload["date"] = "2026-02-30"
        self.assertEqual(self.request("/api/schedules", payload)[0], 400)

    def test_birthday_today_enqueues_once_and_calendar_is_explicit(self):
        voice = self.upload("/api/library")
        today = (datetime.now(timezone.utc) + timedelta(hours=8)).date()
        payload = {"recipient": 0, "trigger": "arrive", "kind": "birthday", "date": today.strftime("%m-%d"),
                   "library_id": voice["id"]}
        self.assertEqual(self.request("/api/schedules", payload)[0], 201)
        self.request("/api/schedules/dispatch", {})
        self.assertEqual(len(self.request("/api/device/poll", role="device")[1]["messages"]), 1)
        context = self.request("/api/context")[1]
        self.assertEqual(context["calendar"]["timezone"], "Asia/Shanghai")
        self.assertEqual(context["calendar"]["date"], today.isoformat())
        self.assertIsNone(context["weather"])
        self.assertEqual(context["ai"]["source"], "unconfigured")

    def test_unconfigured_ai_is_honest(self):
        status, result = self.request("/api/device/greeting?recipient=0&event=arrive", role="device")
        self.assertEqual(status, 200)
        self.assertEqual(result["source"], "unconfigured")
        self.assertEqual(result["text"], "")
        self.assertIsNone(result["audio_url"])

    def test_tokens_generated_once_and_separate(self):
        fresh = Path(self.tmp.name) / "fresh"
        cfg = self.mod.Config(data_dir=fresh, port=0)
        app = self.mod.App(cfg)
        stored = json.loads((fresh / "tokens.json").read_text())
        self.assertNotEqual(stored["phone_token"], stored["device_token"])
        self.assertGreaterEqual(len(stored["phone_token"]), 40)
        other = self.mod.App(self.mod.Config(data_dir=fresh, port=0))
        self.assertEqual(app.config.phone_token, other.config.phone_token)

    def test_annual_solar_table_is_loaded_without_reusing_fixed_dates(self):
        table = json.loads((MODULE.parent / "data" / "solar-terms-2026.json").read_text(encoding="utf-8")) if (MODULE.parent / "data" / "solar-terms-2026.json").exists() else {}
        self.assertEqual(len(table.get("terms", [])), 24, "2026 annual solar-term data is missing")
        self.assertEqual(next(t["date"] for t in table["terms"] if t["name"] == "立冬"), "2026-11-07")
        self.assertIn("hko.gov.hk", table["source"])

    def test_schedule_is_independent_of_deleted_voice_and_can_be_canceled(self):
        voice = self.upload("/api/library")
        tomorrow = (datetime.now(timezone.utc) + timedelta(hours=8, days=1)).date().isoformat()
        payload = {"client_id": "future_s1", "recipient": 0, "trigger": "leave", "kind": "once", "date": tomorrow,
                   "library_id": voice["id"]}
        status, schedule = self.request("/api/schedules", payload)
        self.assertEqual(status, 201)
        self.assertEqual(self.request("/api/schedules", payload)[1]["id"], schedule["id"])
        self.assertEqual(self.request("/api/library/" + voice["id"], method="DELETE")[0], 200)
        self.assertEqual(self.request("/api/schedules/" + schedule["id"], method="DELETE")[0], 200)
        self.assertEqual(self.request("/api/schedules")[1]["schedules"], [])


class GatewayHandler(BaseHTTPRequestHandler):
    hits = {"weather": 0, "ai": 0}
    fail = False
    delay = 0

    def log_message(self, *_):
        pass

    def do_GET(self):
        self.__class__.hits["weather"] += 1
        body = {"temperature": 18.5, "condition": "小雨", "city": "测试城市"}
        self.send_json(body)

    def do_POST(self):
        if self.__class__.delay:
            import time
            time.sleep(self.__class__.delay)
        self.__class__.hits["ai"] += 1
        self.__class__.last = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        self.send_json({"text": "欢迎归家，记得收伞。", "audio_base64": base64.b64encode(wav_bytes()).decode()})

    def send_json(self, body):
        raw = json.dumps(body).encode()
        self.send_response(503 if self.__class__.fail else 200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        try:
            self.wfile.write(raw)
        except (BrokenPipeError, ConnectionResetError):
            pass


class GatewayTests(HTTPTests):
    # Only gateway tests below; inherited tests remain useful with this config.
    def setUp(self):
        super().setUp()
        GatewayHandler.hits = {"weather": 0, "ai": 0}
        GatewayHandler.fail = False
        GatewayHandler.delay = 0
        self.gateway = ThreadingHTTPServer(("127.0.0.1", 0), GatewayHandler)
        self.gateway_thread = threading.Thread(target=self.gateway.serve_forever, daemon=True)
        self.gateway_thread.start()

    def tearDown(self):
        if hasattr(self, "gateway"):
            self.gateway.shutdown()
            self.gateway.server_close()
            self.gateway_thread.join(2)
        super().tearDown()

    def test_configured_real_http_gateways_and_cache(self):
        self.cfg.weather_url = "http://127.0.0.1:%d/weather" % self.gateway.server_port
        self.cfg.ai_gateway_url = "http://127.0.0.1:%d/ai" % self.gateway.server_port
        self.cfg.ai_source = "tuya"
        first = self.request("/api/device/greeting?recipient=1&event=arrive", role="device")[1]
        second = self.request("/api/device/greeting?recipient=1&event=arrive", role="device")[1]
        self.assertEqual(first["text"], "欢迎归家，记得收伞。")
        self.assertEqual(first["source"], "tuya")
        self.assertEqual(first["weather"]["temperature"], 18.5)
        poll_weather = self.request("/api/device/poll", role="device")[1]["weather"]
        self.assertIsInstance(poll_weather, dict, "device polling does not yet include configured weather")
        self.assertEqual(poll_weather["temperature"], 18.5)
        audio_status, greeting_audio = self.request(first["audio_url"], role="device", raw=True)
        self.assertEqual(audio_status, 200)
        self.assertEqual(first.get("sha256"), hashlib.sha256(greeting_audio).hexdigest())
        self.assertEqual(first.get("bytes"), len(greeting_audio))
        self.assertEqual(second["text"], first["text"])
        self.assertEqual(GatewayHandler.hits, {"weather": 1, "ai": 1})
        self.assertEqual(GatewayHandler.last["recipient"], 1)

    def test_gateway_failure_returns_stale_weather_and_no_fake_ai(self):
        self.cfg.weather_url = "http://127.0.0.1:%d/weather" % self.gateway.server_port
        self.cfg.weather_cache_seconds = 0
        first = self.request("/api/context")[1]["weather"]
        self.assertFalse(first["stale"])
        GatewayHandler.fail = True
        self.cfg.ai_gateway_url = "http://127.0.0.1:%d/ai" % self.gateway.server_port
        result = self.request("/api/device/greeting?recipient=0&event=leave", role="device")[1]
        self.assertTrue(result["weather"]["stale"])
        self.assertEqual(result["source"], "unconfigured")
        self.assertEqual(result["text"], "")

    def test_ai_timeout_does_not_return_an_invented_greeting(self):
        import time
        self.cfg.ai_gateway_url = "http://127.0.0.1:%d/ai" % self.gateway.server_port
        self.cfg.network_timeout = 0.05
        GatewayHandler.delay = 0.35
        start = time.monotonic()
        result = self.request("/api/device/greeting?recipient=0&event=arrive", role="device")[1]
        self.assertLess(time.monotonic() - start, 0.3)
        self.assertEqual(result["source"], "unconfigured")
        self.assertEqual(result["text"], "")


def load_tests(loader, _tests, _pattern):
    suite = loader.loadTestsFromTestCase(HTTPTests)
    for name in GatewayTests.__dict__:
        if name.startswith("test_"):
            suite.addTest(GatewayTests(name))
    return suite


if __name__ == "__main__":
    unittest.main(verbosity=2)
