#!/usr/bin/env python3
"""Check the known Superman r37 ARM decompressor against LK's fixed boot data.

This intentionally accepts only the decoded r37 decompressor/loader profile.
Unknown instruction layouts require review; they do not silently pass.
"""
from pathlib import Path
import argparse,hashlib,json,runpy,struct,zlib

def require(condition,message):
    if not condition: raise RuntimeError(message)

def arm_immediate(word):
    value=word&255;rotation=((word>>8)&15)*2
    return ((value>>rotation)|(value<<(32-rotation)))&0xffffffff if rotation else value

def verify_boot_layout(data):
    helpers=runpy.run_path(str(Path(__file__).with_name('prepare-quarantine-boot.py')))
    header,zimage,dtb,ramdisk=helpers['split_boot'](data)
    word=lambda offset:struct.unpack_from('<I',zimage,offset)[0]
    kernel_addr=struct.unpack_from('<I',header,12)[0]
    ramdisk_addr=struct.unpack_from('<I',header,20)[0]
    require(kernel_addr==0x8000 and ramdisk_addr==0x02000000,
            'Unsupported Superman LK kernel/ramdisk load-address profile')
    require(zimage[:32]==struct.pack('<I',0xe1a00000)*8,'Unknown zImage entry layout')
    require(zimage[0x9e0:0x9f0]==bytes.fromhex('0000a0e30710a0e10820a0e104f0a0e1'),
            'Unknown LK decompressor-exit patch site')
    # Decode the actual instructions used in head.S; do not assume kernel sizes.
    expected={0x1a0:0xe1a08006,0x1ac:0xe0461001,0x1b0:0xe0551001,
              0x1b4:0x80899001,0x1e0:0xe084a009,0x1f4:0xe3caa0ff,
              0x1fc:0xe3c5501f,0x228:0xe0469005,0x22c:0xe289901f,
              0x230:0xe3c9901f,0x238:0xe089900a}
    for offset,value in expected.items():
        require(word(offset)==value,'Unknown relocation instruction at '+hex(offset))
    load=word(0x1a4)
    require(load&0xfffff000==0xe59f5000,'Unknown _kernel_bss_size load')
    bss_size=word(0x1a4+8+(load&0xfff))
    adr=word(0x1a8)
    require(adr&0xfffff000==0xe28f1000,'Unknown wont_overwrite address calculation')
    wont_overwrite=0x1a8+8+arm_immediate(adr)
    bump=word(0x1f0);restart_adr=word(0x1f8)
    require(bump&0xfffff000==0xe28aa000 and restart_adr&0xfffff000==0xe24f5000,
            'Unknown relocation rounding/source calculation')
    restart=0x1f8+8-arm_immediate(restart_adr)
    source_offset=restart&~31
    gzip_start=zimage.index(bytes.fromhex('1f8b08'))
    decoder=zlib.decompressobj(31)
    image=decoder.decompress(zimage[gzip_start:],64*1024*1024+1)
    require(decoder.eof and len(image)<=64*1024*1024,'Invalid or oversized kernel gzip')
    inflated=len(image)+max(0,bss_size-(len(zimage)-wont_overwrite))
    destination=(kernel_addr+inflated+arm_immediate(bump))&~255
    # ATAG conversion may grow the DTB into its explicitly clamped workspace.
    dtb_work=min(1<<20,max(1<<15,(len(dtb)+(len(dtb)>>1)+7)&~7))
    copy_min=(len(zimage)+((len(dtb)+7)&~7)-source_offset+31)&~31
    copy_max=(len(zimage)+dtb_work-source_offset+31)&~31
    min_end=destination+copy_min
    max_copy_end=destination+copy_max
    # Include a conservative 4KiB stack/BSS allowance and 64KiB malloc region.
    scratch_end=max_copy_end+4096+65536
    ramdisk_end=ramdisk_addr+len(ramdisk)
    tags_addr=0x01f00000 # verified corrected LK tags01f profile; header tag field is ignored by LK
    overlap=lambda start,end:max(0,min(min_end,end)-max(destination,start))
    unsafe=scratch_end>tags_addr or scratch_end>ramdisk_addr
    return {'loader_profile':'Superman-r37-corrected-LK-tags01f',
            'expected_LK_sha256':'71521909dff148a13c1f0ae367a6be6cf7dd5247ae386d26e31d2ba3bc036bae',
            'boot_sha256':hashlib.sha256(data).hexdigest(),'Image_bytes':len(image),
            'kernel_bss_bytes':bss_size,'restart_offset':restart,'wont_overwrite_offset':wont_overwrite,
            'relocation_start':destination,'copy_end_min':min_end,'copy_end_max':max_copy_end,
            'scratch_end_upper':scratch_end,'LK_forced_tags_addr':tags_addr,
            'header_tags_addr':struct.unpack_from('<I',header,32)[0],
            'ramdisk_addr':ramdisk_addr,'ramdisk_end':ramdisk_end,
            'ramdisk_overlap_min_bytes':overlap(ramdisk_addr,ramdisk_end),
            'tags_start_overwritten':destination<=tags_addr<min_end,
            'ramdisk_start_overwritten':destination<=ramdisk_addr<min_end,
            'safe':not unsafe,
            'error':None if not unsafe else 'Decompressor relocation/scratch reaches fixed LK DTB or ramdisk boot data'}

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('boot',type=Path);args=parser.parse_args()
    record=verify_boot_layout(args.boot.read_bytes());print(json.dumps(record,indent=2));return 0 if record['safe'] else 1
if __name__=='__main__':raise SystemExit(main())
