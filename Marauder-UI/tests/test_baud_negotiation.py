import sys,struct,unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import baud_negotiation as baud
from host_connection import Op

class BaudTests(unittest.TestCase):
    def run_failure(self,failed_op):
        clock=[0.0];calls=[];states=[]
        def request(op,payload=b'',*args,**kwargs):
            calls.append((op,clock[0]))
            return op,payload
        client=SimpleNamespace(limits={'endpoint':1,'operations':[Op.BAUD_PREPARE]},decoder=SimpleNamespace(errors=0),request=request)
        def wait(future,timeout):
            op,payload=future
            if op==failed_op:raise TimeoutError('lost reply')
            if op==Op.BAUD_PROBE:
                clock[0]+=.2 if len(calls)<=2 else .02
                return payload
            if op==Op.BAUD_PREPARE:return struct.pack('<IHH',2000000,150,3000)
            return payload
        bridge=SimpleNamespace(host_client=client,host_wait=wait,ser=SimpleNamespace(baudrate=115200,reset_input_buffer=lambda:None),update=lambda **s:states.append(s),model=SimpleNamespace(log=lambda *a:None))
        def sleep(n):clock[0]+=n
        with patch.object(baud,'CANDIDATES',(2000000,)),patch.object(baud.time,'monotonic',lambda:clock[0]),patch.object(baud.time,'sleep',sleep):
            if failed_op==Op.BAUD_COMMIT:
                with self.assertRaisesRegex(RuntimeError,'重新连接'):baud.negotiate(bridge)
            else:baud.negotiate(bridge)
        return calls,clock[0],bridge
    def test_prepare_reply_loss_waits_for_trial_before_ping(self):
        calls,_,bridge=self.run_failure(Op.BAUD_PREPARE)
        start=next(t for op,t in calls if op==Op.BAUD_PREPARE)
        ping=next(t for op,t in calls if op==Op.PING)
        self.assertGreaterEqual(ping-start,6)
        self.assertEqual(bridge.ser.baudrate,115200)
    def test_commit_reply_loss_waits_for_lease_and_requires_new_session(self):
        calls,end,bridge=self.run_failure(Op.BAUD_COMMIT)
        commit=next(t for op,t in calls if op==Op.BAUD_COMMIT)
        self.assertGreaterEqual(end-commit,13)
        self.assertFalse(any(op==Op.PING for op,_ in calls))
        self.assertEqual(bridge.ser.baudrate,115200)
    def test_busy_baseline_keeps_cli_session(self):
        calls,_,bridge=self.run_failure(Op.BAUD_PROBE)
        self.assertEqual(bridge.ser.baudrate,115200)
        self.assertFalse(any(op==Op.BAUD_PREPARE for op,_ in calls))
    def test_native_usb_skips_baud(self):
        baud.negotiate(SimpleNamespace(host_client=SimpleNamespace(limits={'endpoint':0})))

if __name__=='__main__':unittest.main()
