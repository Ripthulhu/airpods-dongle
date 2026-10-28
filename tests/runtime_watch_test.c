#include <assert.h>
#include <stdio.h>
#include "../src/runtime_watch.h"
int main(void) {
    _Static_assert(WATCH_BUTTON==2 && WATCH_LOCK==3 && WATCH_IDLE==15,
                   "Existing captured stage numbers must remain stable");
    _Static_assert(!(RUNTIME_WATCH_STAGE_MASK &
                     (RUNTIME_WATCH_USB|RUNTIME_WATCH_FAULT|RUNTIME_WATCH_CORE1)),
                   "Stage updates must not overwrite concurrent context bits");
    uint32_t r[8]={0,0,0,0,11,12,13,14};
    for(unsigned stage=WATCH_BOOTSEL_ENTER;stage<=WATCH_BUTTON_ACTION_UNLOCK;++stage) {
        r[0]=RUNTIME_WATCH_MAGIC|RUNTIME_WATCH_USB;
        runtime_watch_mark(r,stage,stage*10u);
        assert(r[0]==(RUNTIME_WATCH_MAGIC|RUNTIME_WATCH_USB|stage));
        assert(r[1]==stage*10u);
    }
    r[0]=0;
    runtime_watch_mark(r,WATCH_ENCODER,123);
    assert(r[0]==(RUNTIME_WATCH_MAGIC|WATCH_ENCODER) && r[1]==123);
    r[0]|=RUNTIME_WATCH_USB;
    runtime_watch_mark(r,WATCH_PACKET,456);
    assert(r[0]&RUNTIME_WATCH_USB);
    r[0]|=RUNTIME_WATCH_CORE1;
    runtime_watch_mark(r,WATCH_BOOTSEL_EXIT,456);
    assert((r[0]&(RUNTIME_WATCH_USB|RUNTIME_WATCH_CORE1))==
           (RUNTIME_WATCH_USB|RUNTIME_WATCH_CORE1));
    runtime_watch_fault(r,0x200,0x40000000);
    assert((r[0]&RUNTIME_WATCH_FAULT) && r[1]==456 && r[2]==0x200 && r[3]==0x40000000);
    uint32_t fault_state=r[0];runtime_watch_mark(r,WATCH_IDLE,999);
    assert(r[0]==fault_state && r[1]==456);
    for(unsigned stage=WATCH_BOOTSEL_ENTER;stage<=WATCH_BUTTON_ACTION_UNLOCK;++stage) {
        runtime_watch_mark(r,stage,1000u+stage);
        assert(r[0]==fault_state && r[1]==456);
    }
    assert(r[4]==11 && r[5]==12 && r[6]==13 && r[7]==14);
    puts("runtime watch: stage, USB context, fault evidence and SDK scratch isolation passed");
    return 0;
}
