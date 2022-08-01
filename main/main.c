
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/event_groups.h>
#include <driver/gpio.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <esp_pm.h>
#include <esp_event.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <esp_http_client.h>

#include <lwip/err.h>
#include <lwip/sys.h>

#define EXAMPLE_ESP_WIFI_SSID      CONFIG_ESP_WIFI_SSID
#define EXAMPLE_ESP_WIFI_PASS      CONFIG_ESP_WIFI_PASSWORD
#define EXAMPLE_ESP_MAXIMUM_RETRY  CONFIG_ESP_MAXIMUM_RETRY

#if CONFIG_ESP_WIFI_AUTH_OPEN
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_OPEN
#elif CONFIG_ESP_WIFI_AUTH_WEP
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WEP
#elif CONFIG_ESP_WIFI_AUTH_WPA_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA_WPA2_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA3_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_WPA3_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WAPI_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WAPI_PSK
#endif

/* Leds */
#define BLINK_GPIO CONFIG_BLINK_GPIO
#define GPIO_BUTTON_1_INPUT 18
#define GPIO_BUTTON_2_INPUT 2
#define GPIO_BUTTON_INPUT_MASK ((1ULL<<GPIO_BUTTON_1_INPUT) | (1ULL<<GPIO_BUTTON_2_INPUT))

/* Telegram stuff */
static uint32_t offset = 0;  //update_id offset
#define TG_BASE_URL "https://api.telegram.org/bot"CONFIG_TG_BOT_TOKEN"/"
#define TG_GET_UPDATES TG_BASE_URL"getUpdates?timeout=58&offset="
#define SIZEOF_TG_GET_UPDATES_FMT 128
static char tg_get_updates_fmt[SIZEOF_TG_GET_UPDATES_FMT];
#define TG_SEND_HEART TG_BASE_URL"sendMessage?chat_id="CONFIG_TG_TARGET_USERID"&text=heart"
#define TG_SEND_CHICK TG_BASE_URL"sendMessage?chat_id="CONFIG_TG_TARGET_USERID"&text=chicken"

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static uint32_t s_retry_num = 0;
static bool connected = false;

/* Event group to signal when we receive a positive response from the server */
static EventGroupHandle_t s_response_event_group;
const int RESPONSE_BIT = BIT0;

/* Queue of button presses events */
static QueueHandle_t button_press_evt_queue = NULL;
// Struct containing information about the button press, set by the interrupt handler
// It will set inside button_press_evt_queue
typedef struct {
    uint32_t gpio_num;
    TickType_t tick;
} press_info_t;

static const char *TAG = "wifi app";


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
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}


/**
    Connects to the net via wifi. It returns when we have an IP
*/
void wifi_init_sta(void)
{
    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = EXAMPLE_ESP_WIFI_SSID,
            .password = EXAMPLE_ESP_WIFI_PASS,
            /* Setting a password implies station will connect to all security modes including WEP/WPA.
             * However these modes are deprecated and not advisable to be used. Incase your Access point
             * doesn't support WPA2, these mode can be enabled by commenting below line */
         .threshold.authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD,
         .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(TAG, "wifi_init_sta finished.");

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
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
                 EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
        connected = true;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
                 EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }
}


/**
    Handles HTTP events
*/
static int data_len;
static char* data;
esp_err_t _http_event_handler(esp_http_client_event_t *evt, bool check_updates)
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
            ESP_LOGI(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            if (!esp_http_client_is_chunked_response(evt->client)) {
                // Write out data
                data_len = evt->data_len;
                data = (char*)evt->data;
                ESP_LOGI(TAG, "Data received:");
                ESP_LOGI(TAG, "%s", data);

                // Get bot updates
                if (check_updates) {
                    int n_matches = sscanf(data, "{\"ok\":true,\"result\":[{\"update_id\":%u,\"message\":{\"message_id\":%u,\"from\":{\"id\":"CONFIG_TG_TARGET_USERID, &offset, NULL);
                    if (n_matches == 0) {
                        ESP_LOGI(TAG, "No new update found");
                    } else if (n_matches == 1) {
                        // update offset and signal positive response
                        ESP_LOGI(TAG, "New update found");
                        offset += 1;
                        xEventGroupSetBits(s_response_event_group, RESPONSE_BIT);
                    } else {
                        ESP_LOGE(TAG, "sscanf found two or more matches. This should not happen");
                    }
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
        case HTTP_EVENT_REDIRECT:
            ESP_LOGE(TAG, "Received HTTP redirect, not handled");
            break;
    }
    return ESP_OK;
}

esp_err_t _http_event_handler_upd(esp_http_client_event_t *evt) {
    return _http_event_handler(evt, true);
}
esp_err_t _http_event_handler_no_upd(esp_http_client_event_t *evt) {
    return _http_event_handler(evt, false);
}


// HTTP client handle
esp_http_client_handle_t client;

// Utility function to init http client
static esp_http_client_handle_t init_client_http(char* url, bool check_updates) {
    ESP_LOGI("http client", "HTTP client init");
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = check_updates ? _http_event_handler_upd : _http_event_handler_no_upd,
        .timeout_ms = 60000,
    };
    return esp_http_client_init(&config);
}

/**
    Loops long polling requests to server.
*/
static void http_request_task(void *pvParameters)
{
    esp_err_t err;
    esp_http_client_handle_t client = init_client_http(TG_BASE_URL, true);

    // Loop long polling requests forever using the same connection
    uint32_t i = 0;
    while (true) {
        // Perform a GET
        ESP_LOGI(TAG, "Request number %d", i+1);

        // Format URL
        memset(tg_get_updates_fmt, 0, SIZEOF_TG_GET_UPDATES_FMT);
        sprintf(tg_get_updates_fmt, TG_GET_UPDATES"%u", offset);
        esp_http_client_set_url(client, tg_get_updates_fmt);

        err = esp_http_client_perform(client);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "HTTP GET Status = %d, content_length = %d",
                    esp_http_client_get_status_code(client),
                    esp_http_client_get_content_length(client));
        } else {
            ESP_LOGE(TAG, "HTTP GET request failed: %s", esp_err_to_name(err));
            esp_http_client_cleanup(client);  //clean http client
            client = init_client_http(TG_BASE_URL, true);  //init new http client
            vTaskDelay(10000 / portTICK_PERIOD_MS);  //backoff time
        }
        i++;
    }        
}



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
#define MIN_DELTA_TICKS (250 / portTICK_PERIOD_MS)   //minimum number of milliseconds between button presses (avoid debounce)
static void button_press_task(void* arg)
{
    esp_err_t err;
    TickType_t last_tick = xTaskGetTickCount();
    press_info_t info;
    bool http_repeat = false;
    esp_http_client_handle_t client = init_client_http(TG_BASE_URL, false);

    // Configure input pin to handle button press
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_POSEDGE,  //interrupt of rising edge
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
                esp_http_client_set_url(client, TG_SEND_HEART);
            } else if (info.gpio_num == GPIO_BUTTON_2_INPUT) {
                esp_http_client_set_url(client, TG_SEND_CHICK);
            } else {
                ESP_LOGE("button task", "unexpected gpio num %d", info.gpio_num);
                continue;
            }
            
            gpio_set_level(BLINK_GPIO, 1);  //turn on status led

            // Send HTTP request
            // With default settings this throws an error since we have no certificate.
            // To disable cert verification go in menuconfig->components->ESP-TLS->Allow potentially insecure options->skip certificate check
            err = esp_http_client_perform(client);
            if (err == ESP_OK) {
                ESP_LOGI("button task", "HTTP GET Status = %d, content_length = %d",
                        esp_http_client_get_status_code(client),
                        esp_http_client_get_content_length(client));
            } else {
                ESP_LOGE("button task", "HTTP GET request failed: %s", esp_err_to_name(err));
                esp_http_client_cleanup(client);  //cleanup client
                client = init_client_http(TG_BASE_URL, false); //restart client
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
    esp_rom_gpio_pad_select_gpio(CONFIG_BLINK_GPIO);
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
    button_press_evt_queue = xQueueCreate(20, sizeof(press_info_t));

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
