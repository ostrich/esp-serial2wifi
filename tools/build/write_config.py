#!/usr/bin/env python3
"""Invalidate objects when build options change, without storing credentials."""
import argparse
import hashlib
import json
import os
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output', type=Path)
for name in ('sdk', 'toolchain', 'flash', 'modules', 'version', 'debug', 'ota', 'compression'):
    parser.add_argument('--' + name, required=True)
args = vars(parser.parse_args())
output = args.pop('output')
args['cflags_digest'] = hashlib.sha256(os.environ['SERIAL2WIFI_BUILD_CFLAGS'].encode()).hexdigest()
content = json.dumps(args, indent=2, sort_keys=True) + '\n'
output.parent.mkdir(parents=True, exist_ok=True)
if not output.exists() or output.read_text() != content:
    output.write_text(content)
