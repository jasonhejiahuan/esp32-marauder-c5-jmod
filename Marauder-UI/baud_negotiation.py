"""Session-only UART speed selection after a verified Host_Connection HELLO."""
import statistics,struct,time
from host_connection import Op,Channel,Reader,Decoder

CANDIDATES=(2000000,1500000,921600,460800)
def negotiate(bridge):
    client=bridge.host_client
    if client.limits.get('endpoint')!=1 or Op.BAUD_PREPARE not in client.limits.get('operations',[]):return
    results=[]
    def probe(count):
        times=[];errors=client.decoder.errors
        for i in range(count):
            payload=bytes((j+i*37)%256 for j in range(1024));started=time.monotonic()
            received=bridge.host_wait(client.request(Op.BAUD_PROBE,payload,Channel.CONTROL,timeout=.8),2)
            if received!=payload:raise ValueError('UART probe payload mismatch')
            times.append((time.monotonic()-started)*1000)
        if client.decoder.errors!=errors:raise ValueError('UART probe CRC errors')
        return statistics.median(times),max(times)
    try:baseline,_=probe(2)
    except Exception as error:
        # Negotiation is optional; an occupied radio must not prevent CLI access.
        bridge.update(uart_baud=115200,uart_baud_trials=[dict(baud=115200,ok=False,error=str(error))])
        bridge.model.log('UART 保持默认 115200；速率基线未通过：'+str(error),'system')
        return
    for rate in CANDIDATES:
        trial_started=time.monotonic();commit_started=None
        try:
            reply=bridge.host_wait(client.request(Op.BAUD_PREPARE,struct.pack('<I',rate),Channel.CONTROL,mutation=True),2)
            r=Reader(reply);accepted,delay,trial=r.unpack('IHH');r.end()
            if accepted!=rate or not 100<=delay<=500 or not 1500<=trial<=5000:raise ValueError('Invalid baud trial parameters')
            trial_started=time.monotonic();time.sleep((delay+30)/1000)
            bridge.ser.baudrate=rate;client.decoder=Decoder()
            median,worst=probe(8)
            if median>=baseline*.9 or worst>baseline:raise ValueError('Baud trial has no reliable latency improvement')
            commit_started=time.monotonic()
            reply=bridge.host_wait(client.request(Op.BAUD_COMMIT,struct.pack('<I',rate),Channel.CONTROL,mutation=True),2)
            if reply!=struct.pack('<I',rate):raise ValueError('Baud confirmation mismatch')
            results.append(dict(baud=rate,ok=True,median_ms=median,max_ms=worst))
            bridge.update(uart_baud=rate,uart_baud_trials=results,uart_baseline_ms=baseline)
            bridge.model.log(f'UART 专有协议速率 {rate} baud；1024 字节往返中位数 {median:.1f} ms（115200 基线 {baseline:.1f} ms）。','system')
            return
        except Exception as error:
            results.append(dict(baud=rate,ok=False,error=str(error)))
            # Unconfirmed trials revert in firmware. A possibly lost COMMIT ack
            # requires the full lease boundary before returning to default speed.
            delay_until=commit_started+13 if commit_started is not None else trial_started+6
            # Do not service the connection while waiting: that would renew its lease.
            while time.monotonic()<delay_until:time.sleep(min(.05,delay_until-time.monotonic()))
            bridge.ser.baudrate=115200;bridge.ser.reset_input_buffer();client.decoder=Decoder();client.last_tx=time.monotonic()
            if commit_started is not None:
                bridge.update(uart_baud=115200,uart_baud_trials=results)
                raise RuntimeError('波特率确认丢失；已等待会话到期并恢复默认速率，请重新连接。') from error
            try:bridge.host_wait(client.request(Op.PING,channel=Channel.CONTROL,timeout=1),2)
            except Exception as fallback_error:
                bridge.update(uart_baud=115200,uart_baud_trials=results)
                raise RuntimeError('波特率回退待确认；关闭连接后等待设备租约恢复默认速率') from fallback_error
    bridge.update(uart_baud=115200,uart_baud_trials=results,uart_baseline_ms=baseline)
    bridge.model.log('UART 保持默认 115200；较高速率未通过稳定性验证。','system')
