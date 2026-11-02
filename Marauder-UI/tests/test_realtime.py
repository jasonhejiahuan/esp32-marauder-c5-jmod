"""Broadcast subscriptions and single-owner command queue regressions."""
import concurrent.futures, http.client, json, secrets, threading, time, unittest
from unittest.mock import patch
from server import Bridge, Model, Server, Handler
from test_server import FakeSerial

class RealtimeTests(unittest.TestCase):
    def test_revision_broadcast_and_no_lost_wakeup(self):
        m=Model(); barrier=threading.Barrier(5)
        def wait():
            barrier.wait(); return m.wait_changed(0,.5)
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as ex:
            futures=[ex.submit(wait) for _ in range(4)]
            barrier.wait(); m.log('one change')
            self.assertTrue(all(f.result() for f in futures))
        self.assertTrue(m.wait_changed(0,.01))
        self.assertFalse(m.wait_changed(m.rev,.01))

    def test_writes_are_owned_by_one_thread(self):
        class TrackingSerial(FakeSerial):
            def __init__(self):super().__init__();self.owners=[]
            def write(self,data):
                self.owners.append(threading.get_ident());time.sleep(.003)
                return super().write(data)
        b=Bridge(); b.ser=TrackingSerial(); b.update(verified=True,connected=True,mode='idle')
        try:
            with concurrent.futures.ThreadPoolExecutor(max_workers=8) as ex:
                results=list(ex.map(lambda i:b.call('console',command=f'protocolinfo --machine test{i}'),range(16)))
            self.assertEqual(len(b.ser.commands),16)
            self.assertEqual(set(b.ser.owners),{b.thread.ident})
            self.assertEqual(len(set(b.ser.commands)),16)
            self.assertTrue(all(r['ok'] for r in results))
        finally:b.running=False;b.thread.join(1);b.close()

    def test_unplug_during_select_does_not_kill_worker(self):
        b=Bridge()
        try:
            with patch.object(b,'wait_io',side_effect=OSError('unplug')):
                # A job must still be serviced after an idle I/O failure.
                b.call('clear')
                self.assertTrue(b.thread.is_alive())
        finally:b.running=False;b.thread.join(1);b.close()

    def test_multiple_sse_readers_never_query_serial(self):
        srv=Server(('127.0.0.1',0),Handler);srv.token=secrets.token_urlsafe(16);srv.bridge=Bridge()
        srv.bridge.ser=FakeSerial();srv.bridge.update(verified=True,connected=True,mode='idle')
        thread=threading.Thread(target=srv.serve_forever,daemon=True);thread.start();connections=[]
        try:
            streams=[]
            for _ in range(3):
                c=http.client.HTTPConnection('127.0.0.1',srv.server_port,timeout=2);connections.append(c)
                c.request('GET','/api/events');r=c.getresponse();self.assertEqual(r.status,200)
                self.assertTrue(r.readline().startswith(b'data: '));r.readline();streams.append(r)
            srv.bridge.model.log('shared update')
            for r in streams[:2]:
                line=r.readline();s=json.loads(line[6:]);r.readline()
                self.assertIn('shared update',[x['text'] for x in s['logs']])
            # Third client deliberately does not read; another client and the model remain available.
            srv.bridge.model.log('next update')
            c=http.client.HTTPConnection('127.0.0.1',srv.server_port,timeout=2);connections.append(c)
            c.request('GET','/api/status');r=c.getresponse();self.assertEqual(r.status,200);r.read()
            self.assertEqual(srv.bridge.ser.commands,[])
        finally:
            for c in connections:c.close()
            srv.shutdown();srv.server_close();srv.bridge.running=False;srv.bridge.thread.join(1);srv.bridge.close()
