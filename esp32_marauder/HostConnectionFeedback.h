#pragma once
#include <stdint.h>
namespace host_connection {
struct FeedbackFlash {
  bool active=false;
  uint32_t until=0;
  bool start(uint32_t now,bool idle,bool enabled) {
    if(active||!idle||!enabled)return false;
    active=true;until=now+180;return true;
  }
  bool finished(uint32_t now,bool idle,bool enabled)const {
    return active&&(!idle||!enabled||int32_t(now-until)>=0);
  }
};
}
