#!/usr/bin/env python3
"""Reproducible board-scoped Host_Connection build; never flashes a device."""
import argparse,json,subprocess
from pathlib import Path
from build_toolchain import arduino_cli_command, build_toolchain_defaults
ROOT=Path(__file__).resolve().parents[1]

def check_sources(build):
    sketch=build/'sketch'
    for file in sketch.rglob('*.cpp'):
        relative=file.relative_to(sketch)
        if str(relative)=='esp32_marauder.ino.cpp':continue
        if not (ROOT/'esp32_marauder'/relative).is_file():
            raise RuntimeError('Unexpected stale/duplicate build source: '+str(file)+'; use a fresh --build-path')

def main():
    cli,config=build_toolchain_defaults(ROOT)
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--arduino-cli',default=cli);p.add_argument('--config-file',default=config)
    p.add_argument('--build-path',type=Path,default=ROOT/'.build'/'Host_Connection');p.add_argument('--jobs',type=int,default=8)
    a=p.parse_args();build=a.build_path.resolve();check_sources(build)
    command=arduino_cli_command(a.arduino_cli,a.config_file)+['compile','--fqbn','esp32:esp32:esp32c5:CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=default_8MB,PSRAM=enabled',
             '--build-path',str(build),'--jobs',str(a.jobs),'--warnings','none',
             '--build-property','compiler.cpp.extra_flags=-DMARAUDER_C5 -DMARAUDER_WAVESHARE_C5_RGB -DMARAUDER_INTERNAL_STORAGE',
             '--build-property','upload.maximum_size=4194304',
             '--build-property','compiler.c.elf.extra_flags=-Wl,-zmuldefs -Wl,--wrap=mspi_timing_enter_low_speed_mode -Wl,--wrap=mspi_timing_enter_high_speed_mode',str(ROOT/'esp32_marauder')]
    build.mkdir(parents=True,exist_ok=True)
    (build/'Host_Connection-build-command.json').write_text(json.dumps(command,indent=2)+'\n')
    subprocess.run(command,check=True);check_sources(build)
    print('Host_Connection build verified; use package_usb_ota.py to validate/package the app.')
if __name__=='__main__':main()
