// SPDX-License-Identifier: GPL-3.0-only
#ifndef AIRPODS_PLAYBACK_OPTIONS_H
#define AIRPODS_PLAYBACK_OPTIONS_H

#include <stdbool.h>
#include <stdint.h>
#include "playback_eld.h"
#include "../tinyusb/dongle_protocol_spec.h"

typedef struct { uint8_t codec, rate_mode; uint32_t bitrate; } playback_options_t;
static inline bool playback_options_valid(playback_options_t p) {
    if (p.codec>DONGLE_AUDIO_PROFILE_REQUESTED_CODEC_AAC_ELD ||
        p.rate_mode>DONGLE_AUDIO_PROFILE_RATE_MODE_VARIABLE_BITRATE) return false;
    if (p.codec<=DONGLE_AUDIO_PROFILE_REQUESTED_CODEC_SBC) return !p.rate_mode && !p.bitrate;
    if (p.codec==DONGLE_AUDIO_PROFILE_REQUESTED_CODEC_AAC && p.rate_mode) return false;
    if (!p.bitrate) return true;
    if (p.codec==DONGLE_AUDIO_PROFILE_REQUESTED_CODEC_AAC && p.bitrate==96000) return true;
    if (p.codec==DONGLE_AUDIO_PROFILE_REQUESTED_CODEC_AAC && p.bitrate>220000) return false;
    return p.bitrate==128000 || p.bitrate==174000 || p.bitrate==192000 ||
           p.bitrate==220000 || p.bitrate==256000;
}
static inline bool playback_options_aac_supported(playback_options_t p, uint8_t objects,
                                                   uint16_t rates, uint8_t channels,
                                                   uint32_t ceiling) {
    if (!playback_options_valid(p) || p.codec!=DONGLE_AUDIO_PROFILE_REQUESTED_CODEC_AAC || !(objects&0x60) ||
        !(rates&8) || !(channels&4)) return false;
    uint32_t desired=p.bitrate ? p.bitrate : 220000;
    return (!p.bitrate || !ceiling || desired<=ceiling) && (!ceiling || ceiling>=96000);
}
// Negative answers require a complete current-peer catalog. A partial catalog
// is unknown and must be negotiated, rather than disconnecting on a guess.
static inline bool playback_options_preflight(playback_options_t p, bool complete,
                                               uint8_t matching_codecs) {
    return playback_options_valid(p) && (!p.codec || !complete ||
        (matching_codecs&(uint8_t)(1u<<p.codec))!=0);
}
static inline uint32_t playback_options_eld_rate(const uint8_t *config, unsigned size,
                                                 playback_options_t p) {
    if (!playback_options_valid(p) || (p.codec!=DONGLE_AUDIO_PROFILE_REQUESTED_CODEC_AUTOMATIC && p.codec!=DONGLE_AUDIO_PROFILE_REQUESTED_CODEC_AAC_ELD) ||
        !playback_eld_configuration(config,size) ||
        (p.rate_mode && !playback_eld_variable(config))) return 0;
    uint32_t ceiling=playback_eld_bitrate(config);
    uint32_t rate=p.bitrate ? p.bitrate : ceiling;
    if (rate>ceiling) return 0;
    if (p.rate_mode) rate=rate/800*800;
    else if (playback_eld_variable(config)) {
        // CBR may need one alignment byte beyond its nominal 10-ms AU budget.
        uint32_t maximum=ceiling/800*800-800;
        if (rate>maximum) rate=maximum;
    }
    return rate>=PLAYBACK_ELD_MIN_BITRATE ? rate : 0;
}
static inline bool playback_options_eld(const uint8_t *cap, unsigned size,
                                         playback_options_t p, uint8_t out[14]) {
    if (!playback_options_valid(p) || (p.codec!=DONGLE_AUDIO_PROFILE_REQUESTED_CODEC_AUTOMATIC && p.codec!=DONGLE_AUDIO_PROFILE_REQUESTED_CODEC_AAC_ELD) ||
        !playback_eld_negotiate(cap,size,out)) return false;
    uint32_t ceiling=playback_eld_bitrate(out);
    uint32_t rate=p.bitrate ? p.bitrate : ceiling;
    if (rate>ceiling || (p.rate_mode && !playback_eld_variable(out))) return false;
    if (!p.rate_mode && playback_eld_variable(out) &&
        ceiling/800*800-800<PLAYBACK_ELD_MIN_BITRATE) out[11]&=0x7f;
    if (!playback_eld_variable(out)) {
        out[11]=(uint8_t)(rate>>16);
        out[12]=(uint8_t)(rate>>8); out[13]=(uint8_t)rate;
    }
    return playback_options_eld_rate(out,size,p)!=0;
}
static inline bool playback_options_eld_configuration(const uint8_t *p, unsigned n,
                                                       playback_options_t options) {
    if (!playback_options_eld_rate(p,n,options)) return false;
    return !options.bitrate || playback_eld_variable(p) ||
           playback_eld_bitrate(p)==options.bitrate;
}

// Game Mode phases are published unchanged as the audio profile's game_mode_state.
enum {
    GAME_OFF=DONGLE_AUDIO_PROFILE_GAME_MODE_STATE_OFF,
    GAME_PENDING=DONGLE_AUDIO_PROFILE_GAME_MODE_STATE_PENDING,
    GAME_SENT=DONGLE_AUDIO_PROFILE_GAME_MODE_STATE_SENT,
    GAME_CONFIRMED=DONGLE_AUDIO_PROFILE_GAME_MODE_STATE_CONFIRMED,
    GAME_FAILED=DONGLE_AUDIO_PROFILE_GAME_MODE_STATE_FAILED,
    GAME_UNSUPPORTED=DONGLE_AUDIO_PROFILE_GAME_MODE_STATE_UNSUPPORTED,
    GAME_RESTORING=DONGLE_AUDIO_PROFILE_GAME_MODE_STATE_RESTORING,
};
typedef struct {
    bool requested, changed, failed, restore_failed, active, returning_to_usb;
    bool waiting_tx, waiting_report, low_latency_confirmed, restore_admitting;
    uint8_t phase;
    uint16_t target;
    uint32_t active_since, sent_since, tx_revision, report_revision, usb_start_revision;
} playback_game_t;
// One post-priming USB check; TX/report/adjustment remain in playback_game_t.
typedef struct {
    uint32_t session, usb_revision, tx_revision, since;
    uint16_t cid;
    uint8_t seid, peer[6];
    bool needed, queued, unverified;
} playback_usb_verification_t;
static inline void playback_game_request(playback_game_t *s, bool enabled) {
    if (!enabled && (s->failed || s->restore_failed) && !s->waiting_tx && !s->waiting_report) {
        // A local clock fault can follow a valid normal-buffer report; remeasure on retry.
        s->requested=s->failed=s->restore_failed=s->restore_admitting=false;
        if (!s->changed) s->target=0;
        s->phase=GAME_RESTORING;
        return;
    }
    if (s->requested==enabled) return;
    s->returning_to_usb=false;
    s->requested=enabled;
    if (enabled) {
        bool retry_restore=s->restore_failed && s->changed;
        s->failed=s->restore_failed=false;
        if (retry_restore) { s->waiting_tx=s->waiting_report=false; s->phase=GAME_RESTORING; }
    }
    else if (s->restore_failed) { s->restore_failed=false; s->waiting_tx=s->waiting_report=false; }
    if (!s->changed) s->phase=enabled ? GAME_PENDING : GAME_OFF;
}
static inline void playback_game_queued(playback_game_t *s, uint16_t target,
                                         uint32_t now, uint32_t tx_revision) {
    s->restore_admitting=false;
    s->target=target; s->changed=true; s->waiting_tx=true; s->waiting_report=false;
    if (target!=60) s->low_latency_confirmed=false;
    s->sent_since=now; s->tx_revision=tx_revision;
    s->phase=target==60 ? GAME_PENDING : GAME_RESTORING;
}
static inline void playback_game_observe_send(playback_game_t *s, uint32_t now,
                                               uint32_t tx_revision, uint16_t last,
                                               uint32_t report_revision) {
    if (!s->waiting_tx || tx_revision==s->tx_revision || last!=s->target) return;
    s->waiting_tx=false; s->waiting_report=true; s->sent_since=now;
    s->report_revision=report_revision;
    s->phase=s->target==60 ? GAME_SENT : GAME_RESTORING;
}
static inline void playback_game_session_lost(playback_game_t *s) {
    s->returning_to_usb=false;
    s->low_latency_confirmed=s->restore_admitting=false;
    s->waiting_tx=s->waiting_report=false;
    if (s->changed) s->phase=GAME_RESTORING;
    else { s->target=0; s->phase=s->requested ? GAME_PENDING : GAME_OFF; }
}
static inline uint16_t playback_game_queue_restore(playback_game_t *s, uint32_t now) {
    s->phase=GAME_RESTORING;
    if (!s->restore_admitting) { s->restore_admitting=true; s->sent_since=now; }
    if ((uint32_t)(now-s->sent_since)<2000u) return 150;
    s->restore_admitting=false;
    s->failed=s->restore_failed=true;
    s->phase=GAME_FAILED;
    return 0;
}
// Returns a bounded 60/150-ms command; transport acceptance is recorded separately.
static inline uint16_t playback_game_step(playback_game_t *s, bool available,
                                           bool eligible, bool active, uint32_t now,
                                           uint32_t report_revision, uint16_t delay) {
    if (active && !s->active) s->active_since=now;
    s->active=active;
    if (s->waiting_report && (uint32_t)(now-s->sent_since)<2000 &&
        report_revision!=s->report_revision && delay && delay!=UINT16_MAX) {
        s->waiting_report=false;
        s->low_latency_confirmed=s->target==60;
        if (s->target==150) {
            s->changed=false;
            // Keep an enable failure visible only while the user still wants Game Mode.
            if (!s->requested) s->failed=false;
            s->phase=s->failed ? GAME_FAILED : s->requested ? GAME_PENDING : GAME_OFF;
        } else s->phase=GAME_CONFIRMED;
    }
    if ((s->waiting_tx || s->waiting_report) && (uint32_t)(now-s->sent_since)>=2000) {
        s->waiting_tx=s->waiting_report=false; s->failed=true;
        if (s->target==150) s->restore_failed=true;
        s->phase=GAME_FAILED;
    }
    if (s->changed && (!s->requested || !active || !eligible || s->failed || s->phase==GAME_RESTORING)) {
        if (s->restore_failed) return 0;
        // A queued/uncertain target must be restored even when capture cancels it.
        if (s->target==60) s->waiting_tx=s->waiting_report=false;
        s->phase=GAME_RESTORING;
        return available && !s->waiting_tx && !s->waiting_report ? playback_game_queue_restore(s,now) : 0;
    }
    if (s->changed || s->waiting_tx || s->waiting_report || s->failed) return 0;
    if (!s->requested) {
        if (s->target==0) {
            if (available && eligible && active) return playback_game_queue_restore(s,now);
            if (s->phase==GAME_RESTORING) return 0;
        }
        s->phase=GAME_OFF; return 0;
    }
    if (!eligible) { s->phase=GAME_UNSUPPORTED; return 0; }
    s->phase=GAME_PENDING;
    return available && active && (uint32_t)(now-s->active_since)>=500 ? 60 : 0;
}
#endif
