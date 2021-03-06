#include <stdint.h>
#include <stdbool.h>


#define TOUCH_PAD_ID 9
#define TOUCH_THRESHOLD 420
//#define TOUCH_THRESHOLD 1000

// With 150kHz osc:
// 0x1000 = 27ms
// 0xA000 = 270ms
// 0x3555 = 90ms
#define TOUCH_SLEEP_CYCLE 0x3555

/* Time to debounce an input - also the hold interval */
#define DEBOUNCE_MS 100

void button_down_event(void);
void button_up_event(uint64_t hold_ms);
void button_hold_event(uint64_t hold_ms);


/* Install interrupt etc... */
void button_init(bool pushed_on);
