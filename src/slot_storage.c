// SPDX-License-Identifier: GPL-3.0-only
#include <stddef.h>
#include <string.h>
#include "btstack_tlv.h"
#include "hardware/flash.h"
#include "slot_storage.h"

#define SLOT_TAG 0x4150534cu // APSL; distinct from BTstack's bond tags.
#define LEGACY_PAGE (PICO_FLASH_SIZE_BYTES - 2u * FLASH_SECTOR_SIZE - FLASH_PAGE_SIZE)

typedef struct {
    uint8_t version;
    uint8_t selected;
    uint8_t mac[2][MAC_LEN];
} slot_record_t;
_Static_assert(sizeof(slot_record_t) == 14, "slot record layout changed");

static bool load_record(const btstack_tlv_t **tlv, void **context,
                        slot_record_t *record, bool *stored) {
    btstack_tlv_get_instance(tlv, context);
    if (*tlv == NULL) return false;
    int length = (*tlv)->get_tag(*context, SLOT_TAG, NULL, 0);
    *stored = length != 0;
    if (length == 0) {
        // Read the old page for upgrades; all new writes use the SDK's two-bank store.
        const uint8_t *legacy = (const uint8_t *)(XIP_BASE + LEGACY_PAGE);
        record->version = 1;
        record->selected = legacy[FLASH_PAGE_SIZE - 1];
        if (record->selected != 1 && record->selected != 2) record->selected = 1;
        memcpy(record->mac, legacy, sizeof(record->mac));
        return true;
    }
    return length == (int)sizeof(*record) &&
           (*tlv)->get_tag(*context, SLOT_TAG, (uint8_t *)record, sizeof(*record)) == (int)sizeof(*record) &&
           record->version == 1 && (record->selected == 1 || record->selected == 2);
}

static bool write_field(size_t offset, const void *data, size_t size) {
    const btstack_tlv_t *tlv;
    void *context;
    slot_record_t record, verify;
    bool stored;
    if (data == NULL || !load_record(&tlv, &context, &record, &stored)) return false;
    if (stored && memcmp((uint8_t *)&record + offset, data, size) == 0) return true;
    memcpy((uint8_t *)&record + offset, data, size);
    if (tlv->store_tag(context, SLOT_TAG, (const uint8_t *)&record, sizeof(record)) != 0) return false;
    return tlv->get_tag(context, SLOT_TAG, NULL, 0) == (int)sizeof(verify) &&
           tlv->get_tag(context, SLOT_TAG, (uint8_t *)&verify, sizeof(verify)) == (int)sizeof(verify) &&
           memcmp(&record, &verify, sizeof(record)) == 0;
}

uint8_t read_uint8_last_flash(void) {
    const btstack_tlv_t *tlv;
    void *context;
    slot_record_t record;
    bool stored;
    return load_record(&tlv, &context, &record, &stored) ? record.selected : 0xff;
}

bool write_uint8_last_flash(uint8_t value) {
    return (value == 1 || value == 2) && write_field(offsetof(slot_record_t, selected), &value, 1);
}

static void read_mac(unsigned slot, uint8_t mac[MAC_LEN]) {
    const btstack_tlv_t *tlv;
    void *context;
    slot_record_t record;
    bool stored;
    if (load_record(&tlv, &context, &record, &stored)) memcpy(mac, record.mac[slot], MAC_LEN);
    else memset(mac, 0xff, MAC_LEN);
}

void read_slot1_mac(uint8_t mac[MAC_LEN]) { read_mac(0, mac); }
void read_slot2_mac(uint8_t mac[MAC_LEN]) { read_mac(1, mac); }
bool write_slot1_mac(const uint8_t mac[MAC_LEN]) { return write_field(offsetof(slot_record_t, mac[0]), mac, MAC_LEN); }
bool write_slot2_mac(const uint8_t mac[MAC_LEN]) { return write_field(offsetof(slot_record_t, mac[1]), mac, MAC_LEN); }
