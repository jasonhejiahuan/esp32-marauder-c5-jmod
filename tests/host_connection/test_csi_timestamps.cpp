#include "CsiMotionModel.h"
#include <cassert>
int main(){
 CsiMotionModel m;int8_t sample[128]{};for(int i=0;i<128;i+=4){sample[i]=20;sample[i+2]=10;}
 m.reset(1000,1000);
 for(unsigned t=1000;t<2000;t+=20)m.addSample(sample,128,-55,t);
 m.tick(2000,50,0);assert(m.samplesPerSecond()==50);
 m.addSample(sample,128,-55,1999);m.tick(1999,50,0);
 assert(m.samplesPerSecond()==50);
 for(unsigned t=2000;t<3000;t+=20)m.addSample(sample,128,-55,t);
 m.tick(3000,100,0);assert(m.samplesPerSecond()==50);
 m.reset(0xfffffe00U,1000);
 m.addSample(sample,128,-55,0xffffff00U);m.tick(0x200U,4,0);
 assert(m.samplesPerSecond()>0);
}
