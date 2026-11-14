"""Syntax-check only our translation unit against the installed ARM SDK headers.

Does not compile SDK objects, link firmware, access serial, or touch SDK files.
The root integration build remains the release gate.
"""
import json
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[2]
commands = json.loads((ROOT / "firmware/.build/compile_commands.json").read_text())
entry = next(x for x in commands if x["file"].replace("\\", "/").endswith("/homehub_app.c"))
parts = shlex.split(entry["command"], posix=False)
args = [parts[0].strip('"')]
skip = False
for part in parts[1:]:
    if skip:
        skip = False
        continue
    if part in ("-o", "-c"):
        skip = True
        continue
    args.append(part.strip('"'))
args += ["-I", "D:/Tuya/TuyaOpen/src/audio_player/include", "-fsyntax-only",
         "-I", "D:/Tuya/TuyaOpen/src/tuya_ai_service/svc_ai_agent/src"]
for source in ("homehub_chat_cloud.c", "homehub_net.c"):
    result = subprocess.run(args + [str(ROOT / "firmware/src" / source)], cwd=entry["directory"], capture_output=True, text=True, encoding="utf-8", errors="replace")
    print(result.stdout + result.stderr, end="")
    if result.returncode:
        raise SystemExit(result.returncode)
    print("PASS production " + source + " ARM API syntax check (no SDK build/link)")
