#!/bin/zsh
set -eu
cd "${0:A:h}"
PYTHON=/opt/homebrew/opt/esptool/libexec/bin/python
if [[ ! -x "$PYTHON" ]]; then
  print "未找到 esptool Python 环境，请使用已安装 esptool 和 pyserial 的 Python 运行 tools/usb_update.py。"
  exit 1
fi
if (( $# )); then
  exec "$PYTHON" tools/usb_update.py "$@"
fi
print "Waveshare C5 USB 双槽更新"
print "将已构建的更新包目录拖入此窗口，然后按回车；输入 status 可只查看状态。"
read -r "bundle?更新包： "
if [[ "$bundle" == status ]]; then
  "$PYTHON" tools/usb_update.py --status
else
  bundle="${(Q)bundle}"
  "$PYTHON" tools/usb_update.py "$bundle"
fi
read -r "answer?按回车关闭。"
