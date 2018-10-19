// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/adc.h"
#include "esp_system.h"
#include "esp_adc_cal.h"
#include "esp_task_wdt.h"
#include "iothub_watertank_client.h"
#include "iothub_message.h"
#include "azure_c_shared_utility/threadapi.h"
#include "azure_c_shared_utility/crt_abstractions.h"
#include "azure_c_shared_utility/platform.h"
#include "iothubtransportmqtt.h"
#include "sdkconfig.h"
#include "Common.h"

//#define TESTDEVICE

#define V_REF   1100

#define TAG "IoTHubDevice"

#ifdef TESTDEVICE
//test device
static const char *connectionString = "HostName=...;DeviceId=...;SharedAccessKey=...";
#else
//real device
static const char *connectionString = "HostName=...;DeviceId=...;SharedAccessKey=...";
#endif

static int activeMessages;
static char msgText[1024];
static char propText[1024];
static bool g_continueRunning;
static bool g_shouldUpdateSoftware = false;
static bool g_shouldSwitchToPreviousPartition = false;
static bool g_shouldReboot = false;

#define MESSAGE_COUNT 128

const char *SofwareUpdateMessage = "TriggerSoftwareUpdate";
const char *SwitchToPreviousPartitionMessage = "SwitchToPreviousPartition";
const char *RebootMessage = "Reboot";
const char *QuitMessage = "Quit";


typedef struct EVENT_INSTANCE_TAG
{
    IOTHUB_MESSAGE_HANDLE messageHandle;
    size_t messageTrackingId;  // For tracking the messages within the user callback.
} EVENT_INSTANCE;

static unsigned char* bytearray_to_str(const unsigned char *buffer, size_t len)
{
    unsigned char* ret = (unsigned char*)malloc(len+1);
    memcpy(ret, buffer, len);
    ret[len] = '\0';
    return ret;
}

static void update_software()
{
	if (get_current_update_offset() > 0) //software update in progress, ignore request
	{
		ESP_LOGI(TAG, "Firmware update is in progress, ignoring request.");
		return;
	}	
	
	ESP_LOGI(TAG, "Starting software update task.");

	g_shouldUpdateSoftware = false;

	BaseType_t ret = xTaskCreate(&ota_task, "ota_task", 8192, NULL, 5, NULL);

	if (ret != pdPASS)
	{
		ESP_LOGE(TAG, "unable to create update task");
	}
}

static IOTHUBMESSAGE_DISPOSITION_RESULT receive_message_callback(IOTHUB_MESSAGE_HANDLE message, void* userContextCallback)
{

    int* counter = (int*)userContextCallback;
    const char* buffer;
    size_t size;

    if (IoTHubMessage_GetByteArray(message, (const unsigned char**)&buffer, &size) != IOTHUB_MESSAGE_OK)
    {
		ESP_LOGE(TAG, "unable to retrieve the message data");
    }
    else
    {
        unsigned char* message_string = bytearray_to_str((const unsigned char *)buffer, size);
		ESP_LOGI(TAG, "IoTHubMessage_GetByteArray received message: \"%s\"", message_string);

		if (size == (strlen(SofwareUpdateMessage) * sizeof(char)) && memcmp(buffer, SofwareUpdateMessage, size) == 0)
		{
			g_shouldUpdateSoftware = true;
		}

        free(message_string);

        // If we receive the word 'quit' then we stop running
        if (size == (strlen(QuitMessage) * sizeof(char)) && memcmp(buffer, QuitMessage, size) == 0)
        {
            g_continueRunning = false;
        }

		if (size == (strlen(SwitchToPreviousPartitionMessage) * sizeof(char)) && memcmp(buffer, SwitchToPreviousPartitionMessage, size) == 0)
		{
			g_shouldSwitchToPreviousPartition = true;
		}

		if (size == (strlen(RebootMessage) * sizeof(char)) && memcmp(buffer, RebootMessage, size) == 0)
		{
			g_shouldReboot = true;
		}
    } 

    // Retrieve properties from the message
    MAP_HANDLE mapProperties = IoTHubMessage_Properties(message);
    if (mapProperties != NULL)
    {
        const char*const* keys;
        const char*const* values;
        size_t propertyCount = 0;
        if (Map_GetInternals(mapProperties, &keys, &values, &propertyCount) == MAP_OK)
        {
            if (propertyCount > 0)
            {
                size_t index = 0;
                for (index = 0; index < propertyCount; index++)
                {
					ESP_LOGV(TAG, "\tKey: %s Value: %s", keys[index], values[index]);
                }
            }
        }
    }

    /* Some device specific action code goes here... */
    (*counter)++;
    return IOTHUBMESSAGE_ACCEPTED;
}

static void send_confirmation_callback(IOTHUB_CLIENT_CONFIRMATION_RESULT result, void* userContextCallback)
{
    EVENT_INSTANCE* eventInstance = (EVENT_INSTANCE*)userContextCallback;
    size_t id = eventInstance->messageTrackingId;

	ESP_LOGI(TAG, "Confirmation received for message tracking id = %d with result = %s,  current active messages: %u", (int)id, ENUM_TO_STRING(IOTHUB_CLIENT_CONFIRMATION_RESULT, result), activeMessages);

    activeMessages--;
    IoTHubMessage_Destroy(eventInstance->messageHandle);
}

static void send_report_confirmation_callback(int status_code, void* userContextCallback)
{
	ESP_LOGI(TAG, "Confirmation received for status %d", status_code);
}

void do_work(size_t time, IOTHUB_CLIENT_LL_HANDLE iotHubClientHandle)
{
     size_t index = 0;
    for (index = 0; index <time; index++)
    {
        IoTHubClient_LL_DoWork(iotHubClientHandle);
        ThreadAPI_Sleep(1);
    }
}

void blink_led(uint32_t gpio, int times)
{
    while (times-- > 0)
    {
        gpio_set_level(gpio, LED_LIGHT_ON);
        vTaskDelay(200 / portTICK_PERIOD_MS);
        /* Blink on (output high) */
        gpio_set_level(gpio, 1 - LED_LIGHT_ON);
        vTaskDelay(200 / portTICK_PERIOD_MS);
    }
}

void blink_led_fast(uint32_t gpio)
{
    gpio_set_level(gpio, LED_LIGHT_ON);
    vTaskDelay(10 / portTICK_PERIOD_MS);
    gpio_set_level(gpio, 1 - LED_LIGHT_ON);
}

void init_status_led(uint32_t gpio)
{
    gpio_pad_select_gpio(gpio);
    /* Set the GPIO as a push/pull output */
    gpio_set_level(gpio, 1 - LED_LIGHT_ON);
    gpio_set_direction(gpio, GPIO_MODE_OUTPUT);
    blink_led(gpio, 5);
}
void init_adc(adc1_channel_t channel)
{
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(channel, ADC_ATTEN_DB_11);
}

uint32_t read_adc(adc1_channel_t channel, uint32_t previousRead, double lastReadWeight, bool bHistory)
{
    uint32_t voltage = adc1_get_raw(channel);

    if (!bHistory)
        return voltage;

    return voltage * lastReadWeight + previousRead * (1 - lastReadWeight);
}


#define SAMPLE_LENGTH 5000

double samples[SAMPLE_LENGTH];

long double SampleAvarage()
{
    long double result = 0;
    for (int i = 0; i < SAMPLE_LENGTH; ++i)
    {
        result += samples[i];
    }
    return result / SAMPLE_LENGTH;
}

long double TotalEnergy(long double avarage)
{
    long double result = 0.0;
    for (int i = 0; i < SAMPLE_LENGTH; ++i)
    {
        result += pow((samples[i] - avarage), 2.0);
    }
    return result / SAMPLE_LENGTH;
}


uint32_t read_ac(adc1_channel_t channel)
{
    int64_t cycleBegin = esp_timer_get_time();

    for (int i = 0; i < SAMPLE_LENGTH; ++i)
    {
        int64_t readBegin = esp_timer_get_time();
        double reading = adc1_get_raw(channel);
        samples[i] = reading;
        esp_task_wdt_reset();
        while (esp_timer_get_time() - readBegin < 99) // (2000 * 100E-6)= 1 second ==> 10 cycles of 50HZ
            ;
    }

    int64_t totalTime = esp_timer_get_time() - cycleBegin;

    long double avarage = SampleAvarage();
    long double totalEnergy = TotalEnergy(avarage);

    uint32_t result = (uint32_t)sqrt(totalEnergy);
    ESP_LOGI(TAG, "read_ac: Result %u,  time:%ju\n", result, totalTime);
    
    return result;
}

void iothub_client_run(void)
{
	ESP_LOGI(TAG, "\nFile:%s Compile Time:%s %s", __FILE__, __DATE__, __TIME__);
   // esp_task_wdt_init(5000, false); //5 seconds, dont panic

	//Init ADC and Characteristics
	init_adc(ADC1_CHANNEL_5); //GPIO 33
	init_adc(ADC1_CHANNEL_6); //GPIO 34
	init_adc(ADC1_CHANNEL_7); //GPIO 35
	init_status_led(OK_STATUS_LED);
	init_status_led(ERROR_STATUS_LED);
	uint32_t voltage5 = 0, voltage6 = 0, voltage7 = 0;
    bool bInitialized = false;

	IOTHUB_CLIENT_LL_HANDLE iotHubClientHandle;

	EVENT_INSTANCE messages[MESSAGE_COUNT];

	g_continueRunning = true;
	srand((unsigned int)time(NULL));

	activeMessages = 0;
	int receiveContext = 0;
	ESP_LOGI(TAG, "Connected to access point success, size before platform_init: %d", esp_get_free_heap_size());
	if (platform_init() != 0)
	{
		ESP_LOGE(TAG, "Failed to initialize the platform.");
		blink_led(ERROR_STATUS_LED, 100);
		esp_restart();
		return;
	}


	ESP_LOGI(TAG, "size before IoTHubClient_LL_CreateFromConnectionString: %d", esp_get_free_heap_size());
	if ((iotHubClientHandle = IoTHubClient_LL_CreateFromConnectionString(connectionString, MQTT_Protocol)) == NULL)
	{
		ESP_LOGE(TAG, "ERROR: iotHubClientHandle is NULL!");
		blink_led(ERROR_STATUS_LED, 50);
		esp_restart();
		return;
	}

	bool traceOn = true;
	ESP_LOGI(TAG, "size before IoTHubClient_LL_SetOption: %d", esp_get_free_heap_size());
	IoTHubClient_LL_SetOption(iotHubClientHandle, "logtrace", &traceOn);

	/* Setting Message call back, so we can receive Commands. */
	ESP_LOGI(TAG, "size before IoTHubClient_LL_SetMessageCallback: %d", esp_get_free_heap_size());
	if (IoTHubClient_LL_SetMessageCallback(iotHubClientHandle, receive_message_callback, &receiveContext) != IOTHUB_CLIENT_OK)
	{
		ESP_LOGE(TAG, "ERROR: IoTHubClient_LL_SetMessageCallback..........FAILED!");
		blink_led(ERROR_STATUS_LED, 25);
		esp_restart();
		return;
	}

	ESP_LOGI(TAG, "IoTHubClient_LL_SetMessageCallback...successful.");

	/* Now that we are ready to receive commands, let's send some messages */
	size_t msgIndex = 0;
	blink_led(OK_STATUS_LED, 3);

	while (g_continueRunning) //the main device loop, until a "quit" command is received
	{
        for (int i = 0; i < 10; ++i) //take 10 samples, each sample has 0.1 ratio effect on the result
        {
            voltage5 = read_adc(ADC1_CHANNEL_5, voltage5, 0.1, bInitialized);
            voltage6 = read_adc(ADC1_CHANNEL_6, voltage6, 0.1, bInitialized);
            bInitialized = true;
			ets_delay_us(10000); //delay 10 milisecond

        }
        voltage7 = read_ac(ADC1_CHANNEL_7);

		ESP_LOGI(TAG, "5) %d mV\t6) %d mV\t7) %d mV", voltage5, voltage6, voltage7);

		esp_task_wdt_reset(); //make sure the watchdog is satisfied

#ifdef TESTDEVICE
		sprintf_s(msgText, sizeof(msgText), "{\"deviceId\":\"testdevice\",\"watertemperature1\":%d,\"watertemperature2\":%d,\"current\":%d}", voltage5, voltage6, voltage7);
#else
		sprintf_s(msgText, sizeof(msgText), "{\"deviceId\":\"watertank\",\"watertemperature1\":%d,\"watertemperature2\":%d,\"current\":%d}", voltage5, voltage6, voltage7);
#endif

		ESP_LOGI(TAG, "Ready to Send String:%s", msgText);
		ESP_LOGV(TAG, "size before IoTHubMessage_CreateFromByteArray: %d", esp_get_free_heap_size());
		if ((messages[msgIndex].messageHandle = IoTHubMessage_CreateFromByteArray((const unsigned char*)msgText, strlen(msgText))) == NULL)
		{
			ESP_LOGE(TAG, "ERROR: iotHubMessageHandle is NULL!");
			blink_led(ERROR_STATUS_LED, 8);
			continue;
		}

		messages[msgIndex].messageTrackingId = msgIndex;
		ESP_LOGV(TAG, "size before IoTHubMessage_Properties: %d", esp_get_free_heap_size());

		ESP_LOGV(TAG, "free heap size before IoTHubClient_LL_SendEventAsync: %d", esp_get_free_heap_size());
		if (IoTHubClient_LL_SendEventAsync(iotHubClientHandle, messages[msgIndex].messageHandle, send_confirmation_callback, &messages[msgIndex]) != IOTHUB_CLIENT_OK)
		{
			ESP_LOGE(TAG, "ERROR: IoTHubClient_LL_SendEventAsync..........FAILED!");
			blink_led(ERROR_STATUS_LED, 4);
			IoTHubMessage_Destroy(messages[msgIndex].messageHandle);
		}
		else
		{
			ESP_LOGI(TAG, "IoTHubClient_LL_SendEventAsync accepted message [%d] for transmission to IoT Hub.", (int)msgIndex);
			blink_led(OK_STATUS_LED, 2);
		}

		sprintf_s(propText, sizeof(propText), "{\"firmwareVersion\":\"%s\",\"currentUpdateOffset\":%u,\"currentUpdateProgress\":%u }", get_firmware_version(), get_current_update_offset(), get_update_progress());
		if (IoTHubClient_LL_SendReportedState(iotHubClientHandle, (const unsigned char *)propText, strlen(propText), send_report_confirmation_callback, NULL) != IOTHUB_CLIENT_OK)
		{
			ESP_LOGE(TAG, "ERROR: IoTHubClient_LL_SendReportedState..........FAILED!");
			blink_led(ERROR_STATUS_LED, 4);
		}

		do_work(5000, iotHubClientHandle); //let the IoT Hub Client SDK system to work
		msgIndex = (msgIndex + 1) % MESSAGE_COUNT;
		activeMessages++;

		if (activeMessages >= MESSAGE_COUNT) //error, sent many messages without an ack
		{
			ESP_LOGE(TAG, "ERROR: sent many messages without an ack");
			do_work(50000, iotHubClientHandle); //second chance
			if (activeMessages >= MESSAGE_COUNT) //restart the device
			{
				ESP_LOGE(TAG, "ERROR: reset the device to be able to send telemetry");
				blink_led(ERROR_STATUS_LED, 10);
				esp_restart();
			}
		}

		if (g_shouldUpdateSoftware)
		{
			update_software();
			g_shouldUpdateSoftware = false;
		}

		if (g_shouldSwitchToPreviousPartition)
		{
			switch_to_previous_partition();
			g_shouldSwitchToPreviousPartition = false;
		}

		if (g_shouldReboot)
			esp_restart();
	}
	IoTHubClient_LL_Destroy(iotHubClientHandle);
	platform_deinit();
}


