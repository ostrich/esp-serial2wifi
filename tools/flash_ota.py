#!/usr/bin/env python3
# ----------------------------------------------------------------------------
# "THE BEER-WARE LICENSE" (Revision 42):
# Thorsten von Eicken wrote this file. As long as you retain
# this notice you can do whatever you want with this stuff. If we meet some day,
# and you think this stuff is worth it, you can buy me a beer in return.
# ----------------------------------------------------------------------------

"""Upload firmware to the inactive slot using the versioned ESP Serial2WiFi API."""
import argparse
import json
from pathlib import Path
import re
import sys
import time
import urllib.request

sys.path.insert(0,str(Path(__file__).resolve().parent/'lib'))
ROOT=Path(__file__).resolve().parent
if not (ROOT/'sdk-lock.json').exists():ROOT=ROOT.parent
RUNTIME_VERSION = json.loads((ROOT / 'sdk-lock.json').read_text())['sdk']['runtime_version']


def validate(info, images):
    if not isinstance(info,dict):raise ValueError('Expected firmware metadata')
    if info.get('sdk') != RUNTIME_VERSION:
        raise ValueError('Target is not the verified SDK 3.0.6 baseline; migrate over serial')
    if info.get('ota_supported') is not True:
        raise ValueError('OTA is disabled or unvalidated on this target; use serial recovery')
    if type(info.get('active_slot')) is not int or type(info.get('target_slot')) is not int or info.get('active_slot') not in (1, 2) or info.get('target_slot') != 3-info['active_slot']:
        raise ValueError('Target has no valid active/inactive slot pair')
    for slot, image in enumerate(images):
        data = image.read_bytes()
        if info.get('image_abi')=='ELSDK306-MAP4-CFG2':
            from ui_package import parse
            package_slot,data,_=parse(data)
            if package_slot!=slot:raise ValueError('Update package is for another slot')
        identities = set(re.findall(rb'ELSDK306-MAP[0234]-CFG[12]', data))
        abi=info.get('image_abi')
        if not isinstance(abi,str):raise ValueError('Missing firmware layout identity')
        expected = abi.encode()
        if identities != {expected} or data[:4] != bytes((0xea, 4, 0, slot)):
            raise ValueError('Candidate SDK/layout/slot does not match the running target')




def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('-v','--verbose',action='store_true')
    parser.add_argument('--check',action='store_true',help='Validate the target and images without uploading')
    parser.add_argument('hostname')
    parser.add_argument('user1',type=Path)
    parser.add_argument('user2',type=Path)
    args=parser.parse_args()
    if not re.fullmatch(r'[-A-Za-z0-9.]+',args.hostname):
        parser.error('Use a device hostname or IPv4 address')
    base='http://'+args.hostname+'/api/v1/firmware'
    def read():
        with urllib.request.urlopen(base,timeout=5) as r:return json.load(r)
    def post(path,data,kind):
        request=urllib.request.Request(base+path,data=data,headers={
            'X-Serial2WiFi-Request':'1','Content-Type':kind})
        with urllib.request.urlopen(request,timeout=120) as r:r.read()
    try:
        before=read();images=[args.user1,args.user2];validate(before,images)
        image=images[before['target_slot']-1].read_bytes()
        if len(image)>before['max_image_bytes']:raise ValueError('Image exceeds the inactive slot')
    except (OSError,ValueError,KeyError,TypeError) as error:
        parser.exit(1,'OTA preflight refused: '+str(error)+'\n')
    if args.check:
        print('OTA SDK/layout preflight passed; no upload performed.');return
    print('Uploading to user'+str(before['target_slot'])+'; keep power connected.',flush=True)
    try:post('/upload',image,'application/octet-stream')
    except (OSError,ValueError) as error:
        parser.exit(1,'Upload was not confirmed; activation was not sent. Inspect the device before retrying: '+str(error)+'\n')
    print('Upload accepted; activating once.',flush=True)
    try:post('/activate',b'action=activate_firmware','application/x-www-form-urlencoded')
    except OSError as error:
        print('Activation reply was not confirmed; checking boot without resending.',file=sys.stderr)
        if args.verbose:print(str(error),file=sys.stderr)
    for attempt in range(30):
        try:
            after=read()
            if (after.get('active_slot')==before['target_slot'] and
                    after.get('sdk')==before['sdk'] and after.get('image_abi')==before['image_abi']):
                print('Running '+after['version']+' in user'+str(after['active_slot']))
                return
        except (OSError,ValueError,KeyError,TypeError):pass
        time.sleep(1)
    parser.exit(1,'Boot was not confirmed. Reconnect and check the device; do not repeat activation.\n')


if __name__=='__main__':main()
