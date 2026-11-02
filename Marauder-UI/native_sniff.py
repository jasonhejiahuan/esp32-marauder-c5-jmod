"""Validated native sniff -> Host_Connection SNIFF_START fields."""
import struct
MODES={name:i for i,name in enumerate(('sniffraw','sniffbeacon','sniffprobe','sniffdeauth','sniffpmkid','sniffsae','sniffpwn','sniffpinescan','sniffmultissid','scanall','scancombined'),1)}
def parse_host_only(command):
    args=command.split()
    if any(x.startswith('--host-only') and x!='--host-only' for x in args):raise ValueError('请使用 --host-only，不带赋值')
    if '--host-only' not in args:return None
    if args.count('--host-only')!=1 or args[0] not in MODES:raise ValueError('此命令不支持 --host-only')
    args.remove('--host-only');name=args.pop(0);flags=0;channel=0
    while args:
        arg=args.pop(0)
        if name!='sniffpmkid' or arg not in ('-c','-d','-l'):raise ValueError('无效的原生监听参数：'+arg)
        bit={'-c':1,'-d':2,'-l':4}[arg]
        if flags&bit:raise ValueError('重复参数：'+arg)
        flags|=bit
        if arg=='-c':
            if not args or not args[0].isascii() or not args[0].isdigit():raise ValueError('需要有效频道')
            channel=int(args.pop(0))
            if not 1<=channel<=196:raise ValueError('无效频道')
    return struct.pack('<BBB',MODES[name],flags,channel)
