#include <stdint.h>
typedef uint64_t absolute_time_t;
extern uint64_t mock_now;
static inline absolute_time_t make_timeout_time_ms(unsigned ms){return mock_now+(uint64_t)ms*1000;}
static inline int time_reached(absolute_time_t end){return mock_now>=end;}
void panic(const char *,...);
