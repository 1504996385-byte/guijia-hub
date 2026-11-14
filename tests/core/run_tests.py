"""Compile and execute the actual C99 core on the host; no Python logic clone."""
from __future__ import annotations
import argparse
import hashlib
import io
import pathlib
import shutil
import subprocess
import sys
import urllib.request
import zipfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
TCC_URL = "https://download.savannah.gnu.org/releases/tinycc/tcc-0.9.27-win64-bin.zip"
TCC_SHA256 = "34a721949a2583fdff725312da092fa0f5f1f284b702e6f811c6954714faabb2"


def compiler_path(explicit: str | None) -> str:
    if explicit:
        return explicit
    for name in ("gcc", "clang", "tcc"):
        candidate = shutil.which(name)
        if candidate:
            return candidate
    if sys.platform != "win32":
        raise SystemExit("Install a host gcc/clang/tcc or pass --compiler PATH")
    executable = HERE / "toolchain" / "tcc" / "tcc.exe"
    if not executable.exists():
        print("Downloading pinned TinyCC 0.9.27 from GNU Savannah", flush=True)
        data = urllib.request.urlopen(TCC_URL, timeout=45).read()
        if hashlib.sha256(data).hexdigest() != TCC_SHA256:
            raise SystemExit("TinyCC archive checksum mismatch")
        destination = (HERE / "toolchain").resolve()
        destination.mkdir(exist_ok=True)
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            for entry in archive.infolist():
                target = (destination / entry.filename).resolve()
                if not target.is_relative_to(destination):
                    raise SystemExit("Unsafe compiler archive path")
            archive.extractall(destination)
    return str(executable)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", help="Existing host C compiler executable")
    parser.add_argument("--red", action="store_true", help="Prove inert stub fails behavioral assertions")
    parser.add_argument("--log", type=pathlib.Path)
    args = parser.parse_args()
    compiler = compiler_path(args.compiler)
    implementation = HERE / "red_stub.c" if args.red else ROOT / "firmware/core/homehub_core.c"
    binary = HERE / ("red-tests.exe" if args.red else "core-tests.exe")
    command = [compiler, "-std=c99", "-Wall", "-Werror", "-I", str(ROOT / "firmware/core"),
               str(HERE / "test_core.c"), str(implementation), "-o", str(binary)]
    print("Compiler:", compiler, flush=True)
    built = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
    if built.returncode:
        print(built.stdout + built.stderr)
        return built.returncode
    result = subprocess.run([str(binary)], cwd=ROOT, capture_output=True, text=True)
    output = result.stdout + result.stderr
    print(output, end="", flush=True)
    log = args.log or HERE / ("red-current-result.txt" if args.red else "green-result.txt")
    log.write_text(output, encoding="utf-8")
    if args.red:
        if result.returncode != 0 and "FAIL " in output and "failure(s)" in output:
            print("Expected behavioral RED verified")
            return 0
        print("ERROR: inert stub did not fail as expected")
        return 1
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
