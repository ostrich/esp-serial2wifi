#!/usr/bin/env python3
"""Build source previews, optimized web assets, or firmware ESPFS in one pipeline."""
import argparse,gzip,hashlib,shutil,struct,subprocess
from pathlib import Path
import zopfli.gzip
ROOT=Path(__file__).resolve().parents[2]
def build(output, optimize=False, vendor=None):
    source = ROOT / 'web/app'
    vendor = vendor or ROOT/'third_party/xterm-5.5.0'
    files = [path for path in source.iterdir() if path.suffix in ('.html', '.css', '.js')]
    if not files or any(path.is_symlink() or not path.is_file() for path in files):
        raise ValueError('Expected ordinary public UI asset files')
    output.mkdir(parents=True, exist_ok=True)
    if optimize:
        subprocess.run(['node', str(ROOT / 'tools/build/optimize_assets.mjs'), str(source), str(output), *([str(vendor)] if vendor else [])], check=True)
        return
    for path in files:
        shutil.copyfile(path, output / path.name)
    for name in ('xterm.js','xterm.css'):shutil.copyfile(vendor/name,output/name)
    shutil.copyfile(ROOT / 'web/custom-pages/favicon.ico', output / 'favicon.ico')

HEADER=struct.Struct('<IBBHII')
MAGIC=0x73665345
TEXT={'.html','.css','.js','.ico'}
def pad(data):return data+b'\0'*(-len(data)%4)
def pack(source,cache,compress=True):
    result=bytearray()
    cache.mkdir(parents=True,exist_ok=True)
    for file in sorted(source.rglob('*')):
        if file.is_symlink():raise ValueError('Symlink in public assets')
        if not file.is_file():continue
        name=pad(file.relative_to(source).as_posix().encode()+b'\0')
        if len(name)>32767:raise ValueError('Asset filename is too long')
        raw=file.read_bytes();data=raw;flags=0
        if compress and file.suffix in TEXT and raw:
            key=hashlib.sha256(b'zopfli-0.4.3-15\0'+raw).hexdigest()
            cached=cache/(key+'.gz')
            if cached.exists():encoded=cached.read_bytes()
            else:
                encoded=zopfli.gzip.compress(raw,numiterations=15)
                temporary=cached.with_suffix('.tmp');temporary.write_bytes(encoded);temporary.replace(cached)
            # Verify cache entries as well as new output; never trust a stale/corrupt cache.
            if gzip.decompress(encoded)!=raw:raise ValueError('Compressed asset verification failed')
            if len(encoded)<len(raw):data=encoded;flags=2
        result.extend(HEADER.pack(MAGIC,flags,0,len(name),len(data),len(raw)))
        result.extend(name);result.extend(pad(data))
    result.extend(HEADER.pack(MAGIC,1,0,0,0,0))
    return bytes(result)
RECOVERY='''<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width"><title>ESP Serial2WiFi recovery</title><style>body{max-width:42rem;margin:2rem auto;padding:1rem;font:1rem system-ui}button,input{font:inherit;margin:.5rem 0}pre{white-space:pre-wrap}</style><h1>ESP Serial2WiFi recovery</h1><p>The installed web interface is missing or doesn't match this firmware. Telnet and the serial bridge remain available. Upload a complete firmware/UI package for the target slot below, or recover over USB.</p><pre id="state">Loading device information…</pre><form><label>Update package <input required type="file" accept=".elu"></label><br><button>Upload package</button></form><button id="activate" hidden>Activate update and restart</button><script type="module">
const state=document.querySelector('#state'),form=document.querySelector('form'),activate=document.querySelector('#activate');
const request=async(path,options={})=>{const r=await fetch('/api/v1/firmware'+path,{...options,headers:{'X-Serial2WiFi-Request':'1',...options.headers}});if(!r.ok)throw new Error('Request failed ('+r.status+'). Reload this page before retrying.');return r;};
try{const info=await(await request('')).json();state.textContent='Running '+info.version+'; upload for user'+info.target_slot;}catch(e){state.textContent=e.message;}
form.onsubmit=async e=>{e.preventDefault();if(!confirm('Upload to the inactive slot? Keep power connected.'))return;form.querySelector('button').disabled=true;activate.hidden=true;state.textContent='Uploading and checking…';try{await request('/upload',{method:'POST',body:form.querySelector('input').files[0]});state.textContent='Firmware and web interface checked. Ready to activate.';activate.hidden=false;}catch(e){state.textContent=e.message;}finally{form.querySelector('button').disabled=false;}};
activate.onclick=async()=>{if(!confirm('Restart ESP Serial2WiFi with the uploaded firmware?'))return;activate.disabled=true;try{await request('/activate',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'action=activate_firmware'});state.textContent='Restart requested. Reconnect to ESP Serial2WiFi and reload this page.';}catch(e){state.textContent=e.message;}finally{activate.disabled=false;}};
</script></html>'''
def write_changed(path,data):
    if not path.exists() or path.read_bytes()!=data:path.write_bytes(data)
COMPAT=('favicon.ico','pure.css','style.css','ui.js','userpage.js')
def build_web(output, external_ui=False):
    output=output.resolve()
    if output in (ROOT,ROOT/'web/custom-pages',ROOT/'web/app',Path('/'),Path.home()):
        raise ValueError('Choose a dedicated public build directory')
    if output.exists():shutil.rmtree(output)
    output.mkdir(parents=True)
    build(output,optimize=True,vendor=ROOT/'third_party/xterm-6.0.0' if external_ui else None)
    for name in COMPAT:shutil.copyfile(ROOT/'web/custom-pages'/name,output/name)
    subprocess.run(['node',str(ROOT/'tools/build/optimize_assets.mjs'),'--compat',str(output)],check=True)
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('stage',choices=('source','web','firmware'))
    p.add_argument('--output',type=Path,default=ROOT/'build/web')
    p.add_argument('--external-ui',action='store_true');p.add_argument('--no-gzip',action='store_true');a=p.parse_args()
    if a.stage=='source':
        build(a.output,vendor=ROOT/'third_party/xterm-6.0.0' if a.external_ui else None);return
    if a.stage=='web':build_web(a.output,a.external_ui);return
    build_dir=ROOT/'build';build_dir.mkdir(exist_ok=True)
    build_web(build_dir/'web',a.external_ui)
    image=pack(ROOT/'build/web',build_dir/'asset-cache',not a.no_gzip);digest=hashlib.sha256(image).hexdigest()
    write_changed(build_dir/'ui_identity.h',('#define UI_SHA256_HEX "'+digest+'"\n').encode())
    if a.external_ui:
        if len(image)>0x3f000:raise ValueError('UI exceeds its bank')
        write_changed(build_dir/'ui.espfs',image)
        fallback=build_dir/'ui-recovery';shutil.rmtree(fallback,ignore_errors=True);fallback.mkdir(parents=True);write_changed(fallback/'index.html',RECOVERY.encode());image=pack(fallback,build_dir/'asset-cache',not a.no_gzip)
    write_changed(build_dir/'espfs.img',image)
    print(f'Embedded ESPFS: {len(image)} bytes; complete UI SHA256: {digest}')
if __name__=='__main__':main()
