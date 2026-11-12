#!/usr/bin/env python3
"""Check capacity-specific entry selection; 2 MB keeps the vendor wrapper."""
import _paths
from pathlib import Path
import subprocess
import tempfile
import flash_layout

ROOT = Path(__file__).resolve().parents[2]
PRIVATE = ROOT / '.build-tests/startup'
PRIVATE.mkdir(parents=True, exist_ok=True)
for target in flash_layout.TARGETS:
    with tempfile.TemporaryDirectory(dir=PRIVATE) as name:
        work = Path(name)
        (work / 'esp8266.h').write_text('#include <stdbool.h>\n#include <stdint.h>\n')
        (work / 'generated_flash_layout.h').write_text(flash_layout.header(flash_layout.layout(target)))
        subprocess.run(['gcc', '-std=c99', '-Wall', '-Wextra', '-Werror', '-I' + str(work),
                        '-c', str(ROOT / 'src/core/startup.c'), '-o', str(work / 'startup.o')], check=True)
        symbols = subprocess.check_output(['nm', '-g', str(work / 'startup.o')], text=True)
        assert (' T call_user_start1' in symbols) == (target != '2MB')
        if target != '2MB':
            (work / 'test.c').write_text('''
#include <stdlib.h>
extern void call_user_start1(void);
void call_user_start(void) { exit(0); }
int main(void) { call_user_start1(); return 1; }
''')
            subprocess.run(['gcc', '-fsanitize=address,undefined', str(work / 'test.c'),
                            str(work / 'startup.o'), '-o', str(work / 'test')], check=True)
            subprocess.run([str(work / 'test')], check=True)
print('PASS capacity-specific startup: direct SDK core for 512KB/1MB/4MB; vendor wrapper retained for 2MB')
