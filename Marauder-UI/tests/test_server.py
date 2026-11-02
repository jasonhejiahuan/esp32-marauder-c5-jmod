import sys, unittest, threading, json, http.client, secrets, time
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from server import Model,Bridge,Server,Handler

class ParserTests(unittest.TestCase):
    def test_manual_scanap_capability_badge_data(self):
        m=Model();m.state['mode']='console';m.log('#scanap');m.log('-60 Ch: 6 02:00:00:00:00:02 ESSID: Example Network 31 04')
        self.assertEqual(m.aps['02:00:00:00:00:02']['ssid'],'Example Network')
        self.assertEqual(m.aps['02:00:00:00:00:02']['capabilities'],'31 04')
        m.log('#sniffbeacon');m.log('-60 Ch: 6 02:00:00:00:00:02 ESSID: Real SSID 31 04')
        self.assertEqual(m.aps['02:00:00:00:00:02']['ssid'],'Real SSID 31 04')

    def test_beacon_names_are_not_truncated(self):
        m=Model();m.state['mode']='sniffbeacon'
        m.log('-63 Ch: 149 aa:bb:cc:dd:ee:ff ESSID: Office 31 14')
        self.assertEqual(m.aps['aa:bb:cc:dd:ee:ff']['ssid'],'Office 31 14')
        self.assertEqual(m.aps['aa:bb:cc:dd:ee:ff']['channel'],149)
    def test_scanall_capability_suffix(self):
        m=Model();m.state['mode']='scanall'
        m.log('> -70 Ch: 36 AA:BB:CC:DD:EE:FF ESSID: Office 31 14 ')
        self.assertEqual(m.aps['aa:bb:cc:dd:ee:ff']['ssid'],'Office')
    def test_dedup_and_history(self):
        m=Model()
        for r in [-80,-65,-60]:m.log(f'{r} Ch: 6 aa:bb:cc:dd:ee:ff ESSID: 同一网络')
        self.assertEqual(len(m.aps),1);self.assertEqual(next(iter(m.aps.values()))['samples'],3)
        self.assertEqual(next(iter(m.aps.values()))['rssi'],-60)
        self.assertEqual(len(m.events),1)
    def test_invalid_and_hidden(self):
        m=Model();m.log('-50 Ch: 1 aa:bb:cc:dd:ee:ff ESSID:')
        self.assertEqual(next(iter(m.aps.values()))['ssid'],'隐藏网络')
        m.log('900 Ch: 1 11:22:33:44:55:66 ESSID: bad');self.assertEqual(len(m.aps),1)
    def test_association_both_directions(self):
        m=Model();m.log('ap: aa:bb:cc:dd:ee:ff -> sta: 11:22:33:44:55:66');m.log('sta: 11:22:33:44:55:66 -> ap: aa:bb:cc:dd:ee:ff')
        self.assertEqual(len(m.stations),1);self.assertEqual(m.stations['11:22:33:44:55:66']['ap'],'aa:bb:cc:dd:ee:ff')
    def test_bounded_snapshot_and_cursor(self):
        m=Model()
        for i in range(1200):m.log(str(i))
        self.assertEqual(len(m.snapshot()['logs']),1000);self.assertEqual(len(m.snapshot(1199)['logs']),1)
        s=m.snapshot();s['state']['connected']=True;self.assertFalse(m.state['connected'])
    def test_restart_invalidates_connection_verification(self):
        m=Model();m.state.update(verified=True,mode='sniffbeacon',deadline=time.time()+10)
        m.log('ESP-ROM:esp32c5-eco3-20250704')
        self.assertFalse(m.state['verified']);self.assertIsNone(m.state['deadline']);self.assertEqual(m.state['mode'],'restarted')
    def test_ap_details_commit_on_complete_security_line(self):
        m=Model();m.log('-62 Ch: 36 aa:bb:cc:dd:ee:ff ESSID: lab')
        before=m.aps['aa:bb:cc:dd:ee:ff']['last']
        for line in ['#info -a 0','   ESSID: lab','   BSSID: aa:bb:cc:dd:ee:ff',' Channel: 36','    RSSI: -62','  Frames: 3','Stations: 2','   Brand: Acme','Complete EAPOL: FALSE']:
            m.log(line)
        self.assertNotIn('security',m.aps['aa:bb:cc:dd:ee:ff'])
        m.log('Security: WPA2');a=m.aps['aa:bb:cc:dd:ee:ff']
        self.assertEqual(a['security'],'WPA2');self.assertEqual(a['station_count'],2);self.assertEqual(a['last'],before)
        m.log('-70 Ch: 36 aa:bb:cc:dd:ee:ff ESSID: lab')
        self.assertEqual(m.aps['aa:bb:cc:dd:ee:ff']['security'],'WPA2')
    def test_details_never_attach_by_unverified_index(self):
        m=Model();m.log('-62 Ch: 36 aa:bb:cc:dd:ee:ff ESSID: duplicate')
        for line in ['#info -a 0','BSSID: 00:11:22:33:44:55','Security: WPA3']:m.log(line)
        self.assertNotIn('security',m.aps['aa:bb:cc:dd:ee:ff'])
    def test_real_protocol_and_psram(self):
        m=Model();m.log('@MARAUDER:{"firmware":"v1.18.0","status":"success"}')
        m.log('USB CLI build: v1.18.0 / ESP32-C5 DevKit; PSRAM total=8388608 free=8386296 bytes')
        self.assertEqual(m.state['firmware'],'v1.18.0');self.assertEqual(m.state['psram']['total'],8388608)

class PresenceTests(unittest.TestCase):
    def model_with_ap(self):
        m=Model();m.log('-60 Ch: 36 aa:bb:cc:dd:ee:ff ESSID: sample');return m
    def test_two_complete_misses_and_return(self):
        m=self.model_with_ap();a=m.aps['aa:bb:cc:dd:ee:ff']
        m.begin_round('scanall',100);m.finish_round(130);self.assertTrue(a['online'])
        m.begin_round('scanall',130)
        self.assertTrue(a['online'])  # Still on screen while round is incomplete.
        m.finish_round(160);self.assertFalse(a['online'])
        m.begin_round('scanall',160);m.log('-70 Ch: 36 aa:bb:cc:dd:ee:ff ESSID: sample')
        self.assertTrue(m.aps['aa:bb:cc:dd:ee:ff']['online']);self.assertEqual(m.aps['aa:bb:cc:dd:ee:ff']['missed_rounds'],0)
    def test_short_or_interrupted_scans_never_expire(self):
        m=self.model_with_ap()
        for i in range(3):
            m.begin_round('scanall',100);m.finish_round(115)
            m.begin_round('scanall',100);m.finish_round(140,aborted=True)
        self.assertTrue(next(iter(m.aps.values()))['online']);self.assertEqual(m.state['completed_rounds'],0)
    def test_beacon_does_not_expire_other_channels_or_stations(self):
        m=self.model_with_ap();m.log('sta: aa:00:00:00:00:01 -> ap: aa:bb:cc:dd:ee:ff')
        for i in range(2):
            m.begin_round('sniffbeacon',100);m.log('-50 Ch: 1 00:11:22:33:44:55 ESSID: other');m.finish_round(130)
        self.assertTrue(m.aps['aa:bb:cc:dd:ee:ff']['online']);self.assertTrue(next(iter(m.stations.values()))['online'])
    def test_seen_each_round_is_not_marked_offline(self):
        m=self.model_with_ap()
        for i in range(3):
            m.begin_round('scanall',100);m.log('-60 Ch: 36 aa:bb:cc:dd:ee:ff ESSID: sample 31 14');m.finish_round(130)
        self.assertTrue(m.aps['aa:bb:cc:dd:ee:ff']['online']);self.assertEqual(m.aps['aa:bb:cc:dd:ee:ff']['missed_rounds'],0)
    def test_snapshot_changes_presence_atomically(self):
        m=self.model_with_ap();m.begin_round('scanall',100);m.finish_round(130);m.begin_round('scanall',130)
        before=m.snapshot();m.finish_round(160);after=m.snapshot()
        self.assertTrue(before['aps'][0]['online']);self.assertFalse(after['aps'][0]['online'])

class FakeSerial:
    def __init__(self):self.buffer=b'';self.commands=[];self.closed=False
    @property
    def in_waiting(self):return len(self.buffer)
    def read(self,n):
        data,self.buffer=self.buffer[:n],self.buffer[n:]
        if not data:time.sleep(.001)
        return data
    def write(self,data):
        cmd=data.decode().strip();self.commands.append(cmd)
        text={'stopscan':'Stopping WiFi tran/recv','sniffbeacon':'StartingBeacon sniff. Stop with stopscan','scanall':'Scanning for APs and Stations. Stop with stopscan','clearlist -a':'#clearlist -a','protocolinfo --machine fresh':'@MARAUDER:{"tx":"fresh","status":"success"}'}.get(cmd,'')
        self.buffer+=(text+'\r\n').encode();return len(data)
    def close(self):self.closed=True

class BridgeTests(unittest.TestCase):
    def make(self):
        b=Bridge();b.ser=FakeSerial();b.update(verified=True,connected=True,mode='idle');self.addCleanup(self.finish,b);return b
    def finish(self,b):b.running=False;b.thread.join(1);b.close()
    def test_start_switch_stop_and_deadline(self):
        b=self.make();b.call('command',command='sniffbeacon',duration=5)
        self.assertEqual(b.ser.commands,['stopscan','sniffbeacon']);self.assertEqual(b.model.state['mode'],'sniffbeacon')
        b.update(deadline=time.time()-.1)
        end=time.monotonic()+1
        while b.model.state['mode']!='idle' and time.monotonic()<end:time.sleep(.01)
        self.assertEqual(b.model.state['mode'],'idle');self.assertEqual(b.ser.commands[-1],'stopscan')
    def test_scanall_refreshes_firmware_ram_list(self):
        b=self.make();b.call('command',command='scanall',duration=30)
        self.assertEqual(b.ser.commands,['stopscan','clearlist -a','protocolinfo --machine fresh','scanall'])
    def test_queries_do_not_interrupt_active_capture(self):
        b=self.make();b.update(mode='sniffbeacon')
        with self.assertRaises(RuntimeError):b.call('command',command='info')
        self.assertEqual(b.ser.commands,[])
    def test_newline_injection_rejected(self):
        b=self.make()
        with self.assertRaises(RuntimeError):b.call('command',command='info\nreboot')
        self.assertEqual(b.ser.commands,[])
    def test_disconnect_releases_port(self):
        b=self.make();s=b.ser;b.call('disconnect');self.assertTrue(s.closed);self.assertIsNone(b.ser)
    def test_bounded_fragment_read(self):
        b=self.make();b.ser.buffer='-61 Ch: 36 aa:bb:cc:dd:ee:ff ESSID: 中文\n'.encode();b.read()
        self.assertEqual(next(iter(b.model.aps.values()))['ssid'],'中文')

class HttpTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server=Server(('127.0.0.1',0),Handler);cls.server.token=secrets.token_urlsafe(16);cls.server.bridge=Bridge()
        cls.t=threading.Thread(target=cls.server.serve_forever,daemon=True);cls.t.start()
    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown();cls.server.server_close();cls.server.bridge.running=False;cls.server.bridge.thread.join(1)
    def request(self,path,method='GET',body=None,headers=None):
        c=http.client.HTTPConnection('127.0.0.1',self.server.server_port,timeout=2)
        c.request(method,path,body,headers or {});r=c.getresponse();result=(r.status,r.read(),dict(r.getheaders()));c.close();return result
    def test_static_and_status(self):
        code,body,headers=self.request('/');self.assertEqual(code,200);self.assertIn(b'Marauder Desk',body);self.assertIn('Content-Security-Policy',headers)
        self.assertEqual(self.request('/api/status')[0],200)
    def test_csrf_and_host(self):
        self.assertEqual(self.request('/api/clear','POST','{}',{'Origin':'https://evil.example','X-Desk-Token':self.server.token})[0],403)
        self.assertEqual(self.request('/api/status',headers={'Host':'evil.example'})[0],403)
        self.assertEqual(self.request('/api/clear','POST','{}')[0],403)
    def test_authorized_clear(self):
        self.assertEqual(self.request('/api/clear','POST','{}',{'Origin':f'http://127.0.0.1:{self.server.server_port}','X-Desk-Token':self.server.token})[0],200)
    def test_no_file_access(self):
        self.assertEqual(self.request('/../server.py')[0],404);self.assertEqual(self.request('/server.py')[0],404)
    def test_sse_initial_state(self):
        c=http.client.HTTPConnection('127.0.0.1',self.server.server_port,timeout=2);c.request('GET','/api/events');r=c.getresponse()
        self.assertEqual(r.status,200);line=r.readline();self.assertTrue(line.startswith(b'data: '));self.assertIn('state',json.loads(line[6:]));c.close()

if __name__=='__main__':unittest.main(verbosity=2)
