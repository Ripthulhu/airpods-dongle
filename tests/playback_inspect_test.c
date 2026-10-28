#include <assert.h>
#include <stdio.h>
#include "../src/btstack/playback_inspect.h"
int main(void) {
    playback_inspect_t s={0};uint8_t out[400];
    int16_t pcm[]={0,INT16_MIN,INT16_MAX,10};
    playback_inspect_samples(&s,pcm,4,false,true,123);
    assert(s.stats[0]==4 && s.stats[1]==3 && s.stats[2]==32768 && s.stats[3]==123);
    float f[]={NAN,INFINITY,0,0.5f,-2};
    playback_inspect_samples(&s,f,5,true,true,124);
    assert(s.stats[0]==9 && s.stats[1]==5 && s.stats[2]==32768);
    playback_inspect_samples(&s,pcm,4,false,false,125);
    assert(s.stats[4]==4 && s.stats[5]==3 && s.stats[6]==32768);
    const uint8_t p[]={0xbf,0xff,0x10,2,0xaa,0xbb,0xb0,0,0x10,1,0xcc};
    assert(playback_inspect_media(&s,p,sizeof(p),456));
    assert(s.stats[8]==2 && s.stats[9]==456 && s.stats[10]==1 && s.stats[12]==0 && s.au[0]==0xcc);
    assert(!playback_inspect_media(&s,p,sizeof(p)-1,457) && s.bad_packets==1 && s.stats[8]==2);
    uint8_t asc[]={0xf8,0xe6,0x50,0};
    assert(!playback_inspect_pack(&s,out,399,1,2,asc,4));
    assert(playback_inspect_pack(&s,out,400,1,2,asc,4)==400);
    assert(out[0]==1 && out[1]==3 && out[3]==4 && out[68]==0xf8 && out[80]==0xcc && out[76]==1);
    uint8_t large[325]={0xb0,1,0x11,0x41};
    assert(playback_inspect_media(&s,large,sizeof(large),500));
    assert(playback_inspect_pack(&s,out,400,1,2,asc,4)==400 && out[2]==1);
    puts("audio inspection: signal levels, nonfinite input, submitted AU bounds, malformed packets and snapshot passed");
    return 0;
}
