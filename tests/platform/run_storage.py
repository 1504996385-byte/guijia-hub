"""Run actual board storage C with host TKL fault injection; no SDK emulation claim."""
import argparse
import importlib.util
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--prove-generation-regression", action="store_true",
                    help="Compile a temporary copy with the original failed-generation bug and require failure")
args = parser.parse_args()
spec = importlib.util.spec_from_file_location("core_runner", ROOT / "tests/core/run_tests.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
compiler = runner.compiler_path(None)
build = HERE / ".build"
build.mkdir(exist_ok=True)
binary = build / ("storage-regression-tests.exe" if args.prove_generation_regression else "storage-tests.exe")
command = [compiler, "-std=c99", "-Wall", "-Werror", "-I", str(HERE / "stubs"), "-I", str(ROOT / "firmware/src"),
           str(HERE / "test_storage.c"), "-o", str(binary)]
if args.prove_generation_regression:
    actual = (ROOT / "firmware/src/homehub_storage.c").read_text(encoding="utf-8")
    before = "e->sequence=snapshot_sequence+1;"
    commit = "if(event.ok)snapshot_sequence=e->sequence;"
    if before not in actual or commit not in actual:
        raise SystemExit("Expected source pattern changed; inspect before applying the targeted mutation")
    mutated = actual.replace(before, "e->sequence=++snapshot_sequence;", 1).replace(commit, "", 1)
    mutation = build / "storage-generation-regression.c"
    mutation.write_text(mutated, encoding="utf-8")
    command.insert(1, '-DHH_STORAGE_SOURCE="' + mutation.as_posix() + '"')
result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
if result.returncode:
    print(result.stdout + result.stderr)
    raise SystemExit(result.returncode)
with tempfile.TemporaryDirectory(prefix="homehub-storage-") as runtime:
    result = subprocess.run([str(binary), runtime], capture_output=True, text=True)
    output = result.stdout + result.stderr
    print(output, end="")
    log = "storage-regression-red-result.txt" if args.prove_generation_regression else "storage-result.txt"
    (HERE / log).write_text(output, encoding="utf-8")
    if args.prove_generation_regression:
        if result.returncode and "FAIL consecutive_failed_renames_preserve_last_good_generation" in output:
            print("Original failed-generation regression caught; production source was not modified")
            raise SystemExit(0)
        raise SystemExit("Expected generation regression was not caught")
    raise SystemExit(result.returncode)
