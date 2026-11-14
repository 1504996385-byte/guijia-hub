"""Exercise the actual startup entry and SDK cJSON with separate counted heaps."""
import importlib.util
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
spec = importlib.util.spec_from_file_location("core_runner", ROOT / "tests/core/run_tests.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
build = ROOT / "tests/platform/.build/json_memory"
build.mkdir(parents=True, exist_ok=True)
source = (ROOT / "firmware/src/homehub_app.c").read_text(encoding="utf-8")
entry = source[source.index("\nvoid tuya_app_main(void)"):]
included = build / "actual_startup.c.inc"
included.write_text(entry, encoding="utf-8")
cjson = ROOT.parent / "TuyaOpen/src/libcjson/cJSON"
output = ""
failed = 0
for enabled in (1, 0):
    binary = build / ("json-psram.exe" if enabled else "json-native.exe")
    command = [runner.compiler_path(None), "-std=c99", "-Wall", "-Werror",
               "-D__cdecl=", "-D__stdcall=", "-DCJSON_HIDE_SYMBOLS",
               "-DENABLE_EXT_RAM=" + str(enabled),
               '-DHH_STARTUP_SOURCE="' + included.as_posix() + '"',
               "-I", str(cjson), str(HERE / "test_json_memory.c"),
               str(cjson / "cJSON.c"), "-o", str(binary)]
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
    if result.returncode == 0:
        result = subprocess.run([str(binary)], cwd=ROOT, capture_output=True, text=True)
    chunk = result.stdout + result.stderr
    output += chunk
    print(chunk, end="")
    failed = failed or result.returncode
(HERE / (sys.argv[1] if len(sys.argv) > 1 else "green-result.txt")).write_text(output, encoding="utf-8")
raise SystemExit(failed)
