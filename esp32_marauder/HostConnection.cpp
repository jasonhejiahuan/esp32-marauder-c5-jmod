#include "HostConnection.h"
#ifdef HOST_CONNECTION_ENABLED
#include "CommandLine.h"
#include "SelectionService.h"
#include "Buffer.h"
#include "NativeSniffRequest.h"
extern Buffer buffer_obj;
#include "StorageService.h"
#include "StorageBackend.h"
#include "DeviceObservability.h"
#include "UsbOta.h"
#include "LedInterface.h"
#include <esp_mac.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <mbedtls/sha256.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <atomic>
#include <new>
#include "hal/usb_serial_jtag_ll.h"
extern CommandLine cli_obj;
extern LedInterface led_obj;
namespace host_connection {
namespace {
template<class T> T* externalObject(){void* memory=ps_malloc(sizeof(T));return memory?new(memory) T{}:nullptr;}
constexpr uint32_t LEASE_MS=12000,HANDSHAKE_MS=2500,WINDOW=8192;
constexpr unsigned CAP_SLOTS=32,SNAPLEN=2324;
#if defined(MARAUDER_XIAO_C5)
constexpr unsigned ACTIVE_ENDPOINTS=1;
constexpr uint32_t SUPPORTED_FEATURES=15; // files, capture, telemetry, CLI; one native USB link
constexpr const char* ENDPOINT_ROLES="\"control\"";
#else
constexpr unsigned ACTIVE_ENDPOINTS=2;
constexpr uint32_t SUPPORTED_FEATURES=31;
constexpr const char* ENDPOINT_ROLES="\"control\",\"data\"";
#endif
enum Mode:uint8_t { CLI,UPGRADE_PENDING,BINARY_ACTIVE,RECOVERY };
struct EndpointBuffers {char line[2049]={};Decoder decoder;uint8_t wire[MAX_ENCODED+1],raw[MAX_RAW];};
struct Endpoint {
 std::atomic<Mode> mode{CLI};bool data=false,closing=false;uint32_t session=0,lastRx=0,rxSeq=0,txSeq=0;
 char nonce[33]={};EndpointBuffers* buffers=nullptr;size_t lineLen=0;bool lineBad=false;uint32_t features=SUPPORTED_FEATURES,fileWindow=WINDOW;
 QueueHandle_t urgent=nullptr,normal=nullptr,bulk=nullptr;
 std::atomic<size_t> wireLen{0};size_t wireOffset=0;uint8_t wireChannel=CONSOLE;
 uint32_t receivedBytes=0,submittedBytes=0;uint8_t recoveryReason=0;
 uint32_t partialWrites=0,frames=0,errors=0,drops=0,transitions=0,wireStarted=0,lastRecoveryNotice=0;
};
// STATUS v1 retains two wire-format slots; XIAO leaves slot 1 inactive.
Endpoint ep[2];TaskHandle_t transportTask=nullptr,serviceTask=nullptr;SemaphoreHandle_t stateLock=nullptr;
struct Work {uint8_t endpoint;bool text;Frame frame;};
Frame* rxFrame=nullptr;Frame* txFrame=nullptr;Work* executingWork=nullptr;
QueueHandle_t urgentRequests=nullptr;QueueHandle_t requests=nullptr;std::atomic<bool> ready{false};
std::atomic<int> control{-1},dataLink{-1};std::atomic<uint32_t> session{0};uint32_t epoch=1;std::atomic<bool> dual{false};
std::atomic<bool> dataFailed{false},negotiationFeedback{false};
#if !defined(MARAUDER_XIAO_C5)
std::atomic<uint32_t> uartBaud{115200},pendingBaud{0},baudSwitchAt{0},baudTrialUntil{0};
#endif
uint8_t bindToken[16]={};uint32_t tokenDeadline=0;bool tokenValid=false;
char identity[18];uint32_t currentRequest=0;int currentEndpoint=0;
SemaphoreHandle_t cacheLock=nullptr;
uint32_t outputDrops=0,requestDrops=0,queueAgeMaxUs=0;std::atomic<uint32_t> callbackMaxUs{0},queueHigh{0};
struct TextBuffer {uint8_t data[512];size_t used=0;};TextBuffer responseText,asyncText;
SemaphoreHandle_t textLock=nullptr;
struct CaptureSlot {uint32_t stream,sequence;uint64_t timestamp;uint16_t original,captured;int8_t rssi;uint8_t channel;uint8_t data[SNAPLEN];};
struct LegacySlot {uint64_t receivedUs;wifi_promiscuous_cb_t callback;wifi_promiscuous_pkt_type_t type;alignas(4) uint8_t packet[sizeof(wifi_pkt_rx_ctrl_t)+SNAPLEN+4];};
LegacySlot* legacySlots=nullptr;QueueHandle_t legacyFree=nullptr;
CaptureSlot* captureSlots=nullptr;QueueHandle_t captureFree=nullptr,captureReady=nullptr,legacyReady=nullptr;
std::atomic<bool> nativeCaptureFilter{false};
LegacySlot* processingSlot=nullptr;bool processingCaptured=false;
std::atomic<bool> captureEnabled{false};std::atomic<uint32_t> captureSequence{0},captureDrops{0};
std::atomic<bool> captureEnding{false};std::atomic<unsigned> captureInFlight{0};
std::atomic<uint32_t> callbackCount{0},invalidRx{0},invalidLength{0};
std::atomic<uint32_t> legacyDrops{0},callbackHistogram[8]{};
struct CallbackTiming {
 uint64_t started=esp_timer_get_time();
 ~CallbackTiming(){uint32_t us=esp_timer_get_time()-started;callbackMaxUs=std::max(callbackMaxUs.load(),us);const uint32_t limits[]={2,5,10,20,50,100,500};unsigned bucket=0;while(bucket<7&&us>limits[bucket])++bucket;++callbackHistogram[bucket];}
};
uint32_t usbSofMissingSamples=0;
uint32_t streamId=0,captureSent=0;wifi_promiscuous_cb_t legacyCallback=nullptr;
struct FileTransfer {File file;bool active=false;uint32_t id=0,size=0,offset=0,acked=0,credit=0,generation=0,lastCredit=0;mbedtls_sha256_context sha;};
FileTransfer transfer;
struct Cached {uint32_t id=0,hash=0;Frame result;};Cached* cached=nullptr;unsigned cacheNext=0;RequestWindow requestWindow;
struct RawInput {char* data=nullptr;size_t limit=0,used=0;unsigned endpoint=0;uint32_t at=0;bool overflow=false;} rawInput;
StaticQueue_t queueObjects[8];unsigned queueObjectIndex=0;
QueueHandle_t psQueue(unsigned count,unsigned size){auto p=static_cast<uint8_t*>(heap_caps_malloc(count*size,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));if(!p)return nullptr;return xQueueCreateStatic(count,size,p,&queueObjects[queueObjectIndex++]);}
#if defined(MARAUDER_XIAO_C5)
int available(unsigned){return Serial.available();}
int readByte(unsigned){return Serial.read();}
int writeSpace(unsigned){return Serial.availableForWrite();}
size_t writeBytes(unsigned,const uint8_t* p,size_t n){return Serial.write(p,n);}
#else
int available(unsigned e){return e?Serial0.available():Serial.available();}
int readByte(unsigned e){return e?Serial0.read():Serial.read();}
int writeSpace(unsigned e){return e?Serial0.availableForWrite():Serial.availableForWrite();}
size_t writeBytes(unsigned e,const uint8_t* p,size_t n){return e?Serial0.write(p,n):Serial.write(p,n);}
#endif
void notifyTransport(){if(transportTask)xTaskNotifyGive(transportTask);}
void notifyService(){if(serviceTask)xTaskNotifyGive(serviceTask);}
Frame make(uint8_t channel,uint8_t kind,uint16_t op,uint32_t request=0){Frame f;f.channel=channel;f.kind=kind;f.op=op;f.request=request;f.session=session;return f;}
bool enqueue(unsigned e,Frame& f,bool urgent=false,bool bulk=false,TickType_t wait=0){
 if(e>=ACTIVE_ENDPOINTS)return false;f.session=ep[e].session;
 bool ok=xQueueSend(urgent?ep[e].urgent:bulk?ep[e].bulk:ep[e].normal,&f,wait)==pdTRUE;
 if(!ok)++ep[e].drops;else notifyTransport();return ok;
}
void textFlush(TextBuffer& b,int endpoint,uint32_t req){
 if(!b.used)return;Frame f=make(CONSOLE,EVENT,CONSOLE_TEXT,req);f.length=b.used;memcpy(f.payload,b.data,b.used);
 // Only executor output may wait, never asynchronous/callback output.
 if(!enqueue(endpoint,f,false,false,0))++outputDrops;b.used=0;
}
void flushResponse(){xSemaphoreTake(textLock,portMAX_DELAY);textFlush(responseText,currentEndpoint,currentRequest);xSemaphoreGive(textLock);}
void error(unsigned e,const Frame& q,const char* message){Frame f=make(q.channel,ERROR,q.op,q.request);Writer w(f.payload,sizeof(f.payload));w.str(message);f.length=w.n;enqueue(e,f,true);}
void result(unsigned e,const Frame& q,Frame& r,bool cache=true,bool urgent=true){
 r.channel=q.channel;r.op=q.op;r.kind=RESULT;r.request=q.request;r.session=session;
 if(cache&&q.request){xSemaphoreTake(cacheLock,portMAX_DELAY);auto& c=cached[cacheNext++%16];c.id=q.request;c.hash=crc32(q.payload,q.length)^q.op;c.result=r;xSemaphoreGive(cacheLock);}
 if(!enqueue(e,r,urgent))ep[e].mode=RECOVERY;
}
String readString(Reader& r,size_t max){unsigned n=r.u16();if(n>max){r.ok=false;return String();}auto p=r.take(n);if(!p)return String();for(unsigned i=0;i<n;i++)if(!p[i]){r.ok=false;return String();}return String(reinterpret_cast<const char*>(p),n);}
void clearCapture(){captureEnabled=false;uint16_t i;while(xQueueReceive(captureReady,&i,0)==pdTRUE){++captureDrops;xQueueSend(captureFree,&i,0);}}
void closeFile(){if(transfer.active){storage_service::closeStream(transfer.file);mbedtls_sha256_free(&transfer.sha);transfer.active=false;}transfer.credit=0;}
void recover(unsigned e,uint8_t reason=0){
#if !defined(MARAUDER_XIAO_C5)
 if(e==1){pendingBaud=115200;baudSwitchAt=millis()+10;baudTrialUntil=0;}
#endif
 auto& p=ep[e];p.recoveryReason=reason;p.mode=RECOVERY;p.lineLen=0;p.lineBad=false;p.buffers->decoder.clear();p.closing=false;++p.transitions;
 xQueueReset(p.normal);xQueueReset(p.bulk);xQueueReset(p.urgent);
 // Finish any already-started frame; never interleave/truncate its tail on a live link.
 if(int(e)==control){captureEnding=false;clearCapture();tokenValid=false;dual=false;dataLink=-1;notifyService();if(ep[1-e].data)ep[1-e].mode=RECOVERY;}
 else if(int(e)==dataLink){
  // An acknowledged ROUTE(0) has already detached data traffic. Expiry of the
  // old data lease must not fail or stop a newly started single-link stream.
  bool wasDual=dual.exchange(false);dataLink=-1;
  if(wasDual){captureEnding=false;clearCapture();dataFailed=true;++epoch;notifyService();}
 }
}
void rawLine(unsigned e,const char* s){Frame f=make(CONSOLE,EVENT,CONSOLE_TEXT);size_t n=strlen(s);f.length=std::min(n,sizeof(f.payload));memcpy(f.payload,s,f.length);enqueue(e,f,false);}
void upgrade(unsigned e,const char* line){
 unsigned version=0;char nonce[33],tail;bool binding=false;uint32_t wanted=0;char token[33];
 int n=sscanf(line,"host_connection upgrade %u %32s %c",&version,nonce,&tail);
 if(n!=2||version!=1){rawLine(e,"Host_Connection: upgrade 1 <32-hex-nonce>\r\n> ");return;}
 if(strlen(nonce)!=32){rawLine(e,"Host_Connection: invalid nonce\r\n> ");return;}
 for(char c:nonce){if(!c)break;if(!isxdigit(uint8_t(c))){rawLine(e,"Host_Connection: invalid nonce\r\n> ");return;}}
 auto& p=ep[e];if(control>=0&&ep[control].mode==BINARY_ACTIVE){
  // The other endpoint may negotiate only a data binding, never a second control owner.
  if(int(e)==control||e!=0||control!=1){rawLine(e,"Host_Connection: endpoint owned\r\n> ");return;}
  binding=true;
 }else{session=esp_random();if(!session)session=1;control=e;dataLink=-1;dual=false;requestWindow={};xSemaphoreTake(cacheLock,portMAX_DELAY);memset(cached,0,16*sizeof(Cached));xSemaphoreGive(cacheLock);}
 p.session=session;p.data=binding;strcpy(p.nonce,nonce);p.rxSeq=p.txSeq=0;p.lastRx=millis();p.mode=UPGRADE_PENDING;++p.transitions;
 xQueueReset(p.normal);xQueueReset(p.bulk);xQueueReset(p.urgent);
 // Existing partial plaintext must drain before READY. Pending async text is discarded.
 xSemaphoreTake(textLock,portMAX_DELAY);responseText.used=asyncText.used=0;xSemaphoreGive(textLock);
 char lineOut[120];snprintf(lineOut,sizeof(lineOut),"\n@HOST_CONNECTION READY %s %lu\n",nonce,(unsigned long)session);rawLine(e,lineOut);
}
void receiveText(unsigned e,uint8_t c){
 auto& p=ep[e];if(c=='\r')return;
 if(c=='\n'){
  if(!p.lineBad&&p.lineLen){p.buffers->line[p.lineLen]=0;
   if(strncmp(p.buffers->line,"host_connection upgrade ",24)==0)upgrade(e,p.buffers->line);
   else if(control>=0&&ep[control].mode==BINARY_ACTIVE){
    if(strncmp(p.buffers->line,"protocolinfo",12)==0){Work q{};q.endpoint=e;q.text=true;q.frame.length=p.lineLen;memcpy(q.frame.payload,p.buffers->line,p.lineLen);if(xQueueSend(requests,&q,0)==pdTRUE)notifyService();}
    else rawLine(e,"Host_Connection: endpoint owned; close control session first\r\n> ");
   }else{Work q{};q.endpoint=e;q.text=true;q.frame.length=p.lineLen;memcpy(q.frame.payload,p.buffers->line,p.lineLen);if(xQueueSend(requests,&q,0)==pdTRUE)notifyService();else{++requestDrops;rawLine(e,"Host_Connection: request queue full\r\n> ");}}
  }p.lineLen=0;p.lineBad=false;return;
 }
 if(c==0||c<32||c==127){p.lineBad=true;return;}if(p.lineLen==2048){p.lineBad=true;return;}if(!p.lineBad)p.buffers->line[p.lineLen++]=c;
}
void handleFrame(unsigned e,Frame& q){
 auto& p=ep[e];if(q.session!=p.session||q.kind!=REQUEST||!q.request||q.sequence<=p.rxSeq||(q.sequence==0xffffffff&&q.op!=CLOSE)){++p.errors;return;}
 p.rxSeq=q.sequence;
 if(p.mode==UPGRADE_PENDING){
  if(q.channel!=CONTROL||q.op!=HELLO){++p.errors;return;}
  Reader r(q.payload,q.length);String nonce=readString(r,32);unsigned maxPayload=r.u16();uint32_t window=r.u32();uint32_t requestedFeatures=r.pos==r.n?31:r.u32();
  if(!r.end()||nonce!=p.nonce||maxPayload<MAX_PAYLOAD||window<1024){error(e,q,"negotiation_limits_or_nonce");return;}
  p.features=requestedFeatures&SUPPORTED_FEATURES;p.fileWindow=std::min<uint32_t>(window,WINDOW);p.mode=BINARY_ACTIVE;p.lastRx=millis();++p.transitions;
  Frame a=make(CONTROL,RESULT,HELLO,q.request);Writer w(a.payload,sizeof(a.payload));w.str(identity);w.u16(MAX_PAYLOAD);w.u32(std::min<uint32_t>(window,WINDOW));w.u32(LEASE_MS);w.u8(e);w.u8(p.data?1:0);w.u32(epoch);w.u32(p.features);
  const uint16_t operations[]={HELLO,PING,CLOSE,
#if !defined(MARAUDER_XIAO_C5)
   BIND_TOKEN,BIND,ROUTE,BAUD_PREPARE,BAUD_PROBE,BAUD_COMMIT,
#endif
   STATUS,GET_VERSION,LINK_DIAGNOSTICS,CLI_EXEC,SELECTION_LIST,SELECTION_SET,FILE_OP,FILE_START,FILE_CREDIT,FILE_CANCEL,GET_TELEMETRY,CAPTURE_START,CAPTURE_STOP,CAPTURE_STATS,SNIFF_START};
  w.u8(sizeof(operations)/sizeof(operations[0]));for(auto op:operations)w.u16(op);a.length=w.n;enqueue(e,a,true);if(!p.data){dataFailed=false;negotiationFeedback=true;notifyService();}return;
 }
 if(p.mode==RECOVERY){
  // Recovery consumes frames only. No expired binary payload ever reaches CLI.
  if(q.channel!=CONTROL||q.op!=CLOSE)return;
 }
 if(p.mode!=BINARY_ACTIVE&&p.mode!=RECOVERY)return;
 p.lastRx=millis();
 if((q.op==PING||q.op==CLOSE||q.op==BIND_TOKEN||q.op==STATUS||q.op==CAPTURE_STOP||q.op==CAPTURE_STATS||q.op==GET_TELEMETRY||q.op==GET_VERSION||q.op==LINK_DIAGNOSTICS)&&q.length){error(e,q,"unexpected_payload");return;}
 if(q.channel==CONTROL&&q.op==PING){Frame a=make(CONTROL,RESULT,PING,q.request);result(e,q,a,false);return;}
 if(q.channel==CONTROL&&q.op==CLOSE){
  if(int(e)==dataLink){dataFailed=true;dual=false;dataLink=-1;++epoch;}
  captureEnding=false;clearCapture();notifyService();Frame a=make(CONTROL,RESULT,CLOSE,q.request);Writer w(a.payload,sizeof(a.payload));w.str(p.nonce);a.length=w.n;
  xQueueReset(p.normal);xQueueReset(p.bulk);result(e,q,a,false);p.mode=RECOVERY;p.closing=true;p.lineLen=0;p.lineBad=false;return;
 }
 if(p.data&&q.op!=BIND){error(e,q,"data_link_management_only");return;}
 uint32_t feature=(q.op==FILE_OP||q.op==FILE_START||q.op==FILE_CREDIT||q.op==FILE_CANCEL)?1:
   (q.op==SNIFF_START||q.op==CAPTURE_START||q.op==CAPTURE_STOP||q.op==CAPTURE_STATS)?2:q.op==GET_TELEMETRY?4:q.op==CLI_EXEC?8:
   (q.op==BIND_TOKEN||q.op==BIND||q.op==ROUTE)?16:0;
 if(feature&&!(p.features&feature)){error(e,q,"feature_not_negotiated");return;}

 // One monotonically increasing request domain on the control link. Bounded cache
 // replays terminal results; evicted/unfinished IDs are indeterminate, never re-executed.
 if(!p.data&&q.request){uint32_t hash=crc32(q.payload,q.length)^q.op;
  xSemaphoreTake(cacheLock,portMAX_DELAY);for(unsigned i=0;i<16;i++)if(cached[i].id==q.request){if(cached[i].hash!=hash)error(e,q,"request_id_reused");else {Frame copy=cached[i].result;enqueue(e,copy,true);}xSemaphoreGive(cacheLock);return;}xSemaphoreGive(cacheLock);
  if(!requestWindow.accept(q.request)){error(e,q,"indeterminate_request");return;}
 }
 bool channelOk=(q.op==SELECTION_LIST||q.op==SELECTION_SET||q.op==CLI_EXEC||q.op==FILE_OP||q.op==FILE_START||q.op==CAPTURE_START||q.op==SNIFF_START)?q.channel==COMMAND:
   q.op==GET_TELEMETRY?q.channel==TELEMETRY:q.channel==CONTROL;
 if(!channelOk){error(e,q,"operation_channel");return;}
 Work work{};work.endpoint=e;work.text=false;work.frame=q;
 QueueHandle_t target=(q.op==FILE_CANCEL||q.op==CAPTURE_STOP)?urgentRequests:requests;
 if(xQueueSend(target,&work,0)!=pdTRUE){++requestDrops;error(e,q,"request_queue_full");}else notifyService();
}
void receiveByte(unsigned e,uint8_t b){
 auto& p=ep[e];++p.receivedBytes;
 if(rawInput.data&&e==rawInput.endpoint){if(rawInput.used<rawInput.limit)rawInput.data[rawInput.used++]=b;else rawInput.overflow=true;rawInput.at=millis();notifyService();return;}
 if(p.mode==CLI){receiveText(e,b);return;}
 if(p.mode==RECOVERY&&p.closing){
  // A host may disconnect after CLOSE but before the nonce boundary. Keep accepting
  // framed CLOSE in quarantine; no other frame/text can invoke a CLI command.
  if(p.buffers->decoder.byte(b,*rxFrame))handleFrame(e,*rxFrame);
  if(!b){p.lineLen=0;p.lineBad=false;return;}
  // The close ACK supplies a nonce. Only this exact recovery line is recognised;
  // it is not passed to the command parser. All other input is discarded.
  if(b=='\n'){
   p.buffers->line[p.lineLen]=0;char expected[80];snprintf(expected,sizeof(expected),"host_connection cli %s",p.nonce);
   if(!p.lineBad&&!strcmp(p.buffers->line,expected)&&!p.wireLen&&uxQueueMessagesWaiting(p.urgent)==0){
    p.mode=CLI;p.closing=false;p.buffers->decoder.clear();p.session=0;p.nonce[0]=0;++p.transitions;
    if(int(e)==control){control=-1;session=0;dataLink=-1;dual=false;tokenValid=false;}rawLine(e,"@HOST_CONNECTION CLI\n> ");
#if !defined(MARAUDER_XIAO_C5)
    if(e==1){pendingBaud=115200;baudSwitchAt=millis()+200;baudTrialUntil=0;}
#endif
   }p.lineLen=0;p.lineBad=false;
  }else if(b>=32&&b<127&&p.lineLen<80)p.buffers->line[p.lineLen++]=b;else p.lineBad=true;
  return;
 }
 Frame& frame=*rxFrame; // Only the transport task runs either endpoint decoder.
 if(p.buffers->decoder.byte(b,frame))handleFrame(e,frame);
}
void transmit(unsigned e){
 auto& p=ep[e];
 // Quarantined USB has no live data to deliver while unplugged. Keep its frame
 // intact for reconnect instead of feeding the disconnected HWCDC TX ring.
 // Active sessions deliberately do not use the unreliable SOF hint as a gate.
 if(e==0&&p.mode==RECOVERY&&!HWCDC::isPlugged())return;
 if(!p.wireLen){
  Frame& f=*txFrame;
  if(xQueueReceive(p.urgent,&f,0)!=pdTRUE){
   // Capture wins over logs/bulk, with one normal frame after eight capture records.
   static unsigned burst[2]={};bool got=false;
   if(captureEnabled&&burst[e]<8&&xQueueReceive(p.bulk,&f,0)==pdTRUE){got=true;++burst[e];}
   if(!got&&xQueueReceive(p.normal,&f,0)==pdTRUE){got=true;burst[e]=0;}
   if(!got&&xQueueReceive(p.bulk,&f,0)==pdTRUE){got=true;++burst[e];}
   if(!got)return;
  }
  if(p.mode==CLI||p.mode==UPGRADE_PENDING){memcpy(p.buffers->wire,f.payload,f.length);p.wireLen=f.length;}
  else {f.sequence=++p.txSeq;if(!f.sequence){recover(e);return;}p.wireLen=encode(f,p.buffers->raw,p.buffers->wire);}
  p.wireChannel=f.channel;p.wireOffset=0;p.wireStarted=millis();
 }
 int space=writeSpace(e);if(space<=0)return;
 // Bound hardware bytes ahead of urgent control to 128, including UART TX buffering.
 size_t n=std::min<size_t>(128,std::min<size_t>(space,p.wireLen-p.wireOffset));
 size_t sent=writeBytes(e,p.buffers->wire+p.wireOffset,n);if(sent<n)++p.partialWrites;
 p.submittedBytes+=sent;p.wireOffset+=sent;if(p.wireOffset==p.wireLen){p.wireLen=p.wireOffset=0;++p.frames;}
}
void sendCapture(){
 if(captureEnding&&!captureInFlight){
  clearCapture();int target=dual?int(dataLink):int(control);
  if(target<0||ep[target].mode!=BINARY_ACTIVE){captureEnding=false;return;}
  if(uxQueueSpacesAvailable(ep[target].bulk)){
   Frame f=make(CAPTURE,EVENT,CAPTURE_STOP);Writer w(f.payload,sizeof(f.payload));
   w.u32(streamId);w.u32(epoch);w.u32(captureSequence);w.u32(captureDrops);w.u32(captureSent);f.length=w.n;
   if(enqueue(target,f,false,true))captureEnding=false;
  }return;
 }
 if(!captureEnabled)return;int target=dual?int(dataLink):int(control);if(target<0||ep[target].mode!=BINARY_ACTIVE){clearCapture();return;}
 auto& p=ep[target];if(uxQueueMessagesWaiting(p.bulk)>0)return;
 uint16_t i;if(xQueueReceive(captureReady,&i,0)!=pdTRUE)return;auto& s=captureSlots[i];
 Frame f=make(CAPTURE,DATA,CAPTURE_RECORD);Writer w(f.payload,sizeof(f.payload));w.u32(s.stream);w.u32(s.sequence);w.u32(epoch);w.u64(s.timestamp);w.u16(s.original);w.u16(s.captured);w.u8(s.rssi);w.u8(s.channel);w.bytes(s.data,s.captured);f.length=w.n;
 uint64_t age=esp_timer_get_time()-s.timestamp;queueAgeMaxUs=std::max<uint32_t>(queueAgeMaxUs,std::min<uint64_t>(age,0xffffffff));
 if(enqueue(target,f,false,true))++captureSent;else ++captureDrops;xQueueSend(captureFree,&i,0);
}
void ioTask(void*){
 Frame ignored;
 for(;;){
  xSemaphoreTake(stateLock,portMAX_DELAY);
  for(unsigned e=0;e<ACTIVE_ENDPOINTS;e++){
   for(unsigned n=0;n<4096&&available(e);n++)receiveByte(e,readByte(e));
   uint32_t now=millis();auto& p=ep[e];
#if !defined(MARAUDER_XIAO_C5)
   if(e==1){
    if(baudTrialUntil&&int32_t(now-baudTrialUntil.load())>=0){pendingBaud=115200;baudSwitchAt=now;baudTrialUntil=0;}
    if(pendingBaud&&int32_t(now-baudSwitchAt.load())>=0&&!p.wireLen&&!uxQueueMessagesWaiting(p.urgent)&&!uxQueueMessagesWaiting(p.normal)){
     Serial0.flush();uint32_t rate=pendingBaud.exchange(0);Serial0.updateBaudRate(rate);uartBaud=rate;
     if(rate!=115200)baudTrialUntil=millis()+3000;
     p.buffers->decoder.clear();
    }
   }
#endif
   if((p.mode==UPGRADE_PENDING&&now-p.lastRx>HANDSHAKE_MS)||(p.mode==BINARY_ACTIVE&&now-p.lastRx>LEASE_MS))recover(e,1);
   // SOF connection detection can briefly report false while the same USB device
   // remains usable (observed on this Mac/C5). Only the protocol lease expires a
   // session: a physical hint must not discard a valid session or queued frames.
   if(e==0&&p.mode==BINARY_ACTIVE&&!HWCDC::isPlugged())++usbSofMissingSamples;
   // Advertise the quarantined session as a *frame*. A new host may recover only
   // after receiving this valid, explicit capability notice; no speculative
   // binary probe is sent to old firmware. Input remains binary until CLOSE.
   if(p.mode==RECOVERY&&(e!=0||HWCDC::isPlugged())&&now-p.lastRecoveryNotice>=500){
    p.lastRecoveryNotice=now;Frame f=make(CONTROL,EVENT,RECOVERY_NOTICE);
    Writer w(f.payload,sizeof(f.payload));w.str(identity);w.str(p.nonce);w.u32(p.rxSeq);w.u8(e);f.length=w.n;
    if(uxQueueMessagesWaiting(p.urgent)==0)enqueue(e,f,true);
   }
   // Re-arm the existing HWCDC driver's interrupt; never install a competing driver.
   // A frame can be fully submitted while bytes remain in HWCDC's 2048-byte
   // ring. Re-arm that tail too: otherwise an idle data-link PING can stall.
   if(e==0&&(p.wireLen||writeSpace(0)<2048)){usb_serial_jtag_ll_txfifo_flush();usb_serial_jtag_ll_ena_intr_mask(USB_SERIAL_JTAG_INTR_SERIAL_IN_EMPTY);}
   // Measured 128-byte/tick transmission left capture records queued for 142 ms.
   // A bounded native burst drains ready USB work without spinning/starving the
   // service executor. Re-arbitrate at each complete frame, never within one.
   for(unsigned part=0;part<(e==0?4:1);++part){
    size_t before=p.wireOffset;uint32_t completed=p.frames;
    transmit(e);
    if(before==p.wireOffset&&completed==p.frames)break;
    sendCapture();
   }
  }
  sendCapture();xSemaphoreGive(stateLock);
  ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(2)); // RX/TX events wake immediately; timer is lease/TX fallback.
 }
}
void onUsb(void*,esp_event_base_t,int32_t,void*){notifyTransport();}
void fileOperation(unsigned e,Frame& q){
 Reader r(q.payload,q.length);storage_service::Request request{};request.op=storage_service::Op(r.u8());request.path=readString(r,160);request.destination=readString(r,160);request.offset=r.u32();request.length=r.u32();
 if(!r.end()){error(e,q,"payload");return;}
 storage_service::Result answer;storage_service::execute(request,answer);if(!answer.ok()){error(e,q,answer.error);return;}
 Frame f;Writer w(f.payload,sizeof(f.payload));w.u8(uint8_t(request.op));w.u32(answer.size);w.u32(answer.offset);w.u32(answer.next);w.u32(answer.total);w.u32(answer.used);
 // 20 worst-case path entries fit by returning fewer and adjusting pagination.
 unsigned count=answer.count;size_t required=27+answer.path.length()+answer.length;
 unsigned fit=0;for(;fit<count;fit++){size_t n=7+answer.entries[fit].name.length();if(required+n>MAX_PAYLOAD)break;required+=n;}
 if(fit<count)put32(w.p+9,request.offset+fit);
 w.u16(fit);w.str(answer.path.c_str());w.u16(answer.length);w.bytes(answer.data,answer.length);
 for(unsigned i=0;i<fit;i++){auto& entry=answer.entries[i];w.str(entry.name.c_str());w.u8(entry.directory);w.u32(entry.size);}f.length=w.n;result(e,q,f);
}
void startFile(unsigned e,Frame& q){
 if(dataFailed){error(e,q,"data_link_failed_explicit_fallback_required");return;}
 Reader r(q.payload,q.length);String path=readString(r,160);uint32_t credits=r.u32();if(!r.end()||credits>ep[e].fileWindow){error(e,q,"payload");return;}
 if(captureEnabled||captureEnding){error(e,q,"capture_active");return;}uint32_t generation;
 if(const char* err=storage_service::openStream(path,transfer.file,generation)){error(e,q,err);return;}
 transfer.active=true;transfer.id=esp_random()|1;transfer.size=transfer.file.size();transfer.offset=transfer.acked=0;transfer.generation=generation;transfer.credit=credits;transfer.lastCredit=millis();
 mbedtls_sha256_init(&transfer.sha);mbedtls_sha256_starts(&transfer.sha,0);
 Frame f;Writer w(f.payload,sizeof(f.payload));w.u32(transfer.id);w.u32(transfer.size);w.u32(generation);w.str(path.c_str());w.u32(ep[e].fileWindow);f.length=w.n;result(e,q,f);
}
void pumpFile(){
 if(!transfer.active)return;
 if(dataFailed||control<0||ep[control].mode!=BINARY_ACTIVE||millis()-transfer.lastCredit>LEASE_MS){closeFile();return;}
 int target=dual?int(dataLink):int(control);if(target<0||ep[target].mode!=BINARY_ACTIVE){closeFile();return;}
 if(uxQueueMessagesWaiting(ep[target].bulk)>=2)return;
 if(transfer.offset==transfer.size){
  // END is enqueued behind all file records and therefore on the same data link.
  Frame f=make(FILE_DATA,EVENT,FILE_END);Writer w(f.payload,sizeof(f.payload));uint8_t digest[32];mbedtls_sha256_finish(&transfer.sha,digest);
  w.u32(transfer.id);w.u32(transfer.size);w.bytes(digest,32);f.length=w.n;
  if(enqueue(target,f,false,true))closeFile();return;
 }
 if(!transfer.credit)return;
 uint32_t n=std::min<uint32_t>(1024,std::min(transfer.credit,transfer.size-transfer.offset));
 Frame f=make(FILE_DATA,DATA,FILE_RECORD);Writer w(f.payload,sizeof(f.payload));w.u32(transfer.id);w.u32(transfer.offset);w.u32(transfer.generation);w.u16(n);
 if(transfer.file.size()!=transfer.size||transfer.file.read(f.payload+w.n,n)!=n){Frame q;q.op=FILE_START;error(control,q,"source_changed_or_read");closeFile();return;}
 mbedtls_sha256_update(&transfer.sha,f.payload+w.n,n);f.length=w.n+n;
 if(enqueue(target,f,false,true)){transfer.offset+=n;transfer.credit-=n;}else{closeFile();}
}
void selectionNotice(const String& line){
 if(line.startsWith("clearlist ")||line.startsWith("load "))selection_service::discardDeferred();
 if(line=="stopscan")selection_service::settle();
 if(!(line.startsWith("select ")||line.startsWith("clearlist ")||line.startsWith("load ")||line.startsWith("add ")||line=="stopscan"))return;
 if(control>=0&&ep[control].mode==BINARY_ACTIVE){Frame f=make(COMMAND,EVENT,SELECTION_CHANGED);enqueue(control,f);}
}
bool supportedCaptureMode(int mode){
 switch(mode){
  case WIFI_SCAN_RAW_CAPTURE:case WIFI_SCAN_AP:case WIFI_SCAN_PROBE:case WIFI_SCAN_DEAUTH:
  case WIFI_SCAN_EAPOL:case WIFI_SCAN_ACTIVE_EAPOL:case WIFI_SCAN_ACTIVE_LIST_EAPOL:
  case WIFI_SCAN_SAE_COMMIT:case WIFI_SCAN_PWN:case WIFI_SCAN_PINESCAN:case WIFI_SCAN_MULTISSID:
  case WIFI_SCAN_AP_STA:case WIFI_SCAN_AP_STA_BEACON:return true;
  default:return false;
 }
}
bool filteredCaptureMode(int mode){return mode!=WIFI_SCAN_RAW_CAPTURE&&mode!=WIFI_SCAN_AP_STA&&mode!=WIFI_SCAN_AP_STA_BEACON;}
int nativeScanMode(const NativeSniffRequest& r){
 switch(r.mode){
  case SniffMode::Raw:return WIFI_SCAN_RAW_CAPTURE;
  case SniffMode::Beacon:return WIFI_SCAN_AP;
  case SniffMode::Probe:return WIFI_SCAN_PROBE;
  case SniffMode::Deauth:return WIFI_SCAN_DEAUTH;
  case SniffMode::Pmkid:return r.flags&2?(r.flags&4?WIFI_SCAN_ACTIVE_LIST_EAPOL:WIFI_SCAN_ACTIVE_EAPOL):WIFI_SCAN_EAPOL;
  case SniffMode::Sae:return WIFI_SCAN_SAE_COMMIT;
  case SniffMode::Pwn:return WIFI_SCAN_PWN;
  case SniffMode::PineScan:return WIFI_SCAN_PINESCAN;
  case SniffMode::MultiSsid:return WIFI_SCAN_MULTISSID;
  case SniffMode::All:return WIFI_SCAN_AP_STA;
  case SniffMode::Combined:return WIFI_SCAN_AP_STA_BEACON;
 }
 return WIFI_SCAN_OFF;
}
void startCaptureResult(unsigned e,const Frame& q){
 clearCapture();nativeCaptureFilter=filteredCaptureMode(wifi_scan_obj.currentScanMode);
 streamId=esp_random()|1;captureSequence=0;captureDrops=0;captureSent=0;queueAgeMaxUs=0;queueHigh=0;captureEnabled=true;
 Frame f;Writer w(f.payload,sizeof(f.payload));w.u32(streamId);w.u32(epoch);w.u16(SNAPLEN);w.u8(1);f.length=w.n;result(e,q,f);
}
void executeRequest(Work& work){
 unsigned e=work.endpoint;Frame& q=work.frame;currentEndpoint=e;currentRequest=work.text?0:q.request;
 if(work.text){if(ep[e].mode!=CLI)return;String line(reinterpret_cast<char*>(q.payload),q.length);cli_obj.runCommand(line);selectionNotice(line);if(ep[e].mode==CLI)console().print("> ");flushResponse();return;}
 if(ep[e].mode!=BINARY_ACTIVE||q.session!=session){return;}
#if !defined(MARAUDER_XIAO_C5)
 if(q.op==BAUD_PREPARE||q.op==BAUD_PROBE||q.op==BAUD_COMMIT){
  if(e!=1||control!=1||dual||transfer.active||captureEnabled||captureEnding||wifi_scan_obj.scanning()||wifi_scan_obj.wifi_connected||device_observation::bleActive()) {error(e,q,"baud_requires_idle_uart_control");return;}
  if(q.op==BAUD_PROBE){
   if(!q.length||q.length>1536){error(e,q,"baud_probe_length");return;}
   Frame f;f.length=q.length;memcpy(f.payload,q.payload,q.length);result(e,q,f,false);return;
  }
  Reader r(q.payload,q.length);uint32_t rate=r.u32();
  if(!r.end()||(rate!=115200&&rate!=460800&&rate!=921600&&rate!=1500000&&rate!=2000000)){error(e,q,"baud_rate");return;}
  if(q.op==BAUD_COMMIT){
   if(pendingBaud||uartBaud!=rate){error(e,q,"baud_not_switched");return;}
   baudTrialUntil=0;Frame f;Writer w(f.payload,sizeof(f.payload));w.u32(rate);f.length=w.n;result(e,q,f,false);return;
  }
  if(pendingBaud||baudTrialUntil){error(e,q,"baud_trial_active");return;}
  Frame f;Writer w(f.payload,sizeof(f.payload));w.u32(rate);w.u16(150);w.u16(3000);f.length=w.n;result(e,q,f,false);
  baudSwitchAt=millis()+150;pendingBaud=rate;return;
 }
#endif
 if(q.op==CLI_EXEC){
  Reader r(q.payload,q.length);String line=readString(r,2048);if(!r.end()||line.indexOf('\n')>=0||line.indexOf('\r')>=0){error(e,q,"command");return;}
  if(line.startsWith("host_connection")||line.startsWith("ota ")){error(e,q,"exclusive_cli_required");return;}
  uint32_t dropped=outputDrops;cli_obj.runCommand(line);selectionNotice(line);flushResponse();Frame f;
  if(outputDrops!=dropped){f.kind=ERROR;Writer w(f.payload,sizeof(f.payload));w.str("console_output_overflow");f.length=w.n;f.channel=q.channel;f.op=q.op;f.request=q.request;enqueue(e,f,false);return;}
  result(e,q,f,true,false);return;
 }
 if(q.op==SELECTION_LIST||q.op==SELECTION_SET){
  Reader r(q.payload,q.length);Frame f;Writer w(f.payload,sizeof(f.payload));
  const char* problem=q.op==SELECTION_LIST?selection_service::list(r,w):selection_service::set(r,w);
  if(problem){error(e,q,problem);return;}f.length=w.n;result(e,q,f);return;
 }
 if(q.op==FILE_CREDIT&&e==unsigned(control)){
  Reader r(q.payload,q.length);uint32_t id=r.u32(),acked=r.u32(),bytes=r.u32();
  const uint32_t limit=ep[e].fileWindow;
  if(!r.end()||!transfer.active||id!=transfer.id||acked<transfer.acked||acked>transfer.offset||bytes>limit||transfer.credit+bytes>limit||transfer.offset-acked+transfer.credit+bytes>limit){char detail[220];snprintf(detail,sizeof(detail),"credit_range active=%u id=%lu/%lu ack=%lu/%lu sent=%lu credit=%lu add=%lu limit=%lu",transfer.active,(unsigned long)id,(unsigned long)transfer.id,(unsigned long)acked,(unsigned long)transfer.acked,(unsigned long)transfer.offset,(unsigned long)transfer.credit,(unsigned long)bytes,(unsigned long)limit);error(e,q,detail);return;}
  transfer.acked=acked;transfer.credit+=bytes;transfer.lastCredit=millis();Frame a;result(e,q,a,false);return;
 }
 if(q.op==FILE_OP){fileOperation(e,q);return;}
 if(q.op==FILE_START){startFile(e,q);return;}
 if(q.op==FILE_CANCEL){Reader r(q.payload,q.length);uint32_t id=r.u32();if(!r.end()||!transfer.active||id!=transfer.id){error(e,q,"transfer_id");return;}closeFile();for(auto& p:ep)xQueueReset(p.bulk);Frame f;result(e,q,f);return;}
 if(q.op==BIND_TOKEN){
  if(e!=1||dual||dataLink>=0||transfer.active||captureEnabled||captureEnding){error(e,q,"binding_requires_idle_uart_control");return;}
  esp_fill_random(bindToken,16);tokenValid=true;tokenDeadline=millis()+5000;
  Frame f;Writer w(f.payload,sizeof(f.payload));w.str(identity);w.u32(session);w.bytes(bindToken,16);w.u32(5000);f.length=w.n;result(e,q,f);return;
 }
 if(q.op==BIND){
  Reader r(q.payload,q.length);String device=readString(r,17);uint32_t controlSession=r.u32();auto token=r.take(16);
  if(!r.end()||e!=0||control!=1||!ep[e].data||!tokenValid||int32_t(tokenDeadline-millis())<=0||device!=identity||controlSession!=session||memcmp(token,bindToken,16)||dataLink>=0){error(e,q,"binding_rejected");return;}
  tokenValid=false;dataLink=e;Frame f;Writer w(f.payload,sizeof(f.payload));w.str(identity);w.u32(session);w.u32(epoch);f.length=w.n;result(e,q,f,false);return;
 }
 if(q.op==ROUTE){
  Reader r(q.payload,q.length);uint8_t mode=r.u8();if(!r.end()||e!=unsigned(control)||mode>1||transfer.active||captureEnabled||captureEnding||(mode==1&&(dataLink<0||ep[dataLink].mode!=BINARY_ACTIVE))){error(e,q,"routing_requires_paused_streams_and_bound_link");return;}
  if(mode&&((ep[dataLink].features&ep[e].features&3)!=(ep[e].features&3))){error(e,q,"data_features_not_negotiated");return;}
  // Stream pause is explicit. At a boundary both links must have drained all bulk frames.
  xSemaphoreTake(stateLock,portMAX_DELAY);
  if(!mode&&dataFailed){ep[0].wireLen=ep[0].wireOffset=0;xQueueReset(ep[0].bulk);}
  bool busy=false;for(auto& p:ep)if((p.wireLen&&(p.wireChannel==CAPTURE||p.wireChannel==FILE_DATA))||uxQueueMessagesWaiting(p.bulk))busy=true;
  if(busy){xSemaphoreGive(stateLock);error(e,q,"routing_boundary_busy");return;}
  dual=mode;dataFailed=false;for(auto& p:ep)xQueueReset(p.bulk);++epoch;xSemaphoreGive(stateLock);
  Frame f;Writer w(f.payload,sizeof(f.payload));w.u8(mode);w.u32(epoch);f.length=w.n;result(e,q,f);return;
 }
 if(q.op==SNIFF_START){
  NativeSniffRequest spec;
  if(!spec.decode(q.payload,q.length)){error(e,q,"invalid_native_sniff_options");return;}
  if(dataFailed||transfer.active||storage_service::leased()||usb_ota::active()||device_observation::bleActive()||wifi_scan_obj.wifi_connected||wifi_scan_obj.scanning()||captureEnabled||captureEnding){error(e,q,"sniff_requires_idle_radio_storage_and_links");return;}
  if((spec.flags&4)&&!wifi_scan_obj.filterActive()){error(e,q,"selected_ap_list_required");return;}
  if(spec.flags&1){
   bool known=false;
   for(auto channel:wifi_scan_obj.dual_band_channels)if(channel==spec.channel)known=true;
   if(!known){error(e,q,"unsupported_channel");return;}
  }
  uint8_t previousChannel=wifi_scan_obj.set_channel;
  if(spec.flags&1)wifi_scan_obj.set_channel=spec.channel;
  int mode=nativeScanMode(spec);
  uint16_t color=spec.mode==SniffMode::Raw?TFT_WHITE:spec.mode==SniffMode::Deauth?TFT_RED:spec.mode==SniffMode::Pmkid?TFT_VIOLET:TFT_MAGENTA;
  // Scoped policy must precede StartScan/openFile, including PCAP global headers.
  buffer_obj.prepareHostOnly(true);
  wifi_scan_obj.save_serial=false;
  wifi_scan_obj.StartScan(mode,color);
  buffer_obj.prepareHostOnly(false);
  bool enabled=false;uint8_t actualChannel=0;wifi_second_chan_t secondary;
  bool channelOk=!(spec.flags&1)||(esp_wifi_get_channel(&actualChannel,&secondary)==ESP_OK&&actualChannel==spec.channel);
  if(!channelOk||wifi_scan_obj.currentScanMode!=mode||esp_wifi_get_promiscuous(&enabled)!=ESP_OK||!enabled){
   if(wifi_scan_obj.scanning())wifi_scan_obj.StartScan(WIFI_SCAN_OFF,TFT_BLACK);
   wifi_scan_obj.set_channel=previousChannel;
   error(e,q,"native_sniff_start_failed");return;
  }
  buffer_obj.suppressLocalPcap();flushResponse();startCaptureResult(e,q);return;
 }
 if(q.op==CAPTURE_START){
  if(dataFailed){error(e,q,"data_link_failed_explicit_fallback_required");return;}
  if((q.length>1||(q.length==1&&q.payload[0]>1))||transfer.active||device_observation::bleActive()||wifi_scan_obj.wifi_connected||!wifi_scan_obj.scanning()){error(e,q,"requires_existing_scan_and_no_file_or_ble");return;}
  bool enabled=false;if(esp_wifi_get_promiscuous(&enabled)!=ESP_OK||!enabled){error(e,q,"radio_not_running");return;}
  // No new radio action; attach to an already running receive path only.
  if(!supportedCaptureMode(wifi_scan_obj.currentScanMode)){error(e,q,"unsupported_capture_scan");return;}
  if(captureEnabled||captureEnding){error(e,q,"capture_already_active");return;}
  if(q.length==1&&q.payload[0]==1)buffer_obj.suppressLocalPcap();
  startCaptureResult(e,q);return;
 }
 if(q.op==CAPTURE_STOP){if(captureEnabled){captureEnabled=false;captureEnding=true;notifyTransport();}Frame f;result(e,q,f);return;}
 if(q.op==STATUS||q.op==CAPTURE_STATS){
  Frame f;Writer w(f.payload,sizeof(f.payload));w.u32(epoch);w.u8(dual);w.u8(captureEnabled);w.u32(streamId);w.u32(captureSequence);w.u32(captureDrops);w.u32(captureSent);w.u32(uxQueueMessagesWaiting(captureReady));w.u32(queueHigh);w.u32(callbackMaxUs);w.u32(queueAgeMaxUs);w.u32(outputDrops);w.u32(requestDrops);
  for(auto& p:ep){w.u32(p.buffers->decoder.errors);w.u32(p.buffers->decoder.oversized);w.u32(p.partialWrites);w.u32(p.drops);w.u32(p.transitions);}
  wifi_mode_t mode=WIFI_MODE_NULL;bool promisc=false;w.u32(callbackCount);w.u32(invalidRx);w.u32(invalidLength);w.u32(esp_wifi_get_mode(&mode));w.u32(mode);w.u32(esp_wifi_get_promiscuous(&promisc));w.u8(promisc);w.u8(dataFailed);w.u32(legacyDrops);
  for(auto& count:callbackHistogram)w.u32(count);w.u32(uxTaskGetStackHighWaterMark(transportTask));w.u32(uxTaskGetStackHighWaterMark(serviceTask));f.length=w.n;result(e,q,f);return;
 }
 if(q.op==LINK_DIAGNOSTICS){
  Frame f;Writer w(f.payload,sizeof(f.payload));w.u8(1);w.u8(ACTIVE_ENDPOINTS);
  for(unsigned i=0;i<ACTIVE_ENDPOINTS;++i){auto& p=ep[i];w.u8(p.mode);w.u8(p.recoveryReason);w.u32(millis()-p.lastRx);w.u32(p.rxSeq);w.u32(p.txSeq);w.u32(p.receivedBytes);w.u32(p.submittedBytes);w.u32(p.wireLen);w.u32(p.wireOffset);w.u32(p.frames);}
  w.u8(HWCDC::isPlugged());w.u32(writeSpace(0));w.u32(usbSofMissingSamples);f.length=w.n;result(e,q,f);return;
 }
 if(q.op==GET_VERSION){
  Frame f;Writer w(f.payload,sizeof(f.payload));w.str(MARAUDER_FIRMWARE_SEMVER);w.str(MARAUDER_UPSTREAM_VERSION);w.str(MARAUDER_EXTENSION_VERSION);w.str(MARAUDER_BUILD_PROFILE);w.str(MARAUDER_BUILD_TARGET);f.length=w.n;result(e,q,f);return;
 }
 if(q.op==GET_TELEMETRY){
  // Typed snapshot is implemented by the observability service; no high-rate text query.
  Frame f;Writer w(f.payload,sizeof(f.payload));device_observation::binaryReport(w);f.length=w.n;result(e,q,f);return;
 }
 error(e,q,"unsupported_operation");
}
}
String readLegacyInput(size_t limit,uint32_t timeout){
 if(active()||limit>65536||!limit){console().println("Interactive input requires exclusive legacy CLI");return String();}
 char* data=static_cast<char*>(ps_malloc(limit));if(!data)return String();
 xSemaphoreTake(stateLock,portMAX_DELAY);rawInput={data,limit,0,unsigned(currentEndpoint),millis(),false};xSemaphoreGive(stateLock);
 for(;;){xSemaphoreTake(stateLock,portMAX_DELAY);bool done=millis()-rawInput.at>=timeout;size_t size=rawInput.used;bool overflow=rawInput.overflow;
  if(done)rawInput.data=nullptr;xSemaphoreGive(stateLock);
  if(done){String result=overflow?String():String(data,size);free(data);return result;}ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(10));
 }
}
String readLegacyBytes(size_t length,uint32_t timeout,const String& readyLine){
 if(active()||!length||length>4096)return String();
 char* data=static_cast<char*>(ps_malloc(length));if(!data)return String();
 xSemaphoreTake(stateLock,portMAX_DELAY);rawInput={data,length,0,unsigned(currentEndpoint),millis(),false};xSemaphoreGive(stateLock);
 console().print("\n");console().println(readyLine);flushResponse();uint32_t started=millis();
 for(;;){
  xSemaphoreTake(stateLock,portMAX_DELAY);size_t size=rawInput.used;bool overflow=rawInput.overflow;
  bool done=size==length||overflow||millis()-started>=timeout;
  if(done){rawInput.data=nullptr;if(size!=length||overflow){ep[currentEndpoint].lineBad=true;ep[currentEndpoint].lineLen=0;}}
  xSemaphoreGive(stateLock);
  if(done){String result=(size==length&&!overflow)?String(data,size):String();free(data);return result;}
  ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(2));
 }
}
Output& console(){static Output output;return output;}
size_t Output::write(uint8_t c){return write(&c,1);}
size_t Output::write(const uint8_t* data,size_t n){
 if(!ready){command_bytes_+=n;return Serial.write(data,n);}
 bool executor=xTaskGetCurrentTaskHandle()==serviceTask;
 if(executor)command_bytes_+=n;
 if(xSemaphoreTake(textLock,executor?pdMS_TO_TICKS(100):0)!=pdTRUE){++outputDrops;return n;}
 TextBuffer& b=executor?responseText:asyncText;int endpoint=executor?currentEndpoint:(control>=0?int(control):0);uint32_t request=executor?currentRequest:0;
 if(ep[endpoint].mode==UPGRADE_PENDING||ep[endpoint].mode==RECOVERY){b.used=0;xSemaphoreGive(textLock);return n;}
 for(size_t i=0;i<n;i++){b.data[b.used++]=data[i];if(b.used==sizeof(b.data)||data[i]=='\n')textFlush(b,endpoint,request);}
 xSemaphoreGive(textLock);return n;
}
int Output::availableForWrite(){return !ready?Serial.availableForWrite():uxQueueSpacesAvailable(ep[control>=0?int(control):0].normal)*512;}
void Output::flush(){if(ready)flushResponse();else Serial.flush();}
bool initialized(){return ready;}
bool active(){return ready&&control>=0&&ep[control].mode==BINARY_ACTIVE;}
bool capturing(){return captureEnabled;}
bool serviceBusy(){return transfer.active||captureEnabled||captureEnding;}
void discovery(const char* tx){
 if(!ready){console().printf("@MARAUDER:{\"protocol\":1,\"tx\":\"%s\",\"command\":\"protocolinfo\",\"status\":\"success\",\"code\":\"OK\",\"capabilities\":[\"files-v1\",\"files-delete-v1\",\"selection-v1\",\"capture-host-only-v1\",\"device-metrics-v1\"],\"Host_Connection_error\":\"initialization_failed\"}\n",tx);return;}
 console().printf("@MARAUDER:{\"protocol\":1,\"tx\":\"%s\",\"command\":\"protocolinfo\",\"status\":\"success\",\"code\":\"OK\",\"firmware\":\"v%s\",\"firmware_version\":\"%s\",\"capabilities\":[\"usb-ota-v1\",\"files-v1\",\"files-delete-v1\",\"selection-v1\",\"capture-host-only-v1\",\"native-sniff-stream-v1\",\"device-metrics-v1\",\"ble-observer-v1\",\"discovery-v1\",\"spiffs-backup\",\"spiffs-backup-status\",\"spiffs-restore\",\"Host_Connection\"],\"backupPath\":\"/spiffs\",\"Host_Connection\":{\"versions\":[1],\"device_id\":\"%s\",\"services\":[\"files\",\"telemetry\",\"capture\",\"CLI_EXEC\",\"version-v1\"],\"features\":%u,\"roles\":[%s],\"endpoint\":\"%s\",\"max_payload\":3072,\"file_window\":8192,\"lease_ms\":12000}}\n",tx,MARAUDER_UPSTREAM_VERSION,MARAUDER_FIRMWARE_SEMVER,identity,SUPPORTED_FEATURES,ENDPOINT_ROLES,currentEndpoint?"uart":"native_usb");
}
void begin(){
 selection_service::begin();
 serviceTask=xTaskGetCurrentTaskHandle();stateLock=xSemaphoreCreateMutex();cacheLock=xSemaphoreCreateMutex();textLock=xSemaphoreCreateMutex();
 rxFrame=externalObject<Frame>();txFrame=externalObject<Frame>();executingWork=externalObject<Work>();
 requests=psQueue(8,sizeof(Work));urgentRequests=psQueue(4,sizeof(Work));for(auto& p:ep){p.buffers=externalObject<EndpointBuffers>();p.urgent=psQueue(8,sizeof(Frame));p.normal=psQueue(128,sizeof(Frame));p.bulk=psQueue(2,sizeof(Frame));}
 cached=static_cast<Cached*>(ps_calloc(16,sizeof(Cached)));captureSlots=static_cast<CaptureSlot*>(ps_calloc(CAP_SLOTS,sizeof(CaptureSlot)));legacySlots=static_cast<LegacySlot*>(ps_calloc(CAP_SLOTS,sizeof(LegacySlot)));
 legacyFree=xQueueCreate(CAP_SLOTS,sizeof(uint16_t));captureFree=xQueueCreate(CAP_SLOTS,sizeof(uint16_t));captureReady=xQueueCreate(CAP_SLOTS,sizeof(uint16_t));legacyReady=xQueueCreate(CAP_SLOTS,sizeof(uint16_t));
 if(!rxFrame||!txFrame||!executingWork||!stateLock||!cacheLock||!textLock||!urgentRequests||!legacyFree||!legacySlots||!requests||!cached||!captureSlots||!captureFree||!captureReady||!legacyReady){Serial.println("Host_Connection allocation failed");return;}
 for(auto& p:ep)if(!p.buffers||!p.urgent||!p.normal||!p.bulk){Serial.println("Host_Connection queue allocation failed");return;}
 for(uint16_t i=0;i<CAP_SLOTS;i++){xQueueSend(captureFree,&i,0);xQueueSend(legacyFree,&i,0);}
 uint8_t mac[6];esp_read_mac(mac,ESP_MAC_BASE);snprintf(identity,sizeof(identity),"%02x:%02x:%02x:%02x:%02x:%02x",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
 esp_log_set_vprintf([](const char* format,va_list args)->int{char line[384];int n=vsnprintf(line,sizeof(line),format,args);if(n>0)console().write(reinterpret_cast<uint8_t*>(line),std::min<int>(n,sizeof(line)-1));return n;});
 Serial.setDebugOutput(false);Serial.setTxTimeoutMs(0);Serial.onEvent(onUsb);
#if !defined(MARAUDER_XIAO_C5)
 Serial0.setDebugOutput(false);Serial0.setRxBufferSize(4096);Serial0.setTxBufferSize(256);Serial0.begin(115200,SERIAL_8N1,RX,TX);Serial0.onReceive([](){notifyTransport();},false);
#endif
 ready=true;if(xTaskCreate(ioTask,"Host_Connection",16384,nullptr,2,&transportTask)!=pdPASS){ready=false;Serial.println("Host_Connection task allocation failed; legacy CLI retained");}
}
void execute(){
 if(!ready)return;Work& work=*executingWork;
 if(negotiationFeedback.exchange(false))led_obj.hostConnectionFeedback();
 // Legacy radio processing also runs outside the Wi-Fi driver callback.
 for(unsigned n=0;n<8;n++){uint16_t i;if(xQueueReceive(legacyReady,&i,0)!=pdTRUE)break;auto& slot=legacySlots[i];
  processingSlot=&slot;processingCaptured=false;
  if(wifi_scan_obj.scanning()&&slot.callback==legacyCallback){
   // Ordinary beacon sniffing previously printed APs without registering them.
   // Decode/register in the existing executor, preserving callback and PCAP filters.
   if(wifi_scan_obj.currentScanMode==WIFI_SCAN_AP&&slot.type==WIFI_PKT_MGMT){
    const auto* p=reinterpret_cast<const wifi_promiscuous_pkt_t*>(slot.packet);discovery::Observation o{};
    if(p->rx_ctrl.sig_len>=4&&discovery::decode(p->payload,p->rx_ctrl.sig_len-4,o)&&o.kind=='a')
     selection_service::observe(o,o.primary?o.primary:p->rx_ctrl.channel,p->rx_ctrl.rssi,uint32_t(slot.receivedUs/1000));
   }
   slot.callback(slot.packet,slot.type);
  }
  processingSlot=nullptr;
  xQueueSend(legacyFree,&i,0);
 }
 // The executor is the existing application task. No concurrent state-changing operations.
 if(xQueueReceive(urgentRequests,&work,0)==pdTRUE||xQueueReceive(requests,&work,0)==pdTRUE){executeRequest(work);currentRequest=0;if(control>=0)currentEndpoint=control;}
 if(captureEnabled&&!wifi_scan_obj.scanning()){captureEnabled=false;captureEnding=true;notifyTransport();}
 if(transfer.active){if(control<0||ep[control].mode!=BINARY_ACTIVE)closeFile();else pumpFile();}
}
void idle(){if(ready){if(uxQueueMessagesWaiting(legacyReady)||uxQueueMessagesWaiting(requests)||uxQueueMessagesWaiting(urgentRequests)){taskYIELD();return;}ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(transfer.active?1:10));}else delay(10);}
void setLegacyCallback(wifi_promiscuous_cb_t cb){legacyCallback=cb;if(!ready)return;uint16_t i;while(xQueueReceive(legacyReady,&i,0)==pdTRUE)xQueueSend(legacyFree,&i,0);}
void receive(void* packet,wifi_promiscuous_pkt_type_t type){
 if(!ready){if(legacyCallback)legacyCallback(packet,type);return;}
 CallbackTiming timing;
 ++callbackCount;
 if(captureEnabled&&!nativeCaptureFilter)capture(packet,type);
 if(!packet||!legacyCallback||(type!=WIFI_PKT_MGMT&&type!=WIFI_PKT_DATA))return;
 auto p=static_cast<const wifi_promiscuous_pkt_t*>(packet);
 if(p->rx_ctrl.rx_state){++invalidRx;return;}
 if(p->rx_ctrl.sig_len<4||p->rx_ctrl.sig_len>SNAPLEN+4){++invalidLength;return;}
 uint16_t i;if(xQueueReceive(legacyFree,&i,0)!=pdTRUE){++legacyDrops;return;}
 auto& slot=legacySlots[i];slot.receivedUs=esp_timer_get_time();slot.callback=legacyCallback;slot.type=type;
 memcpy(slot.packet,packet,sizeof(wifi_pkt_rx_ctrl_t)+p->rx_ctrl.sig_len);
 if(xQueueSend(legacyReady,&i,0)!=pdTRUE){++legacyDrops;xQueueSend(legacyFree,&i,0);}else notifyService();
}
void captureAccepted(const wifi_promiscuous_pkt_t* packet){
 if(!nativeCaptureFilter||!captureEnabled||!processingSlot||processingCaptured||packet!=reinterpret_cast<const wifi_promiscuous_pkt_t*>(processingSlot->packet))return;
 processingCaptured=true;capture(const_cast<wifi_promiscuous_pkt_t*>(packet),processingSlot->type);
}
void capture(void* packet,wifi_promiscuous_pkt_type_t type){
 if(!captureEnabled||!packet||(type!=WIFI_PKT_MGMT&&type!=WIFI_PKT_DATA))return;
 struct InFlight {InFlight(){++captureInFlight;}~InFlight(){--captureInFlight;}} inFlight;
 if(!captureEnabled)return;
 auto p=static_cast<const wifi_promiscuous_pkt_t*>(packet);if(p->rx_ctrl.rx_state||p->rx_ctrl.sig_len<4)return;
 uint64_t start=nativeCaptureFilter&&processingSlot?processingSlot->receivedUs:esp_timer_get_time();uint32_t seq=captureSequence.load();uint16_t i;
 // End this stream explicitly before the 32-bit record counter can wrap.
 do {if(seq==0xffffffff){captureEnabled=false;captureEnding=true;notifyTransport();return;}}
 while(!captureSequence.compare_exchange_weak(seq,seq+1));
 if(xQueueReceive(captureFree,&i,0)!=pdTRUE){++captureDrops;return;}
 auto& s=captureSlots[i];s.stream=streamId;s.sequence=seq;s.timestamp=start;s.original=p->rx_ctrl.sig_len-4;s.captured=std::min<unsigned>(s.original,SNAPLEN);s.rssi=p->rx_ctrl.rssi;s.channel=p->rx_ctrl.channel;memcpy(s.data,p->payload,s.captured);
 if(xQueueSend(captureReady,&i,0)!=pdTRUE){++captureDrops;xQueueSend(captureFree,&i,0);}else notifyTransport();
 queueHigh=std::max<uint32_t>(queueHigh.load(),uxQueueMessagesWaiting(captureReady));
}
}
#endif
