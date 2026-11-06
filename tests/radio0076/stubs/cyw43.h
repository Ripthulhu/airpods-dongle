#ifndef MOCK_CYW43_H
#define MOCK_CYW43_H
#include <stdint.h>
typedef struct {int cyw43_ll;} probe_state_t;
extern probe_state_t cyw43_state;
int cyw43_bluetooth_hci_init(void);
#endif
