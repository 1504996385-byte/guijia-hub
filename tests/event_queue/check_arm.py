"""Syntax-check the real adapter with the current T5 build's compiler flags."""
import json
from pathlib import Path
import shlex
import subprocess

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]
entries=json.loads((ROOT/'firmware/.build/compile_commands.json').read_text())
entry=next(e for e in entries if e['file'].replace('\\','/').endswith('/homehub_app.c'))
args=shlex.split(entry['command'],posix=False)
result=[]
skip=False
for arg in args:
    if skip:
        skip=False
        continue
    if arg=='-o':
        skip=True
    elif arg=='-c':
        continue
    elif arg.strip('"').replace('\\','/').endswith('/homehub_app.c'):
        result.append(str(ROOT/'firmware/src/homehub_event_queue.c'))
    else:
        result.append(arg.strip('"'))
result.append('-fsyntax-only')
r=subprocess.run(result,cwd=entry['directory'],capture_output=True,text=True)
output=r.stdout+r.stderr+f'ARM syntax exit={r.returncode}\n'
if not r.returncode:
    build=HERE/'.build'
    build.mkdir(exist_ok=True)
    probe=build/'size-probe.c'
    probe.write_text('#include "homehub_event_queue.c"\n'
                     'char hh_event_size[sizeof(HH_Event)];\n'
                     'char hh_pointer_size[sizeof(HH_Event*)];\n'
                     'char hh_pool_size[64*sizeof(HH_Event)];\n'
                     'char hh_queue_state_size[sizeof(event_queue)];\n',encoding='utf-8')
    obj=build/'size-probe.o'
    probe_args=[str(probe) if a==str(ROOT/'firmware/src/homehub_event_queue.c') else a
                for a in result[:-1]]+['-c','-o',str(obj)]
    compiled=subprocess.run(probe_args,cwd=entry['directory'],capture_output=True,text=True)
    output+=compiled.stdout+compiled.stderr
    if compiled.returncode:
        r=compiled
    else:
        nm=Path(result[0]).parent/'arm-none-eabi-nm.exe'
        sizes=subprocess.run([str(nm),'-S',str(obj)],capture_output=True,text=True,check=True)
        for line in sizes.stdout.splitlines():
            parts=line.split()
            if len(parts)==4 and parts[3] in ('hh_event_size','hh_pointer_size','hh_pool_size','hh_queue_state_size'):
                output+=f'{parts[3]}={int(parts[1],16)}\n'
print(output,end='')
(HERE/'arm-syntax-result.txt').write_text(output,encoding='utf-8')
raise SystemExit(r.returncode)
