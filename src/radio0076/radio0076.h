#ifndef AIRPODS_RADIO0076_H
#define AIRPODS_RADIO0076_H
#include <stdbool.h>

void radio0076_begin(void);
bool radio0076_start(void);
bool radio0076_after_hci(void);
void radio0076_register_gate(void);
const char *radio0076_error(void);
#endif
