// Compile the production SDK transport; only bus registers and scheduling are fake.
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <stdarg.h>
#include <setjmp.h>
#include <assert.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"check failed %s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while(0)
#define BTSDIO_FWBUF_SIZE 0x1000u
#define ROUNDUP(x,a) ((((x)+((a)-1))/(a))*(a))
#define ISALIGNED(a,x) (((uintptr_t)(a)&((x)-1))==0)
#define CIRC_BUF_CNT(in,out) (((in)-(out))&(BTSDIO_FWBUF_SIZE-1))
#define CIRC_BUF_SPACE(in,out) CIRC_BUF_CNT((out),((in)+4))
#define HCI_OUTGOING_PRE_BUFFER_SIZE 4
#define HCI_ACL_CHUNK_SIZE_ALIGNMENT 4
#define HCI_INCOMING_PRE_BUFFER_SIZE 4
#define HCI_INCOMING_PACKET_BUFFER_SIZE 2048
#define HCI_EVENT_TRANSPORT_PACKET_SENT 0x6e
#define HCI_EVENT_PACKET 4
#define HCI_ACL_DATA_PACKET 2
#define HCI_COMMAND_DATA_PACKET 1
#define HCI_SCO_DATA_PACKET 3
#define ERROR_CODE_HARDWARE_FAILURE 3
#define ERROR_CODE_SUCCESS 0
#define log_debug(...)
#define log_error(...)
#define CYW43_THREAD_ENTER
#define CYW43_THREAD_EXIT
#define CYW43_THREAD_LOCK_CHECK
#define DUMP_BYTES(...)
#define cybt_debug(...)
#define cybt_error(...)
#define UNUSED(x) (void)(x)
#define CYW43_PRINTF(...) fixture_print(__VA_ARGS__)
#define BD_ADDR_LEN 6
#define DATA_SOURCE_CALLBACK_POLL 1
typedef int cybt_result_t;
enum { CYBT_SUCCESS=0, CYBT_ERR_BADARG=0xb1, CYBT_ERR_QUEUE_FULL=0xc4 };
enum { H2B_BUF_ADDR_IDX, H2B_BUF_IN_ADDR_IDX };
typedef struct { uint32_t host2bt_in_val, host2bt_out_val; } cybt_fw_membuf_index_t;
typedef uint8_t bd_addr_t[6];
typedef int cyw43_t;
static cyw43_t cyw43_state;
typedef int btstack_data_source_t;
typedef int btstack_data_source_callback_type_t;
typedef struct timer { void (*handler)(struct timer*); uint32_t timeout; } btstack_timer_source_t;
typedef struct {
    const char *name;
    void (*init)(const void*);
    int (*open)(void), (*close)(void);
    void (*register_packet_handler)(void (*)(uint8_t,uint8_t*,uint16_t));
    int (*can_send_packet_now)(uint8_t);
    int (*send_packet)(uint8_t,uint8_t*,int);
    int (*set_baudrate)(uint32_t);
    void (*reset_link)(void), (*set_sco_config)(uint16_t,int);
} hci_transport_t;
static cybt_fw_membuf_index_t indices;
static unsigned writes, commits, events, prints, polls, timer_adds, timer_removes, panics;
static uint32_t now;
static int ensure_error;
static bool have_rx, close_on_rx, queue_on_sent, close_on_sent;
static bool fragment_chain;
static btstack_timer_source_t *armed_timer;
static jmp_buf panic_jump;
static _Alignas(4) uint8_t first[2048], second[2048];
static const hci_transport_t *transport_api;
static uint8_t committed[4096];
static uint32_t committed_size;
typedef struct linked_item { struct linked_item *next; } btstack_linked_item_t;
typedef struct { btstack_linked_item_t item; uint16_t num_packets_sent,num_packets_completed,con_handle; } hci_connection_t;
typedef struct {
    uint16_t acl_data_packet_length,le_data_packets_length;
    uint16_t acl_fragmentation_pos,acl_fragmentation_total_size;
    uint8_t *hci_packet_buffer;
    unsigned acl_fragmentation_tx_active;
    const hci_transport_t *hci_transport;
    hci_connection_t *connections;
    unsigned host_completed_packets;
    bool hci_packet_buffer_reserved;
} stack_t;
static stack_t stack, *hci_stack=&stack;
static hci_connection_t connection;
static bool hci_is_le_connection(hci_connection_t *c) { (void)c;return false; }
static uint16_t little_endian_read_16(const uint8_t *p,unsigned at) { return p[at]|((uint16_t)p[at+1]<<8); }
static void little_endian_store_16(uint8_t *p,unsigned at,uint16_t value) { p[at]=value;p[at+1]=value>>8; }
static void hci_dump_packet(int kind,int dir,uint8_t *p,int size) { (void)kind;(void)dir;(void)p;(void)size; }
static void hci_release_packet_buffer(void) { CHECK(false); }
static void hci_reserve_packet_buffer(void) { CHECK(!stack.hci_packet_buffer_reserved);stack.hci_packet_buffer_reserved=true; }
static uint8_t *hci_get_outgoing_packet_buffer(void) { return stack.hci_packet_buffer; }
static bool hci_transport_synchronous(void) { return hci_stack->hci_transport->can_send_packet_now==NULL; }
static void hci_schedule_transport_packet_sent(void) { CHECK(false); }

static void fixture_print(const char *fmt,...) { (void)fmt;++prints; }
static void __attribute__((noreturn)) panic(const char *fmt,...) { (void)fmt;++panics;longjmp(panic_jump,1); }
static void cybt_get_bt_buf_index(cybt_fw_membuf_index_t *p) { *p=indices; }
static void cybt_mem_write_idx(int kind,uint32_t offset,const uint8_t *data,uint32_t size) {
    (void)kind;CHECK(offset+size<=4096);memcpy(committed+offset,data,size);++writes;committed_size+=size;
}
static void cybt_reg_write_idx(int kind,uint32_t value) { (void)kind;indices.host2bt_in_val=value;++commits; }
static void cybt_toggle_bt_intr(void) {}
static void cybt_bus_request(void) {}
static void cybt_bus_release(void) {}
static int cyw43_ensure_bt_up(cyw43_t *state) { (void)state;return ensure_error; }
static int cyw43_bluetooth_hci_init(void) { return 0; }
static int cyw43_bluetooth_hci_read(uint8_t *buf,uint32_t max,uint32_t *len) {
    (void)max;*len=0;if(have_rx){buf[3]=HCI_EVENT_PACKET;buf[4]=0;buf[5]=0;*len=6;have_rx=false;}return 0;
}
static void cyw43_hal_get_mac(int index,uint8_t *address) { (void)index;memset(address,0,6); }
static void hci_set_chipset(void *chipset) { (void)chipset; }
static void *btstack_chipset_cyw43_instance(void) { return NULL; }
static void hci_set_bd_addr(uint8_t *address) { (void)address; }
static void btstack_run_loop_set_data_source_handler(btstack_data_source_t *s,void(*f)(btstack_data_source_t*,btstack_data_source_callback_type_t)) { (void)s;(void)f; }
static void btstack_run_loop_enable_data_source_callbacks(btstack_data_source_t *s,int bits) { (void)s;(void)bits; }
static void btstack_run_loop_disable_data_source_callbacks(btstack_data_source_t *s,int bits) { (void)s;(void)bits; }
static void btstack_run_loop_add_data_source(btstack_data_source_t *s) { (void)s; }
static void btstack_run_loop_remove_data_source(btstack_data_source_t *s) { (void)s; }
static void btstack_run_loop_poll_data_sources_from_irq(void) { ++polls; }
static uint32_t btstack_run_loop_get_time_ms(void) { return now; }
static void btstack_run_loop_set_timer_handler(btstack_timer_source_t *timer,void(*handler)(btstack_timer_source_t*)) { timer->handler=handler; }
static void btstack_run_loop_set_timer(btstack_timer_source_t *timer,uint32_t ms) { timer->timeout=now+ms+1; }
static void btstack_run_loop_add_timer(btstack_timer_source_t *timer) { CHECK(!armed_timer);armed_timer=timer;++timer_adds; }
static void btstack_run_loop_remove_timer(btstack_timer_source_t *timer) { CHECK(armed_timer==timer);armed_timer=NULL;++timer_removes; }

#include "cyw43_production_fixture.h"

static void callback(uint8_t type,uint8_t *packet,uint16_t size) {
    CHECK(type==HCI_EVENT_PACKET);CHECK(size==2);
    if(packet[0]==HCI_EVENT_TRANSPORT_PACKET_SENT) {
        ++events;CHECK(transport_api->can_send_packet_now(2));CHECK(!armed_timer);
        if(fragment_chain&&stack.acl_fragmentation_total_size) CHECK(hci_send_acl_packet_fragments(&connection)==0);
        if(queue_on_sent) { queue_on_sent=false;CHECK(transport_api->send_packet(2,second+4,24)==0); }
        if(close_on_sent) { close_on_sent=false;CHECK(transport_api->close()==0); }
    } else if(close_on_rx) { close_on_rx=false;CHECK(transport_api->close()==0); }
}
static void setup(void) {
    if(hci_transport_ready) transport_api->close();
    memset(first,0x55,sizeof(first));memset(second,0xaa,sizeof(second));
    indices=(cybt_fw_membuf_index_t){0};writes=commits=events=prints=polls=timer_adds=timer_removes=panics=0;
    now=0;ensure_error=0;have_rx=close_on_rx=queue_on_sent=close_on_sent=fragment_chain=false;armed_timer=NULL;committed_size=0;
    transport_api=hci_transport_cyw43_instance();CHECK(transport_api->can_send_packet_now!=NULL);
    transport_api->init(NULL);transport_api->register_packet_handler(callback);CHECK(transport_api->open()==0);
    memset(&stack,0,sizeof(stack));connection=(hci_connection_t){0};stack.hci_transport=transport_api;
    stack.hci_packet_buffer=first+4;stack.acl_data_packet_length=1021;
}
static void submit(void) { CHECK(transport_api->send_packet(2,first+4,644)==0);CHECK(!transport_api->can_send_packet_now(1)); }
static void expire_timer(void) { CHECK(armed_timer);btstack_timer_source_t *t=armed_timer;armed_timer=NULL;now=t->timeout;t->handler(t); }
static void sco_backpressure_preserves_packet(void) {
    setup();indices.host2bt_in_val=4092;
    first[4]=0x42;first[5]=0;first[6]=24;
    for(unsigned i=0;i<24;++i)first[7+i]=(uint8_t)(i+1);
    uint8_t expected[27];memcpy(expected,first+4,sizeof(expected));
    CHECK(transport_api->send_packet(HCI_SCO_DATA_PACKET,first+4,sizeof(expected))==0);
    CHECK(!transport_api->can_send_packet_now(HCI_COMMAND_DATA_PACKET));
    CHECK(!transport_api->can_send_packet_now(HCI_ACL_DATA_PACKET));
    CHECK(!transport_api->can_send_packet_now(HCI_SCO_DATA_PACKET));
    have_rx=true;hci_transport_cyw43_process();
    CHECK(!have_rx&&!writes&&!events&&armed_timer&&pending_packet==first);
    expire_timer();hci_transport_cyw43_process();
    CHECK(!writes&&!events&&pending_packet==first&&!memcmp(first+4,expected,sizeof(expected)));
    indices.host2bt_out_val=4092;expire_timer();hci_transport_cyw43_process();
    CHECK(commits==1&&events==1&&!pending_packet&&!armed_timer);
    CHECK(committed[4095]==HCI_SCO_DATA_PACKET&&!memcmp(committed,expected,sizeof(expected)));
    hci_transport_cyw43_process();CHECK(commits==1&&events==1);
    puts("SCO backpressure: payload retained, all packet types gated, receive progress and one completion after commit");
}
int main(void) {
    sco_backpressure_preserves_packet();
    setup();submit();CHECK(!writes&&!events);hci_transport_cyw43_process();CHECK(commits==1&&events==1);CHECK(!pending_packet);
    setup();indices.host2bt_in_val=4092;submit();hci_transport_cyw43_process();CHECK(!writes&&!events&&armed_timer&&pending_packet==first);
    unsigned count=timer_adds;now=1;hci_transport_cyw43_process();CHECK(timer_adds==count&&!prints);
    expire_timer();CHECK(polls==2);hci_transport_cyw43_process();CHECK(!writes&&!events&&armed_timer&&!prints);
    indices.host2bt_out_val=indices.host2bt_in_val;expire_timer();hci_transport_cyw43_process();
    CHECK(commits==1&&events==1&&!armed_timer&&!pending_packet);CHECK(committed_size==648);CHECK(first[3]==2&&first[0]==0x84&&first[1]==2);
    hci_transport_cyw43_stats_t stats;hci_transport_cyw43_get_stats(&stats);CHECK(stats.blocked==3&&stats.committed==1&&stats.accepted==1&&stats.max_pending_ms==4);
    puts("full/full/timer-only progress/success: one commit and one completion, no transient logging");
    setup();queue_on_sent=true;submit();hci_transport_cyw43_process();CHECK(pending_packet==second&&events==1&&commits==1);
    hci_transport_cyw43_process();CHECK(!pending_packet&&events==2&&commits==2);puts("completion reentry: second original packet retained and committed once");
    setup();fragment_chain=true;indices.host2bt_in_val=4092;stack.acl_fragmentation_pos=4;stack.acl_fragmentation_total_size=1604;
    first[4]=1;first[5]=0x20;CHECK(hci_send_acl_packet_fragments(&connection)==0);CHECK(connection.num_packets_sent==1&&stack.acl_fragmentation_pos==1024);
    hci_transport_cyw43_process();hci_transport_cyw43_process();CHECK(connection.num_packets_sent==1&&pending_packet==first&&!events);
    indices.host2bt_out_val=4092;expire_timer();hci_transport_cyw43_process();CHECK(commits==1&&events==1&&pending_packet==first+1020&&connection.num_packets_sent==2);
    hci_transport_cyw43_process();CHECK(commits==2&&events==2&&!pending_packet&&stack.acl_fragmentation_total_size==0);CHECK((first[1025]&0x30)==0x10);
    puts("real BTstack ACL fragmentation: credit advances once per accepted fragment, original continuation pointer survives");
    setup();indices.host2bt_in_val=4092;submit();hci_transport_cyw43_process();CHECK(armed_timer);transport_api->close();CHECK(!pending_packet&&!armed_timer);
    CHECK(transport_stats.cancelled==1);transport_api->open();hci_transport_cyw43_process();CHECK(!writes&&!events);puts("close/reopen: pending packet and retry retired");
    setup();submit();close_on_rx=true;have_rx=true;hci_transport_cyw43_process();CHECK(!writes&&!events&&!pending_packet&&!armed_timer);
    setup();submit();close_on_sent=true;hci_transport_cyw43_process();CHECK(events==1&&commits==1&&!hci_transport_ready&&!armed_timer);puts("receive and completion callbacks may close safely");
    setup();indices.host2bt_in_val=4092;CHECK(transport_api->send_packet(1,first+4,7)==0);hci_transport_cyw43_process();CHECK(!writes&&!prints);
    indices.host2bt_out_val=4092;expire_timer();hci_transport_cyw43_process();CHECK(commits==1&&events==1);CHECK(first[3]==1);puts("control command: same deferred ownership and wake path");
    setup();indices.host2bt_in_val=4092;connection.num_packets_completed=3;connection.con_handle=0x42;stack.connections=&connection;stack.host_completed_packets=1;
    hci_host_num_completed_packets();CHECK(!connection.num_packets_completed&&!stack.host_completed_packets&&stack.hci_packet_buffer_reserved&&pending_packet==first);
    hci_transport_cyw43_process();CHECK(!writes&&!events);CHECK(first[4]==0x35&&first[5]==0x0c&&first[7]==1&&first[10]==3);
    const uint8_t host_completed[]={0x35,0x0c,5,1,0x42,0,3,0};
    CHECK(!memcmp(first+4,host_completed,sizeof(host_completed)));
    indices.host2bt_out_val=4092;expire_timer();hci_transport_cyw43_process();CHECK(commits==1&&events==1&&first[3]==1);
    CHECK(!memcmp(committed,host_completed,sizeof(host_completed)));
    puts("real Host Number Of Completed Packets: cleared receive credits remain in the reserved command through backpressure");
    setup();indices.host2bt_in_val=4080;indices.host2bt_out_val=4080;submit();hci_transport_cyw43_process();CHECK(writes==2&&commits==1&&events==1);puts("ring wrap: one commit, two physical writes");
    setup();submit();ensure_error=-9;if(!setjmp(panic_jump)){hci_transport_cyw43_process();CHECK(false);}CHECK(panics==1&&!events&&!writes&&transport_stats.fatal_errors==1&&pending_packet==first);
    setup();if(!setjmp(panic_jump)){transport_api->send_packet(2,first+4,4090);CHECK(false);}CHECK(panics==1&&!pending_packet&&!writes&&!events);puts("unexpected errors and impossible sizes fail explicitly without completion");
    setup();now=UINT32_MAX-1;indices.host2bt_in_val=4092;submit();hci_transport_cyw43_process();expire_timer();indices.host2bt_out_val=4092;hci_transport_cyw43_process();CHECK(transport_stats.max_pending_ms==2);
    transport_stats.blocked=UINT32_MAX;indices.host2bt_out_val=indices.host2bt_in_val+4;submit();hci_transport_cyw43_process();CHECK(transport_stats.blocked==UINT32_MAX);puts("counter saturation and time wrap: bounded diagnostics");
    transport_api->close();return 0;
}
