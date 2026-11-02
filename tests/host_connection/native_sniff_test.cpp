#include "NativeSniffRequest.h"
#include <cassert>
#include <iostream>
using namespace host_connection;
int main(){NativeSniffRequest r{};
 for(uint8_t m=1;m<=11;m++){uint8_t p[]={m,0,0};assert(r.decode(p,3));assert(r.nativeFilter()==(m!=1&&m!=10&&m!=11));}
 for(unsigned m=0;m<256;m++)for(unsigned flags=0;flags<256;flags++){
  uint8_t p[]={uint8_t(m),uint8_t(flags),uint8_t(flags&1?6:0)};
  bool ok=m>=1&&m<=11&&flags<=7&&(m==5||flags==0);assert(r.decode(p,3)==ok);
 }
 uint8_t good[]={5,7,36};assert(r.decode(good,3));assert(r.flags==7&&r.channel==36);
 assert(!r.decode(good,2));assert(!r.decode(good,4));
 uint8_t invalid[]={5,1,0};assert(!r.decode(invalid,3));invalid[2]=255;assert(!r.decode(invalid,3));
 std::cout<<"PASS: Host_Connection native sniff request: exhaustive mode/flag bounds and routing policy"<<std::endl;
}
