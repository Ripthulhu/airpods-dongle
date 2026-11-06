#ifndef MOCK_LL_H
#define MOCK_LL_H
#include <stdbool.h>
#include <stdint.h>
typedef int cyw43_ll_t;
int cyw43_ll_read_backplane_mem(void *,uint32_t,unsigned,uint8_t *);
int cyw43_ll_write_backplane_mem(void *,uint32_t,unsigned,const uint8_t *);
#endif
