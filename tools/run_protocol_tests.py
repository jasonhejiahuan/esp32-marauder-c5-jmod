#!/usr/bin/env python3
"""Run the C5 protocol/storage C++ tests without hardware or persistent output."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CASES = {
    'codec': ['tests/host_connection/codec_test.cpp'],
    'native-sniff': ['tests/host_connection/native_sniff_test.cpp'],
    'storage-removal': ['tests/host_connection/storage_removal_test.cpp'],
    'csi-timestamps': ['tests/host_connection/test_csi_timestamps.cpp', 'esp32_marauder/CsiMotionModel.cpp'],
    'discovery': ['tests/discovery/frame_test.cpp'],
}

def main():
    compiler = os.environ.get('CXX') or shutil.which('clang++') or shutil.which('g++')
    if not compiler:
        raise SystemExit('Install a C++17 compiler or set CXX')
    with tempfile.TemporaryDirectory() as directory:
        for name, sources in CASES.items():
            binary = Path(directory) / name
            subprocess.run([compiler, '-std=c++17', '-fsanitize=address,undefined',
                            '-I' + str(ROOT / 'esp32_marauder'),
                            *[str(ROOT / source) for source in sources],
                            '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
            print(name + ': passed', flush=True)

if __name__ == '__main__':
    main()
