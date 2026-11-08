// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdio.h>
#include "../src/slot_storage.c"

uint8_t legacy_flash[PICO_FLASH_SIZE_BYTES];
static uint8_t value[32];
static unsigned length, stores;
static uint8_t policy_value[32];
static unsigned policy_length, policy_stores;
static bool available, fail_store, corrupt_store;
static const uint8_t first[6] = {0x10, 2, 3, 4, 5, 6};
static const uint8_t second[6] = {0x20, 8, 9, 10, 11, 12};

static int get(void *context, uint32_t tag, uint8_t *buffer, uint32_t size) {
    assert(context == &value && (tag == SLOT_TAG || tag == RECONNECT_TAG));
    unsigned count=tag==SLOT_TAG?length:policy_length;
    if (buffer == NULL) return (int)count;
    if (size > count) size = count;
    memcpy(buffer, tag==SLOT_TAG?value:policy_value, size);
    return (int)size;
}
static int store(void *context, uint32_t tag, const uint8_t *data, uint32_t size) {
    assert(context == &value && ((tag == SLOT_TAG && size == 14) || (tag==RECONNECT_TAG && size==2)));
    if(tag==SLOT_TAG)stores++;else policy_stores++;
    if (fail_store) return 2;
    uint8_t *out=tag==SLOT_TAG?value:policy_value;
    memcpy(out, data, size);
    if(tag==SLOT_TAG)length=size;else policy_length=size;
    if (corrupt_store) out[tag==SLOT_TAG?2:1] ^= 1;
    return 0;
}
static const btstack_tlv_t impl = {get, store, NULL};
void btstack_tlv_get_instance(const btstack_tlv_t **tlv, void **context) {
    *tlv = available ? &impl : NULL;
    *context = &value;
}
static void reset(void) {
    memset(legacy_flash, 0xff, sizeof(legacy_flash));
    memset(value, 0, sizeof(value));
    length = stores = 0;
    policy_length=policy_stores=0;memset(policy_value,0,sizeof(policy_value));
    available = true;
    fail_store = corrupt_store = false;
}
static void expect_mac(bool left, const uint8_t expected[6]) {
    uint8_t mac[6];
    if (left) read_slot1_mac(mac);
    else read_slot2_mac(mac);
    assert(memcmp(mac, expected, 6) == 0);
}
int main(void) {
    const uint8_t empty[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    reset();
    assert(read_uint8_last_flash() == 1 && stores == 0);
    expect_mac(true, empty);
    reset();
    bool automatic=true;
    assert(read_reconnect_policy(&automatic) && !automatic && !policy_stores);
    assert(write_reconnect_policy(false) && !policy_stores);
    assert(write_slot1_mac(first) && write_slot2_mac(second) && write_uint8_last_flash(2));
    unsigned previous_stores=stores;
    assert(write_reconnect_policy(true) && policy_stores==1);
    assert(write_reconnect_policy(true) && policy_stores==1);
    assert(read_reconnect_policy(&automatic) && automatic);
    assert(write_reconnect_policy(false) && policy_stores==2);
    assert(read_uint8_last_flash()==2 && stores==previous_stores);
    expect_mac(true,first);expect_mac(false,second);
    fail_store=true;assert(!write_reconnect_policy(true));
    assert(read_reconnect_policy(&automatic) && !automatic);
    fail_store=false;corrupt_store=true;assert(!write_reconnect_policy(true));
    for(unsigned bad=0;bad<4;++bad) {
        reset();policy_length=bad==0?1:bad==1?3:2;
        policy_value[0]=bad==2?2:1;policy_value[1]=bad==3?2:0;
        assert(!read_reconnect_policy(&automatic) && !automatic);
        assert(!write_reconnect_policy(true) && !policy_stores);
    }
    reset();available=false;
    assert(!read_reconnect_policy(&automatic) && !automatic && !write_reconnect_policy(true));
    assert(!read_reconnect_policy(NULL));
    reset();
    assert(write_slot1_mac(first) && stores == 1);
    assert(write_slot1_mac(first) && stores == 1);
    assert(write_uint8_last_flash(2) && stores == 2);
    assert(write_uint8_last_flash(2) && stores == 2);
    assert(write_slot2_mac(second) && stores == 3);
    assert(read_uint8_last_flash() == 2);
    expect_mac(true, first);
    expect_mac(false, second);
    assert(!write_uint8_last_flash(0) && !write_uint8_last_flash(3));
    assert(!write_slot1_mac(NULL) && stores == 3);
    fail_store = true;
    assert(!write_uint8_last_flash(1));
    assert(read_uint8_last_flash() == 2);
    expect_mac(true, first);
    fail_store = false;
    corrupt_store = true;
    assert(!write_uint8_last_flash(1));

    reset();
    memcpy(legacy_flash + LEGACY_PAGE, first, 6);
    memcpy(legacy_flash + LEGACY_PAGE + 6, second, 6);
    legacy_flash[LEGACY_PAGE + FLASH_PAGE_SIZE - 1] = 2;
    uint8_t before[PICO_FLASH_SIZE_BYTES];
    memcpy(before, legacy_flash, sizeof(before));
    assert(read_uint8_last_flash() == 2 && stores == 0);
    expect_mac(true, first);
    expect_mac(false, second);
    assert(write_uint8_last_flash(2) && stores == 1); // Migrate even if this field is unchanged.
    assert(memcmp(before, legacy_flash, sizeof(before)) == 0);
    memset(legacy_flash, 0, sizeof(legacy_flash));
    expect_mac(true, first); // New records take precedence over the untouched legacy sector.
    expect_mac(false, second);

    for (unsigned bad_length = 13; bad_length <= 15; ++bad_length) {
        reset();
        length = bad_length;
        value[0] = 9; // Unknown version or wrong length must not be silently overwritten.
        value[1] = 1;
        assert(read_uint8_last_flash() == 0xff);
        expect_mac(true, empty);
        assert(!write_slot1_mac(first) && stores == 0);
    }
    reset();
    length = 14;
    value[0] = 1;
    value[1] = 3;
    assert(!write_uint8_last_flash(1) && stores == 0);
    available = false;
    assert(read_uint8_last_flash() == 0xff && !write_slot1_mac(first));
    expect_mac(true, empty);
    puts("slot TLV: migration, verified reconnect policy, peer preservation and failed/unknown storage passed");
}
