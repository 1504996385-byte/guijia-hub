"""Exercise actual hh_load_config with host TKL and the installed SDK cJSON."""
import argparse
import importlib.util
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--log", type=Path, default=HERE / "config-result.txt")
args = parser.parse_args()
sdk_json = ROOT.parent / "TuyaOpen/src/libcjson/cJSON"
if not (sdk_json / "cJSON.c").is_file():
    raise SystemExit("Installed TuyaOpen cJSON source not found: " + str(sdk_json))
spec = importlib.util.spec_from_file_location("core_runner", ROOT / "tests/core/run_tests.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
build = HERE / ".build"
build.mkdir(exist_ok=True)
binary = build / "config-tests.exe"
command = [runner.compiler_path(None), "-std=c99", "-Wall", "-Werror",
           # Win64 uses one calling convention; TinyCC needs these SDK-header
           # decorations removed for function-pointer declarations.
           "-D__cdecl=", "-D__stdcall=", "-DCJSON_HIDE_SYMBOLS",
           "-I", str(sdk_json), "-I", str(HERE / "stubs"),
           "-I", str(HERE), "-I", str(ROOT / "firmware/core"),
           str(HERE / "test_config.c"), str(sdk_json / "cJSON.c"), "-o", str(binary)]
result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
if result.returncode:
    print(result.stdout + result.stderr)
    raise SystemExit(result.returncode)
with tempfile.TemporaryDirectory(prefix="homehub-config-") as runtime:
    result = subprocess.run([str(binary), runtime], capture_output=True, text=True)
    output = result.stdout + result.stderr
    print(output, end="")
    args.log.write_text(output, encoding="utf-8")
    raise SystemExit(result.returncode)
