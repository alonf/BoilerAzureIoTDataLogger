# BoilerAzureIoTDataLogger
The ESP32 device code that collects water tank temperature telemetry and sends it to an Azure IoT Hub.
The device is an ESP32 with two thermistors (I use my 3D printer spare part) and an AC current sensor: 
https://learn.openenergymonitor.org/electricity-monitoring/ct-sensors/interface-with-arduino?redirected=true

To use this code, you need to install the ESP32 tool chain:
https://docs.espressif.com/projects/esp-idf/en/latest/get-started/index.html
and the Azure IoT Hub:
I use this: https://github.com/Azure/azure-iot-pal-esp32 but there is a new repo:
https://github.com/espressif/esp-azure

You also need to create Azure IoT Hub and cloud service that read the information, or you can route the telemetry messages to Azure Stream Analytics service, Power BI and Azure Functions that take action, for example in my case I turn on and off the water heater (this code is not included).

To implement the OTA (Over the air update, you need to put your new firmware in a cloud blob storage and create an Azure function app with the functions that you'll find in "Azure Functions for OTA" text file.

You also need to update the variuos Azure urls and connection strings. 
