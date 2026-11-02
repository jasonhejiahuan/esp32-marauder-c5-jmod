#!/usr/bin/env python3
"""Board-scoped C5 USB dual-slot updater. App-only: no full backup, erase-all or eFuse writes."""
import argparse, base64, binascii, hashlib, json, secrets, subprocess, sys, time
from pathlib import Path
import urllib.request as url
import serial
from serial.tools import list_ports
from package_usb_ota import PROFILES, validate_image
from host_connection_recovery import RecoveryDecoder, notice, recover

class NoResetSerial(serial.Serial):
    # Both existing endpoints must retain pin state when ownership changes.
    def _update_dtr_state(self): pass
    def _update_rts_state(self): pass

class Link:
    def __init__(self, port, log):
        self.port=port;self.log=log;self.ser=None;self.buf=b'';self.recovery_decoder=RecoveryDecoder()
        self.serial_number=next((p.serial_number for p in list_ports.comports() if p.device==port),None)
    def open(self):
        if self.serial_number:
            matches=[p.device for p in list_ports.comports() if p.vid==0x303a and p.pid==0x1001 and p.serial_number==self.serial_number]
            if len(matches)!=1:raise serial.SerialException('原设备尚未重新枚举')
            self.port=matches[0]
        s=NoResetSerial(port=None,baudrate=115200,timeout=.05,write_timeout=3,exclusive=True)
        s.dtr=False;s.rts=False;s.port=self.port;s.open();self.ser=s
    def close(self):
        if self.ser:self.ser.close();self.ser=None
    def request(self, operation, arguments='', timeout=2, attempts=3):
        # Firmware implements duplicate writes by session/offset/length/CRC.
        # Each retry uses a fresh response transaction and the same operation session.
        last=None
        for attempt in range(attempts):
            tx='up_'+secrets.token_hex(6)
            try:
                if not self.ser:self.open()
                line=f'ota {operation} {tx}'+(' '+arguments if arguments else '')+'\n'
                self.ser.write(line.encode());end=time.monotonic()+timeout
                while time.monotonic()<end:
                    incoming=self.ser.read(min(self.ser.in_waiting,4096) or 1)
                    recovery=next((n for frame in self.recovery_decoder.receive(incoming) if (n:=notice(frame))),None)
                    if recovery:
                        recover(self.ser,recovery);self.buf=b'';self.recovery_decoder=RecoveryDecoder()
                        self.log.append({'Host_Connection':'explicit quarantine-to-CLI recovery completed'})
                        self.ser.write(line.encode());end=time.monotonic()+timeout;continue
                    self.buf+=incoming
                    while b'\n' in self.buf:
                        raw,self.buf=self.buf.split(b'\n',1)
                        text=raw.decode('utf-8','replace').strip().lstrip('> ')
                        if not text.startswith('@OTA:'):
                            # Uncorrelated bytes may belong to a previous binary session.
                            # Never persist them as diagnostic text or capture content.
                            continue
                        try:r=json.loads(text[5:])
                        except ValueError:continue
                        if r.get('tx')!=tx:continue
                        if not r.get('ok'):raise RuntimeError('设备拒绝操作：'+str(r.get('code')))
                        if operation!='write':self.log.append({'operation':operation,'result':r})
                        return r
                    if len(self.buf)>16384:self.buf=b''
                last=TimeoutError('USB 响应超时：'+operation)
            except (serial.SerialException,OSError) as e:
                last=e;self.close();time.sleep(.3)
        raise last or TimeoutError(operation)

def dashboard_request(base, action, data, token):
    r=url.Request(base+'/api/'+action,data=json.dumps(data).encode(),headers={'Content-Type':'application/json','Origin':base,'X-Desk-Token':token})
    return json.load(url.urlopen(r,timeout=20))

def main():
    p=argparse.ArgumentParser(description='USB 双槽应用更新；保留 NVS、配置和 LittleFS。')
    p.add_argument('bundle',nargs='?',type=Path,help='包含 manifest.json 和 application.bin 的发布目录')
    p.add_argument('--port',help='默认自动发现唯一 ESP32 原生 USB 设备')
    p.add_argument('--status',action='store_true',help='只查询固件、分区、校验与回滚状态')
    p.add_argument('--dashboard',default='http://127.0.0.1:8765',help='已运行的本机仪表盘地址')
    p.add_argument('--log-dir',type=Path,default=Path(__file__).resolve().parent.parent/'update-logs')
    p.add_argument('--test-rollback',action='store_true',help=argparse.SUPPRESS)
    p.add_argument('--legacy-writes',action='store_true',help='Use compatible Base64 stop-and-wait writes')
    p.add_argument('--rewrite-existing',action='store_true',help='Reinstall the same validated image into the inactive slot for throughput verification')
    a=p.parse_args(); log=[]; manifest=None; data=None; profile=None
    if not a.status:
        if not a.bundle:p.error('请指定更新包目录，或用 --status 查询')
        manifest=json.loads((a.bundle/'manifest.json').read_text())
        if manifest.get('format')!=1 or manifest.get('profile') not in PROFILES or manifest.get('app')!='application.bin':raise ValueError('更新包格式或构建目标不匹配')
        profile=manifest['profile']; spec=PROFILES[profile]
        data=validate_image(a.bundle/'application.bin',profile)
        if len(data)!=manifest['size'] or hashlib.sha256(data).hexdigest()!=manifest['sha256']:raise ValueError('更新包大小/SHA-256 不匹配')
    ports=[x for x in list_ports.comports() if x.vid==0x303a and x.pid==0x1001 and (not a.port or x.device==a.port)]
    if len(ports)!=1:raise RuntimeError('未找到唯一 ESP32 原生 USB 设备；请连接 USB 口，并用 --port 明确选择。')
    port=ports[0].device; token=None; reconnect=False; link=None; session=None; committed=False
    try:
        try:state=json.load(url.urlopen(a.dashboard+'/api/status',timeout=2))['state']
        except OSError:state=None
        if state and state.get('connected') and (state.get('port')==port or state.get('data_port')==port):
            if state.get('mode')!='idle' or state.get('manual'):raise RuntimeError('请先停止仪表盘中的采集或手动任务，然后重新运行更新。')
            token=json.load(url.urlopen(a.dashboard+'/api/session',timeout=2))['token']
            dashboard_request(a.dashboard,'disconnect',{},token);reconnect=True
        owner=subprocess.run(['lsof',port,port.replace('/cu.','/tty.')],capture_output=True,text=True)
        if owner.stdout.strip():raise RuntimeError('目标串口仍被占用：\n'+owner.stdout)
        link=Link(port,log); before=link.request('status',timeout=3)
        if before.get('profile') not in PROFILES or (profile and before.get('profile')!=profile):raise RuntimeError('设备尚未迁移到本更新包要求的分区/USB 构建')
        print(f"设备 {port} · {before['build']} · 当前 {before['slot']} · 每槽 {before['slot_size']//1048576} MiB",flush=True)
        if a.status:print(json.dumps(before,ensure_ascii=False,indent=2));return
        if before.get('pending') or not before.get('healthy') or not before.get('rollback_enabled'):raise RuntimeError('设备未通过启动检查或仍等待上次更新确认')
        if before['table_sha256']!=manifest['partition_sha256'] or before['boot_sha256']!=manifest['boot_sha256']:raise RuntimeError('bootloader 或分区表改变：本次不能使用仅应用 OTA；需要完整备份及专门迁移。')
        if before['slot_size']!=spec['slot_size'] or before['storage_total']!=spec['storage_size']:raise RuntimeError('设备布局不匹配')
        if before.get('app_sha256')==manifest['sha256'] and not a.test_rollback and not a.rewrite_existing:
            print('当前镜像与更新包完全一致，无需写入。',flush=True);return
        session='fw_'+secrets.token_hex(8)
        prepare_started=time.monotonic()
        print('准备空闲应用槽…',flush=True)
        link.request('begin',f"{session} {len(data)} {manifest['sha256']} {profile}",timeout=40)
        prepare_seconds=time.monotonic()-prepare_started
        log.append({'prepare_seconds':prepare_seconds})
        print(f"写入空闲槽 {before['next_slot']}；保留当前槽和数据。",flush=True)
        started=time.monotonic();last_percent=-1
        window=min(4,max(1,int(before.get('write_window',1))))
        def progress(offset):
            nonlocal last_percent
            percent=offset*100//len(data)
            if percent//10!=last_percent//10:print(f'  {percent:3d}% · {offset//1024} KiB',flush=True);last_percent=percent
        if before.get('binary_chunk_size') and not a.legacy_writes:
            from ota_window import upload_binary
            log.append(upload_binary(link,session,data,min(4096,int(before['binary_chunk_size'])),progress))
        elif window>1 and not a.legacy_writes:
            from ota_window import upload
            log.append(upload(link,session,data,window,progress))
        else:
            for offset in range(0,len(data),1024):
                chunk=data[offset:offset+1024]
                r=link.request('write',f'{session} {offset} {binascii.crc32(chunk)} '+base64.b64encode(chunk).decode(),timeout=2)
                if r['offset']!=offset+len(chunk):raise RuntimeError('设备写入偏移不匹配')
                percent=(offset+len(chunk))*100//len(data)
                if percent//10!=last_percent//10:print(f'  {percent:3d}% · {(offset+len(chunk))//1024} KiB',flush=True);last_percent=percent
        elapsed=time.monotonic()-started;log.append({'bytes':len(data),'write_seconds':elapsed,'prepare_and_write_seconds':prepare_seconds+elapsed})
        # A missing commit reply may mean the board already rebooted. Never re-upload blindly.
        try:link.request('commit',session,timeout=3,attempts=1);committed=True
        except (TimeoutError,serial.SerialException,OSError):committed=True
        print('镜像已传输，正在核验新槽启动与配置…',flush=True)
        end=time.monotonic()+40;after=None
        while time.monotonic()<end:
            try:
                candidate=link.request('status',timeout=2,attempts=1)
                if candidate.get('slot')==before['next_slot'] and candidate.get('app_sha256')==manifest['sha256']:
                    after=candidate;break
            except (TimeoutError,serial.SerialException,OSError):pass
            time.sleep(.3)
        if not after:raise RuntimeError('未能确认新槽；不标记更新成功，设备会保留/回退旧槽。')
        if manifest.get('version') and after.get('build')!=manifest['version']:raise RuntimeError('新槽版本号与更新包不一致；不确认，等待自动回退。')
        if not after.get('pending') or not after.get('healthy') or after.get('config_sha256')!=before.get('config_sha256') or after.get('table_sha256')!=before.get('table_sha256') or after.get('boot_sha256')!=before.get('boot_sha256'):
            raise RuntimeError('新槽启动、配置或布局检查失败；不确认，等待自动回退。')
        if a.test_rollback:
            print('验证模式：不确认新槽，等待自动回退…',flush=True);end=time.monotonic()+75
            while time.monotonic()<end:
                try:
                    r=link.request('status',timeout=2,attempts=1)
                    if r.get('slot')==before['slot'] and r.get('app_sha256')==before['app_sha256'] and not r.get('pending'):
                        print('自动回退已通过。',flush=True);log.append({'rollback_verified':True});return
                except (TimeoutError,serial.SerialException,OSError):pass
                time.sleep(2)
            raise RuntimeError('回退未在期限内确认')
        link.request('confirm',manifest['sha256']);final=link.request('status')
        if final.get('pending') or final.get('state')!=2 or final.get('slot')!=before['next_slot']:raise RuntimeError('更新确认状态不符')
        print(f"更新成功：{before['slot']} → {final['slot']}，{elapsed:.1f} 秒写入，准备+写入 {prepare_seconds+elapsed:.1f} 秒；配置和 LittleFS 保留。",flush=True)
    finally:
        if link:
            if session and not committed:
                try:link.request('abort',session,attempts=1)
                except Exception:pass
            link.close()
        a.log_dir.mkdir(parents=True,exist_ok=True)
        log_path=a.log_dir/(time.strftime('%Y%m%d-%H%M%S')+'.json');log_path.write_text(json.dumps(log,ensure_ascii=False,indent=2)+'\n')
        print('日志：'+str(log_path),flush=True)
        if reconnect:
            try:
                dashboard_request(a.dashboard,'connect',{'port':state['port']},token)
                if state.get('transport')=='DUAL_LINK':
                    # Dashboard auto mode restores the identity-verified data link.
                    # A second bind races that restoration and reports a false failure.
                    current=json.load(url.urlopen(a.dashboard+'/api/status',timeout=2))['state']
                    if current.get('transport')!='DUAL_LINK':
                        dashboard_request(a.dashboard,'transport',{'operation':'configure','mode':'auto'},token)
            except Exception as e:print('仪表盘重连未确认：'+str(e),file=sys.stderr)

if __name__=='__main__':
    try:main()
    except (Exception,KeyboardInterrupt) as e:print('更新停止：'+str(e),file=sys.stderr);sys.exit(1)
