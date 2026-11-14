"""Test ticket gating and doorway dispatch using verbatim app slices + storage/core."""
import hashlib
import importlib.util
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
spec = importlib.util.spec_from_file_location("core_runner", ROOT / "tests/core/run_tests.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
build = HERE / ".build"
build.mkdir(exist_ok=True)
source = (ROOT / "firmware/src/homehub_app.c").read_text(encoding="utf-8")
start = source.index("typedef struct { bool used; bool downloaded; uint32_t ticket; HH_Action action; } DeferredReceipt;")
end = source.index("\nvoid hh_post(", start)
globals_start = source.index("static HH_Action ui_state;")
greeting_start = source.index("static void greeting(")
handle_end = source.index("\nstatic void app_thread(", greeting_start)
pending_start = source.index("        if(pending_greeting&&", handle_end)
pending_end = source.index("        retry_receipts(now);", pending_start)
functions = source[globals_start:start] + source[start:end] + source[greeting_start:handle_end] + "\n"
functions += "static void app_pending_greetings_under_test(const bool buttons[2], bool chat_key, uint32_t now) {\n"
functions += source[pending_start:pending_end] + "}\n"
included = build / "receipt-functions-under-test.c"
included.write_text(functions, encoding="utf-8")
binary = build / "ticket-tests.exe"
command = [runner.compiler_path(None), '-DHH_RECEIPT_SOURCE="' + included.as_posix() + '"',
           "-std=c99", "-Wall", "-Werror", "-I", str(HERE / "stubs"),
           str(HERE / "test_tickets.c"), str(ROOT / "firmware/core/homehub_core.c"), "-o", str(binary)]
result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
if result.returncode:
    print(result.stdout + result.stderr)
    raise SystemExit(result.returncode)
with tempfile.TemporaryDirectory(prefix="homehub-tickets-") as runtime:
    result = subprocess.run([str(binary), runtime], capture_output=True, text=True)
    output = "App receipt/dispatch source-slice SHA256: " + hashlib.sha256(functions.encode()).hexdigest() + "\n"
    output += result.stdout + result.stderr
    print(output, end="")
    (HERE / "tickets-result.txt").write_text(output, encoding="utf-8")
    raise SystemExit(result.returncode)
