"""Compile actual portable face state and exercise lifecycle and pose geometry."""
import importlib.util
import pathlib
import subprocess
import sys
import tempfile
HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
spec = importlib.util.spec_from_file_location("core_runner", HERE.parent / "core/run_tests.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
compiler = runner.compiler_path(None)
binary = HERE / "face-tests.exe"
command = [compiler, "-std=c99", "-Wall", "-Werror", "-I", str(ROOT / "firmware/core"),
           str(HERE / "test_face.c"), str(ROOT / "firmware/core/homehub_face.c"), "-o", str(binary)]
result = subprocess.run(command, capture_output=True, text=True)
if result.returncode:
    print(result.stdout + result.stderr)
    raise SystemExit(result.returncode)
result = subprocess.run([str(binary)], capture_output=True, text=True)
output = result.stdout + result.stderr
failed = result.returncode
# Extract, rather than duplicate, the production UI scheduling function. This
# avoids running the Windows LVGL renderer while covering its sleep policy.
ui = (ROOT / "firmware/src/homehub_ui.c").read_text(encoding="utf-8-sig")
start = ui.index("void hh_ui_tick(")
opening = ui.index("{", start)
depth = 1
end = opening + 1
while depth:
    if ui[end] == "{":
        depth += 1
    elif ui[end] == "}":
        depth -= 1
    end += 1
with tempfile.TemporaryDirectory(prefix="homehub-ui-idle-") as temporary:
    actual = pathlib.Path(temporary) / "ui_tick_actual.h"
    actual.write_text(ui[start:end] + "\n", encoding="utf-8")
    idle_binary = HERE / "ui-idle-tests.exe"
    command = [compiler, "-std=c99", "-Wall", "-Werror", "-I", temporary,
               "-I", str(ROOT / "firmware/core"), str(HERE / "test_ui_idle.c"),
               str(ROOT / "firmware/core/homehub_face.c"), "-o", str(idle_binary)]
    idle = subprocess.run(command, capture_output=True, text=True)
    if idle.returncode == 0:
        idle = subprocess.run([str(idle_binary)], capture_output=True, text=True)
    output += idle.stdout + idle.stderr
    failed = failed or idle.returncode
print(output, end="")
log = HERE / ("red-result.txt" if "--red" in sys.argv else "green-result.txt")
log.write_text(output, encoding="utf-8")
raise SystemExit(failed)
