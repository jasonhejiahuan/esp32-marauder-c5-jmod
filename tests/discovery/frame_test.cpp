#include "../../esp32_marauder/DiscoveryFrame.h"
#include <vector>
#include <cassert>
#include <random>
#include <iostream>
using namespace discovery;
std::vector<uint8_t> beacon(){std::vector<uint8_t> p(36);p[0]=0x80;p[16]=2;p[21]=1;p[34]=0x11;p.insert(p.end(),{0,3,'a','b','c'});return p;}
int main(){
 auto p=beacon();Observation o{};assert(decode(p.data(),p.size(),o)&&o.kind=='a'&&o.security==1&&o.ssidLen==3);
 p[34]=1;o={};assert(decode(p.data(),p.size(),o)&&o.security==0);
 p.insert(p.end(),{48,20,1,0,0,15,172,4,1,0,0,15,172,4,1,0,0,15,172,2,0,0});o={};assert(decode(p.data(),p.size(),o)&&o.security==3);
 p[p.size()-3]=8;o={};assert(decode(p.data(),p.size(),o)&&o.security==4);
 p[43]=255;o={};assert(!decode(p.data(),p.size(),o));
 auto primary=beacon();primary.insert(primary.end(),{3,1,6});o={};assert(decode(primary.data(),primary.size(),o)&&o.primary==6);
 auto q=beacon();for(size_t n=0;n<q.size();n++){o={};assert(!decode(q.data(),n,o));}
 q[37]=33;o={};assert(!decode(q.data(),q.size(),o));
 std::vector<uint8_t>d(24);d[0]=8;d[1]=1;d[4]=2;d[9]=1;d[10]=4;d[15]=2;o={};assert(decode(d.data(),d.size(),o)&&o.kind=='s'&&o.ap[0]==2&&o.mac[0]==4);
 d[1]=2;o={};assert(decode(d.data(),d.size(),o)&&o.mac[0]==2&&o.ap[0]==4);
 d[4]=255;o={};assert(!decode(d.data(),d.size(),o));d[1]=3;o={};assert(!decode(d.data(),d.size(),o));
 std::mt19937 rng(123);for(int k=0;k<100000;k++){std::vector<uint8_t>f(rng()%512);for(auto& b:f)b=rng();o={};decode(f.data(),f.size(),o);}
 std::cout<<"PASS: bounded Beacon/RSN, malformed frames, data direction, multicast exclusion, 100000 fuzz frames\n";
}
