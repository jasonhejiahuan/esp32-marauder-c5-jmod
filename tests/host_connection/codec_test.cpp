#include "../../esp32_marauder/HostConnectionCodec.h"
#include "../../esp32_marauder/HostConnectionFeedback.h"
#include <assert.h>
#include <stdio.h>
#include <random>
using namespace host_connection;
int main(){
 RequestWindow requests;assert(requests.accept(10));assert(requests.accept(8));assert(!requests.accept(8));assert(!requests.accept(0));assert(requests.accept(80));assert(!requests.accept(10));
 FeedbackFlash flash;assert(!flash.start(0,false,true));assert(!flash.start(0,true,false));assert(flash.start(0xfffffff0,true,true));assert(!flash.start(0xfffffff1,true,true));assert(!flash.finished(50,true,true));assert(flash.finished(180,true,true));assert(flash.finished(1,false,true));assert(flash.finished(1,true,false));

 assert(crc32(reinterpret_cast<const uint8_t*>("123456789"),9)==0xcbf43926);
 uint8_t raw[MAX_RAW],wire[MAX_ENCODED+1];Frame f;f.channel=COMMAND;f.kind=REQUEST;f.op=FILE_START;f.session=0x12345678;f.request=7;f.sequence=9;f.length=4;memcpy(f.payload,"\0abc",4);
 size_t n=encode(f,raw,wire);for(size_t i=0;i<n;i++)printf("%02x",wire[i]);puts("");
 Decoder decoder;Frame decoded;unsigned received=0;
 for(size_t i=0;i<n;i++)if(decoder.byte(wire[i],decoded))++received;
 assert(received==1&&decoded.length==4&&!memcmp(f.payload,decoded.payload,4)&&decoded.session==f.session);
 std::mt19937 rng(71);
 for(unsigned k=0;k<10000;k++){
  for(unsigned j=0,n=rng()%10000;j<n;j++)decoder.byte(rng(),decoded);
  decoder.byte(0,decoded);received=0;for(size_t i=0;i<n;i++)received+=decoder.byte(wire[i],decoded);
  assert(received==1&&decoder.used==0);
 }
 for(size_t j=0;j<n-1;j++){
  uint8_t save=wire[j];wire[j]^=128;for(size_t i=0;i<n;i++)decoder.byte(wire[i],decoded);wire[j]=save;
  received=0;for(size_t i=0;i<n;i++)received+=decoder.byte(wire[i],decoded);assert(received==1);
 }
 for(unsigned i=0;i<MAX_ENCODED*3;i++)decoder.byte(65,decoded);
 assert(decoder.discard&&decoder.used<=MAX_ENCODED);decoder.byte(0,decoded);
 assert(decoder.oversized>0);
 puts("Host_Connection codec: vectors, 10000 fuzz rounds, corruption, overflow and recovery passed");
}
