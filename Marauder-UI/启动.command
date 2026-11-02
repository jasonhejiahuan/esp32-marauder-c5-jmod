#!/bin/zsh
cd "${0:A:h}"
PYTHON_BIN="/opt/homebrew/opt/esptool/libexec/bin/python"
if [[ -x .venv/bin/python ]]; then PYTHON_BIN="${PWD}/.venv/bin/python"; fi
if [[ ! -x "$PYTHON_BIN" ]]; then PYTHON_BIN="$(command -v python3)"; fi
if ! "$PYTHON_BIN" -c 'import serial' >/dev/null 2>&1; then
  print '缺少 pyserial。请使用含 pyserial 的 Python，或按 README 创建独立环境。'
  read '?按回车退出'; exit 1
fi
if curl -fsS --max-time 1 http://127.0.0.1:8765/api/session 2>/dev/null | "$PYTHON_BIN" -c 'import sys,json; assert json.load(sys.stdin)["app"] == "marauder-desk"' 2>/dev/null; then
  open 'http://127.0.0.1:8765'; exit 0
fi
"$PYTHON_BIN" server.py
