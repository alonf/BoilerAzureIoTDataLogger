#include "freertos/event_groups.h"

#define LED_LIGHT_ON 1
#define OK_STATUS_LED 18
#define ERROR_STATUS_LED 23

//change this value to update firmware
#define FIRMWARE_VERSION "9.0.0.0"

void ota_task(void *pvParameters);
const char *get_firmware_version();
unsigned int get_current_update_offset();
unsigned char get_update_progress();
void switch_to_previous_partition();
void blink_led_fast(uint32_t gpio);
extern EventGroupHandle_t wifi_event_group;
