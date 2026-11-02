# Testing

Install Python 3.10+, Node.js and a C++17 compiler. From the repository root, create a virtual environment and install `requirements-dev.txt`.

```sh
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements-dev.txt
.venv/bin/python -m unittest discover -s tools
.venv/bin/python tools/run_protocol_tests.py
cd Marauder-UI
../.venv/bin/python -m unittest discover -s tests
for test_file in tests/*.cjs; do node "$test_file"; done
```

UI tests use synthetic MACs, network names and serial substitutes. The protocol client's recovery and package-version tests also load firmware tools using paths relative to this repository.

Real-device scripts under `Marauder-UI/tools/` require explicit `--run-hardware`, `--control` and `--data` parameters. They can start radio observation and write test files. These dual-link scripts are for the Waveshare profile; do not use them on the single-USB XIAO profile. Their output directory is ignored by Git.

A successful build or simulated test does not prove external peer/peripheral behavior, abrupt-power-loss recovery, or all board revisions. Real-device logs, identifiers, captures and backup hashes stay outside published source. Revalidate the actual board/profile before any firmware write.

## GitHub checks

The JMOD workflow runs the firmware tools, native protocol/storage checks and UI tests. It does not claim to build or test every upstream board. In this fork, the upstream all-hardware installer and nightly publisher run only when manually dispatched; automatic upstream releases remain scoped to the upstream repository.
