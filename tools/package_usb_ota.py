#!/usr/bin/env python3
"""Package only this board profile's application; never a partition migration."""
import argparse, hashlib, json, shutil, struct, re
from pathlib import Path
from esptool.bin_image import LoadFirmwareImage

PROFILE = 'waveshare-c5-rgb-usb-4m6m-v1'
LAYOUT = [('nvs',1,2,0x9000,0x5000),('otadata',1,0,0xe000,0x2000),
          ('app0',0,0x10,0x10000,0x400000),('app1',0,0x11,0x410000,0x400000),
          ('spiffs',1,0x82,0x810000,0x180000),('coredump',1,3,0x990000,0x10000),
          ('storage',1,0x82,0x9a0000,0x600000)]
XIAO_PROFILE = 'xiao-c5-usb-3m1664k-v1'
PROFILES = {
    PROFILE: dict(layout=LAYOUT, slot_size=0x400000, storage_size=0x600000,
                  flash_header=0x4f, flash_mode=2, revision=102,
                  build_target='Waveshare ESP32-C5-WIFI6-KIT-N32R8-U', led_order='RGB', scan_color='80FFFF'),
    XIAO_PROFILE: dict(layout=[('nvs',1,2,0x9000,0x5000),('otadata',1,0,0xe000,0x2000),
                  ('app0',0,0x10,0x10000,0x300000),('app1',0,0x11,0x310000,0x300000),
                  ('spiffs',1,0x82,0x610000,0x40000),('coredump',1,3,0x650000,0x10000),
                  ('storage',1,0x82,0x660000,0x1a0000)],
                  slot_size=0x300000, storage_size=0x1a0000,
                  flash_header=0x3f, flash_mode=2, revision=100,
                  build_target='Seeed Studio XIAO ESP32-C5', led_order='single-color', scan_color='yellow'),
}
def detect_profile(data):
    matches=[name for name in PROFILES if name.encode()+b'\0' in data]
    if len(matches)!=1: raise ValueError('Expected exactly one supported C5 board profile')
    return matches[0]
def sha(data): return hashlib.sha256(data).hexdigest()
def validate_image(path, profile=PROFILE):
    spec=PROFILES[profile]
    data=Path(path).read_bytes()
    image=LoadFirmwareImage('esp32c5',str(path)); image.verify()
    if not (288 <= len(data) <= spec['slot_size'] and image.chip_id==23 and image.flash_size_freq==spec['flash_header'] and image.flash_mode==spec['flash_mode']):
        raise ValueError('Image size, chip, flash mode or flash size does not match '+profile)
    if not image.append_digest or image.stored_digest!=image.calc_digest:
        raise ValueError('Invalid ESP image digest')
    if not image.min_rev_full <= spec['revision'] <= image.max_rev_full:
        raise ValueError('Image excludes the target chip revision')
    if detect_profile(data)!=profile: raise ValueError('Application belongs to a different board profile')
    return data
def version_metadata(header, data, profile=PROFILE):
    source=header.read_text()
    def value(name):
        match=re.search(r'^#define '+name+r' "([^"\n]+)"$',source,re.M)
        if not match:raise ValueError('Missing build version field: '+name)
        return match.group(1)
    upstream=value('MARAUDER_UPSTREAM_VERSION');extension=value('MARAUDER_EXTENSION_VERSION')
    n=r'(?:0|[1-9][0-9]*)'
    if not re.fullmatch(n+r'\.'+n+r'\.'+n,upstream) or not re.fullmatch(n+r'\.'+n+r'(?:\.'+n+r')?',extension):raise ValueError('Invalid upstream/jmod version')
    version=upstream+'-jmod'+extension
    if version.encode()+b'\0' not in data:raise ValueError('Build version does not match application image')
    return dict(version=version,upstream_version=upstream,extension_version=extension,build_target=PROFILES[profile]['build_target'])

def package(build, output):
    app=build/'esp32_marauder.ino.bin'; profile=detect_profile(app.read_bytes()); spec=PROFILES[profile]; data=validate_image(app,profile)
    versions=version_metadata(build/'sketch'/'FirmwareVersion.h',data,profile)
    table=(build/'esp32_marauder.ino.partitions.bin').read_bytes()
    entries=[]
    for off in range(0,len(table),32):
        magic,typ,sub,start,size,name,flags=struct.unpack('<HBBII16sI',table[off:off+32])
        if magic==0xebeb:
            if table[off+16:off+32]!=hashlib.md5(table[:off]).digest():raise ValueError('Partition MD5 invalid')
            break
        if magic!=0x50aa:raise ValueError('Unexpected partition entry')
        if flags:raise ValueError('Unexpected partition flags')
        entries.append((name.rstrip(b'\0').decode(),typ,sub,start,size))
    if entries!=spec['layout']:raise ValueError('Partition layout mismatch')
    boot=(build/'esp32_marauder.ino.bootloader.bin').read_bytes()
    if len(boot)>0x6000 or len(table)>0x1000:raise ValueError('Boot/table region overflow')
    output.mkdir(parents=True,exist_ok=True)
    manifest=dict(**versions,format=1,profile=profile,app='application.bin',size=len(data),sha256=sha(data),
                  partition_sha256=sha(table.ljust(0x1000,b'\xff')),
                  boot_sha256=sha(boot.ljust(0x6000,b'\xff')),
                  chip='esp32c5',revision=spec['revision'],slot_size=spec['slot_size'],storage_size=spec['storage_size'],
                  led_order=spec['led_order'],scan_color=spec['scan_color'])
    shutil.copy2(app,output/'application.bin')
    (output/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    return manifest
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('build',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
    print(json.dumps(package(a.build,a.output),indent=2))
