#!/usr/bin/env python3
"""Validate real images and hostile image geometry with actual firmware code."""
import _paths
import binascii
from pathlib import Path
import random
import struct
import subprocess
import tempfile
import unittest
import flash_layout

ROOT = Path(__file__).resolve().parents[1]
PRIVATE = ROOT / '.build-tests/image'


def footer(data, checksum=False):
    value = bytearray(data)
    if checksum:
        offset = 24 + struct.unpack_from('<I', value, 12)[0]
        total = 0xef
        for _ in range(3):
            count = struct.unpack_from('<I', value, offset + 4)[0]
            offset += 8
            for byte in value[offset:offset + count]:
                total ^= byte
            offset += count
        value[-5] = total
    crc = binascii.crc32(value[:-4])
    struct.pack_into('<I', value, len(value)-4, ~crc & 0xffffffff if crc & 0x80000000 else crc+1)
    return value


class ImageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        PRIVATE.mkdir(parents=True, exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(dir=PRIVATE)
        cls.work = Path(cls.temp.name)
        (cls.work / 'generated_flash_layout.h').write_text(flash_layout.header(flash_layout.layout('4MB')))
        (cls.work / 'esp8266.h').write_text('''
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#define ICACHE_FLASH_ATTR
#define os_memset memset
''')
        (cls.work / 'test.c').write_text('#include "' + str(ROOT / 'src/core/firmware_image.c') + '"\n' + r'''
int main(int argc,char **argv) {
 if(argc!=4)return 2;
 FILE *f=fopen(argv[1],"rb");if(!f)return 2;
 fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);
 FirmwareImage image;
 if(size<0||!firmwareImageInit(&image,(uint32_t)size,atoi(argv[2]))) {fclose(f);return 1;}
 uint8_t data[1024];size_t count,chunk=atoi(argv[3]);
 if(chunk==0||chunk>sizeof data)return 2;
 while((count=fread(data,1,chunk,f)))if(!firmwareImageFeed(&image,data,count)){fclose(f);return 1;}
 fclose(f);return firmwareImageFinish(&image)?0:1;
}
''')
        subprocess.run(['gcc', '-std=c99', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-I' + str(cls.work),
                        str(cls.work / 'test.c'), '-o', str(cls.work / 'test')], check=True)
        cls.user1 = (ROOT / 'firmware/user1.bin').read_bytes()
        cls.user2 = (ROOT / 'firmware/user2.bin').read_bytes()

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def check(self, data, expected, slot=0, chunk=1024):
        path = self.work / 'candidate.bin'
        path.write_bytes(data)
        result = subprocess.run([str(self.work / 'test'), str(path), str(slot), str(chunk)], capture_output=True)
        self.assertEqual(result.returncode, 0 if expected else 1, result.stderr.decode())

    def test_real_images_across_chunk_boundaries(self):
        for slot, data in enumerate((self.user1, self.user2)):
            for chunk in (1, 7, 64, 1024):
                with self.subTest(slot=slot, chunk=chunk):
                    self.check(data, True, slot, chunk)
            self.check(data, False, 1-slot)

    def test_checksum_correct_malformed_images(self):
        inner = 16 + struct.unpack_from('<I', self.user1, 12)[0]
        mutations = []
        for offset, value in ((0, 0), (1, 3), (3, 1), (inner, 0),
                              (inner+1, 2), (inner+3, 0x2f)):
            data = bytearray(self.user1); data[offset] = value; mutations.append(data)
        for offset, value in ((12, 0xfffffff0), (4, 0x60000000),
                              (inner+4, 0x40100000), (inner+8, 0x60000000),
                              (inner+12, 0xfffffffc)):
            data = bytearray(self.user1); struct.pack_into('<I', data, offset, value); mutations.append(data)
        data = bytearray(self.user1); data[-5] ^= 1; mutations.append(data)
        for index, data in enumerate(mutations):
            with self.subTest(index=index):
                self.check(footer(data), False)
        marker = flash_layout.layout('4MB')['image_abi'].encode()
        self.assertIn(marker, self.user1)
        data = self.user1.replace(marker, b'X' * len(marker))
        self.check(footer(data, checksum=True), False)

    def test_truncated_corrupt_and_random_inputs(self):
        for size in (0, 4, 1024, len(self.user1)-1, len(self.user1)-4):
            self.check(self.user1[:size], False)
        self.check(self.user1 + bytes(0x7b004), False)
        rng = random.Random(306)
        for _ in range(30):
            data = bytearray(self.user1)
            at = rng.randrange(len(data));data[at] ^= 1 << rng.randrange(8)
            self.check(data, False, chunk=7)
        for size in (1028, 1044, 2052):
            self.check(rng.randbytes(size), False)


if __name__ == '__main__':
    unittest.main(verbosity=2)
