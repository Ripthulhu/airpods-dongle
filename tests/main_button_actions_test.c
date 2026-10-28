// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include "../src/runtime_watch.h"

typedef unsigned async_context_t;
static async_context_t context;
static unsigned locks,claims,reconnects,scans,games,longs,phone_pairs;
static bool claim_ok=true;
static uint8_t peer[6]={1,2,3,4,5,6};
static async_context_t *cyw43_arch_async_context(void){return &context;}
static void async_context_acquire_lock_blocking(async_context_t *ctx){assert(ctx==&context && !locks);++locks;}
static void async_context_release_lock(async_context_t *ctx){assert(ctx==&context && locks==1);--locks;}
static const uint8_t *get_device_addr(void){return peer;}
static bool aacp_takeover_request(const uint8_t *selected){assert(locks==1 && selected==peer);++claims;return claim_ok;}
static void a2dp_source_reconnect(void){assert(locks==1);++reconnects;}
static void gap_start_scanning(void){assert(locks==1);++scans;}
static bool dongle_playback_game_toggle(void){assert(locks==1);++games;return true;}
static void avdtp_disconnect_and_scan(void){assert(locks==1);++longs;}
static void bt_hci_phone_pairing_start(void){assert(locks==1);++phone_pairs;}
#include "main_button_actions.inc"

int main(void){
 on_single_press();assert(!claims && !reconnects);process_button_actions();
 assert(claims==1 && reconnects==1 && !scans && !locks);
 process_button_actions();assert(claims==1 && reconnects==1);
 claim_ok=false;on_single_press();process_button_actions();assert(claims==2 && reconnects==1);
 for(unsigned i=0;i<6;++i)peer[i]=0;
 on_single_press();process_button_actions();assert(scans==1 && claims==2 && reconnects==1);
 on_double_press();on_long_press();atomic_store(&btn_phone_pending,true);process_button_actions();
 assert(games==1 && longs==1 && phone_pairs==1 && !locks);
 puts("BOOTSEL actions: explicit paired takeover, unpaired scan, deferred lock and unchanged double/long/phone actions passed");
}
