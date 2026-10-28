// SPDX-License-Identifier: GPL-3.0-only
#ifndef PHONE_CALL_TEST_HFP_HF_H
#define PHONE_CALL_TEST_HFP_HF_H
#include "btstack.h"
uint8_t hfp_hf_init(uint8_t);
void hfp_hf_init_supported_features(uint32_t);
void hfp_hf_init_codecs(uint8_t,const uint8_t*);
uint8_t hfp_hf_set_default_speaker_gain(uint8_t);
uint8_t hfp_hf_set_default_microphone_gain(uint8_t);
void hfp_hf_register_packet_handler(btstack_packet_handler_t);
void hfp_hf_create_sdp_record_with_codecs(uint8_t*,uint32_t,int,const char*,uint16_t,uint8_t,const uint8_t*);
uint8_t hfp_hf_establish_service_level_connection(uint8_t*);
uint8_t hfp_hf_release_service_level_connection(uint16_t);
uint8_t hfp_hf_release_audio_connection(uint16_t);
uint8_t hfp_hf_answer_incoming_call(uint16_t);
uint8_t hfp_hf_terminate_call(uint16_t);
#endif
