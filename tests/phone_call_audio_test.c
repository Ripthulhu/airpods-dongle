// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/btstack/phone_call_audio.c"

static bool route_ready;
static uint32_t bridge_frames, mic_available, mic_generated;
bool bridge_call_ready(void) { return route_ready; }
void bridge_call_pcm_push(const int16_t *pcm, uint16_t frames) {
    assert(route_ready && frames <= 128);
    for (unsigned i=0;i<frames;++i) assert(pcm[2*i]==pcm[2*i+1]);
    bridge_frames+=frames;
}
uint32_t aacp_mic_call_read(int16_t *pcm, uint32_t capacity) {
    assert(capacity==256);
    uint32_t count=mic_available<capacity?mic_available:capacity;
    for(unsigned i=0;i<count;++i)pcm[i]=(int16_t)(1000+(mic_generated++%1000));
    mic_available-=count;
    return count;
}
static void test_interpolation_and_chunks(void) {
    int16_t input[80], whole[960], split[960];
    for(unsigned i=0;i<80;++i)input[i]=(int16_t)(((int)i%21-10)*1200);
    for(unsigned rate=8000;rate<=16000;rate*=2) {
        unsigned ratio=48000/rate, frames=80*ratio;
        phone_call_audio_set_gain(15,15);
        assert(phone_call_audio_reset(rate));
        assert(phone_call_audio_downlink(input,80,false));
        phone_call_audio_read(whole,(uint16_t)frames);
        assert(!audio.down.used && !audio.phase);
        assert(phone_call_audio_reset(rate));
        for(unsigned offset=0;offset<80;) {
            unsigned count=offset%7+1;if(count>80-offset)count=80-offset;
            assert(phone_call_audio_downlink(input+offset,(uint16_t)count,false));offset+=count;
        }
        for(unsigned offset=0;offset<frames;) {
            unsigned count=offset%11+1;if(count>frames-offset)count=frames-offset;
            phone_call_audio_read(split+2*offset,(uint16_t)count);offset+=count;
        }
        assert(!memcmp(whole,split,frames*2*sizeof(*whole)));
        for(unsigned i=0;i<frames;++i) {
            assert(whole[2*i]==whole[2*i+1]);
            if((i+1)%ratio==0)assert(whole[2*i]==input[i/ratio]);
        }
        unsigned mic_ratio=64000/rate;
        int16_t mono[480], a[160], b[160];
        for(unsigned i=0;i<480;++i)mono[i]=(int16_t)(12000*sin(6.283185307179586*1000*i/64000));
        phone_call_audio_reset(rate);phone_call_audio_mic(mono,256);phone_call_audio_mic(mono+256,224);
        assert(audio.up.used==480/mic_ratio);phone_call_audio_uplink(a,(uint16_t)(480/mic_ratio));
        phone_call_audio_reset(rate);
        for(unsigned offset=0;offset<480;) {
            unsigned count=offset%13+1;if(count>480-offset)count=480-offset;
            phone_call_audio_mic(mono+offset,(uint16_t)count);offset+=count;
        }
        phone_call_audio_uplink(b,(uint16_t)(480/mic_ratio));
        assert(!memcmp(a,b,(480/mic_ratio)*sizeof(*a)));
    }
}
static double mic_rms(unsigned rate, unsigned frequency) {
    phone_call_audio_reset(rate);phone_call_audio_set_gain(15,15);
    int16_t input[240], output[80];double sum=0;unsigned measured=0;
    for(unsigned block=0;block<100;++block) {
        for(unsigned i=0;i<240;++i)input[i]=(int16_t)(16000*sin(6.283185307179586*frequency*(block*240+i)/64000));
        phone_call_audio_mic(input,240);
        unsigned count=rate*3/800;assert(audio.up.used==count);
        phone_call_audio_uplink(output,(uint16_t)count);
        if(block<5)continue;
        for(unsigned i=0;i<count;++i){sum+=(double)output[i]*output[i];++measured;}
    }
    return sqrt(sum/measured);
}
static void test_filter_dc_alias_and_saturation(void) {
    for(unsigned rate=8000;rate<=16000;rate*=2) {
        double pass=mic_rms(rate,1000), edge=mic_rms(rate,rate/2), stop=mic_rms(rate,rate*5/8);
        assert(pass>11000 && pass<11500 && edge<pass*.07 && stop<pass*.01);
        int16_t input[240],output[80];
        for(unsigned i=0;i<240;++i)input[i]=30000;
        phone_call_audio_reset(rate);phone_call_audio_mic(input,240);
        phone_call_audio_uplink(output,(uint16_t)(rate*3/800));
        assert(output[rate*3/800-1]==30000); // Exact unity DC after filter history fills.
    }
    assert(rounded_sample(1,2)==1 && rounded_sample(-1,2)==-1);
    assert(rounded_sample(32768,1)==INT16_MAX && rounded_sample(-32769,1)==INT16_MIN);
    int16_t input[128]={0}, output[16];
    for(unsigned sign=0;sign<2;++sign) {
        phone_call_audio_reset(8000);
        for(unsigned tap=0;tap<127;++tap)
            input[127-tap]=((filter8[tap]>=0)^sign)?INT16_MAX:INT16_MIN;
        phone_call_audio_mic(input,128);phone_call_audio_uplink(output,16);
        assert(output[15]==(sign?INT16_MIN:INT16_MAX)); // FIR ringing saturates instead of wrapping.
    }
}
static void test_bounds_loss_reset_and_gains(void) {
    int16_t input[257],output[512*2];for(unsigned i=0;i<257;++i)input[i]=3000;
    phone_call_audio_set_gain(15,15);phone_call_audio_reset(8000);
    assert(!phone_call_audio_downlink(NULL,1,false));
    assert(!phone_call_audio_downlink(input,257,false) && !audio.down.used);
    phone_call_audio_mic(input,257);phone_call_audio_mic(NULL,1);
    assert(!audio.up.used && !phone_call_audio_mic_samples());
    assert(phone_call_audio_downlink(NULL,60,true));
    phone_call_audio_read(output,360);for(unsigned i=0;i<720;++i)assert(!output[i]);
    assert(phone_call_audio_downlink(input,256,false));
    for(unsigned i=0;i<256;++i)input[i]=-6000;
    assert(phone_call_audio_downlink(input,256,false));
    assert(phone_call_audio_downlink(input,256,false) && audio.down.used==PHONE_CALL_AUDIO_FIFO_SAMPLES);
    phone_call_audio_read(output,12);
    assert(output[22]==-6000 && audio.down.used==PHONE_CALL_AUDIO_FIFO_SAMPLES-2);
    phone_call_audio_set_gain(7,7);phone_call_audio_reset(8000);
    input[0]=3000;assert(phone_call_audio_downlink(input,1,false));
    phone_call_audio_read(output,6);assert(output[10]==1400 && speaker_gain==7 && microphone_gain==7);
    for(unsigned i=0;i<240;++i)input[i]=30000;
    phone_call_audio_mic(input,240);phone_call_audio_uplink(output,30);
    assert(output[29]==14000 && phone_call_audio_mic_samples()==240);
    phone_call_audio_set_gain(0,0);
    assert(phone_call_audio_downlink(input,1,false));phone_call_audio_read(output,6);
    phone_call_audio_mic(input,240);phone_call_audio_uplink(output+12,40);
    for(unsigned i=0;i<52;++i)assert(!output[i]);
    phone_call_audio_set_gain(255,255);assert(speaker_gain==15 && microphone_gain==15);
    assert(!phone_call_audio_reset(44100) && !audio.down_ratio && !audio.mic_ratio);
    memset(output,0x55,sizeof(output));phone_call_audio_read(output,12);phone_call_audio_uplink(output+24,12);
    for(unsigned i=0;i<36;++i)assert(!output[i]);
    phone_call_audio_reset(16000);phone_call_audio_mic(input,1);assert(audio.mic_phase==1);
    phone_call_audio_reset(0);assert(!audio.mic_phase && !audio.history_write && !audio.down.used && !audio.up.used);
    assert(!phone_call_audio_mic_samples());
    phone_call_audio_read(NULL,1);phone_call_audio_uplink(NULL,1);
    phone_call_audio_reset(8000);memset(input,0,sizeof(input));phone_call_audio_mic(input,12);
    assert(phone_call_audio_mic_samples()==12); // Valid silent PCM is microphone progress.
    microphone_samples=UINT32_MAX-3u;phone_call_audio_mic(input,8);
    assert(phone_call_audio_mic_samples()==4);
}
static void test_poll_fairness_and_8ms_flow(void) {
    int16_t packet[120], uplink[120];for(unsigned i=0;i<120;++i)packet[i]=3000;
    for(unsigned rate=8000;rate<=16000;rate*=2) for(unsigned period=7500;period<=8000;period+=500) {
        phone_call_audio_reset(rate);phone_call_audio_set_gain(7,7);
        bridge_frames=mic_available=mic_generated=0;route_ready=false;
        phone_call_audio_downlink(packet,60,false);phone_call_audio_poll();assert(!audio.down.used);
        route_ready=true;phone_call_audio_poll();assert(speaker_gain==7 && microphone_gain==7);
        mic_available=480;phone_call_audio_poll();
        uint32_t mic_frames=480, received=0, sent=0, consumed=0;
        unsigned next_packet=7500,next_mic=7500;
        for(unsigned now=period;now<=4000000;now+=period) {
            while(next_mic<=now){mic_available+=480;mic_frames+=480;next_mic+=7500;}
            unsigned packets=0;
            while(next_packet<=now) {
                assert(phone_call_audio_downlink(packet,(uint16_t)(rate*75/10000),false));
                received+=rate*75/10000;++packets;next_packet+=7500;
            }
            uint32_t before=bridge_frames;phone_call_audio_poll();
            assert(bridge_frames-before<=512 && mic_available<=192);
            while(packets--) {
                unsigned count=rate*75/10000;assert(audio.up.used>=count);
                unsigned before_read=audio.up.used;
                phone_call_audio_uplink(uplink,(uint16_t)count);sent+=count;
                consumed+=before_read-audio.up.used;
            }
            unsigned pending=audio.down.used*audio.down_ratio+(audio.phase?audio.down_ratio-audio.phase:0);
            assert(received*audio.down_ratio==bridge_frames+pending); // No source FIFO overflow/drop.
            assert((mic_frames-mic_available)/audio.mic_ratio==consumed+audio.up.used);
            assert(consumed+rate/50>sent && sent+rate/50>consumed);
        }
        assert(phone_call_audio_mic_samples()==mic_frames-mic_available);
        uint32_t progress=phone_call_audio_mic_samples();
        route_ready=false;phone_call_audio_poll();assert(!audio.down.used && !audio.up.used && !audio.phase);
        assert(phone_call_audio_mic_samples()==progress);
        mic_available=0;route_ready=true;phone_call_audio_poll();assert(phone_call_audio_mic_samples()==progress);
        assert(speaker_gain==7 && microphone_gain==7);
    }
    phone_call_audio_set_gain(15,15);
}

static int16_t clock_sample(uint32_t index) {
    unsigned position=index%4000;
    return (int16_t)((position<2000?(int)position-1000:3000-(int)position)*16);
}
static void test_ten_minute_mic_clocks(void) {
    const int drifts[]={0,100,-100,500,-500};
    unsigned all_lowest=512,all_highest=0;
    for(unsigned rate=8000;rate<=16000;rate*=2) for(unsigned mode=0;mode<5;++mode) {
        int ppm=drifts[mode];phone_call_audio_reset(rate);phone_call_audio_set_gain(15,15);
        uint32_t produced=0,consumed=0,outputs=0,next_mic=7500,next_sco=7500;
        uint64_t source_fraction=0;
        for(unsigned i=0;i<rate*75/10000;++i)fifo_push(&audio.up,clock_sample(produced++));
        unsigned lowest=512,highest=0;
        int16_t pcm[120];
        for(uint32_t now=8000;now<=600000000;now+=8000) {
            // 480 nominal source64k frames per7.5ms, after the separately tested FIR.
            while(next_mic<=now) {
                source_fraction+=(uint64_t)rate*75u*(1000000+ppm);
                unsigned count=(unsigned)(source_fraction/10000000000ull);source_fraction%=10000000000ull;
                assert(count<=PHONE_CALL_AUDIO_FIFO_SAMPLES-audio.up.used);
                for(unsigned i=0;i<count;++i)fifo_push(&audio.up,clock_sample(produced++));
                next_mic+=7500;
            }
            if(audio.up.used>highest)highest=audio.up.used;
            while(next_sco<=now) {
                unsigned count=rate*75/10000,before=audio.up.used;
                assert(before>=count+2);
                uint32_t fraction=audio.uplink_fraction;
                phone_call_audio_uplink(pcm,(uint16_t)count);
                assert(audio.uplink_step>=65536-65 && audio.uplink_step<=65536+65);
                for(unsigned i=0;i<count;++i) {
                    double blend=clock_sample(consumed)*(1.0-fraction/65536.0)+
                                 clock_sample(consumed+1)*(fraction/65536.0);
                    assert(pcm[i]==(int16_t)llround(blend));
                    fraction+=audio.uplink_step;consumed+=fraction>>16;fraction&=0xffff;
                }
                outputs+=count;
                assert(audio.uplink_fraction==fraction && produced==consumed+audio.up.used);
                assert(before-audio.up.used<=count+1 && before-audio.up.used>=count-1);
                if(audio.up.used<lowest)lowest=audio.up.used;
                next_sco+=7500;
            }
        }
        int64_t ideal=(int64_t)outputs*(1000000+ppm)/1000000;
        assert(llabs((int64_t)consumed-ideal)<rate/50);
        assert(lowest>0 && highest<PHONE_CALL_AUDIO_FIFO_SAMPLES);
        if(lowest<all_lowest)all_lowest=lowest;
        if(highest>all_highest)all_highest=highest;
    }
    printf("mic clocks: 10min each at 8/16k, 0,+/-100,+/-500ppm; FIFO range %u..%u of512\n",all_lowest,all_highest);
    phone_call_audio_reset(16000);audio.uplink_fraction=12345;audio.uplink_step=65536+65;
    audio.uplink_average_q8=99999;
    assert(phone_call_audio_downlink(NULL,120,true));
    assert(audio.uplink_step==65536 && audio.uplink_average_q8==200*256 && audio.uplink_fraction==12345);
    int16_t pcm[120];phone_call_audio_uplink(pcm,120);
    for(unsigned i=0;i<120;++i)assert(!pcm[i]);
    assert(!audio.uplink_fraction && audio.uplink_step==65536);
    for(unsigned i=0;i<512;++i)fifo_push(&audio.up,1000);
    audio.uplink_step=65536+65;fifo_push(&audio.up,1000);
    assert(audio.uplink_step==65536 && audio.up.used==512);
}
int main(void) {
    test_interpolation_and_chunks();
    test_filter_dc_alias_and_saturation();
    test_bounds_loss_reset_and_gains();
    test_poll_fairness_and_8ms_flow();
    test_ten_minute_mic_clocks();
    puts("call PCM: 8/16k phone, 48k speaker, 64k mic, chunks, antialias, loss, gains and 8ms flow passed");
}
