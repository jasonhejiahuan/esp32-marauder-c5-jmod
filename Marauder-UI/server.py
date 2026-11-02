#!/usr/bin/env python3
"""Marauder Desk: loopback HTTP/SSE bridge, one serial owner. JASON Studio."""
import struct, hashlib
import argparse, base64, binascii, collections, copy, http.server, json, os, queue, re, secrets
import select, signal, socket, socketserver, subprocess, threading, time, urllib.parse, webbrowser
from pathlib import Path
import serial
from serial.tools import list_ports
from transfers import TransferManager, VerifiedSink, Cancelled
from transport import LegacyCliClient, HostConnectionClient
from host_connection import Op, Channel, Kind, Reader, Decoder, string
from capture import CaptureSink
from native_sniff import parse_host_only
from serial_endpoint import EndpointSerial
from host_storage import DeskSettings,prepare_directory,remove_empty

ROOT = Path(__file__).resolve().parent
MAC = r'(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}'
AP = re.compile(r'(?:RSSI:\s*)?(-?\d+)\s+Ch:\s*(\d+)\s+(?:BSSID:\s*)?('+MAC+r')\s+ESSID:\s?(.*)$')
ANSI = re.compile(r'\x1b\[[0-?]*[ -/]*[@-~]')
ROUND_SECONDS = 30
MIN_ROUND_SECONDS = 29
MISSED_ROUNDS = 2
MANAGED_COMMANDS = {'scancombined','stopscan','scanall','sniffbeacon','info','help','list -a','list -s','channel','protocolinfo','bleobserve start','bleobserve stop','bleobserve clear','bleobserve status'}

VERIFIED_C5_PROFILES = {
    'waveshare-c5-rgb-usb-4m6m-v1': 'ESP32-C5 DevKit',
    'xiao-c5-usb-3m1664k-v1': 'Seeed Studio XIAO ESP32-C5',
}

def verified_c5_identity(state):
    profile = state.get('build_profile')
    if profile:
        return profile in VERIFIED_C5_PROFILES and state.get('hardware') == VERIFIED_C5_PROFILES[profile]
    return state.get('firmware') == 'v1.18.0' and state.get('hardware') == 'ESP32-C5 DevKit'

def ports():
    return [dict(path=p.device, label=('USB · ESP32' if p.vid==0x303a else 'UART · CH343'), serial=p.serial_number)
            for p in list_ports.comports() if (p.vid,p.pid) in {(0x303a,0x1001),(0x1a86,0x55d3)} and '/cu.' in p.device]

class EndpointOwnedError(RuntimeError):
    pass

class Model:
    def __init__(self):
        self.lock=threading.RLock(); self.changed=threading.Condition(self.lock); self._rev=0; self.seq=0; self.instance=secrets.token_hex(8)
        self.logs=collections.deque(maxlen=1000); self.events=collections.deque(maxlen=30)
        self.selection={"ap":{},"sta":{}}; self.log_cleared=0; self.aps={}; self.stations={}; self.buckets=collections.deque(maxlen=120); self.round=None; self.detail=None; self.list_indices=None; self.combined_counts={}; self.help_names=None; self.help_lines=None; self.metrics=None; self.metric_history=collections.deque(maxlen=120); self.ble={}; self.ble_status={}; self.ble_clock=0; self.ble_status_at=None
        self.state=dict(connected=False, verified=False, port=None, mode='disconnected', firmware=None, hardware=None,
                        telemetry_enabled=True, telemetry_interval=0, manual_refresh_interval=8, idle_refresh_interval=2, work_refresh_interval=8, link_override="auto", link_status="", usb_rx_bytes=0, usb_tx_bytes=0, manual=False, command_names=[], command_help=[], last_rx=None, error=None, started=None, deadline=None, psram=None, idf=None, completed_rounds=0, round_started=None, last_round_at=None)
    @property
    def rev(self): return self._rev
    @rev.setter
    def rev(self,value):
        with self.changed:
            self._rev=value; self.changed.notify_all()
    def wait_changed(self,revision,timeout=2):
        with self.changed:
            return self.changed.wait_for(lambda:self.rev!=revision,timeout)
    def begin_round(self,mode,at=None):
        with self.lock:
            now=time.time() if at is None else at
            self.round=dict(mode=mode,started=now,aps=set(),stations=set(),channels=set(),overflow=self.state.get('combined_overflow',0))
            self.state['round_started']=now; self.rev+=1
    def finish_round(self,at=None,aborted=False):
        with self.lock:
            r=self.round; self.round=None; self.state['round_started']=None
            now=time.time() if at is None else at
            if not r or aborted or now-r['started']<MIN_ROUND_SECONDS:
                self.rev+=1; return False
            if r['mode']=='scancombined' and (now-self.state.get('combined_status_at',0)>3 or self.state.get('combined_overflow',0)>r.get('overflow',0)):
                self.rev+=1; return False
            # Silence is only meaningful after a complete observation window.
            # scanall hops independently of ChanHop; Beacon may be fixed-channel.
            for key,ap in self.aps.items():
                covered=r['mode'] in {'scanall','scancombined'} or ap['channel'] in r['channels']
                if key in r['aps']: ap.update(online=True,missed_rounds=0)
                elif covered:
                    misses=ap.get('missed_rounds',0)+1
                    ap.update(missed_rounds=misses,online=misses<MISSED_ROUNDS)
            if r['mode'] in {'scanall','scancombined'}:
                for key,sta in self.stations.items():
                    if key in r['stations']: sta.update(online=True,missed_rounds=0)
                    else:
                        misses=sta.get('missed_rounds',0)+1
                        sta.update(missed_rounds=misses,online=misses<MISSED_ROUNDS)
            self.state['completed_rounds']+=1; self.state['last_round_at']=now
            self.rev+=1; return True
    def log(self,line,kind='rx'):
        line=ANSI.sub('',line).replace('\x00','')[:4096]
        with self.lock:
            if kind=='rx' and (line.startswith('@FILE:') or line.lstrip('> ').startswith('#file ')):
                self.state['last_rx']=time.time(); return
            if kind=='rx' and line.startswith('@DEVICE:'):
                try:
                    sample=json.loads(line[8:]); required=['uptime_ms','heap_total','heap_free','psram_total','psram_free','storage_total','storage_used']
                    if sample.get('v')!=1 or not all(isinstance(sample.get(k),(int,float)) and 0<=sample[k]<2**64 for k in required):return
                    sample['received_at']=time.time(); self.metrics=sample; self.state['last_rx']=sample['received_at']
                    self.metric_history.append([sample['received_at'],sample.get('temperature_c'),sample['heap_free'],sample['psram_free']]); self.rev+=1
                    if str(sample.get('tx','')).startswith('desk_metrics'):return
                except (ValueError,TypeError):return
            if kind=='rx' and line.lstrip('> ').startswith('#deviceinfo --machine desk_metrics'):return
            if kind=='rx' and line.startswith('@BLE:'):
                try:
                    event=json.loads(line[5:]); kind_ble=event.get('kind')
                    if kind_ble=='clear':self.ble.clear()
                    elif kind_ble=='status':
                        now=time.monotonic(); previous=self.ble_status
                        healthy=all(event.get(k,0)==previous.get(k,0) for k in ('dropped','tableFull'))
                        if previous.get('active') and self.ble_status_at is not None and healthy and 0<=now-self.ble_status_at<=5:
                            self.ble_clock+=now-self.ble_status_at
                        self.ble_status_at=now
                        for device in self.ble.values():device['online']=self.ble_clock-device.get('seen_clock',self.ble_clock)<20
                        self.ble_status=event
                        self.state['mode']='bleobserve' if event.get('active') else 'idle'
                    elif kind_ble=='device':
                        mac=event.get('mac','').lower(); typ=event.get('address_type');key=f'{mac}/{typ}'
                        if not re.fullmatch(MAC,mac) or typ not in range(4) or not isinstance(event.get('rssi'),int) or not -127<=event['rssi']<=20:return
                        if key not in self.ble and len(self.ble)>=1000:return
                        previous=self.ble.get(key,{});now=time.time();event.update(key=key,mac=mac,name=bytes.fromhex(event.get('name','')).decode('utf-8','replace'),last=now-min(max(event.get('age_ms',0),0),3600000)/1000,first=previous.get('first',now))
                        if not event['name'] and previous.get('name'):event['name']=previous['name']
                        event.update(online=True,seen_clock=self.ble_clock)
                        event['history']=(previous.get('history',[])+[[event['last'],event['rssi']]])[-60:];self.ble[key]=event
                    self.state['last_rx']=time.time();self.rev+=1;return
                except (ValueError,TypeError,AttributeError):return
            self.seq+=1; now=time.time(); self.logs.append(dict(id=self.seq,t=now,kind=kind,text=line)); self.rev+=1
            if kind!='rx': return
            self.state['last_rx']=now
            s=line.lstrip('> ').strip()
            if 'ESP-ROM:' in s:
                self.finish_round(aborted=True)
                self.metrics=None; self.metric_history.clear(); self.ble_status={}
                self.state.update(verified=False,mode='restarted',deadline=None,error='设备已重启，请重新连接以核验固件。')
            if s == '============ Commands ============': self.help_names={'help','stopscan'}; self.help_lines=['help','stopscan']
            elif self.help_names is not None:
                if re.fullmatch(r'={10,}',s):
                    self.state['command_names']=sorted(self.help_names); self.state['command_help']=self.help_lines; self.help_names=None; self.help_lines=None
                elif match:=re.match(r'^([a-z][a-z0-9_-]*)(?:\s|$)',line.removeprefix('> ')):
                    self.help_names.add(match[1]); self.help_lines.append(s)
                elif line[:1].isspace() and s.startswith('[') and self.help_lines: self.help_lines[-1]+=' '+s
            if s.startswith('#'):
                native=s[1:].split()[0] if s[1:].split() else ''
                if native in {'scanall','scanap','scansta','sniffbeacon','scancombined'}:self.native_ap_caps=native in {'scanall','scanap','scansta'}
            if s.startswith('Version:'): self.state['firmware']=s.split(':',1)[1].strip()
            if s.startswith('Firmware Build:'): self.state['firmware']=s.split(':',1)[1].strip()
            if s.startswith('Hardware:'): self.state['hardware']=s.split(':',1)[1].strip()
            if s.startswith('ESP-IDF:'): self.state['idf']=s.split(':',1)[1].strip()
            if 'PSRAM total=' in s:
                m=re.search(r'total=(\d+) free=(\d+)',s)
                if m: self.state['psram']={'total':int(m[1]),'free':int(m[2]),'at':now}
            if '@MARAUDER:' in s:
                try:
                    data=json.loads(s.split('@MARAUDER:',1)[1]); self.state['firmware']=data.get('firmware_version',data.get('firmware',self.state['firmware'])); self.state['capabilities']=data.get('capabilities',[])
                except (ValueError,TypeError): pass
            if 'Stopping WiFi tran/recv' in s: self.state.update(mode='idle',deadline=None)
            if 'Scanning for APs and Stations.' in s: self.state['mode']='scanall'
            if 'StartingBeacon sniff.' in s: self.state['mode']='sniffbeacon'
            if 'Starting AP + STA + Beacon;' in s: self.state['mode']='scancombined'
            if s.startswith('@DISC:'):
                self.discovery(s[6:],now); return
            if s.startswith('#info -a '): self.detail={}
            if self.list_indices is not None:
                idx=re.match(r'\[(\d+)\]\[CH:',s)
                if idx and 0<=int(idx[1])<2000: self.list_indices.add(int(idx[1]))
            if self.detail is not None:
                field=re.match(r'(ESSID|BSSID|Channel|RSSI|Frames|Stations|Brand|Complete EAPOL|Security):\s*(.*)',s)
                if field:
                    key,value=field[1],field[2]
                    self.detail[key]=value
                    if key=='Security':
                        details=self.detail; self.detail=None
                        bssid=details.get('BSSID','').lower()
                        if re.fullmatch(MAC,bssid) and bssid in self.aps:
                            def number(k):
                                try: return int(details[k])
                                except (KeyError,ValueError): return None
                            ap=self.aps[bssid]
                            ap.update(security=value,brand=details.get('Brand') or None,frames=number('Frames'),
                                      station_count=number('Stations'),eapol_complete=details.get('Complete EAPOL')=='TRUE',details_at=now)
            m=AP.search(s)
            if m:
                rssi,ch,bssid,name=int(m[1]),int(m[2]),m[3].lower(),m[4]
                if not (-127<=rssi<=0 and 1<=ch<=196): return
                # scanall appends two capability bytes; beacon lines do not.
                caps=None
                if getattr(self,'native_ap_caps',self.state['mode']=='scanall'):
                    tail=re.search(r' ([0-9A-F]{2}) ([0-9A-F]{2})\s*$',name)
                    if tail: caps=tail[1]+' '+tail[2]; name=name[:tail.start()]
                name=name.strip(); old=self.aps.get(bssid)
                if not name and old and old.get('named'):name=old['ssid']
                if old is None and len(self.aps)>=2000: self.aps.pop(next(iter(self.aps)))
                hist=(old or {}).get('history',[])
                if not hist or now-hist[-1][0]>=1: hist=(hist+[[now,rssi]])[-60:]
                self.aps[bssid]=dict(old or {},bssid=bssid,ssid=name or '隐藏网络',named=bool(name),channel=ch,rssi=rssi,first=(old or {}).get('first',now),last=now,
                                      samples=(old or {}).get('samples',0)+1,history=hist,online=True,missed_rounds=0)
                if caps is not None: self.aps[bssid]['capabilities']=caps
                if self.round:
                    self.round['aps'].add(bssid); self.round['channels'].add(ch)
                if old is None: self.events.appendleft(dict(bssid=bssid,t=now,text='发现 '+(name or '隐藏网络'),detail=f'CH {ch} · {bssid}'))
                sec=int(now)
                if self.buckets and self.buckets[-1][0]==sec: self.buckets[-1][1]+=1
                else: self.buckets.append([sec,1])
            for a,b in [(r'ap: ('+MAC+r') -> sta: ('+MAC+r')',False),(r'sta: ('+MAC+r') -> ap: ('+MAC+r')',True)]:
                m=re.search(a,s)
                if m:
                    sta,ap=(m[1],m[2]) if b else (m[2],m[1])
                    if len(self.stations)>=2000 and sta not in self.stations: self.stations.pop(next(iter(self.stations)))
                    sta=sta.lower(); ap=ap.lower()
                    self.stations[sta]=dict(mac=sta,ap=ap,last=now,online=True,missed_rounds=0)
                    if self.round: self.round['stations'].add(sta)
    def discovery(self,payload,now):
        """Coalesced passive observations; device age prevents delayed output becoming fresh."""
        try:
            d=json.loads(payload); kind=d['k']
            if kind=='status':
                if any(type(d[k]) is not int or d[k]<0 for k in ('beacons','data','overflow')): return
                self.state.update(combined_status_at=now,combined_overflow=d['overflow'],combined_beacons=d['beacons'],combined_data=d['data']); return
            key=d['mac'].lower(); rssi=d['r']; channel=d['ch']; count=d['n']; age=d['age']
            if kind not in {'a','s'} or not re.fullmatch(MAC,key) or int(key[:2],16)&1: return
            if any(type(x) is not int for x in (rssi,channel,count,age)): return
            if not (-127<=rssi<=0 and 1<=channel<=196 and count>0 and 0<=age<=600000): return
            seen=now-age/1000
            if kind=='a':
                raw=bytes.fromhex(d['ssid'])
                if len(raw)>32:return
                name=raw.decode('utf-8',errors='replace').replace('\x00','') or '隐藏网络'
                security={0:'Open',1:'WEP',2:'WPA',3:'WPA2',4:'WPA3',5:'WPA/WPA2 Mixed',6:'WPA2 Enterprise',7:'WPA3 Enterprise',8:'WAPI',10:'WPA2/WPA3 Mixed'}.get(d['sec'],'Unknown')
                caps=d['cap']
                if type(caps) is not int or not 0<=caps<=65535:return
                old=self.aps.get(key)
                named=bool(raw.strip(b'\0'))
                if not named and old and old.get('named'):name=old['ssid'];named=True
                hist=(old or {}).get('history',[])
                if not hist or seen-hist[-1][0]>=1:hist=(hist+[[seen,rssi]])[-60:]
                prior=self.combined_counts.get(key,0);delta=count-prior if count>=prior else count;self.combined_counts[key]=count
                self.aps[key]=dict(old or {},bssid=key,ssid=name,named=named,channel=channel,rssi=rssi,first=(old or {}).get('first',seen),last=seen,history=hist,samples=(old or {}).get('samples',0)+delta,frames=count,security=security,capabilities=f'{caps&255:02X} {caps>>8:02X}',details_at=seen,online=True,missed_rounds=0)
                self.aps[key].pop('eapol_complete',None)
                if old is None:self.events.appendleft(dict(bssid=key,t=seen,text='发现 '+name,detail=f'CH {channel} · {key}'))
                sec=int(now)
                if self.buckets and self.buckets[-1][0]==sec:self.buckets[-1][1]+=delta
                else:self.buckets.append([sec,delta])
                if self.round and seen>=self.round['started']:self.round['aps'].add(key);self.round['channels'].add(channel)
            else:
                ap=d['ap'].lower()
                if not re.fullmatch(MAC,ap) or int(ap[:2],16)&1:return
                previous_ap=self.stations.get(key,{}).get('ap')
                self.stations[key]=dict(mac=key,ap=ap,last=seen,rssi=rssi,channel=channel,frames=count,online=True,missed_rounds=0)
                if self.round and seen>=self.round['started']:self.round['stations'].add(key)
            affected={key} if kind=='a' else {ap,previous_ap}
            for bssid in affected:
                if bssid in self.aps:self.aps[bssid]['station_count']=sum(x['ap']==bssid and x.get('online',True) for x in self.stations.values())
        except (ValueError,KeyError,TypeError,AttributeError): return

    def snapshot(self,since=0):
        with self.lock:
            return copy.deepcopy(dict(instance=self.instance,rev=self.rev,now=time.time(),state=self.state,selection=self.selection,log_cleared=self.log_cleared,aps=list(self.aps.values()),stations=list(self.stations.values()),
                                      metrics=self.metrics,metric_history=list(self.metric_history),ble=list(self.ble.values()),ble_status=self.ble_status,activity=list(self.buckets),events=list(self.events),logs=[x for x in self.logs if x['id']>since],seq=self.seq))

class Bridge:
    def __init__(self,temp_directory=None,settings_path=None):
        self.recovery_notice=None;self.discovery_decoder=Decoder();self.discovering=False
        self.selection_dirty=False;self.selection_retry=0;self.host_client=None;self.data_client=None;self.data_ser=None;self.capture_sink=None;self.file_records=None;self.protocol_caps=None;self.next_host_stats=0;self.pending_capture=None;self.early_capture_drops=0;self.host_lines=b''
        self.capture_history=collections.deque();self.binary_rx=0;self.binary_tx=0;self.binary_last_rx=None
        self.model=Model(); self.jobs=queue.Queue(maxsize=24); self.ser=None; self.buf=b''
        self.capture_mode=None; self.round_until=None
        self.telemetry_clients={}; self.next_metrics=0; self.metrics_pending=0; self.metric_counter=0
        self.settings=DeskSettings(settings_path or ROOT/'.desk-settings.json')
        self.model.state['telemetry_enabled']=self.settings.get('telemetry_enabled',True) is True
        interval=self.settings.get('manual_refresh_interval',8)
        self.model.state['manual_refresh_interval']=interval if type(interval) is int and 0<=interval<=60 else 8
        for key,default in [('idle_refresh_interval',2),('work_refresh_interval',self.model.state['manual_refresh_interval'])]:
            value=self.settings.get(key,default);self.model.state[key]=value if type(value) is int and 0<=value<=60 else default
        self.link_override='auto'
        self.auto_link_enabled=False;self.auto_link_device=None;self.auto_link_resume=False
        self.next_link_check=0;self.auto_fallback_pending=False;self.link_probe_ignored=set()
        self.host_storage_lock=threading.RLock()
        self.host_storage_state=prepare_directory(temp_directory if temp_directory is not None else self.settings.get('temp_directory',''))
        self.model.state['host_storage']=dict(self.host_storage_state)
        self._wake_r,self._wake_w=socket.socketpair(); self._wake_r.setblocking(False); self._wake_w.setblocking(False)
        self.client=LegacyCliClient(self)
        self.transfers=TransferManager(self,self.host_storage_state['directory'])
        self.running=True; self.thread=threading.Thread(target=self.loop,daemon=True); self.thread.start()
    def call(self,action,**kw):
        job=dict(action=action,kw=kw,done=threading.Event(),result=None,error=None,expires=time.monotonic()+18)
        self.jobs.put_nowait(job)
        try: self._wake_w.send(b'!')
        except BlockingIOError: pass
        if not job['done'].wait(30 if action in {'connect','transport'} else 18):
            if action=='files' and kw.get('operation') in {'delete','emptytrash'}:raise TimeoutError('删除结果未确认；请刷新核对内容，不会自动重试。')
            raise TimeoutError('设备响应超时，请检查连接。')
        if job['error']: raise RuntimeError(job['error'])
        return job['result']
    def update(self,**kw):
        with self.model.lock: self.model.state.update(kw); self.model.rev+=1
    def write_serial(self,data):
        if self.host_client:
            line=data.decode('utf-8').strip();self.host_client.cli(line);return len(data)
        n=0;deadline=time.monotonic()+2
        while n<len(data):
            n+=self.ser.write(data[n:])
            if time.monotonic()>deadline:raise TimeoutError('Legacy CLI partial write timeout')
        with self.model.lock:self.model.state['usb_tx_bytes']+=n;self.model.rev+=1
        return n
    def telemetry(self,client=None,view=None,enabled=None,manual_refresh_interval=None,idle_refresh_interval=None,work_refresh_interval=None):
        now=time.monotonic();old_interval=self.metrics_interval(now)
        if manual_refresh_interval is not None:work_refresh_interval=manual_refresh_interval
        changes={}
        for key,value in [('idle_refresh_interval',idle_refresh_interval),('work_refresh_interval',work_refresh_interval)]:
            if value is None:continue
            if type(value) is not int or not 0<=value<=60:raise ValueError('状态刷新间隔须为 0–60 秒，0 表示暂停')
            changes[key]=value
        if changes:
            self.settings.update(**changes);self.update(**changes);self.next_metrics=0
            if 'work_refresh_interval' in changes:self.update(manual_refresh_interval=changes['work_refresh_interval'])
        if enabled is not None:
            if not isinstance(enabled,bool):raise ValueError('无效设置')
            self.settings.update(telemetry_enabled=enabled)
            self.update(telemetry_enabled=enabled);self.next_metrics=0
        if client is not None:
            if not isinstance(client,str) or not re.fullmatch(r'[a-zA-Z0-9_-]{8,64}',client) or view not in {'overview','background','hidden','closed'}:raise ValueError('无效订阅')
            if view=='closed':self.telemetry_clients.pop(client,None)
            else:
                if len(self.telemetry_clients)>=32 and client not in self.telemetry_clients:raise ValueError('订阅过多')
                self.telemetry_clients[client]=(view,time.monotonic()+25)
        interval=self.metrics_interval(now)
        if interval!=old_interval:self.next_metrics=now+.1 if interval else 0
        return {'ok':True,'enabled':self.model.state['telemetry_enabled'],'manual_refresh_interval':self.model.state['manual_refresh_interval']}
    def metrics_interval(self,now):
        self.telemetry_clients={k:v for k,v in self.telemetry_clients.items() if v[1]>now}
        if not self.model.state['telemetry_enabled']:return 0
        visible=[v[0] for v in self.telemetry_clients.values() if v[0]!='hidden']
        if 'overview' not in visible:return 0
        busy=self.model.state['manual'] or self.model.state['mode'] not in {'idle','connecting'} or bool(self.capture_sink and self.capture_sink.running) or self.file_records is not None
        return self.model.state['work_refresh_interval'] if busy else self.model.state['idle_refresh_interval']
    def poll_metrics(self):
        allowed=bool(self.idle_for_link_change())
        if self.model.state.get('connection_switch_allowed')!=allowed:self.update(connection_switch_allowed=allowed)
        now=time.monotonic();interval=self.metrics_interval(now)
        if self.model.state['telemetry_interval']!=interval:self.update(telemetry_interval=interval)
        if not interval or not self.ser or not self.model.state['verified'] or (not self.host_client and (self.model.state['manual'] or not self.jobs.empty())):return
        if 'device-metrics-v1' not in self.model.state.get('capabilities',[]):return
        if self.metrics_pending:
            if now-self.metrics_pending>5:self.metrics_pending=0;self.next_metrics=now+15
            return
        if now<self.next_metrics:return
        self.metrics_pending=now
        if self.host_client:
            future=self.host_client.request(Op.TELEMETRY,channel=Channel.TELEMETRY)
            future.add_done_callback(self.host_metrics);self.next_metrics=now+interval;return
        self.metric_counter+=1;self.write_serial(f'deviceinfo --machine desk_metrics{self.metric_counter}\n'.encode());self.next_metrics=now+interval
    def wait_io(self,timeout=.1,include_jobs=True):
        data_fd=None
        readers=[self._wake_r] if include_jobs else []
        if self.ser:
            try: readers.append(self.ser.fileno())
            except (AttributeError,ValueError):
                # In-memory serial adapters used by tests have no selectable fd.
                timeout=min(timeout,.005)
        if self.data_ser:
            try:data_fd=self.data_ser.fileno();readers.append(data_fd)
            except (OSError,ValueError) as e:self.data_failed(str(e))
        if any(c and c.tx_bytes for c in (self.host_client,self.data_client)):timeout=min(timeout,.001)
        try:ready,_,_=select.select(readers,[],[],timeout)
        except OSError:
            if data_fd is None:raise
            # macOS may retain a valid fd after USB removal while select raises
            # ENXIO. fstat alone cannot distinguish that from control loss.
            try:select.select([data_fd],[],[],0)
            except OSError as data_error:
                self.data_failed('USB 数据端点已断开: '+str(data_error))
                ready,_,_=select.select([fd for fd in readers if fd!=data_fd],[],[],timeout)
            else:raise
        if self._wake_r in ready:
            try: self._wake_r.recv(4096)
            except BlockingIOError: pass
    def read(self):
        if self.host_client:
            if self.host_client.closed:raise ConnectionError(self.host_client.close_reason or 'Host_Connection control session ended')
            self.binary_tx+=self.host_client.pump(self.ser) or 0
            raw=self.ser.read(min(self.ser.in_waiting,8192) or 1)
            if raw:self.host_client.feed(raw)
            if self.data_client and self.data_ser:
                try:
                    if self.data_client.closed:raise OSError('Host_Connection data session ended')
                    self.binary_tx+=self.data_client.pump(self.data_ser) or 0
                    data=self.data_ser.read(min(self.data_ser.in_waiting,8192) or 1)
                    if data:self.data_client.feed(data)
                    raw+=data
                except (OSError,serial.SerialException) as e:self.data_failed(str(e))
            if raw:self.binary_rx+=len(raw);self.binary_last_rx=time.time()
            if time.monotonic()>self.next_host_stats:
                self.next_host_stats=time.monotonic()+2
                self.publish_link_counts()
                if self.capture_sink:
                    self.update(capture=self.capture_sink.snapshot())
                    if self.capture_sink.running:self.host_client.request(Op.STATUS,channel=Channel.CONTROL).add_done_callback(self.host_stats)
            return
        data=self.ser.read(min(self.ser.in_waiting,8192) or 1)
        if data and self.discovering:
            for frame in self.discovery_decoder.feed(data):
                if frame.channel==Channel.CONTROL and frame.kind==Kind.EVENT and frame.op==Op.RECOVERY_NOTICE:
                    r=Reader(frame.payload);device=r.string(17);nonce=r.string(32);seq,endpoint=r.unpack('IB');r.end()
                    if re.fullmatch(MAC,device) and re.fullmatch('[0-9a-fA-F]{32}',nonce) and endpoint<2:
                        self.recovery_notice=(frame.session,seq,nonce);self.buf=b'';return
            # A previous session may still be draining binary records. During
            # discovery accept only the correlated human-readable capability
            # response; never feed arbitrary pre-negotiation bytes to text logs.
            self.buf+=data
            while b'\n' in self.buf:
                line,self.buf=self.buf.split(b'\n',1)
                marker=line.find(b'@MARAUDER:')
                if marker<0:continue
                try:
                    text=line[marker:].decode('utf-8','strict').rstrip('\r');report=json.loads(text[10:])
                except (UnicodeError,ValueError):continue
                if report.get('tx')!='desk' or report.get('status')!='success':continue
                self.model.log(text);self.protocol_caps=report.get('Host_Connection')
                self.exchange_lines=(getattr(self,'exchange_lines',[])+[text])[-100:]
            if len(self.buf)>16384:self.buf=b''
            return
        if data:
            with self.model.lock:self.model.state['usb_rx_bytes']+=len(data)
            self.buf+=data
            while b'\n' in self.buf:
                line,self.buf=self.buf.split(b'\n',1); text=line.decode('utf-8','replace').rstrip('\r'); self.model.log(text)
                if '@MARAUDER:' in text:
                    try:self.protocol_caps=json.loads(text.split('@MARAUDER:',1)[1]).get('Host_Connection')
                    except ValueError:pass
                if hasattr(self,'file_trace'):self.file_trace=(self.file_trace+[{'length':len(text),'prefix':text[:65],'suffix':text[-16:]}])[-8:]
                if text.startswith('@FILE:') and hasattr(self,'file_frames'):
                    try:self.file_frames=(self.file_frames+[json.loads(text[6:])])[-30:]
                    except (ValueError,TypeError):pass
                if text.startswith('@DEVICE:') and f'"tx":"desk_metrics{self.metric_counter}"' in text:self.metrics_pending=0
                if hasattr(self,'exchange_lines'):self.exchange_lines=(self.exchange_lines+[text])[-100:]
            if len(self.buf)>16384:
                self.model.log(self.buf[:4096].decode('utf-8','replace')); self.buf=b''
    def exchange(self,cmd,pattern,timeout=4):
        if self.host_client:
            self.exchange_lines=[];self.model.log(cmd,'tx');self.host_wait(self.client.command(cmd,pattern,timeout));return
        return self.client.command(cmd,pattern,timeout)
    def _legacy_exchange(self,cmd,pattern,timeout=4):
        if not self.ser: raise RuntimeError('请先连接设备。')
        before=self.model.seq; self.exchange_lines=[]; self.model.log(cmd,'tx'); self.write_serial((cmd+'\n').encode())
        end=time.monotonic()+timeout
        while time.monotonic()<end:
            if not self.ser.in_waiting:self.wait_io(min(.05,max(0,end-time.monotonic())),False)
            self.read()
            if self.discovering and self.recovery_notice:raise ConnectionError('Host_Connection recovery advertised')
            with self.model.lock:
                if any('Host_Connection: endpoint owned' in x for x in self.exchange_lines):raise EndpointOwnedError('设备旧会话仍持有端点，正在等待租约释放')
                if any(re.search(pattern,x) for x in self.exchange_lines): return
        self.update(mode='unconfirmed',deadline=None)
        raise TimeoutError('已发送 '+cmd+'，但未收到预期确认；请查看控制台。')
    def release_host_connection(self,data_only=False):
        # Called by the serial owner only, before handing an endpoint to an ordinary terminal.
        for data in (True,False):
            if data_only and not data:continue
            client=self.data_client if data else self.host_client;ser=self.data_ser if data else self.ser
            if not client or not ser:continue
            reply=self.host_wait(client.request(Op.CLOSE,channel=Channel.CONTROL),5)
            r=Reader(reply);nonce=r.string(32);r.end();line=('host_connection cli '+nonce+'\n').encode()
            sent=0;deadline=time.monotonic()+2
            while sent<len(line) and time.monotonic()<deadline:sent+=ser.write(line[sent:]);time.sleep(.001)
            buf=b''
            while time.monotonic()<deadline and b'@HOST_CONNECTION CLI' not in buf:
                buf+=ser.read(min(ser.in_waiting,4096) or 1);time.sleep(.002)
            if b'@HOST_CONNECTION CLI' not in buf:raise TimeoutError('Host_Connection CLI recovery acknowledgment missing')
            if client.limits.get('endpoint')==1 and ser.baudrate!=115200:
                ser.baudrate=115200;time.sleep(.25)
            client.close('Host_Connection clean close')
            if data:self.data_client=None;self.data_ser.close();self.data_ser=None
            else:self.host_client=None;self.client=LegacyCliClient(self)
    def publish_link_counts(self):
        if self.binary_rx or self.binary_tx:
            with self.model.lock:
                self.model.state['usb_rx_bytes']+=self.binary_rx;self.model.state['usb_tx_bytes']+=self.binary_tx
                if self.binary_last_rx:self.model.state['last_rx']=self.binary_last_rx
                self.model.rev+=1
            self.binary_rx=self.binary_tx=0
    def close(self):
        self.publish_link_counts()
        self.model.finish_round(aborted=True); self.capture_mode=None; self.round_until=None
        if self.host_client:self.host_client.close();self.host_client=None
        self.client=LegacyCliClient(self)
        if self.data_client:self.data_client.close();self.data_client=None
        if self.data_ser:
            data_ser=self.data_ser;self.data_ser=None
            try:data_ser.close()
            except OSError:pass
        if self.capture_sink:self.capture_sink.close(wait=False)
        if self.ser:
            ser=self.ser;self.ser=None
            try:ser.close()
            except OSError:pass
        self.buf=b''
        if not self.running:
            self._wake_r.close(); self._wake_w.close()
            remove_empty(self.host_storage_state['directory'])
        self.update(connected=False,verified=False,mode='disconnected',deadline=None,manual=False)
    def connect(self,path,mode='auto',expected_device=None):
        if mode not in {'auto','LEGACY_CLI','SINGLE_LINK'}:raise ValueError('Invalid transport mode')
        candidates=ports()
        if not path:
            uart=[p for p in candidates if p['label'].startswith('UART')]
            native=[p for p in candidates if p['label'].startswith('USB')]
            candidates=uart or native or candidates
            if len(candidates)!=1: raise RuntimeError('请选择一个 ESP32 串口。')
            path=candidates[0]['path']
        if path not in [p['path'] for p in ports()]: raise RuntimeError('目标串口已变化，请重新选择。')
        if self.ser:
            if self.host_client:self.release_host_connection()
            self.close()
        # An explicit owner check prevents two independent readers on macOS.
        check=subprocess.run(['/usr/sbin/lsof','-t',path,path.replace('/cu.','/tty.')],capture_output=True,text=True,timeout=3)
        owners=[p for p in check.stdout.split() if p!=str(os.getpid())]
        if owners: raise RuntimeError('串口正被其他程序占用（PID '+', '.join(owners)+'）。请断开原来的串口页面或终端。')
        s=EndpointSerial(port=None,baudrate=115200,timeout=0,write_timeout=0,exclusive=True)
        s.dtr=False; s.rts=False; s.port=path
        try: s.open()
        except Exception: s.close(); raise
        self.ser=s; self.buf=b''; self.metrics_pending=0; self.next_metrics=0
        self.protocol_caps=None;self.recovery_notice=None;self.discovery_decoder=Decoder();self.discovering=True
        self.update(connected=True,verified=False,port=path,mode='connecting',manual=False,command_names=[], command_help=[],error=None,firmware=None,hardware=None,idf=None,psram=None,upstream_version=None,extension_version=None,build_profile=None,build_target=None)
        try:
            # A reboot returns the device to CLI while the old owner may still
            # send binary bytes. Invalidate any partial line before terminating
            # it, so reconnect never executes leftover text or loses discovery.
            if path in getattr(self,'known_protocol_ports',set()):s.write(b'\x00\n')
            try:self.exchange('protocolinfo --machine desk',r'@MARAUDER:.*"tx"\s*:\s*"desk".*"status"\s*:\s*"success"',16)
            except ConnectionError:
                if not self.recovery_notice:raise
                session,seq,nonce=self.recovery_notice
                # Discard discovery text accumulated by the quarantined decoder.
                self.ser.write(b'\0')
                self.host_client=HostConnectionClient(session,self.host_wake,self.host_console,self.host_event);self.client=self.host_client
                self.host_client.sequence=seq
                self.release_host_connection()
                self.recovery_notice=None;self.discovery_decoder=Decoder()
                self.exchange('protocolinfo --machine desk',r'@MARAUDER:.*"tx"\s*:\s*"desk".*"status"\s*:\s*"success"')
            if expected_device and (self.protocol_caps or {}).get('device_id')!=expected_device:raise ValueError('USB 设备身份不同，未自动接管')
            self.discovering=False
            if self.protocol_caps and 1 in self.protocol_caps.get('versions',[]):
                if not hasattr(self,'known_protocol_ports'):self.known_protocol_ports=set()
                self.known_protocol_ports.add(path)
            if mode!='LEGACY_CLI' and self.protocol_caps and 1 in self.protocol_caps.get('versions',[]):self.negotiate()
            if mode=='SINGLE_LINK' and not self.host_client:raise ValueError('Firmware does not advertise Host_Connection v1')
            if self.host_client:
                self.exchange('info','ESP-IDF:')
                if Op.VERSION in self.host_client.limits.get('operations',[]):
                    version=self.host_client.version_result(self.host_wait(self.host_client.request(Op.VERSION,channel=Channel.CONTROL)))
                    self.update(**version)
            elif 'usb-ota-v1' in self.model.state.get('capabilities',[]):
                # Old firmware's info changes its scan mode. Query its existing
                # read-only profile instead; never "restore" state with stopscan.
                self.exchange('ota status desk_identity',r'@OTA:.*"tx"\s*:\s*"desk_identity"')
                report=next(json.loads(line.split('@OTA:',1)[1]) for line in self.exchange_lines if '@OTA:' in line)
                if report.get('ok') and report.get('profile') in VERIFIED_C5_PROFILES:self.update(hardware=VERIFIED_C5_PROFILES[report['profile']],build_profile=report['profile'])
            st=self.model.state
            if not verified_c5_identity(st):
                raise RuntimeError('固件与已核验的 C5 构建配置不符。保留日志，请核对构建目标。')
            self.exchange('help',r'^={10,}$')
            self.update(verified=True,mode='idle',started=time.time(),transport='SINGLE_LINK' if self.host_client else 'LEGACY_CLI')
            if self.host_client:self.host_metrics_payload(self.host_wait(self.host_client.request(Op.TELEMETRY,channel=Channel.TELEMETRY)))
            elif 'device-metrics-v1' in self.model.state.get('capabilities',[]):
                self.exchange('deviceinfo --machine desk_identity',r'@DEVICE:.*"tx"\s*:\s*"desk_identity"')
                if self.model.metrics:self.apply_observed_mode(self.model.metrics)
            if self.host_client:
                self.auto_link_device=self.host_client.limits['device_id'];self.auto_link_enabled=mode=='auto';self.auto_link_resume=True
            self.update(link_status='已连接',data_port=None,data_link_error=None,error=None)
            self.model.log('已连接并确认 '+st['firmware']+' / '+(st.get('build_target') or st['hardware']),'system')
        except Exception: self.close(); raise
        return {'ok':True}
    def host_wake(self):
        try:self._wake_w.send(b'!')
        except (BlockingIOError,OSError):pass
    def host_wait(self,future,timeout=10):
        deadline=time.monotonic()+timeout
        while not future.done():
            if time.monotonic()>deadline:raise TimeoutError('Host_Connection owner wait timeout')
            self.poll_metrics();self.read();self.wait_io(.01,False)
        return future.result()
    def host_console(self,data,request):
        self.host_lines+=data
        while b'\n' in self.host_lines:
            line,self.host_lines=self.host_lines.split(b'\n',1);text=line.decode('utf-8','replace').rstrip('\r');self.model.log(text)
            if hasattr(self,'exchange_lines'):self.exchange_lines=(self.exchange_lines+[text])[-100:]
        if len(self.host_lines)>16384:self.host_lines=b''
    def host_event(self,frame):
        if frame.channel==Channel.COMMAND and frame.op==Op.SELECTION_CHANGED and frame.kind==Kind.EVENT:
            self.selection_dirty=True;self.selection_retry=0
        elif frame.channel==Channel.FILE and ((frame.op==Op.FILE_RECORD and frame.kind==Kind.DATA) or (frame.op==Op.FILE_END and frame.kind==Kind.EVENT)):
            target=self.file_records
            if target:
                try:target.put_nowait(frame)
                except queue.Full:self.file_error='Host_Connection file sink queue full'
        elif frame.channel==Channel.CAPTURE and ((frame.op==Op.CAPTURE_RECORD and frame.kind==Kind.DATA) or (frame.op==Op.CAPTURE_STOP and frame.kind==Kind.EVENT)):
            if self.pending_capture is not None:
                try:self.pending_capture.put_nowait(frame)
                except queue.Full:self.early_capture_drops+=1
            elif self.capture_sink:
                if frame.op==Op.CAPTURE_RECORD:self.capture_sink.accept(frame.payload)
                else:self.capture_sink.finish(frame.payload)
    def negotiate(self):
        nonce=secrets.token_hex(16)
        deadline=time.monotonic()+14;waiting=False
        while True:
            try:
                self.exchange('host_connection upgrade 1 '+nonce,r'@HOST_CONNECTION READY '+nonce+r' \d+',3);break
            except EndpointOwnedError:
                if not waiting:self.model.log('旧会话仍持有端点；等待最多 14 秒释放，不重发任何 CLI 操作。','system');waiting=True
                self.update(link_status='等待旧会话释放端点')
                if time.monotonic()>=deadline:raise RuntimeError('端点仍被活动会话占用。请关闭另一控制程序或断开其会话，然后重新连接；无需重启固件。')
                self.wait_io(.25,False)

        line=next(x for x in self.exchange_lines if '@HOST_CONNECTION READY '+nonce in x)
        session=int(line.rsplit(' ',1)[-1]);self.buf=b'';self.host_lines=b''
        self.host_client=HostConnectionClient(session,self.host_wake,self.host_console,self.host_event);self.client=self.host_client
        limits=self.host_client.accept_hello(self.host_wait(self.host_client.hello(nonce,self.protocol_caps.get('features'))))
        if limits['device_id']!=self.protocol_caps['device_id']:raise ValueError('Host_Connection device identity changed')
        from baud_negotiation import negotiate as negotiate_uart
        negotiate_uart(self)
        self.update(host_connection=limits,transport='SINGLE_LINK');self.selection_dirty=True;self.selection_retry=0
    def host_metrics(self,future):
        self.metrics_pending=0
        try:self.host_metrics_payload(future.result())
        except Exception as e:self.model.log('Host_Connection telemetry: '+str(e),'error')
    def host_link_diagnostics(self,future):
        try:
            r=Reader(future.result());version,count=r.unpack('BB')
            if version!=1 or count not in (1,2):raise ValueError('Host_Connection diagnostics format')
            fields='mode recovery_reason last_rx_age_ms rx_sequence tx_sequence received_bytes submitted_bytes wire_length wire_offset frames'.split()
            endpoints=[dict(zip(fields,r.unpack('BBIIIIIIII'))) for _ in range(count)]
            plugged,space=r.unpack('BI');missing=r.unpack('I')[0] if r.offset<len(r.data) else 0;r.end()
            self.update(device_link_diagnostics=dict(endpoints=endpoints,native_plugged=bool(plugged),native_tx_free=space,usb_sof_missing_samples=missing))
        except Exception as e:self.model.log('Host_Connection diagnostics: '+str(e),'error')
    def host_stats(self,future):
        try:
            r=Reader(future.result());epoch,dual,active=r.unpack('IBB')
            fields='stream produced device_queue_drops transmitted queue queue_high callback_max_us queue_age_max_us console_drops request_drops'.split()
            stats=dict(zip(fields,r.unpack('I'*len(fields))));stats.update(epoch=epoch,dual=bool(dual),active=bool(active))
            stats['links']=[dict(zip(['framing_crc_errors','oversized','partial_writes','queue_drops','transitions'],r.unpack('IIIII'))) for _ in range(2)]
            if r.offset<len(r.data):
                stats.update(zip(['callback_count','invalid_rx','invalid_length','wifi_mode_error','wifi_mode','promiscuous_error','promiscuous','data_link_failed','legacy_queue_drops'],r.unpack('IIIIIIBBI')))
                stats['callback_histogram_us_le_2_5_10_20_50_100_500_inf']=list(r.unpack('IIIIIIII'))
                stats['transport_stack_min'],stats['executor_stack_min']=r.unpack('II')
            r.end();self.update(host_connection_stats=stats)
        except Exception as e:self.model.log('Host_Connection statistics: '+str(e),'error')
    def host_metrics_payload(self,payload):
        r=Reader(payload);uptime,temperature=r.unpack('Qi')
        keys='cpu_mhz cores revision reset_reason heap_total heap_free heap_min heap_largest psram_total psram_free flash_physical flash_window app_size app_slot storage_total storage_used settings_total settings_used wifi_mode ble_active'.split()
        sample=dict(zip(keys,r.unpack('I'*len(keys))));r.end()
        sample.update(v=1,uptime_ms=uptime,temperature_c=None if temperature==-2147483648 else temperature/1000,received_at=time.time(),radios=['wifi6-dual-band','ble','802.15.4'])
        with self.model.lock:
            self.model.metrics=sample;self.model.metric_history.append([sample['received_at'],sample['temperature_c'],sample['heap_free'],sample['psram_free']]);self.model.rev+=1
            self.apply_observed_mode(sample)
    def apply_observed_mode(self,sample):
        with self.model.lock:
            if self.model.state['mode'] in {'idle','connecting'}:
                mode={0:'idle',101:'idle',2:'sniffbeacon',6:'scanall',89:'scancombined',90:'csimotion'}.get(sample['wifi_mode'],'console')
                self.model.state['mode']='bleobserve' if sample['ble_active'] else mode
    def host_download(self,t,manager):
        client=self.host_client
        if not client:raise ConnectionError('Host_Connection disconnected')
        records=queue.Queue(maxsize=16);self.file_records=records;self.file_error=None;sink=None;transfer_id=None
        try:
            manager._check(t)
            # Queue receives before START so immediate records cannot outrun registration.
            payload=client.request(Op.FILE_START,string(t['path'])+struct.pack('<I',0),mutation=True).result(8)
            r=Reader(payload);transfer_id,size,generation=r.unpack('III');path=r.string(160);window,=r.unpack('I');r.end()
            if path!=t['path'] or not 1024<=window<=8192:raise ValueError('Host_Connection source identity/window')
            identity=(client.limits['device_id'],client.session,generation,path,size)
            manager._update(t,source_identity=list(identity),expected_size=size)
            sink=VerifiedSink(t['sink_path'],size,identity)
            # Initial 4 KiB credit; replenish after sink writes; fsync occurs at verified completion.
            granted=min(window,4096,size);issued=granted;pending_credit=0;credit_threshold=min(2048,granted)
            manager._check(t)
            if size:client.request(Op.FILE_CREDIT,struct.pack('<III',transfer_id,0,granted),Channel.CONTROL).result(5)
            deadline=time.monotonic()+8
            while True:
                manager._check(t)
                if self.file_error:raise IOError(self.file_error)
                if client.closed:raise ConnectionError('Host_Connection file link disconnected')
                try:frame=records.get(timeout=.1)
                except queue.Empty:
                    if time.monotonic()>deadline:raise TimeoutError('Host_Connection file records timeout')
                    continue
                deadline=time.monotonic()+8;r=Reader(frame.payload)
                if frame.op==Op.FILE_RECORD:
                    id_,offset,gen,n=r.unpack('IIIH');data=r.take(n);r.end()
                    if id_!=transfer_id or gen!=generation or n>1024:raise ValueError('Host_Connection file record identity')
                    sink.record(identity,offset,data);pending_credit+=n
                    manager._update(t,offset=sink.offset,progress=sink.offset/max(1,size))
                    if pending_credit>=credit_threshold and issued<size:
                        credit=min(pending_credit,size-issued);client.request(Op.FILE_CREDIT,struct.pack('<III',transfer_id,sink.offset,credit),Channel.CONTROL).result(5);issued+=credit;pending_credit=0
                else:
                    id_,final_size=r.unpack('II');digest=r.take(32);r.end()
                    if id_!=transfer_id:raise ValueError('Host_Connection END identity')
                    sha=sink.complete(final_size,digest);manager._check(t)
                    manager._update(t,status='COMPLETED',progress=1,sha256=sha,verification='Host_Connection-sha256');return
        except BaseException:
            if transfer_id and not client.closed:
                try:client.request(Op.FILE_CANCEL,struct.pack('<I',transfer_id),Channel.CONTROL,mutation=True).result(5)
                except Exception:pass
            raise
        finally:
            self.file_records=None
            if sink:sink.close()
    def host_storage(self,operation='status',directory=None):
        if operation=='status':
            with self.host_storage_lock:return dict(self.host_storage_state)
        if operation!='set':raise ValueError('临时目录操作无效')
        def require_idle():
            if (any(t['status'] not in {'COMPLETED','CANCELLED','FAILED'} or t['worker'].is_alive() for t in self.transfers.items.values())
                or self.pending_capture is not None or any(s.running or s.thread.is_alive() for s in self.capture_history)):
                raise ValueError('请先结束文件传输或 PCAP 捕获，再修改临时目录')
        with self.host_storage_lock,self.transfers.lock:require_idle()
        # Directory checks run in the HTTP thread, outside the serial owner and
        # outside the locks used by capture start/file transfer creation.
        prepared=prepare_directory(directory)
        try:
            with self.host_storage_lock,self.transfers.lock:
                require_idle()
                if prepared['configured']==self.host_storage_state['configured']:
                    self.settings.update(temp_directory=prepared['configured'])
                    remove_empty(prepared['directory']);return dict(self.host_storage_state)
                self.settings.update(temp_directory=prepared['configured'])
                old=self.host_storage_state
                self.transfers.directory=Path(prepared['directory'])
                self.host_storage_state=prepared;self.update(host_storage=dict(prepared))
            remove_empty(old['directory']);return dict(prepared)
        except Exception:
            remove_empty(prepared['directory']);raise
    def capture_action(self,operation,host_only=False,native_request=None):
        with self.host_storage_lock:return self._capture_action(operation,host_only,native_request)
    def _capture_action(self,operation,host_only=False,native_request=None):
        if type(host_only) is not bool:raise ValueError('Invalid host_only flag')
        if host_only and 'capture-host-only-v1' not in self.model.state.get('capabilities',[]):raise ValueError('固件不支持 --host-only')
        if not self.host_client:raise ValueError('Host_Connection required')
        if native_request is not None and 'native-sniff-stream-v1' not in self.model.state.get('capabilities',[]):raise ValueError('固件不支持原生命令 --host-only，请先更新固件')
        if operation=='start':
            if self.capture_sink and self.capture_sink.running:raise ValueError('Capture already running')
            while len(self.capture_history)>=4:
                oldest=self.capture_history[0]
                if oldest.thread.is_alive():raise ValueError('Previous capture sink has not drained')
                oldest.path.unlink(missing_ok=True);self.capture_history.popleft()
            # Data may beat the control result on the other link; bounded staging until sink registration.
            self.pending_capture=queue.Queue(maxsize=32);self.early_capture_drops=0
            try:payload=self.host_wait(self.host_client.request(Op.SNIFF_START if native_request is not None else Op.CAPTURE_START,native_request if native_request is not None else (b'\1' if host_only else b''),mutation=True))
            except Exception:self.pending_capture=None;raise
            r=Reader(payload);stream,epoch,snaplen,clock=r.unpack('IIHB');r.end()
            self.capture_sink=CaptureSink(self.transfers.directory,stream,epoch,snaplen)
            self.capture_sink.host_only=host_only;self.capture_history.append(self.capture_sink)
            early=self.pending_capture;self.pending_capture=None
            while not early.empty():self.host_event(early.get_nowait())
            self.capture_sink.stats['backend_drops']+=self.early_capture_drops
            self.update(capture=self.capture_sink.snapshot());return self.capture_sink.snapshot()
        if operation=='stop':
            self.host_wait(self.host_client.request(Op.CAPTURE_STOP,channel=Channel.CONTROL,mutation=True))
            deadline=time.monotonic()+3
            while self.capture_sink and self.capture_sink.running:
                if time.monotonic()>deadline:
                    self.capture_sink.error='Host_Connection capture end boundary unconfirmed'
                    self.capture_sink.close(wait=False);raise TimeoutError(self.capture_sink.error)
                self.read();self.wait_io(.005,False)
            if self.capture_sink:self.update(capture=self.capture_sink.snapshot())
            return {'ok':True}
        if operation=='status':return self.capture_sink.snapshot() if self.capture_sink else {'active':False}
        raise ValueError('Unknown capture operation')
    def data_failed(self,reason):
        self.auto_fallback_pending=self.idle_for_link_change()
        if self.data_client:
            c=self.data_client
            self.update(data_link_diagnostics=dict(metrics=dict(c.metrics),framing_crc_errors=c.decoder.errors,
                        oversized=c.decoder.oversized,last_rx_sequence=c.rx_sequence,last_tx_sequence=c.sequence))
            c.close(reason);self.data_client=None
        if self.data_ser:
            ser=self.data_ser;self.data_ser=None
            try:ser.close()
            except OSError:pass
        self.file_error='Host_Connection data link failed'
        if self.capture_sink:self.capture_sink.close(wait=False)
        self.update(transport='DATA_LINK_FAILED',data_link_error=reason)
        if self.host_client and Op.LINK_DIAGNOSTICS in self.host_client.limits.get('operations',[]):
            self.host_client.request(Op.LINK_DIAGNOSTICS,channel=Channel.CONTROL).add_done_callback(self.host_link_diagnostics)
    def recover_data_endpoint(self,ser,notice,expected_endpoint=0):
        """Owner-only quarantine recovery; control keeps receiving throughout."""
        r=Reader(notice.payload);device=r.string(17);nonce=r.string(32);seq,endpoint=r.unpack('IB');r.end()
        if device!=self.host_client.limits['device_id'] or endpoint!=expected_endpoint or not re.fullmatch('[0-9a-fA-F]{32}',nonce):
            raise ValueError('Host_Connection recovery device mismatch')
        ser.write(b'\0')
        client=HostConnectionClient(notice.session,self.host_wake);client.sequence=seq
        future=client.request(Op.CLOSE,channel=Channel.CONTROL);deadline=time.monotonic()+3
        while not future.done() and time.monotonic()<deadline:
            self.read();client.pump(ser);client.feed(ser.read(min(ser.in_waiting,8192) or 1));self.wait_io(.002,False)
        r=Reader(future.result(timeout=0));confirmed=r.string(32);r.end()
        if confirmed!=nonce:raise ValueError('Host_Connection recovery nonce mismatch')
        line=('host_connection cli '+nonce+'\n').encode();offset=0;buf=b''
        while time.monotonic()<deadline:
            self.read()
            if offset<len(line):offset+=ser.write(line[offset:])
            buf=(buf+ser.read(min(ser.in_waiting,4096) or 1))[-4096:]
            if b'@HOST_CONNECTION CLI' in buf:return
            self.wait_io(.002,False)
        raise TimeoutError('Host_Connection data CLI recovery timeout')
    def transport_action(self,operation,port=None,mode=None):
        if operation=="configure":return self.configure_transport(mode)
        if not self.host_client:raise ValueError('Host_Connection required')
        if operation=='status':return dict(mode=self.model.state.get('transport'),control=self.host_client.limits,data=self.data_client.limits if self.data_client else None)
        if operation=='fallback':
            # Explicit stream stop and acknowledged boundary; never silent rerouting.
            if self.file_records:raise ValueError('Cancel the file transfer before fallback')
            self.capture_action('stop')
            deadline=time.monotonic()+1
            while True:
                try:payload=self.host_wait(self.host_client.request(Op.ROUTE,b'\0',Channel.CONTROL,mutation=True));break
                except RuntimeError as e:
                    # This explicit rejection is known not to have changed routing.
                    # Ambiguous timeouts are never retried.
                    if str(e)!='routing_boundary_busy' or time.monotonic()>deadline:raise
                    self.read();self.wait_io(.005,False)
            r=Reader(payload);mode,epoch=r.unpack('BI');r.end();self.host_client.mode='SINGLE_LINK';self.update(transport='SINGLE_LINK',stream_epoch=epoch,data_port=None)
            return {'ok':True,'mode':'SINGLE_LINK','epoch':epoch}
        if operation!='bind':raise ValueError('Unknown transport operation')
        if self.host_client.limits.get('endpoint')!=1:raise ValueError('Dual link requires UART control session')
        if self.data_ser or self.file_records or (self.capture_sink and self.capture_sink.running):raise ValueError('Binding requires idle unbound data link')
        if port==self.model.state['port'] or port not in [p['path'] for p in ports()]:raise ValueError('Select another endpoint')
        owners=subprocess.run(['/usr/sbin/lsof','-t',port],capture_output=True,text=True,timeout=3).stdout.split()
        if owners:raise ValueError('Data endpoint already owned')
        s=EndpointSerial(port=None,baudrate=115200,timeout=0,write_timeout=0,exclusive=True);s.dtr=False;s.rts=False;s.port=port;s.open()
        try:
            # Human-readable discovery verifies identity before token exposure. VID/order never pairs devices.
            tx='hc_bind_'+secrets.token_hex(6);probe=('protocolinfo --machine '+tx+'\n').encode()
            s.write(probe);buf=b'';deadline=time.monotonic()+16;caps=None;decoder=Decoder();recovered=False
            while time.monotonic()<deadline and caps is None:
                self.read();raw=s.read(min(s.in_waiting,8192) or 1)
                notices=[f for f in decoder.feed(raw) if f.channel==Channel.CONTROL and f.kind==Kind.EVENT and f.op==Op.RECOVERY_NOTICE]
                if notices:
                    if recovered:raise ValueError('Host_Connection data recovery did not settle')
                    self.recover_data_endpoint(s,notices[-1]);recovered=True;buf=b'';decoder=Decoder();s.write(probe);continue
                buf+=raw
                while b'\n' in buf:
                    line,buf=buf.split(b'\n',1)
                    if b'@MARAUDER:' in line:
                        try:info=json.loads(line.split(b'@MARAUDER:',1)[1])
                        except ValueError:continue
                        if info.get('tx')==tx:caps=info.get('Host_Connection')
                if len(buf)>16384:raise ValueError('Data discovery overflow')
                self.wait_io(.005,False)
            if not caps:raise TimeoutError('USB 已枚举但未收到设备身份；等待端点就绪后自动重试')
            if caps.get('device_id')!=self.host_client.limits['device_id'] or caps.get('endpoint')!='native_usb':raise ValueError('Host_Connection data endpoint identity mismatch')
            token_reply=self.host_wait(self.host_client.request(Op.BIND_TOKEN,channel=Channel.CONTROL,mutation=True))
            r=Reader(token_reply);device=r.string(17);session,=r.unpack('I');token=r.take(16);expiry,=r.unpack('I');r.end()
            nonce=secrets.token_hex(16);s.write(('host_connection upgrade 1 '+nonce+'\n').encode());deadline=time.monotonic()+2.5;ready=False
            while time.monotonic()<deadline and not ready:
                self.read();buf+=s.read(min(s.in_waiting,8192) or 1)
                while b'\n' in buf:
                    line,buf=buf.split(b'\n',1)
                    if line==f'@HOST_CONNECTION READY {nonce} {session}'.encode():ready=True
                self.wait_io(.005,False)
            if not ready:raise TimeoutError('Host_Connection data READY timeout')
            self.data_ser=s;self.data_client=HostConnectionClient(session,self.host_wake,lambda b,r:None,self.host_event)
            limits=self.data_client.accept_hello(self.host_wait(self.data_client.hello(nonce,caps.get('features'))))
            if limits['device_id']!=device or limits['role']!=1:raise ValueError('Host_Connection data HELLO identity/role')
            reply=self.host_wait(self.data_client.request(Op.BIND,string(device)+struct.pack('<I',session)+token,Channel.CONTROL,mutation=True))
            r=Reader(reply);bound_device=r.string(17);bound_session,epoch=r.unpack('II');r.end()
            if (bound_device,bound_session)!=(device,session):raise ValueError('Host_Connection binding mismatch')
            reply=self.host_wait(self.host_client.request(Op.ROUTE,b'\1',Channel.CONTROL,mutation=True))
            r=Reader(reply);mode,epoch=r.unpack('BI');r.end()
            if mode!=1:raise ValueError('Host_Connection routing not acknowledged')
            self.host_client.mode='DUAL_LINK';self.update(transport='DUAL_LINK',stream_epoch=epoch,data_port=port);return {'ok':True,'mode':'DUAL_LINK','epoch':epoch}
        except Exception:
            if self.data_ser is s:
                try:self.release_host_connection(data_only=True)
                except Exception:pass
                self.data_failed('Host_Connection binding failed')
            else:s.close()
            raise
    def sync_selection(self):
        if not self.host_client or 'selection-v1' not in self.model.state.get('capabilities',[]):
            self.selection_dirty=False;return
        snapshot={}
        for kind,name in enumerate(('ap','sta')):
            offset=0;identity=0;items={}
            while True:
                payload=self.host_wait(self.host_client.request(Op.SELECTION_LIST,struct.pack('<BHI',kind,offset,identity)))
                r=Reader(payload);got,version,total,start,count=r.unpack('BIHHH')
                if got!=kind or start!=offset or total>4096 or count>128 or start+count>total or (identity and identity!=version):raise ValueError('Selection snapshot changed')
                identity=version
                for _ in range(count):
                    index,=r.unpack('H');mac=r.take(6).hex(':');chosen,=r.unpack('B')
                    if index!=len(items) or mac in items or chosen>1:raise ValueError('Invalid selection record')
                    items[mac]={'index':index,'selected':bool(chosen)}
                r.end();offset+=count
                if offset==total:break
                if not count:raise ValueError('Incomplete selection snapshot')
            snapshot[name]=items
        with self.model.lock:self.model.selection=snapshot;self.model.rev+=1
        self.selection_dirty=False;self.selection_retry=0
    def select_devices(self,items,selected=True):
        if type(selected) is not bool or not isinstance(items,list) or not 1<=len(items)<=64:raise ValueError('无效选择')
        if not self.host_client or 'selection-v1' not in self.model.state.get('capabilities',[]):raise ValueError('请连接支持设备选择的新固件')
        identities=[]
        for item in items:
            if not isinstance(item,dict) or item.get('kind') not in ('ap','sta') or not re.fullmatch(MAC,item.get('mac','')):raise ValueError('无效设备标识')
            identity=(item['kind'],item['mac'].lower())
            if identity in identities:raise ValueError('重复设备')
            identities.append(identity)
        completed=[]
        try:
            for kind,mac in identities:
                with self.model.lock:
                    source=copy.deepcopy((self.model.aps if kind=='ap' else self.model.stations).get(mac))
                if not source:raise ValueError('观察记录已不存在，请刷新')
                name=source.get('ssid','') if kind=='ap' else '';name='' if source.get('named') is False else name
                raw=name.encode()[:32];channel=source.get('channel') or self.model.aps.get(source.get('ap'),{}).get('channel',0)
                security={'Open':0,'WEP':1,'WPA':2,'WPA2':3,'WPA3':4,'WPA/WPA2 Mixed':5,'WPA2 Enterprise':6,'WPA3 Enterprise':7,'WAPI':8,'WPA2/WPA3 Mixed':10}.get(source.get('security'),255)
                payload=struct.pack('<BB6sBbBB',kind=='sta',selected,bytes.fromhex(mac.replace(':','')),channel,source.get('rssi',-127),security,len(raw))+raw+bytes.fromhex(source.get('ap','00:00:00:00:00:00').replace(':',''))
                result=self.host_wait(self.host_client.request(Op.SELECTION_SET,payload,mutation=True))
                response=Reader(result);got,index=response.unpack('BH');address=response.take(6).hex(':');chosen,=response.unpack('B');response.end()
                if got!=(kind=='sta') or address!=mac or chosen!=selected:raise ValueError('Selection result mismatch')
                completed.append({'kind':kind,'mac':mac,'index':index,'selected':selected})
        except Exception as e:
            raise RuntimeError(f'已确认 {len(completed)}/{len(identities)} 项；未自动重试：{e}') from e
        finally:
            self.selection_dirty=True;self.selection_retry=0
            try:self.sync_selection()
            except Exception:self.selection_dirty=True
        return {'ok':True,'items':completed}
    def enrich_aps(self):
        self.model.list_indices=set()
        try:
            self.model.log('list -a','tx'); self.write_serial(b'list -a\n')
            self.exchange('protocolinfo --machine apinfo','@MARAUDER:.*"tx":"apinfo".*"status":"success"')
            indices=sorted(self.model.list_indices)
        finally: self.model.list_indices=None
        end=time.monotonic()+8
        for index in indices:
            if time.monotonic()>=end: break
            self.exchange('info -a '+str(index),r'^Security:',timeout=min(2,max(.1,end-time.monotonic())))
    def stop_capture(self):
        if self.host_client and self.capture_sink and self.capture_sink.running:self.capture_action('stop')
        mode=self.capture_mode
        self.exchange('stopscan','Stopping WiFi tran/recv')
        self.model.finish_round()
        self.capture_mode=None; self.round_until=None; self.update(manual=False)
        if mode=='scanall':
            try: self.enrich_aps()
            except Exception as e: self.model.log('AP information: '+str(e),'error')
    def start_round(self,cmd):
        if cmd=='scanall':
            # Official C5 clearlist -a clears AP+STA RAM caches, not Flash/SSIDs.
            # A protocol barrier confirms it completed before fresh discovery.
            self.exchange('clearlist -a',r'#clearlist -a')
            self.exchange('protocolinfo --machine fresh','@MARAUDER:.*"tx":"fresh".*"status":"success"')
        self.model.begin_round(cmd)
        try:
            self.exchange(cmd,{'scanall':'Scanning for APs and Stations','sniffbeacon':'StartingBeacon sniff','scancombined':r'Starting AP \+ STA \+ Beacon;'}[cmd])
        except Exception:
            self.model.finish_round(aborted=True); raise
        self.capture_mode=cmd; self.round_until=time.time()+ROUND_SECONDS
    @staticmethod
    def file_path(path):
        if not isinstance(path,str) or not path.startswith('/') or len(path.encode())>160 or any(ord(c)<32 or ord(c)==127 or c=='\\' for c in path):raise ValueError('文件路径无效')
        if path!='/' and (path.endswith('/') or any(p in {'','..','.'} for p in path.split('/')[1:])):raise ValueError('文件路径无效')
        return path
    def file_request(self,operation,path,extra=''):
        # Only idempotent reads are retried; mutation timeouts require explicit refresh.
        for attempt in range(3 if operation in {'read','stat','list'} else 1):
            try:return self.file_exchange(operation,path,extra)
            except TimeoutError:
                if attempt==(2 if operation in {'read','stat','list'} else 0):raise
    def file_exchange(self,operation,path,extra=''):
        tx='deskfile_'+secrets.token_hex(6);self.file_frames=[];self.file_trace=[]
        self.write_serial(f'file {operation} {tx} {path.encode().hex()}{extra}\n'.encode())
        end=time.monotonic()+(1.2 if operation in {'read','stat','list'} else 14 if operation in {'delete','emptytrash'} else 4)
        while time.monotonic()<end:
            self.wait_io(.02,False);self.read()
            frames=[f for f in self.file_frames if isinstance(f,dict) and f.get('tx')==tx]
            done=next((f for f in frames if f.get('kind')=='end'),None)
            if done:
                if not done.get('ok'):
                    if operation=='list' and path=='/.trash' and done.get('error')=='not_directory':return [],{'ok':True,'next':-1,'total':None,'used':None}
                    errors={'busy':'请先停止采集或手动控制','unmounted':'内部存储未挂载','not_directory':'目录不存在','not_file':'文件不存在','missing_or_exists':'源文件不存在，或目标名称已存在','exists_or_protected':'名称已存在或路径受保护','protected_path':'不能修改根目录或回收站本身','path':'文件路径无效','rename':'文件移动失败','read':'文件读取失败','mkdir':'无法创建目录'}
                    raise ValueError(errors.get(done.get('error'),'文件操作失败：'+str(done.get('error'))))
                return frames,done
        (ROOT/'.file-transfer-debug.json').write_text(json.dumps({'operation':operation,'extra':extra,'trace':self.file_trace,'pending_length':len(self.buf),'pending_prefix':self.buf[:65].decode('utf-8','replace'),'pending_suffix':self.buf[-16:].decode('utf-8','replace')},ensure_ascii=False))
        raise TimeoutError('文件操作确认超时；请刷新目录核对结果，不会自动重试写入。')
    def file_job(self,job):
        if job['kw'].get('operation') in {'delete','emptytrash'} and time.monotonic()>=job.get('expires',0):
            raise TimeoutError('删除请求排队超时，未执行。')
        return self.files(**job['kw'])
    def files(self,operation,path='/',destination=None,offset=0,length=1024,confirm=False):
        if not self.ser or not self.model.state['verified']:raise ValueError('请先连接设备')
        if self.model.state['mode']!='idle' or self.model.state['manual']:raise ValueError('请先停止采集或手动控制')
        if 'files-v1' not in self.model.state.get('capabilities',[]):raise ValueError('当前固件不支持文件管理')
        if operation not in {'list','stat','read','rename','mkdir','trash','restore','delete','emptytrash'}:raise ValueError('文件操作无效')
        path=self.file_path(path);extra=''
        destructive=operation in {'delete','emptytrash'}
        if destructive:
            if 'files-delete-v1' not in self.model.state.get('capabilities',[]):raise ValueError('当前固件不支持永久删除')
            if confirm is not True:raise ValueError('请确认永久删除，此操作无法恢复')
            if (operation=='delete' and not path.startswith('/.trash/')) or (operation=='emptytrash' and path!='/.trash'):raise ValueError('永久删除仅限回收站内；清空仅限回收站根目录')
        if operation=='rename':extra=' '+self.file_path(destination).encode().hex()
        if operation in {'list','read'}:
            if type(offset)!=int or not 0<=offset<=6291456:raise ValueError('偏移无效')
            extra=f' {offset}'
        if operation=='read':
            if type(length)!=int or not 1<=length<=1024:raise ValueError('读取长度无效')
            extra+=f' {length}'
        try:
            if self.host_client:
                result=self.host_client.file_result(self.host_wait(self.host_client.file_operation(operation,path,destination,offset,length),16 if destructive else 10),path)
                raw=result.pop('raw')
                if operation=='read':result.update(raw=raw,crc32=binascii.crc32(raw))
                return result
            frames,result=self.file_request(operation,path,extra)
        except (RuntimeError,ValueError,TimeoutError) as error:
            if destructive:
                code=str(error).removeprefix('文件操作失败：')
                messages={'delete_partial':'删除未完成，部分内容已删除；请刷新核对剩余内容',
                          'delete_limit_partial':'已达到本次删除上限，部分内容已删除；请刷新后重新确认剩余内容',
                          'delete_limit':'已达到本次删除上限；请刷新后重新确认',
                          'delete_io':'存储读取或删除失败；请刷新核对内容','missing':'项目已不存在；请刷新目录'}
                if isinstance(error,TimeoutError):raise TimeoutError('删除结果未确认；请刷新核对内容，不会自动重试。') from error
                raise ValueError(messages.get(code,str(error))) from error
            raise
        if operation=='list':
            entries=[]
            for f in frames:
                if f.get('kind')!='entry':continue
                name=bytes.fromhex(f['name']).decode('utf-8','strict')
                if not name or '/' in name or '\\' in name:raise ValueError('目录响应无效')
                full=self.file_path(path.rstrip('/')+'/'+name)
                entries.append({'name':name,'path':full,'directory':f['directory'] is True,'size':f['size']})
            return {'ok':True,'entries':entries,'next':result['next'],'total':result['total'],'used':result['used']}
        if operation=='read':
            try:raw=base64.b64decode(result['data'],validate=True)
            except (ValueError,binascii.Error,KeyError):raise ValueError('文件数据损坏')
            if result.get('offset')!=offset or len(raw)>length or binascii.crc32(raw)!=result.get('crc32'):raise ValueError('文件分块校验失败')
            if not raw and offset<result.get('size',0):raise ValueError('文件数据提前结束')
            return {k:result[k] for k in ['ok','offset','size','data','crc32']}
        return {k:v for k,v in result.items() if k not in {'tx','kind'}}
    def console(self,cmd):
        if not isinstance(cmd,str) or not cmd.strip() or any(ord(c)<32 or ord(c)==127 for c in cmd) or len(cmd.encode('utf-8'))>2048:
            raise ValueError('请输入单行命令（最多 2048 字节）。')
        if not self.ser or not self.model.state['verified']: raise RuntimeError('请先连接并核验设备。')
        cmd=cmd.strip()
        if cmd.split()[0]=='capture':raise ValueError('请使用 sniffxxx [原生参数] --host-only；停止使用 stopscan')
        native_request=parse_host_only(cmd)
        if native_request is not None:
            result=self.capture_action('start',host_only=True,native_request=native_request)
            self.model.finish_round(aborted=True);self.capture_mode=None;self.round_until=None
            self.update(deadline=None,manual=True,mode='console',error=None)
            self.model.log(cmd,'tx');self.model.log('Host_Connection PCAP: '+result.get('path',''),'system');return result
        # Manual CLI owns subsequent radio state; stop only the host scheduler.
        # Do not insert stopscan/list queries into the user's command sequence.
        self.model.finish_round(aborted=True); self.capture_mode=None; self.round_until=None
        self.update(deadline=None,manual=True,mode='console',error=None)
        if cmd=='stopscan':
            self.stop_capture()
        else:
            self.model.log(cmd,'tx')
            if self.host_client:self.host_wait(self.host_client.cli(cmd))
            else:self.write_serial((cmd+'\n').encode())
            if cmd.split()[0]=='reboot': self.update(verified=False,mode='restarted')
        return {'ok':True,'command':cmd}
    def command(self,cmd,duration=30):
        if not isinstance(cmd,str) or cmd not in MANAGED_COMMANDS: return self.console(cmd)
        if not self.model.state['verified']: raise RuntimeError('请先连接并核验设备。')
        if cmd in {'scanall','sniffbeacon','scancombined'}:
            if self.model.state['mode']=='bleobserve':raise ValueError('请先停止蓝牙发现。')
            if not isinstance(duration,int) or not 5<=duration<=3600: raise ValueError('采集时长须为 5–3600 秒。')
            if cmd=='scancombined':
                if 'discovery-v1' not in self.model.state.get('capabilities',[]):raise ValueError('当前固件不支持 AP + STA + Beacon。')
                self.model.combined_counts.clear()
            self.stop_capture(); self.start_round(cmd)
            self.update(deadline=time.time()+duration,error=None)
        elif cmd.startswith('bleobserve '):
            action=cmd.split()[1]
            if 'ble-observer-v1' not in self.model.state.get('capabilities',[]):raise ValueError('当前固件不支持蓝牙观察。')
            if action=='start':
                if self.model.state['mode']!='idle' or self.model.state['manual']:raise ValueError('请先停止当前任务。')
                self.exchange(cmd,'BLE observation started; passive');self.update(mode='bleobserve')
            elif action=='stop':self.exchange(cmd,'BLE observation stopped');self.update(mode='idle',manual=False)
            elif action=='clear':
                if self.model.state['mode']!='idle':raise ValueError('请先停止蓝牙发现。')
                self.exchange(cmd,'@BLE:.*"kind":"clear"')
            else:self.exchange(cmd,'@BLE:.*"kind":"status"')
        elif cmd=='stopscan': self.stop_capture(); self.update(error=None)
        elif self.model.state['mode']!='idle': raise ValueError('请先停止采集，再执行查询。')
        elif cmd=='info':
            self.exchange(cmd,'ESP-IDF:')
        elif cmd=='protocolinfo': self.exchange(cmd,'Protocol|protocol|Firmware')
        else:
            self.model.log(cmd,'tx'); self.write_serial((cmd+'\n').encode())
        return {'ok':True,'command':cmd}
    def configure_transport(self,mode):
        with self.transfers.lock:return self._configure_transport(mode)
    def _configure_transport(self,mode):
        if mode not in {'auto','uart','usb','dual'}:raise ValueError('无效连接模式')
        if not self.idle_for_link_change():raise ValueError('任务执行中禁止切换连接模式；请先停止任务和文件传输。')
        if not self.host_client:raise ValueError('请先连接支持 Host_Connection 的固件')
        if mode=='auto':
            self.link_override=mode;self.auto_link_enabled=True;self.next_link_check=0;self.update(link_override=mode);return {'ok':True,'mode':mode}
        candidates=ports();uart=[p['path'] for p in candidates if p['label'].startswith('UART')];usb=[p['path'] for p in candidates if p['label'].startswith('USB')]
        choices=usb if mode=='usb' else uart
        if len(choices)!=1 or (mode=='dual' and len(usb)!=1):raise ValueError('所需连接线尚未枚举或有多个设备；请检查 USB 数据线和端口。')
        target=choices[0];device=self.host_client.limits['device_id'];old=self.link_override
        try:
            if target!=self.model.state['port']:
                if target!=self.model.state.get('data_port') and self.probe_idle_identity(target)!=device:raise ValueError('目标端口不是同一设备；未切换')
                self.release_host_connection();self.connect(target,expected_device=device)
            elif mode!='dual' and self.data_ser:
                self.transport_action('fallback');self.release_host_connection(data_only=True)
            if mode=='dual' and not self.data_ser:self.transport_action('bind',usb[0])
            self.link_override=mode;self.auto_link_enabled=True;self.auto_link_resume=True
            self.update(link_override=mode,link_status='连接模式已手动覆盖（仅本次会话）',error=None)
            return {'ok':True,'mode':mode,'transport':self.model.state['transport']}
        except Exception:
            self.link_override=old;self.update(link_override=old);raise
    def idle_for_link_change(self):
        state=self.model.state
        with self.transfers.lock:
            transferring=any(t.get('status') not in {'COMPLETED','CANCELLED','FAILED'} for t in self.transfers.items.values())
        return not transferring and state.get('verified') and state.get('mode')=='idle' and not state.get('manual') and self.file_records is None and not (self.capture_sink and self.capture_sink.running)
    def connection_failed(self,error):
        resume=self.idle_for_link_change()
        if self.host_client:
            c=self.host_client
            self.update(control_link_diagnostics=dict(reason=str(error),metrics=dict(c.metrics),framing_crc_errors=c.decoder.errors,last_rx_sequence=c.rx_sequence,last_tx_sequence=c.sequence))
        self.close();self.auto_link_resume=resume
        self.update(error=None if resume else str(error),link_status='连接已拔出，等待同一设备' if resume else '连接中断；未重放操作')
        self.model.log('连接已中断；'+('空闲时自动恢复同一设备。' if resume else '当前操作未自动重试。')+str(error),'system' if resume else 'error')
    def probe_idle_identity(self,path):
        owners=subprocess.run(['/usr/sbin/lsof','-t',path],capture_output=True,text=True,timeout=2).stdout.split()
        if owners:return None
        probe=EndpointSerial(port=None,baudrate=115200,timeout=0,write_timeout=0,exclusive=True);probe.dtr=False;probe.rts=False;probe.port=path;probe.open()
        try:
            tx='link_'+secrets.token_hex(5);query=('protocolinfo --machine '+tx+'\n').encode()
            probe.write(query);buf=b'';end=time.monotonic()+1;decoder=Decoder();recovered=False
            while time.monotonic()<end:
                self.read();raw=probe.read(min(probe.in_waiting,4096) or 1)
                notices=[f for f in decoder.feed(raw) if f.channel==Channel.CONTROL and f.kind==Kind.EVENT and f.op==Op.RECOVERY_NOTICE]
                if notices and not recovered:
                    # A reinserted former UART control endpoint may still be in
                    # binary quarantine. Recover only its explicit identity-bound
                    # notice while continuing to service the live USB control.
                    self.recover_data_endpoint(probe,notices[-1],expected_endpoint=1)
                    recovered=True;decoder=Decoder();buf=b'';probe.write(query);end=time.monotonic()+1
                    continue
                buf+=raw
                while b'\n' in buf:
                    line,buf=buf.split(b'\n',1)
                    if b'@MARAUDER:' not in line:continue
                    try:report=json.loads(line.split(b'@MARAUDER:',1)[1])
                    except ValueError:continue
                    if report.get('tx')==tx:return report.get('Host_Connection',{}).get('device_id')
                if len(buf)>16384:return None
                self.wait_io(.01,False)
        finally:probe.close()
    def maintain_idle_links(self):
        with self.transfers.lock:return self._maintain_idle_links()
    def _maintain_idle_links(self):
        now=time.monotonic()
        if not self.auto_link_enabled or now<self.next_link_check:return
        self.next_link_check=now+2
        candidates=ports();paths={p['path'] for p in candidates}
        self.link_probe_ignored.intersection_update(paths)
        if self.ser and self.model.state.get('port') not in paths:
            self.connection_failed(OSError('控制线已拔出'));return
        if not self.ser:
            if not self.auto_link_resume or not self.auto_link_device:return
            for candidate in sorted(candidates,key=lambda p:not p['label'].startswith('UART')):
                path=candidate['path']
                if self.link_override=='uart' and not candidate['label'].startswith('UART'):continue
                if self.link_override=='usb' and not candidate['label'].startswith('USB'):continue
                if path in self.link_probe_ignored:continue
                self.update(link_status='正在恢复同一设备连接')
                try:self.connect(path,expected_device=self.auto_link_device);self.model.log('空闲连接已自动恢复。','system');return
                except Exception as e:
                    if '身份不同' in str(e):self.link_probe_ignored.add(path)
                    self.update(link_status='等待设备端点释放后重连',error=None);self.next_link_check=time.monotonic()+5
            return
        if not self.idle_for_link_change():return
        if self.data_ser and self.model.state.get('data_port') not in paths:self.data_failed('数据线已拔出')
        if self.auto_fallback_pending and self.host_client:
            try:
                self.transport_action('fallback');self.auto_fallback_pending=False
                self.update(data_port=None,data_link_error=None,error=None,link_status='单线连接；数据线已拔出')
                self.model.log('数据线已拔出；空闲状态已自动切换为单线。','system')
            except Exception as e:self.update(link_status='等待数据链路租约释放');self.next_link_check=time.monotonic()+3
            return
        if not self.host_client or self.data_ser:return
        endpoint=self.host_client.limits.get('endpoint')
        if endpoint==0 and self.link_override in {'auto','dual','uart'}:
            uart=[p for p in candidates if p['label'].startswith('UART') and p['path'] not in self.link_probe_ignored]
            for candidate in uart:
                path=candidate['path']
                try:
                    identity=self.probe_idle_identity(path)
                    if identity is None:
                        self.update(link_status='UART 已枚举，等待设备响应；将自动重试',error=None)
                        self.next_link_check=time.monotonic()+5
                        continue
                    if identity!=self.auto_link_device:
                        self.link_probe_ignored.add(path)
                        continue
                    self.update(link_status='空闲状态切换到 UART 控制')
                    self.release_host_connection();self.connect(path,expected_device=self.auto_link_device);return
                except Exception as e:self.update(link_status='保持当前连接；稍后重试双线',error=None);self.next_link_check=time.monotonic()+5;return
        elif endpoint==1 and self.link_override in {'auto','dual'}:
            for candidate in candidates:
                path=candidate['path']
                if not candidate['label'].startswith('USB') or path in self.link_probe_ignored:continue
                try:
                    self.update(link_status='正在恢复双线连接');self.transport_action('bind',path)
                    self.update(link_status='双线连接',data_link_error=None,error=None);self.model.log('同一设备的 USB 数据线已自动绑定。','system');return
                except Exception as e:
                    if 'identity mismatch' in str(e):self.link_probe_ignored.add(path)
                    self.update(link_status='单线连接；等待数据线就绪',error=None);self.next_link_check=time.monotonic()+5;return
    def loop(self):
        while self.running:
            try: job=self.jobs.get_nowait()
            except queue.Empty:
                job=None
                try: self.wait_io()
                except (OSError,ValueError) as e:
                    self.connection_failed(e)
            if job:
                try:
                    action=job['action']; kw=job['kw']
                    if action=='connect': job['result']=self.connect(kw.get('port'),kw.get('mode','auto'))
                    elif action=='disconnect':
                        self.auto_link_enabled=False;self.auto_link_resume=False
                        try:
                            if self.ser and self.model.state['verified']: self.stop_capture()
                        finally:
                            try:
                                if self.host_client:self.release_host_connection()
                            finally:self.close()
                        job['result']={'ok':True}
                    elif action=='files': job['result']=self.file_job(job);self.update(error=None)
                    elif action=='transport': job['result']=self.transport_action(**kw)
                    elif action=='capture': job['result']=self.capture_action(**kw)
                    elif action=='selection': job['result']=self.select_devices(**kw)
                    elif action=='clear_logs':
                        with self.model.lock:self.model.logs.clear();self.model.log_cleared=self.model.seq;self.model.rev+=1
                        job['result']={'ok':True,'through':self.model.log_cleared}
                    elif action=='telemetry': job['result']=self.telemetry(**kw)
                    elif action=='console': job['result']=self.console(kw.get('command',''))
                    elif action=='command': job['result']=self.command(kw.get('command',''),kw.get('duration',30))
                    elif action=='clear':
                        if self.model.state['mode'] in {'scanall','sniffbeacon','scancombined'}: raise ValueError('请先停止采集。')
                        with self.model.lock:
                            self.model.aps.clear(); self.model.stations.clear(); self.model.buckets.clear(); self.model.events.clear(); self.model.state.update(completed_rounds=0,last_round_at=None); self.model.rev+=1
                        job['result']={'ok':True}
                    else: raise ValueError('未知操作')
                except Exception as e:
                    job['error']=str(e); self.update(error=str(e)); self.model.log(str(e),'error')
                finally: job['done'].set()
            try:
                self.maintain_idle_links()
                self.poll_metrics()
                if self.selection_dirty and self.host_client and time.monotonic()>=self.selection_retry:
                    try:self.sync_selection()
                    except Exception as e:
                        self.selection_dirty=False;self.model.log('Selection snapshot: '+str(e),'error')
                if self.ser:
                    self.read()
                    deadline=self.model.state['deadline']
                    if deadline and time.time()>=deadline:
                        self.stop_capture(); self.model.log('定时采集已结束','system')
                    elif deadline and self.round_until and time.time()>=self.round_until:
                        cmd=self.capture_mode
                        if cmd=='scancombined':
                            self.model.finish_round(); self.model.begin_round(cmd); self.round_until=time.time()+ROUND_SECONDS
                        else:
                            paused=time.time(); self.stop_capture(); self.start_round(cmd)
                            self.update(deadline=deadline+time.time()-paused)
            except Exception as e:
                self.connection_failed(e)

class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version='HTTP/1.1'
    def log_message(self,*args): pass
    def handle(self):
        try: super().handle()
        except (BrokenPipeError,ConnectionResetError,TimeoutError): pass
    def valid_host(self):
        return self.headers.get('Host') in {f'127.0.0.1:{self.server.server_port}',f'localhost:{self.server.server_port}'}
    def respond_headers(self,status,kind,length=None):
        self.send_response(status); self.send_header('Content-Type',kind); self.send_header('Cache-Control','no-store')
        self.send_header('X-Content-Type-Options','nosniff'); self.send_header('Referrer-Policy','no-referrer')
        self.send_header('Content-Security-Policy',"default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'")
        if length is not None: self.send_header('Content-Length',str(length))
        self.end_headers()
    def reply(self,obj,status=200):
        data=json.dumps(obj,ensure_ascii=False).encode(); self.respond_headers(status,'application/json; charset=utf-8',len(data)); self.wfile.write(data)
    def do_GET(self):
        if not self.valid_host(): return self.reply({'error':'Invalid host'},403)
        u=urllib.parse.urlparse(self.path); qs=urllib.parse.parse_qs(u.query)
        try: since=max(0,int(qs.get('since',['0'])[0]))
        except ValueError: return self.reply({'error':'Invalid cursor'},400)
        if u.path=='/api/status': return self.reply(self.server.bridge.model.snapshot(since))
        if u.path.startswith('/api/capture/stream/'):
            sink=self.server.bridge.capture_sink
            if not sink or sink.id!=u.path.rsplit('/',1)[-1]:return self.reply({'error':'Capture not found'},404)
            try:subscriber=sink.subscribe()
            except ValueError as e:return self.reply({'error':str(e)},409)
            self.close_connection=True;self.connection.settimeout(1)
            try:
                self.respond_headers(200,'application/vnd.tcpdump.pcap');self.wfile.write(sink.header());self.wfile.flush()
                while sink.running or not subscriber.empty():
                    try:record=subscriber.get(timeout=.25)
                    except queue.Empty:continue
                    self.wfile.write(record);self.wfile.flush()
            except (OSError,TimeoutError):pass
            finally:sink.unsubscribe(subscriber)
            return
        if u.path.startswith('/api/capture/download/'):
            sink=next((x for x in self.server.bridge.capture_history if x.id==u.path.rsplit('/',1)[-1]),None)
            if not sink or sink.running or sink.thread.is_alive():return self.reply({'error':'Capture not completed'},409)
            self.connection.settimeout(5);self.respond_headers(200,'application/vnd.tcpdump.pcap',sink.path.stat().st_size)
            with sink.path.open('rb') as file:
                while chunk:=file.read(65536):self.wfile.write(chunk)
            return
        if u.path=='/api/transfers': return self.reply(self.server.bridge.transfers.request('list'))
        if u.path.startswith('/api/transfers/download/'):
            try: file,size,name=self.server.bridge.transfers.download(u.path.rsplit('/',1)[-1])
            except ValueError as e: return self.reply({'error':str(e)},404)
            self.connection.settimeout(5)
            self.send_response(200);self.send_header('Content-Type','application/octet-stream')
            self.send_header('Content-Disposition',"attachment; filename*=UTF-8''"+urllib.parse.quote(name))
            self.send_header('Content-Length',str(size));self.send_header('Cache-Control','no-store');self.end_headers()
            with file:
                while chunk:=file.read(65536):self.wfile.write(chunk)
            return
        if u.path=='/api/ports': return self.reply(ports())
        if u.path=='/api/session': return self.reply({'token':self.server.token,'app':'marauder-desk'})
        if u.path=='/api/events':
            self.respond_headers(200,'text/event-stream; charset=utf-8'); self.connection.settimeout(5)
            last_rev=-1; last_send=0
            try:
                while True:
                    model=self.server.bridge.model
                    changed=model.wait_changed(last_rev,2)
                    if not changed:
                        self.wfile.write(b': session_refresh\n\n'); self.wfile.flush(); continue
                    # Coalesce bursts to <=20 updates/s per reader, never hold the model lock on network I/O.
                    pause=.05-(time.monotonic()-last_send)
                    if pause>0: time.sleep(pause)
                    data=model.snapshot(since); since=data['seq']; last_rev=data['rev']; last_send=time.monotonic()
                    self.wfile.write(('data: '+json.dumps(data,ensure_ascii=False)+'\n\n').encode()); self.wfile.flush()
            except (BrokenPipeError,ConnectionResetError,TimeoutError,OSError): pass
            return
        files={'/':'index.html','/app.js':'app.js','/completion.js':'completion.js','/presence.js':'presence.js','/style.css':'style.css','/favicon.svg':'favicon.svg'}
        if u.path not in files: return self.reply({'error':'Not found'},404)
        p=ROOT/'static'/files[u.path]; data=p.read_bytes()
        kind={'.html':'text/html','.js':'text/javascript','.css':'text/css','.svg':'image/svg+xml'}[p.suffix]
        self.respond_headers(200,kind+'; charset=utf-8',len(data)); self.wfile.write(data)
    def do_POST(self):
        expected={f'http://127.0.0.1:{self.server.server_port}',f'http://localhost:{self.server.server_port}'}
        if not self.valid_host() or self.headers.get('Origin') not in expected or not secrets.compare_digest(self.headers.get('X-Desk-Token',''),self.server.token):
            self.close_connection=True; return self.reply({'error':'Forbidden'},403)
        try:
            n=int(self.headers.get('Content-Length','0'))
            if not 0<n<=4096: raise ValueError('请求长度无效')
            data=json.loads(self.rfile.read(n)); action=self.path.removeprefix('/api/')
            if action not in {'connect','disconnect','command','console','clear','telemetry','files','transfers','transport','capture','selection','clear_logs','host_storage'}: raise ValueError('操作不存在')
            if not isinstance(data,dict): raise ValueError('请求格式无效')
            if action=='host_storage':result=self.server.bridge.host_storage(**data)
            else:result=self.server.bridge.transfers.request(**data) if action=='transfers' else self.server.bridge.call(action,**data)
            if action=='files' and isinstance(result.get('raw'),bytes):
                raw=result['raw'];self.respond_headers(200,'application/octet-stream',len(raw));self.wfile.write(raw);return
            self.reply(result)
        except (ValueError,RuntimeError,TimeoutError,OSError,queue.Full) as e: self.reply({'error':str(e)},400)

class Server(http.server.ThreadingHTTPServer):
    daemon_threads=True
    def server_bind(self):
        socketserver.TCPServer.server_bind(self)
        self.server_name="localhost"; self.server_port=self.server_address[1]

def main():
    parser=argparse.ArgumentParser(); parser.add_argument('--port',type=int,default=8765); parser.add_argument('--no-browser',action='store_true'); parser.add_argument('--temp-dir',help='PCAP 和文件下载临时目录的父目录（支持 ~）'); args=parser.parse_args()
    server=Server(('127.0.0.1',args.port),Handler); server.token=secrets.token_urlsafe(32); server.bridge=Bridge(temp_directory=args.temp_dir)
    url=f'http://127.0.0.1:{args.port}'
    print('Marauder Desk · '+url+'\n保持此窗口开启。按 Control+C 退出。',flush=True)
    if not args.no_browser: webbrowser.open(url)
    def stop(*_): raise KeyboardInterrupt
    signal.signal(signal.SIGTERM,stop)
    try: server.serve_forever()
    except KeyboardInterrupt: pass
    finally:
        try: server.bridge.call('disconnect')
        except Exception: pass
        server.bridge.transfers.close()
        server.bridge.running=False; server.bridge.host_wake();server.bridge.thread.join(2); server.bridge.close(); server.server_close()

if __name__=='__main__': main()
