#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include "bme280.h"
#include "bme280_defs.h"
#include "driver/i2c.h"
#include "freertos/semphr.h"
#include "esp_websocket_client.h"
#include <time.h>
#include <sys/time.h>
#include "esp_attr.h"
#include "esp_sntp.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "circularBuffer.h"

/*I2C definitions*/
#define BME280_SLAVE_ADDR 0x77 //BME280 I2C address
#define I2C_MASTER_SCL_IO 4               	/*!< gpio number for I2C master clock */
#define I2C_MASTER_SDA_IO 0               	/*!< gpio number for I2C master data  */
//#define I2C_MASTER_NUM I2C_NUMBER(0) 		/*!< I2C port number for master dev */
#define I2C_MASTER_NUM 0 		/*!< I2C port number for master dev */
#define I2C_MASTER_FREQ_HZ 400000        	/*!< I2C master clock frequency */
//#define I2C_MASTER_FREQ_HZ 3400000        	/*!< I2C master clock frequency */
#define I2C_MASTER_TX_BUF_DISABLE 0                           /*!< I2C master doesn't need buffer */
#define I2C_MASTER_RX_BUF_DISABLE 0                           /*!< I2C master doesn't need buffer */

#define ACK_VAL 0x0                             /*!< I2C ack value */
#define NACK_VAL 0x1                            /*!< I2C nack value */
#define ACK_CHECK_EN 0x1                        /*!< I2C master will check ack from slave*/
//#define READ_BIT I2C_MASTER_READ                /*!< I2C master read */
#define READ_BIT 0x1
#define WRITE_BIT 0x0

/*Websocket Definitions*/
#define NO_DATA_TIMEOUT_SEC 10
#define CONFIG_WEBSOCKET_URI "ws://" //ENTER YOUR WEBSOCKET IP HERE

/*BME280 processing related definitions*/
#define SENSOR_OUTPUT_BUFFER_SIZE 100 //Output buffer used for short internet interuptions and sensor value pile ups. TODO: Investigate longer disruption times handling
#define SENSOR_HUMMIDITY_CIRCULAR_BUFFER_SIZE 20
#define SENSOR_TEMPERATURE_CIRCULAR_BUFFER_SIZE 20
#define SENSOR_PRESSURE_CIRCULAR_BUFFER_SIZE 20

/*Wifi Definitions"*/
#define WIFI_SSID      "SSID" //ENTER YOUR WIFI SSID
#define WIFI_PASS      "PASSWORD" //ENTER YOUR WIFI PASSWORD
#define WIFI_MAXIMUM_RETRY  5 //number of wifi connection attempts
#define WIFI_CONNECTED_BIT BIT0 //Connected to THE AP with an IP event bit
#define WIFI_FAIL_BIT      BIT1 //Failed to connect after the maximum number of retries event

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

/*Log Tags for debugging purposes*/
static const char *TAG_WIFI = "WIFI";
static const char *TAG_SNTP = "SNTP";
static const char *TAG_WEBSOCKET = "WEBSOCKET";
static const char *TAG_BME280 = "BME280";

/*Websocket variables*/
static TimerHandle_t shutdown_signal_timer;
static SemaphoreHandle_t shutdown_sema;

static int s_retry_num = 0; //Wifi retry variable

//BME280 variables
uint32_t req_delay = 0; //delay between sensor reads
struct bme280_dev dev; //Device representation

/*Data structure that is used in the upload buffer*/
struct sensorOutputBufferStruct  {
    double pressure; //Compensated pressure
    double temperature; //Compensated temperature
    double humidity; //Compensated humidity
    int64_t time_us; //time in microseconds
    bool readyToUploadFlag;
};

/*Output buffer array and current positions*/
struct sensorOutputBufferStruct outputBuffer[SENSOR_OUTPUT_BUFFER_SIZE];
uint32_t currentReadPosition; //current sensor read position
uint32_t currentOututPosition; //current output position

/*Function Prototypes*/
static void shutdown_signaler(TimerHandle_t xTimer);
static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
void websocket_app_start(void * pvParameters);
static void event_handler(void* arg, esp_event_base_t event_base,int32_t event_id, void* event_data);
void wifi_init_sta(void);
int user_i2c_write(uint8_t reg_addr, const uint8_t *reg_data, uint32_t len, void *intf_ptr);
int user_i2c_write(uint8_t reg_addr, const uint8_t *reg_data, uint32_t len, void *intf_ptr);
void user_delay_us(uint32_t period, void *intf_ptr);
static esp_err_t i2c_master_init(void);
void print_sensor_data(struct bme280_data *comp_data);
struct bme280_data get_stream_sensor_data_forced_mode(struct bme280_dev *dev);
static void obtain_time(void);
static void initialize_sntp(void);
void vTaskReadBME280Sensor( void * pvParameters );
void clearOutputBuffer(void);

/*Shutdown signaler used when no data had been received for the set timeout*/
static void shutdown_signaler(TimerHandle_t xTimer) {
    ESP_LOGI(TAG_WEBSOCKET, "No data received for %d seconds, signaling shutdown", NO_DATA_TIMEOUT_SEC);
    xSemaphoreGive(shutdown_sema);
}

/*Handle websocket events*/
static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG_WEBSOCKET, "WEBSOCKET_EVENT_CONNECTED");
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG_WEBSOCKET, "WEBSOCKET_EVENT_DISCONNECTED");
        break;
    case WEBSOCKET_EVENT_DATA:
        ESP_LOGI(TAG_WEBSOCKET, "WEBSOCKET_EVENT_DATA");
        ESP_LOGI(TAG_WEBSOCKET, "Received opcode=%d", data->op_code);
        ESP_LOGW(TAG_WEBSOCKET, "Received=%.*s", data->data_len, (char *)data->data_ptr);
        ESP_LOGW(TAG_WEBSOCKET, "Total payload length=%d, data_len=%d, current payload offset=%d\r\n", data->payload_len, data->data_len, data->payload_offset);

        xTimerReset(shutdown_signal_timer, portMAX_DELAY);
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGI(TAG_WEBSOCKET, "WEBSOCKET_EVENT_ERROR");
        break;
    }
}

/*Websocket Task. This Task uploads the BME280 data to the selected IP address*/
void websocket_app_start(void * pvParameters) {
	ESP_LOGI(TAG_WEBSOCKET, "Initializing...");
	//esp_log_level_set("*", ESP_LOG_INFO);
	//esp_log_level_set("WEBSOCKET_CLIENT", ESP_LOG_DEBUG);
	//esp_log_level_set("TRANS_TCP", ESP_LOG_DEBUG);
    esp_websocket_client_config_t websocket_cfg = {};

    shutdown_signal_timer = xTimerCreate("Websocket shutdown timer", NO_DATA_TIMEOUT_SEC * 1000 / portTICK_PERIOD_MS,
                                         pdFALSE, NULL, shutdown_signaler);
    shutdown_sema = xSemaphoreCreateBinary();

    websocket_cfg.uri = CONFIG_WEBSOCKET_URI;

    ESP_LOGI(TAG_WEBSOCKET, "Connecting to %s...", websocket_cfg.uri);

    esp_websocket_client_handle_t client = esp_websocket_client_init(&websocket_cfg);
    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, websocket_event_handler, (void *)client);

    esp_websocket_client_start(client);
    xTimerStart(shutdown_signal_timer, portMAX_DELAY);

    uint32_t numSensorEntriesToUpload = 0;
    currentOututPosition = 0;
    while (true) { //TDDO:  Error checking needed here
    	vTaskDelay(10 / portTICK_RATE_MS);
        if (esp_websocket_client_is_connected(client)) {
        	//Get the next batch of entries to upload by comparing the read posiition with the output position
        	uint32_t currentReadPositionC = currentReadPosition; //TODO: Need a better concurrency check, relying on the fact that this is a 32 bit microcontroller and the data will not change during the read
        	if(currentReadPositionC >= currentOututPosition) {
        		numSensorEntriesToUpload = currentReadPositionC - currentOututPosition;
        	} else {
        		numSensorEntriesToUpload = (SENSOR_OUTPUT_BUFFER_SIZE - currentOututPosition) + currentReadPositionC;
        	}
        	/*Upload the BME280 data if it exists*/
        	if(numSensorEntriesToUpload > 0) {
				char *dataOuput = (char *)malloc(numSensorEntriesToUpload * 50 * sizeof(char)); //TODO: Need to determine more precisely how many characters to decrease memory usage
				int i = 0;
				uint32_t len = 0;
				while(i < numSensorEntriesToUpload) {
					int entryLen = sprintf(dataOuput + len, "%lld,%0.2f,%0.2f,%0.2f\n", outputBuffer[currentOututPosition].time_us, outputBuffer[currentOututPosition].temperature, outputBuffer[currentOututPosition].pressure, outputBuffer[currentOututPosition].humidity);
					len += entryLen;
					outputBuffer[currentOututPosition].readyToUploadFlag = false;
					i++;
					currentOututPosition++;
					if(currentOututPosition >= SENSOR_OUTPUT_BUFFER_SIZE) {
						currentOututPosition = 0;
					}
				}
				esp_websocket_client_send(client, dataOuput, len, portMAX_DELAY);
				free(dataOuput);
        	}
        }
    }

    /*Deinitializes the websocket. Should never reach here but may use in future*/
    xSemaphoreTake(shutdown_sema, portMAX_DELAY);
    esp_websocket_client_stop(client);
    ESP_LOGI(TAG_WEBSOCKET, "Websocket Stopped");
    esp_websocket_client_destroy(client);
}


/*Handles wifi events*/
static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < WIFI_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG_WIFI, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG_WIFI,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG_WIFI, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/*Connect the ESP32 to wifi in station mode*/
void wifi_init_sta(void) {
	ESP_LOGI(TAG_WIFI, "wifi_init_sta started.");
    s_wifi_event_group = xEventGroupCreate();

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            /* Setting a password implies station will connect to all security modes including WEP/WPA.
             * However these modes are deprecated and not advisable to be used. Incase your Access point
             * doesn't support WPA2, these mode can be enabled by commenting below line */
	     .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(TAG_WIFI, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG_WIFI, "connected to ap SSID:%s password:%s",
                 WIFI_SSID, WIFI_PASS);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG_WIFI, "Failed to connect to SSID:%s, password:%s",
                 WIFI_SSID, WIFI_PASS);
    } else {
        ESP_LOGE(TAG_WIFI, "UNEXPECTED EVENT");
    }

    ESP_ERROR_CHECK(esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler));
    ESP_ERROR_CHECK(esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler));
    vEventGroupDelete(s_wifi_event_group);
}


/*!
 * @brief Bus communication function pointer which should be mapped to
 * the platform specific read functions of the user
 *
 * @param[in] reg_addr       : Register address from which data is read.
 * @param[out] reg_data     : Pointer to data buffer where read data is stored.
 * @param[in] len            : Number of bytes of data to be read.
 * @param[in, out] intf_ptr  : Void pointer that can enable the linking of descriptors
 *                                  for interface related call backs.
 *
 * @retval   0 -> Success.
 * @retval Non zero value -> Fail.
 *
 */
int user_i2c_read(uint8_t reg_addr, uint8_t *reg_data, uint32_t len, void *intf_ptr) {
	if (len == 0) {
	        return ESP_OK;
	    }
	    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
	    i2c_master_start(cmd);
	    i2c_master_write_byte(cmd, (BME280_SLAVE_ADDR << 1) | WRITE_BIT, true);
	    i2c_master_write_byte(cmd, reg_addr, true);
	    i2c_master_start(cmd);
	    i2c_master_write_byte(cmd, (BME280_SLAVE_ADDR << 1) | READ_BIT, true);
	    for(int i = 0; i < len; i++) {
	    	if(i == (len-1)) {
	    		i2c_master_read_byte(cmd, &reg_data[i],  I2C_MASTER_NACK);
	    	} else {
	    		i2c_master_read_byte(cmd, &reg_data[i],  I2C_MASTER_ACK);
	    	}
	    }
		i2c_master_stop(cmd);
	    esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, 1000 / portTICK_RATE_MS);
		i2c_cmd_link_delete(cmd);
		return ret;
}

/*!
 * @brief Bus communication function pointer which should be mapped to
 * the platform specific write functions of the user
 *
 * @param[in] reg_addr      : Register address to which the data is written.
 * @param[in] reg_data     : Pointer to data buffer in which data to be written
 *                            is stored.
 * @param[in] len           : Number of bytes of data to be written.
 * @param[in, out] intf_ptr : Void pointer that can enable the linking of descriptors
 *                            for interface related call backs
 *
 * @retval   0   -> Success.
 * @retval Non zero value -> Fail.
 *
 */
int user_i2c_write(uint8_t reg_addr, const uint8_t *reg_data, uint32_t len, void *intf_ptr) {
	i2c_cmd_handle_t cmd = i2c_cmd_link_create();
	i2c_master_start(cmd);
	i2c_master_write_byte(cmd, (BME280_SLAVE_ADDR << 1) | WRITE_BIT, false);
	i2c_master_write_byte(cmd, reg_addr, true);
	for(int i = 0; i < len; i++) {
		i2c_master_write_byte(cmd, reg_data[i], true);
	}
	i2c_master_stop(cmd);
	esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, 1000 / portTICK_RATE_MS);
	i2c_cmd_link_delete(cmd);
	return ret;
}


/*!
 * @brief Delay function pointer which should be mapped to
 * delay function of the user
 *
 * @param[in] period              : Delay in microseconds.
 * @param[in, out] intf_ptr       : Void pointer that can enable the linking of descriptors
 *                                  for interface related call backs
 *
 */
void user_delay_us(uint32_t period, void *intf_ptr) {
	uint32_t startTime =  esp_timer_get_time() / 1000;
	uint32_t timeElapsed = 0;
	while(timeElapsed <= period) { //TODO: Investigate converting this to the FreeRTOS implementation however I only see this being used for initializing of the BME280 sensor
		timeElapsed = (esp_timer_get_time() / 1000) - startTime;
	}
}


/*I2C master initialization*/
static esp_err_t i2c_master_init(void) {
    int i2c_master_port = I2C_MASTER_NUM;
	//int i2c_master_port = 0;
    i2c_config_t conf;
    conf.mode = I2C_MODE_MASTER;
    conf.sda_io_num = I2C_MASTER_SDA_IO;
    conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
    conf.scl_io_num = I2C_MASTER_SCL_IO;
    conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
    conf.master.clk_speed = I2C_MASTER_FREQ_HZ;
    i2c_param_config(i2c_master_port, &conf);
    return i2c_driver_install(i2c_master_port, conf.mode, I2C_MASTER_RX_BUF_DISABLE, I2C_MASTER_TX_BUF_DISABLE, 0);
}

/*Print BME280 data for debugging purposes*/
void print_sensor_data(struct bme280_data *comp_data)
{
#ifdef BME280_FLOAT_ENABLE
        printf("%0.2f, %0.2f, %0.2f\r\n",comp_data->temperature, comp_data->pressure, comp_data->humidity);
#else
        printf("%ld, %ld, %ld\r\n",comp_data->temperature, comp_data->pressure, comp_data->humidity);
#endif
}

/*Initiates the BME280 into forced mode*/
int8_t init_stream_sensor_data_forced_mode(struct bme280_dev *dev) {
	int8_t rslt;
	uint8_t settings_sel;

	/* Recommended mode of operation: Indoor navigation */
	dev->settings.osr_h = BME280_OVERSAMPLING_1X;
	dev->settings.osr_p = BME280_OVERSAMPLING_16X;
	dev->settings.osr_t = BME280_OVERSAMPLING_2X;
	dev->settings.filter = BME280_FILTER_COEFF_16;

	settings_sel = BME280_OSR_PRESS_SEL | BME280_OSR_TEMP_SEL | BME280_OSR_HUM_SEL | BME280_FILTER_SEL;

	rslt = bme280_set_sensor_settings(settings_sel, dev);

	/*Calculate the minimum delay required between consecutive measurement based upon the sensor enabled
	 *and the oversampling configuration. */
	req_delay = bme280_cal_meas_delay(&dev->settings);
	ESP_LOGI(TAG_BME280, "Required Delay: %d", req_delay);
	return rslt;
}

/*Gets the BME280 sensor data in forced mode*/
struct bme280_data get_stream_sensor_data_forced_mode(struct bme280_dev *dev) {
	int8_t rslt;
	struct bme280_data comp_data;
	rslt = bme280_set_sensor_mode(BME280_FORCED_MODE, dev);
	/* Wait for the measurement to complete and print data @25Hz */
	//dev->delay_us(req_delay, dev->intf_ptr);
	vTaskDelay(50 / portTICK_RATE_MS); //TODO Doesn't work when I put req_delay instead of 50 will investigate why and replace with req_delay
	//uint32_t
	rslt = bme280_get_sensor_data(BME280_ALL, &comp_data, dev);
	return comp_data;
}


/*Obtain the current time from SNTP, wait for a limited time until SNTP time is set*/
static void obtain_time(void){
    initialize_sntp();
    int retry = 0;
    const int retry_count = 10;
    while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && ++retry < retry_count) {
        ESP_LOGI(TAG_SNTP, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }
}

/*Initiates the SNTP needed for getting the current time*/
static void initialize_sntp(void) {
    ESP_LOGI(TAG_SNTP, "Initializing SNTP");
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();
}

/*Read the BME280 sensor periodically at close to the maximum speed the BME280 can read at for settings chosen for the BME280*/
void vTaskReadBME280Sensor( void * pvParameters ) {
	//BME280 sensor data variable which is 24 bytes
	struct bme280_data read_bme280_data;

	//current time variable for storing time in seconds and microseconds, 16 bytes
	struct timeval tv_now;
	int64_t time_us = 0; //time in microseconds

    //Circular Buffer Arrays
	double hummidityCirArray[SENSOR_HUMMIDITY_CIRCULAR_BUFFER_SIZE];
	double pressureCirArray[SENSOR_PRESSURE_CIRCULAR_BUFFER_SIZE];
	double temperatureCirArray[SENSOR_TEMPERATURE_CIRCULAR_BUFFER_SIZE];

	//Get the ciruclarBuffer struct required to interact with the circularBuffer.h API
	struct circularBuffer hummidityCirBuffer= initializeCircularBuffer(hummidityCirArray, SENSOR_HUMMIDITY_CIRCULAR_BUFFER_SIZE);
	struct circularBuffer pressureCirBuffer = initializeCircularBuffer(pressureCirArray, SENSOR_PRESSURE_CIRCULAR_BUFFER_SIZE);
	struct circularBuffer temperatureCirBuffer = initializeCircularBuffer(temperatureCirArray, SENSOR_TEMPERATURE_CIRCULAR_BUFFER_SIZE);

	currentReadPosition = 0;
	while(true) {
		if(!outputBuffer[currentReadPosition].readyToUploadFlag) {
			gettimeofday(&tv_now, NULL); //Get the current time before reading the BME280 sensor values
			read_bme280_data = get_stream_sensor_data_forced_mode(&dev); //Read the BME280 sensor
			time_us = (int64_t)tv_now.tv_sec * 1000000L + (int64_t)tv_now.tv_usec; //convert the current time into microseconds
			/*Insert the measured BME280 sensor values into the circular buffer*/
			insertCircularBuffer(&hummidityCirBuffer, read_bme280_data.humidity);
			insertCircularBuffer(&pressureCirBuffer, read_bme280_data.pressure);
			insertCircularBuffer(&temperatureCirBuffer, read_bme280_data.temperature);
			/*Waits until each of the respective circular buffers are full before preparing the data for sending
			 * TODO: Investigate the possibility of not waiting for each three circular buffer to become full before sending out*/
			if(hummidityCirBuffer.isFull && pressureCirBuffer.isFull && temperatureCirBuffer.isFull) {
				/*Update the outputBuffer, another task will upload these values via websocket*/
				outputBuffer[currentReadPosition].humidity = getAverage(hummidityCirBuffer);
				outputBuffer[currentReadPosition].pressure = getAverage(pressureCirBuffer);
				outputBuffer[currentReadPosition].temperature = getAverage(temperatureCirBuffer);
				outputBuffer[currentReadPosition].time_us = time_us;
				outputBuffer[currentReadPosition].readyToUploadFlag = true;
				/*Update the current position and check if current position is at the end*/
				currentReadPosition++;
				if(currentReadPosition >= SENSOR_OUTPUT_BUFFER_SIZE) {
					currentReadPosition = 0;
				}
			}

			/*Below commented code uses no filtering with circular buffer method*/
//			outputBuffer[currentReadPosition].humidity = read_bme280_data.humidity;
//			outputBuffer[currentReadPosition].pressure = read_bme280_data.pressure;
//			outputBuffer[currentReadPosition].temperature = read_bme280_data.temperature;
		}
    }
	vTaskDelete(NULL);
}

/*Clears the output buffer*/
void clearOutputBuffer(void) {
	for(int i = 0; i < SENSOR_OUTPUT_BUFFER_SIZE; i++) {
		outputBuffer[i].readyToUploadFlag = false;
		outputBuffer[i].humidity = 0;
		outputBuffer[i].pressure = 0;
		outputBuffer[i].temperature = 0;
		outputBuffer[i].time_us = 0;
	}
}



void app_main(void) {
	/*initialize the I2C, we need this for reading the BME280 sensor*/
	i2c_master_init();

	/*Initialize the BME280 sensor*/
	int8_t rslt = BME280_OK; //error code variable
	uint8_t dev_addr = BME280_I2C_ADDR_PRIM; //BME280 I2C address

	/*Initialize BME280 sensor object. This is the interface that allows the ESP32 to use the BME280 API*/
	dev.intf_ptr = &dev_addr; //I2C address for the BME280 sensor
	dev.intf = BME280_I2C_INTF;
	dev.read = user_i2c_read; //I2C read function implemented specific for the ESP32 in the required format for BME280 API
	dev.write = user_i2c_write; //I2C write function implemented specific for the ESP32 in the required format for BME280 API
	dev.delay_us = user_delay_us; //Delay function specific for ES32 that delays for the inputed microseconds.

	/*reads the calibration data from the BME280 and sets up the BME280 sensing mode. If this fails it will repetitively attempt it until it succeeds*/
	do {
		rslt = bme280_init(&dev); //determine if can talk to the BME280 sensor. If yes then get the calibration data and return the result. Should be BME280_OK
		if(rslt == BME280_OK) {
			rslt = init_stream_sensor_data_forced_mode(&dev); //TODO: Error checking needed her
		}
		if(rslt != BME280_OK) {
			ESP_LOGI(TAG_BME280, "Failed to initialize, attempting a retry");
			vTaskDelay(2000 / portTICK_PERIOD_MS);
		}
	} while(rslt != BME280_OK);
	ESP_LOGI(TAG_BME280, "Successfully initialized");

	//initialize the output buffer for uploading BME280 sensor values
	clearOutputBuffer();

	//Initialize NVS, needed for Wifi, websocket and SNTP
	esp_err_t ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
	  ESP_ERROR_CHECK(nvs_flash_erase());
	  ret = nvs_flash_init();
	}
	ESP_ERROR_CHECK(ret);

	//Initialize the underlying TCP/IP stack, needed for Wifi, websocket and SNTP
	ESP_ERROR_CHECK(esp_netif_init());

	//Create default event loop
	ESP_ERROR_CHECK(esp_event_loop_create_default());

	//Connect to the Wi-fi
	wifi_init_sta();

	/*Initialize the time with the NTP server and set to local New Zealand time*/
	time_t now;
	struct tm timeinfo;
	obtain_time();
	time(&now); // update 'now' variable with current time
	char strftime_buf[64];
	setenv("TZ", "NZST-12NZDT,M9.5.0,M4.1.0/3", 1); //setup to the New Zealand timezone
	tzset();
	localtime_r(&now, &timeinfo);
	strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
	ESP_LOGI(TAG_SNTP, "The current date/time in New Zealand is: %s", strftime_buf); //for confirming correct time was set
	ESP_LOGI(TAG_SNTP,"Initialized Time");

	//Start the BME280 Sensor read task that periodically reads the BME280 sensor
	xTaskCreate(vTaskReadBME280Sensor, "BME280 Sensor Read Task", 8192, NULL, 5, NULL); //start the temperature reading task, the size of the task stack depends on the size of the circular buffer, consider moving it out stack

	/*Initialize the Websocket task for uploading the sensored BME280 data*/
	xTaskCreate(websocket_app_start, "Websocket Task", 4096, NULL, 4, NULL);
}
