"""Compile the actual cloud backend and HTTP startup against SDK fakes."""
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
spec = importlib.util.spec_from_file_location("core_runner", ROOT / "tests/core/run_tests.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
build = HERE / ".build"
build.mkdir(exist_ok=True)
# Compare the actual backend config with the installed official example.
# These public KV constants are never printed or copied from device storage.
example = ROOT.parent / "TuyaOpen/apps/tuya.ai/your_chat_bot/src/tuya_main.c"
example_source = example.read_text(encoding="utf-8")
expected = {}
for field in ("seed", "key"):
    matches = re.findall(r'\.' + field + r'\s*=\s*"([^"\n]*)"', example_source)
    if len(matches) != 1 or len(matches[0].encode("utf-8")) != 16:
        raise RuntimeError("Official example must have one 16-byte KV " + field)
    expected[field] = matches[0]
(build / "official_kv.h").write_text(
    "\n".join("#define HH_OFFICIAL_KV_" + field.upper() + " " + json.dumps(value)
              for field, value in expected.items()) + "\n", encoding="utf-8")
cjson = ROOT.parent / "TuyaOpen/src/libcjson/cJSON"
output = ""
failed = 0
for name, source, stubs in [
    ("emotion-tests", "test_emotion.c", HERE / "stubs"),
    ("cloud-tests", "test_chat_cloud.c", HERE / "stubs"),
    ("net-start-tests", "test_net_start.c", HERE / "stubs/net_start"),
]:
    binary = build / (name + ".exe")
    command = [runner.compiler_path(None), "-std=c99", "-Wall", "-Werror", "-I", str(stubs), "-I", str(ROOT / "firmware/core"),
               str(HERE / source), "-o", str(binary)]
    if name in ("emotion-tests", "cloud-tests"):
        command += [str(ROOT / "firmware/core/homehub_emotion.c")]
    if name == "cloud-tests":
        command += ["-I", str(cjson), str(cjson / "cJSON.c")]
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
    if result.returncode == 0:
        result = subprocess.run([str(binary)], cwd=ROOT, capture_output=True, text=True)
    chunk = result.stdout + result.stderr
    output += chunk
    print(chunk, end="")
    failed = failed or result.returncode
log = HERE / (sys.argv[1] if len(sys.argv) > 1 else "green-result.txt")
log.write_text(output, encoding="utf-8")
raise SystemExit(failed)
