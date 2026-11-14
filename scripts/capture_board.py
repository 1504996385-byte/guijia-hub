"""Read a board log UART without sending commands; raw logs stay local."""
import argparse
from pathlib import Path
import re
import time
import serial

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--port', required=True)
parser.add_argument('--baud', type=int, default=460800)
parser.add_argument('--seconds', type=int, default=600)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
args.output.parent.mkdir(parents=True, exist_ok=True)
stop = args.output.with_suffix('.stop')
if args.output.exists() or stop.exists():
    raise SystemExit('Use a new capture filename')
started = time.monotonic()
total = 0
with args.output.open('xb') as output, serial.Serial(args.port, args.baud, timeout=0.2) as uart:
    print(f'CAPTURE_OPEN {args.port} baud={args.baud}; local file={args.output}', flush=True)
    while time.monotonic() - started < args.seconds and not stop.exists():
        data = uart.read(max(1, min(uart.in_waiting, 65536)))
        if data:
            output.write(data)
            output.flush()
            total += len(data)
            if total >= 10 * 1024 * 1024:
                break
print(f'CAPTURE_CLOSED bytes={total}', flush=True)
