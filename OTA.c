#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "mbedtls/md5.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "azure_c_shared_utility\base64.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <esp_http_client.h>
//#include "esp_request.h"
#include "Common.h"
#include "cJSON.h"

#define MAX_HTTP_RECV_BUFFER 16384

extern int CONNECTED_BIT;
const int nMaxDownloadPacketRetries = 20;
int g_firmwareOffset = 0;
int g_downloadPacketRetries = 0;
const char *g_currentFirmwareVersion = FIRMWARE_VERSION; 
size_t g_newFirmwareSize = 0;
STRING_HANDLE g_blobName;
esp_ota_handle_t g_update_handle = 0;
bool g_fatalError = false;
STRING_HANDLE g_md5base64;

#define TAG "ota"

const char *get_firmware_version()
{
	return g_currentFirmwareVersion;
}

unsigned int get_current_update_offset()
{
	return g_firmwareOffset;
}

unsigned char get_update_progress()
{
	return g_newFirmwareSize == 0 ? 0 : (unsigned char)(g_firmwareOffset * 100 / g_newFirmwareSize);
}



esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ERROR:
        ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
        break;
    case HTTP_EVENT_HEADER_SENT:
        ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
        break;
    case HTTP_EVENT_ON_HEADER:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
        STRING_HANDLE header, md5;
        header = STRING_construct(evt->header_key);
        md5 = STRING_construct("Content-MD5");
        if (STRING_compare(header, md5) == 0)
        {
            g_md5base64 = STRING_construct(evt->header_value);
        }
        STRING_delete(header);
        STRING_delete(md5);
        break;

    case HTTP_EVENT_ON_DATA:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
        if (!esp_http_client_is_chunked_response(evt->client)) {
            // Write out data
            // printf("%.*s", evt->data_len, (char*)evt->data);
        }

        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
        break;
    }
    return ESP_OK;
}




bool download_and_update_firmware(const char *blobName, size_t blobSize)
{
	const char *urlTemplate = "https://watertank.azurewebsites.net/api/DownloadFirmware?code=Ql9c0REMbeoxTpBXgav65AjRntUlT2kYnjUYOB674anaLFScUbXyWA==&firmwareBlobName=%s&offset=%d&chunkSize=%d";
	size_t chunkSize = 512;
	size_t maxChunkSize = 8192;
	const size_t minChunkSize = 128;

	g_firmwareOffset = 0;
	g_downloadPacketRetries = 0;
	int noErrorCounter = 0;
	int status = ESP_OK;

	const esp_partition_t *configured = esp_ota_get_boot_partition();
	const esp_partition_t *running = esp_ota_get_running_partition();

	if (configured != running) 
	{
		ESP_LOGW(TAG, "Configured OTA boot partition at offset 0x%08x, but running from offset 0x%08x", configured->address, running->address);
		ESP_LOGW(TAG, "(This can happen if either the OTA boot data or preferred boot image become corrupted somehow.)");
	}

	ESP_LOGI(TAG, "Running partition type %d subtype %d (offset 0x%08x)", running->type, running->subtype, running->address);
	const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);

	ESP_LOGI(TAG, "Writing to partition subtype %d at offset 0x%x", update_partition->subtype, update_partition->address);
	assert(update_partition != NULL);
	
	
	esp_err_t err = esp_ota_begin(update_partition, blobSize, &g_update_handle);
	if (err != ESP_OK) 
	{
		ESP_LOGE(TAG, "esp_ota_begin failed, error=%d", err);
		g_fatalError = true;
		return false;
	}
	ESP_LOGI(TAG, "esp_ota_begin succeeded");

  	while (g_firmwareOffset < blobSize && g_downloadPacketRetries < nMaxDownloadPacketRetries)
	{
		ESP_LOGI(TAG, "Continue download %s from offset:%u  chunk size:%u  out of total: %u", blobName,  g_firmwareOffset, chunkSize, blobSize);
		STRING_HANDLE url = STRING_construct_sprintf(urlTemplate, blobName, g_firmwareOffset, chunkSize);

        esp_http_client_config_t config =
        {
            .url = STRING_c_str(url),
            .event_handler = _http_event_handler,
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);

        //esp_http_client_set_method(client, HTTP_METHOD_GET);

        err = esp_http_client_open(client, 0);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Err=%d Status = %d", err, esp_http_client_get_status_code(client));
            return false;
        }

        STRING_delete(url);

        ESP_LOGI(TAG, "Finish request firmware download, status=%d, freemem=%d",
             esp_http_client_get_status_code(client), esp_get_free_heap_size());

        char *buffer = malloc(MAX_HTTP_RECV_BUFFER + 1);
        if (buffer == NULL) 
        {
            ESP_LOGE(TAG, "Cannot malloc http receive buffer");
            return false;
        }

        size_t contentLength = esp_http_client_fetch_headers(client);
        int totalReadLen = 0, readLen = 0;
        if (totalReadLen < contentLength && contentLength <= MAX_HTTP_RECV_BUFFER) {
            readLen = esp_http_client_read(client, buffer, contentLength);
            if (readLen <= 0) 
            {
                ESP_LOGE(TAG, "Error read data");
            }
            buffer[readLen] = 0;
            ESP_LOGD(TAG, "read_len = %d", readLen);
        }

        ESP_LOGD(TAG, "HTTP Stream reader Status = %d, content_length = %d, Buffer=%.*s",
                esp_http_client_get_status_code(client),
                readLen,
                readLen,
                buffer);

        char *text = buffer;
        ESP_LOGV(TAG, "original content: %s", text);
        text[strlen(text) - 1] = '\0'; //remove last "
        text++; //skip first "

        ESP_LOGV(TAG, "after triming content: %s", text);

        BUFFER_HANDLE hBuffer = Base64_Decoder(text);
        size_t bufferlength = BUFFER_length(hBuffer);
        ESP_LOGI(TAG, "decode size: %u, ", bufferlength);
        const unsigned char *updateChunk;
        BUFFER_content(hBuffer, &updateChunk);

        /*for (int i = 0; i < bufferlength; ++i)
        {
        ESP_LOGV(TAG, "%d) %x", i, updateChunk[i]);
        }*/
        unsigned char md5result[16];
        mbedtls_md5(updateChunk, bufferlength, md5result);

        STRING_HANDLE encodedMD5 = Base64_Encode_Bytes(md5result, 16);
        ESP_LOGD(TAG, "MD5 after hash: %s, md5 in the header: %s", STRING_c_str(encodedMD5), STRING_c_str(g_md5base64));

        if (STRING_compare(encodedMD5, g_md5base64) != 0)
        {
            ++g_downloadPacketRetries;
            ESP_LOGE(TAG, "MD5 differ, droping last packet. Retries: %d", g_downloadPacketRetries);
            blink_led_fast(ERROR_STATUS_LED);
        }
        else //md5 ok
        {
            esp_err_t err = esp_ota_write(g_update_handle, (const void *)updateChunk, bufferlength);
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "Error: esp_ota_write failed! err=0x%x", err);
                g_fatalError = true;
                return 1;
            }
            //else
            ESP_LOGI(TAG, "Success writing %d to OTA partition", bufferlength);
            blink_led_fast(OK_STATUS_LED);
            g_downloadPacketRetries = 0;
            g_firmwareOffset += bufferlength;
        }

        ESP_LOGV(TAG, "before deleteing hBuffer, freemem=%d", esp_get_free_heap_size());
        BUFFER_delete(hBuffer);

        ESP_LOGV(TAG, "before deleteing encodedMD5 & md5base64 strings, freemem=%d", esp_get_free_heap_size());
        STRING_delete(encodedMD5);
        STRING_delete(g_md5base64);

        ESP_LOGV(TAG, "after freeing all, freemem=%d", esp_get_free_heap_size());

        free(buffer);
       
		//optimize chunk size
		if (g_downloadPacketRetries == 0)
		{
			if (++noErrorCounter > 10) //10 packets with no errors, increase packet size
			{
				maxChunkSize = maxChunkSize * 1.5;
				noErrorCounter = 0;
			}

			if (chunkSize < maxChunkSize)
				chunkSize *= 1.5;
		}
		
		if (g_downloadPacketRetries > 0)
		{
			noErrorCounter = 0;
		}
		
		if (g_downloadPacketRetries > 10) //10 packets with errors, decrease packet size
		{
			chunkSize /= 2;
			if (chunkSize < minChunkSize)
				chunkSize = minChunkSize;
			maxChunkSize = chunkSize;
		}
        esp_http_client_close(client);
        esp_http_client_cleanup(client);

		ESP_LOGI(TAG, "Finish request, status=%d, freemem=%d", status, esp_get_free_heap_size());
	}
   

	bool isSuccess = g_firmwareOffset >= blobSize;
	if (isSuccess)
	{
		ESP_LOGI(TAG, "Finish download firmware, size:%d", g_firmwareOffset);
		if (esp_ota_end(g_update_handle) != ESP_OK) 
		{
			ESP_LOGE(TAG, "esp_ota_end failed!");
			g_fatalError = true;
			return false;
		}

		err = esp_ota_set_boot_partition(update_partition);
		if (err != ESP_OK) 
		{
			ESP_LOGE(TAG, "esp_ota_set_boot_partition failed! err=0x%x", err);
			g_fatalError = true;
			return false;
		}

		ESP_LOGI(TAG, "Finish updating firmware, Prepare to restart system!");
		esp_restart();
		return isSuccess; 
	}
	//else
	ESP_LOGI(TAG, "Error downloading firmware, stop at: %d", g_firmwareOffset);
	g_firmwareOffset = 0;
	return isSuccess;
}




void do_check_firmware(esp_http_client_handle_t client, char *data, int len)
{
	//payload is like this: "{'lastVersion':'1.0.0.0','blobName':'esp32.bin','blobSize':'732800'}"

	//currentFirmwareVersion
	ESP_LOGI(TAG, "Check firmware input data: %s", data);
	if (data == NULL)
	{
		ESP_LOGE(TAG, "Error, data is NULL");
		return;
	}
	
	cJSON * root = cJSON_Parse(data); 

	if (root == NULL)
	{
		ESP_LOGE(TAG, "Error parsing JSON");
		return;
	}

	cJSON *jLatestVersion = cJSON_GetObjectItemCaseSensitive(root, "latestVersion");
	ESP_LOGI(TAG, "Available firmware version: %s", jLatestVersion->valuestring);

	if (strcmp(jLatestVersion->valuestring, get_firmware_version()) == 0)// same fimware do nothing
	{
		ESP_LOGI(TAG, "Available firmware has the same version, hence, no need to update!");
		g_newFirmwareSize = 0;
	}
	else
	{
		g_blobName = STRING_construct(cJSON_GetObjectItemCaseSensitive(root, "blobName")->valuestring);
		g_newFirmwareSize = cJSON_GetObjectItemCaseSensitive(root, "blobSize")->valueint;

		ESP_LOGI(TAG, "Available firmware version: %s blobName: %s  Size:%d", jLatestVersion->valuestring, STRING_c_str(g_blobName), g_newFirmwareSize);
	}

	cJSON_Delete(root);
}


esp_err_t  check_firmware_callback(esp_http_client_event_t *evt)
{
    switch (evt->event_id)
    {
    case HTTP_EVENT_ERROR:
        ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
        break;

    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
        break;

    case HTTP_EVENT_HEADER_SENT:
        ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
        break;

    case HTTP_EVENT_ON_HEADER:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER");
        printf("%.*s", evt->data_len, (char*)evt->data);
        break;

    case HTTP_EVENT_ON_FINISH:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
        break;

    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
        break;

    case HTTP_EVENT_ON_DATA:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
        //if (!esp_http_client_is_chunked_response(evt->client))
        //{
        //    do_check_firmware(evt->client, (char*)evt->data, evt->data_len);
        //}
        ////else
        //ESP_LOGE(TAG, "HTTP data is chunked");

        break;
    }
    return ESP_OK;
}

void switch_to_previous_partition()
{
	ESP_LOGI(TAG, "Switching to previous software partition.");
	const esp_partition_t *previous_partition = esp_ota_get_next_update_partition(NULL);
	ESP_LOGI(TAG, "witching to partition subtype %d at offset 0x%x", previous_partition->subtype, previous_partition->address);
	assert(previous_partition != NULL);

	esp_err_t err = esp_ota_set_boot_partition(previous_partition);
	if (err != ESP_OK)
	{
		ESP_LOGE(TAG, "esp_ota_set_boot_partition failed! err=0x%x", err);
		return;
	}

	ESP_LOGI(TAG, "Finish switching back to previous partition, prepare to restart system!");
	esp_restart();
}


void ota_task(void *pvParameters)
{
	xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, portMAX_DELAY);
	ESP_LOGI(TAG, "Connected to AP, freemem=%d", esp_get_free_heap_size());
	// vTaskDelay(1000/portTICK_RATE_MS);
	
	const char *checkFirmwareUrl = "https://watertank.azurewebsites.net/api/CheckForNewFirmware?code=ndfjZi2tudJeZ9SCFwHWTfOynsWQQLPZlPgiooCnTakQiDopm/7Raw==";
	
    esp_http_client_config_t config = 
    {
        .url = checkFirmwareUrl,
        .method = HTTP_METHOD_GET,
        .event_handler = &check_firmware_callback,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK) 
    {
        ESP_LOGI(TAG, "Finish request firmware information, status=%d, freemem=%d", 
            esp_http_client_get_status_code(client), esp_get_free_heap_size());
        size_t bufferLength = esp_http_client_get_content_length(client);

        char *buffer = malloc(bufferLength+1);

        int read_len = esp_http_client_read(client, buffer, bufferLength);
        buffer[read_len] = 0;
        ESP_LOGI(TAG, "HTTP Stream reader Status = %d, content_length = %d, Buffer=%.*s",
            esp_http_client_get_status_code(client),
            read_len,
            read_len,
            buffer);

        do_check_firmware(client, buffer, read_len);
        free(buffer);
    }
    else
    {
        ESP_LOGE(TAG, "Error http request to get firmware information");
        return;
    }

    esp_http_client_cleanup(client);

	if (g_newFirmwareSize > 0) //new firmware, start update
	{
		download_and_update_firmware(STRING_c_str(g_blobName), g_newFirmwareSize);
	}

	if (g_fatalError)
	{
		ESP_LOGE(TAG, "OTA update failure.");
	}

	STRING_delete(g_blobName);
	
	vTaskDelete(NULL);
}