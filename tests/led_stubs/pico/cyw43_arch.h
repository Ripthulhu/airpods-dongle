#include <stdbool.h>
#include <stdint.h>
#define CYW43_WL_GPIO_LED_PIN 0
typedef struct {unsigned unused;} async_context_t;
typedef struct async_at_time_worker {
    void (*do_work)(async_context_t *,struct async_at_time_worker *);
} async_at_time_worker_t;
async_context_t *cyw43_arch_async_context(void);
void cyw43_arch_gpio_put(unsigned pin,bool value);
void async_context_add_at_time_worker_in_ms(async_context_t *,async_at_time_worker_t *,uint32_t);
void async_context_remove_at_time_worker(async_context_t *,async_at_time_worker_t *);
