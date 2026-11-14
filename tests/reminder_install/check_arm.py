"""Syntax-check real installer and both seeder modes with current T5 flags."""
import json
from pathlib import Path
import shlex
import subprocess
HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]
entries=json.loads((ROOT/'firmware/.build/compile_commands.json').read_text())
entry=next(e for e in entries if e['file'].replace('\\','/').endswith('/homehub_app.c'))
base=[];skip=False
for a in shlex.split(entry['command'],posix=False):
    if skip:skip=False;continue
    if a=='-o':skip=True
    elif a=='-c':continue
    elif not a.startswith('-DHOMEHUB_EMBED_TEST_VOICES='):base.append(a.strip('"'))
output='';code=0
for source,mode in [('homehub_reminder_install.c',0),('homehub_seed.c',0),('homehub_seed.c',1)]:
    args=[str(ROOT/'firmware/src'/source) if a.replace('\\','/').endswith('/homehub_app.c') else a for a in base]
    args+=['-fsyntax-only',f'-DHOMEHUB_EMBED_TEST_VOICES={mode}']
    r=subprocess.run(args,cwd=entry['directory'],capture_output=True,text=True)
    output+=r.stdout+r.stderr+f'ARM {source} embed={mode} exit={r.returncode}\n'
    code=code or r.returncode
print(output,end='');(HERE/'arm-syntax-result.txt').write_text(output,encoding='utf-8')
raise SystemExit(code)
