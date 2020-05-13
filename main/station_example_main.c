/* WiFi station Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_pm.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_http_client.h"

#include "lwip/err.h"
#include "lwip/sys.h"

/* The examples use WiFi configuration that you can set via project configuration menu

   If you'd rather not, just change the below entries to strings with
   the config you want - ie #define EXAMPLE_WIFI_SSID "mywifissid"
*/
#define EXAMPLE_ESP_WIFI_SSID      CONFIG_ESP_WIFI_SSID
#define EXAMPLE_ESP_WIFI_PASS      CONFIG_ESP_WIFI_PASSWORD
#define EXAMPLE_ESP_MAXIMUM_RETRY  CONFIG_ESP_MAXIMUM_RETRY

/* Leds */
#define BLINK_GPIO CONFIG_BLINK_GPIO
#define GPIO_BUTTON_1_INPUT 18
#define GPIO_BUTTON_2_INPUT 2
#define GPIO_BUTTON_INPUT_MASK ((1ULL<<GPIO_BUTTON_1_INPUT) | (1ULL<<GPIO_BUTTON_2_INPUT))

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;
static int connected = 0;

/* The event group allows multiple bits for each event, but we only care about one event 
 * - are we connected to the AP with an IP? */
const int WIFI_CONNECTED_BIT = BIT0;

/* Event group to signal when we receive a positive response from the server */
static EventGroupHandle_t s_response_event_group;
const int RESPONSE_BIT = BIT0;

/* Queue of button presses events */
static xQueueHandle button_press_evt_queue = NULL;

static const char *TAG = "wifi app";

static int s_retry_num = 0;


// Power saving modes
#if CONFIG_EXAMPLE_POWER_SAVE_MIN_MODEM
#define DEFAULT_PS_MODE WIFI_PS_MIN_MODEM
#elif CONFIG_EXAMPLE_POWER_SAVE_MAX_MODEM
#define DEFAULT_PS_MODE WIFI_PS_MAX_MODEM
#elif CONFIG_EXAMPLE_POWER_SAVE_NONE
#define DEFAULT_PS_MODE WIFI_PS_NONE
#else
#define DEFAULT_PS_MODE WIFI_PS_NONE
#endif /*CONFIG_POWER_SAVE_MODEM*/

/**
    Handle events regarding wifi connection and ip obtaining
*/
static void conn_event_handler(void* arg, esp_event_base_t event_base, 
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY) {
            esp_wifi_connect();
            connected = 0;
            xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:%s",
                 ip4addr_ntoa(&event->ip_info.ip));
        s_retry_num = 0;
        
        // Sets bit to notify we have an IP assigned
        connected = 1;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}


/**
    Connects to the net via wifi. It returns when we have an IP
*/
void wifi_init_sta(void)
{
    tcpip_adapter_init();

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &conn_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &conn_event_handler, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = EXAMPLE_ESP_WIFI_SSID,
            .password = EXAMPLE_ESP_WIFI_PASS
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(TAG, "connect to ap SSID:%s password:%s",
             EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);

    // Wait for IP to be set
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, true, true, portMAX_DELAY);

    // Set wifi power save
    esp_wifi_set_ps(DEFAULT_PS_MODE);
}


/**
    Handles HTTP events
*/
static int data_len;
static char* data;
esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    switch(evt->event_id) {
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
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            if (!esp_http_client_is_chunked_response(evt->client)) {
                // Write out data
                data_len = evt->data_len;
                data = (char*)evt->data;
                printf("%.*s\n", data_len, data);
                if (data_len > 0) {
                    if (data[0] == 'Y') {
                        // Positive response, notify event group
                        xEventGroupSetBits(s_response_event_group, RESPONSE_BIT);
                    }
                } else {
                    ESP_LOGD(TAG, "empty response received");
                }
            }

            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
            int mbedtls_err = 0;
            // Maybe to check what it does
            esp_err_t err = ESP_OK;
            //err = esp_tls_get_and_clear_last_error(evt->data, &mbedtls_err, NULL);
            if (err != 0) {
                ESP_LOGI(TAG, "Last esp error code: 0x%x", err);
                ESP_LOGI(TAG, "Last mbedtls failure: 0x%x", mbedtls_err);
            }
            break;
    }
    return ESP_OK;
}


// HTTP client handle
esp_http_client_handle_t client;

// Utility function to init http client
#define URL_DEFAULT "http://my-word-service.herokuapp.com"
static esp_http_client_handle_t init_client_http(char* url) {
    ESP_LOGI("http client", "HTTP client init");
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = _http_event_handler,
        .timeout_ms = 60000,
    };
    return esp_http_client_init(&config);
}

/**
    Loops long polling requests to server.
*/
#define URL_BLINK "http://my-word-service.herokuapp.com/cazzo/28"
static void http_request_task(void *pvParameters)
{
    esp_err_t err;
    esp_http_client_handle_t client = init_client_http(URL_BLINK);

    while (1) {
        // Set header to keep alive connection to heroku
        //esp_http_client_set_header(client, "connection", "keep-alive");

        // Loop long polling requests forever using the same connection
        for (int i=0; i>=0; i++) {
            // Perform a GET
            ESP_LOGI(TAG, "Request number %d", i+1);
            err = esp_http_client_perform(client);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "HTTP GET Status = %d, content_length = %d",
                        esp_http_client_get_status_code(client),
                        esp_http_client_get_content_length(client));
            } else {
                ESP_LOGE(TAG, "HTTP GET request failed: %s", esp_err_to_name(err));
                esp_http_client_cleanup(client);
                client = init_client_http(URL_BLINK);
                vTaskDelay(3000 / portTICK_PERIOD_MS);  //backoff time
                // In case of error reset HTTP client and retry
                break;
            }
        }        
    }
}


// Struct containing information about the button press, set by the interrupt handler
typedef struct {
    uint32_t gpio_num;
    TickType_t tick;
} press_info_t;

/**
    Handle button press
*/
static void IRAM_ATTR gpio_isr_handler(void* _gpio_num)
{
    press_info_t info;
    info.gpio_num = (uint32_t) _gpio_num;
    info.tick = xTaskGetTickCountFromISR();
    xQueueSendFromISR(button_press_evt_queue, &info, NULL);
}


/**
    Task sending a http request on button press
*/
#define ESP_INTR_FLAG_DEFAULT 0
#define URL_BUTTON_1 "http://my-word-service.herokuapp.com/figa/1"
#define URL_BUTTON_2 "http://my-word-service.herokuapp.com/figa/2"
#define MIN_DELTA_TICKS (250 / portTICK_PERIOD_MS)   //minimum number of milliseconds between button presses (avoid debounce)
static void button_press_task(void* arg)
{
    esp_err_t err;
    TickType_t last_tick = xTaskGetTickCount();
    press_info_t info;
    bool http_repeat = false;
    esp_http_client_handle_t client = init_client_http(URL_DEFAULT);

    // Configure input pin to handle button press
    gpio_config_t io_conf = {
        .intr_type = GPIO_PIN_INTR_POSEDGE,  //interrupt of rising edge
        .pin_bit_mask = GPIO_BUTTON_INPUT_MASK, //bit mask of the pins
        .mode = GPIO_MODE_INPUT,             //set as input mode
        .pull_down_en = 1,                     //enable pull-down mode
    };
    gpio_config(&io_conf);

    // Register interrupt handler
    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    gpio_isr_handler_add(GPIO_BUTTON_1_INPUT, gpio_isr_handler, (void*) GPIO_BUTTON_1_INPUT);
    gpio_isr_handler_add(GPIO_BUTTON_2_INPUT, gpio_isr_handler, (void*) GPIO_BUTTON_2_INPUT);

    for(;;) {
        if(xQueueReceive(button_press_evt_queue, &info, portMAX_DELAY)) {
            // Check if this event failed previously, if yes skip next timestamp check
            if (http_repeat) {
                http_repeat = false;
            } else {
                // Check the timestamp difference to discard close events if they are too close (button debounce)
                if (info.tick - last_tick < MIN_DELTA_TICKS) {
                    ESP_LOGI("button task", "rejected event pin %d at tick %d", info.gpio_num, info.tick);
                    continue;
                } else {
                    ESP_LOGI("button task", "accepted event pin %d at tick %d", info.gpio_num, info.tick);
                    last_tick = info.tick;
                }
            }

            // Check which button was pressed and change URL accordingly
            if (info.gpio_num == GPIO_BUTTON_1_INPUT) {
                esp_http_client_set_url(client, URL_BUTTON_1);
            } else if (info.gpio_num == GPIO_BUTTON_2_INPUT) {
                esp_http_client_set_url(client, URL_BUTTON_2);
            } else {
                ESP_LOGE("button task", "unexpected gpio num %d", info.gpio_num);
                continue;
            }
            
            gpio_set_level(BLINK_GPIO, 1);  //turn on status led

            // Send HTTP request
            err = esp_http_client_perform(client);
            if (err == ESP_OK) {
                ESP_LOGI("button task", "HTTP GET Status = %d, content_length = %d",
                        esp_http_client_get_status_code(client),
                        esp_http_client_get_content_length(client));
            } else {
                ESP_LOGE("button task", "HTTP GET request failed: %s", esp_err_to_name(err));
                esp_http_client_cleanup(client);  //cleanup client
                client = init_client_http(URL_DEFAULT); //restart client
                vTaskDelay(3000 / portTICK_PERIOD_MS);  //backoff time
                xQueueSendToFront(button_press_evt_queue, &info, 0); //put event again in queue
                http_repeat = true;   //signal that the next event in queue should be repeated (skip timestamp check)
            }

            gpio_set_level(BLINK_GPIO, 0);  //turn off status LED
        }
    }
}

/**
    LED task. It will blink when it is connecting to wifi, then turing off.
    When a positive response is received by the server it will turn on
*/
static void led_task(void *pvParameters)
{
    // Set up LED pin
    ESP_LOGI("LED_task", "Setup leds and starting blink");
    gpio_pad_select_gpio(CONFIG_BLINK_GPIO);
    gpio_set_direction(CONFIG_BLINK_GPIO, GPIO_MODE_OUTPUT);

    // Loop blink until wifi connection is successful
    while (!connected) {
        gpio_set_level(BLINK_GPIO, 1);
        vTaskDelay(500 / portTICK_PERIOD_MS);
        gpio_set_level(BLINK_GPIO, 0);
        vTaskDelay(500 / portTICK_PERIOD_MS);
    }
    ESP_LOGI("LED_task", "Connected, stop blinking LED");

    // Loop waiting positive response to server. The LED are turned off
    // by pressing the button
    while (1) {
        // If we are here, we are connected with the LED turned off.
        // Now we wait for a positive response from the server
        xEventGroupWaitBits(s_response_event_group, RESPONSE_BIT, true, true, portMAX_DELAY);

        // Response received, turn on led and finish task
        ESP_LOGI("LED_task", "Positive response, turn on LED");
        gpio_set_level(BLINK_GPIO, 1);

        // Clear event bits for the next loop cycle
        xEventGroupClearBits(s_response_event_group, RESPONSE_BIT);
    }
}



void app_main(void)
{
    //Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Set CPU power save  //not supported on my ESP32
    /*esp_pm_config_esp32_t pm_config = {
            .max_freq_mhz = CONFIG_EXAMPLE_MAX_CPU_FREQ_MHZ,
            .min_freq_mhz = CONFIG_EXAMPLE_MIN_CPU_FREQ_MHZ,
            //.light_sleep_enable = true
    };
    ESP_ERROR_CHECK( esp_pm_configure(&pm_config) );*/

    // Initialize wifi connection, response, button press event groups
    s_wifi_event_group = xEventGroupCreate();
    s_response_event_group = xEventGroupCreate();
    button_press_evt_queue = xQueueCreate(10, sizeof(press_info_t));

    // Start LED task
    xTaskCreate(&led_task, "led_task", 2048, NULL, 5, NULL);

    // Start button press task
    xTaskCreate(&button_press_task, "button_task", 8192, NULL, 5, NULL);
    
    ESP_LOGI(TAG, "wifi_init_sta started");
    wifi_init_sta();
    ESP_LOGI(TAG, "wifi_init_sta finished");

    // Creates HTTP task
    xTaskCreate(&http_request_task, "http_request_task", 8192, NULL, 5, NULL);
}
