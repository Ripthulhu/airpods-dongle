#ifndef AIRPODS_PLAYBACK_TEST_HOOKS_H
#define AIRPODS_PLAYBACK_TEST_HOOKS_H
#include "../src/btstack/playback_trace.h"
#include "../src/btstack/playback_usb_clock.h"
#include "../src/btstack/playback_bitrate.h"
static playback_bitrate_t playback_bitrate;
static uint32_t bitrate_pressure;
#define playback_bitrate_transport_stalled() ((void)0)
#define playback_bitrate_apply(context) ((void)(context), true)
#define PB_ADD(stat,value) ((void)(stat),(void)(value))
#define PB_MAX(stat,value) ((void)(stat),(void)(value))
#define PB_EVENT(kind,a,b) ((void)(kind),(void)(a),(void)(b))
static uint32_t packet_oldest_us,media_last_sent_us;
static bool playback_work_pending;
static bool playback_transport_stale;
#endif
