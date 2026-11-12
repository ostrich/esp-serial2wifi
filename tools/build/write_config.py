#!/usr/bin/env python3
"""Derive firmware versions and invalidate objects when build options change."""
import argparse
import hashlib
import json
import os
import re
import subprocess
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]
RELEASE=re.compile(r'v(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-([A-Za-z0-9]+(?:[.-][A-Za-z0-9]+)*))?')

def git_version(root=ROOT,development=False,tag=None):
 def git(*args):
  return subprocess.check_output(['git','-C',str(root),*args],text=True,stderr=subprocess.DEVNULL).strip()
 try:
  if Path(git('rev-parse','--show-toplevel')).resolve()!=root.resolve():raise ValueError('Not the project Git checkout')
  revision=git('rev-parse','--short=8','HEAD')
  dirty=bool(git('status','--porcelain','--untracked-files=normal'))
  tags=[t for t in git('tag','--merged','HEAD').splitlines() if RELEASE.fullmatch(t)]
  def order(t):
   m=RELEASE.fullmatch(t);return (*map(int,m.groups()[:3]),m[4] is None,m[4] or '')
  if tag is not None:
   if not RELEASE.fullmatch(tag) or git('rev-parse',f'refs/tags/{tag}^{{commit}}')!=git('rev-parse','HEAD'):
    raise ValueError('Release tag must identify the checked-out commit')
   if dirty:raise ValueError('Tagged CI releases require a clean checkout')
   return tag
  exact=[] if development else [t for t in git('tag','--points-at','HEAD').splitlines() if RELEASE.fullmatch(t)]
  if exact:version=max(exact,key=order)
  else:
   stable=[t for t in tags if RELEASE.fullmatch(t)[4] is None]
   version=(max(stable,key=order)+'-' if stable else '')+'dev.'+revision
  return version+('-dirty' if dirty else '')
 except (OSError,subprocess.CalledProcessError) as error:
  raise ValueError('Git metadata is unavailable; build with an explicit VERSION=...') from error

def main():
 parser=argparse.ArgumentParser(description=__doc__)
 parser.add_argument('--print-version',action='store_true')
 parser.add_argument('--development',action='store_true',help='Include the commit even when HEAD has a release tag')
 parser.add_argument('--tag',help='Validate and use this exact release tag (CI)')
 parser.add_argument('output',type=Path,nargs='?')
 for name in ('sdk','toolchain','flash','modules','version','debug','ota','compression'):
  parser.add_argument('--'+name)
 args=vars(parser.parse_args())
 if args.pop('print_version'):
  if args['output'] or args['development'] and args['tag']:parser.error('Choose either automatic, development or tagged version output')
  try:print(git_version(development=args['development'],tag=args['tag']))
  except ValueError as error:parser.error(str(error))
  return
 if args.pop('development') or args.pop('tag'):parser.error('--development and --tag require --print-version')
 if any(value is None for value in args.values()):parser.error('Supply the output and all build configuration options')
 output=args.pop('output')
 args['cflags_digest']=hashlib.sha256(os.environ['SERIAL2WIFI_BUILD_CFLAGS'].encode()).hexdigest()
 content=json.dumps(args,indent=2,sort_keys=True)+'\n'
 output.parent.mkdir(parents=True,exist_ok=True)
 if not output.exists() or output.read_text()!=content:output.write_text(content)

if __name__=='__main__':main()
