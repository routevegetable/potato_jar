

/* Approx 20ms */
#define FAST_ADV_INTERVAL 0x20

/* How long do we transmit for when nothing is changing.
 * If this is 70, we will end up transmitting approximately 3 times
 * before going silent */
#define RETRANSMIT_TIME_MS 70

/* Start bluetooth stuff */
void beacon_init(void);

void beacon_set_int_var(char *name, int value);
