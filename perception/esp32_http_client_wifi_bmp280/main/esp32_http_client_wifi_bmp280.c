#include <stdio.h>
#include <inttypes.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

// Incluimos la libreria correspondiente
#include <bmp180.h>

#include "sdkconfig.h"

#include "esp_http_client.h"

#ifndef APP_CPU_NUM
#define APP_CPU_NUM PRO_CPU_NUM
#endif

#define WIFI_SSID CONFIG_APP_WIFI_SSID
#define WIFI_PASSWORD CONFIG_APP_WIFI_PASSWORD

#define SERVER_URL CONFIG_APP_SERVER_URL

#define DEVICE_ID CONFIG_APP_DEVICE_ID

#define WIFI_CONNECTED_BIT BIT0

#define I2C_MASTER_SDA 21
#define I2C_MASTER_SCL 22

static const char *TAG = "HTTP_TEST";
static EventGroupHandle_t wifi_event_group;

/* -----------------------------------------------------------
 * WiFi
 * --------------------------------------------------------- */
static void bmp180_setup(bmp180_dev_t *dev)
{
    memset(dev, 0, sizeof(bmp180_dev_t));

    // Iniciamos sensor
    ESP_ERROR_CHECK(bmp180_init_desc(dev, I2C_NUM_0, I2C_MASTER_SDA, I2C_MASTER_SCL));
    ESP_ERROR_CHECK(bmp180_init(dev));

    ESP_LOGI(TAG, "BMP180 found at I2C address 0x%02x", BMP180_DEVICE_ADDRESS);
}

// Modificamos lectura del sensor, no tengo humedad en el bmp180 y pressure es uint32
static bool bmp180_read(bmp180_dev_t *dev, uint32_t *pressure, float *temperature)
{
    esp_err_t err = bmp180_measure(
        dev,
        temperature,
        pressure,
        BMP180_MODE_HIGH_RESOLUTION
    );

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "BMP180 reading failed: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG,
             "Pressure: %" PRIu32 " Pa, Temperature: %.2f C",
             *pressure,
             *temperature);

    return true;
}

/* -----------------------------------------------------------
 * WiFi
 * --------------------------------------------------------- */

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_START)
    {

        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED)
    {

        ESP_LOGI(TAG, "WiFi disconnected, reconnecting...");
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
    else if (event_base == IP_EVENT &&
             event_id == IP_EVENT_STA_GOT_IP)
    {

        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;

        ESP_LOGI(TAG, "Got IP: " IPSTR,
                 IP2STR(&event->ip_info.ip));

        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init(void)
{
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(
        esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(
        esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            &wifi_event_handler,
            NULL));

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            &wifi_event_handler,
            NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
        },
    };

    ESP_ERROR_CHECK(
        esp_wifi_set_mode(WIFI_MODE_STA));

    ESP_ERROR_CHECK(
        esp_wifi_set_config(
            WIFI_IF_STA,
            &wifi_config));

    ESP_ERROR_CHECK(
        esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to WiFi...");
}

/* -----------------------------------------------------------
 * HTTP event
 * --------------------------------------------------------- */

static esp_err_t http_event_handler(
    esp_http_client_event_t *evt)
{
    switch (evt->event_id)
    {

    case HTTP_EVENT_ON_DATA:

        printf("%.*s",
               evt->data_len,
               (char *)evt->data);

        break;

    default:
        break;
    }

    return ESP_OK;
}

/* -----------------------------------------------------------
 * GET
 * --------------------------------------------------------- */

static void http_get(void)
{
    ESP_LOGI(TAG, "GET %s", SERVER_URL);

    esp_http_client_config_t config = {
        .url = SERVER_URL,
        .event_handler = http_event_handler,
    };

    esp_http_client_handle_t client =
        esp_http_client_init(&config);

    esp_http_client_set_method(
        client,
        HTTP_METHOD_GET);

    esp_err_t err =
        esp_http_client_perform(client);

    if (err == ESP_OK)
    {

        ESP_LOGI(TAG,
                 "GET status = %d, content_length = %lld",
                 esp_http_client_get_status_code(client),
                 esp_http_client_get_content_length(client));
    }
    else
    {

        ESP_LOGE(TAG,
                 "GET failed: %s",
                 esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
}

/* -----------------------------------------------------------
 * POST
 * --------------------------------------------------------- */

// Reemplazamos la plantilla en application/x-www-form-urlencoded a JSON
// static char *POST_DATA_TEMPLATE = "id=" DEVICE_ID "&t=%0.2f&h=%0.2f&p=%0.2f";
static const char *POST_DATA_TEMPLATE =
    "{\"id\":\"" DEVICE_ID "\",\"t\":%.2f,\"h\":null,\"p\":%" PRIu32 "}"; // no tengo humedad, mando null para no modificar api

static void http_post(uint32_t *pressure, float *temperature)
{

    // Aumentamos tamaño del buffer
    char post_data[128];

    // Replace sprintf with snprintf to be more secure
    int buff = snprintf(
        post_data,
        sizeof(post_data),
        POST_DATA_TEMPLATE,
        *temperature,
        *pressure);

    if (buff < 0)
    {
        ESP_LOGE(TAG, "Error formatting POST data");
        return;
    }

    if ((size_t)buff >= sizeof(post_data))
    {
        ESP_LOGE(TAG, "POST data was truncated, increment buffer size");
        return;
    }

    ESP_LOGI(TAG, "POST %s", SERVER_URL);
    ESP_LOGI(TAG, "POST_DATA %s", post_data);

    esp_http_client_config_t config = {
        .url = SERVER_URL,
        .event_handler = http_event_handler,
    };

    esp_http_client_handle_t client =
        esp_http_client_init(&config);

    esp_http_client_set_method(
        client,
        HTTP_METHOD_POST);

    // Reemplazamos el content type a JSON
    esp_http_client_set_header(
        client,
        "Content-Type",
        "application/json");
    //"application/x-www-form-urlencoded");

    esp_http_client_set_post_field(
        client,
        post_data,
        strlen(post_data));

    esp_err_t err =
        esp_http_client_perform(client);

    if (err == ESP_OK)
    {

        ESP_LOGI(TAG,
                 "POST status = %d",
                 esp_http_client_get_status_code(client));
    }
    else
    {

        ESP_LOGE(TAG,
                 "POST failed: %s",
                 esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
}

/* -----------------------------------------------------------
 * Main
 * --------------------------------------------------------- */

void app_main(void)
{
    ESP_ERROR_CHECK(
        nvs_flash_init());

    bmp180_dev_t dev;

    ESP_ERROR_CHECK(i2cdev_init());

    ESP_LOGI(TAG, "BMP180 init...");
    bmp180_setup(&dev);

    wifi_init();

    /* Wait for WiFi */
    xEventGroupWaitBits(
        wifi_event_group,
        WIFI_CONNECTED_BIT,
        pdFALSE,
        pdTRUE,
        portMAX_DELAY);

    ESP_LOGI(TAG, "WiFi connected");

    vTaskDelay(pdMS_TO_TICKS(1000));

    uint32_t pressure;
    float temperature;
    while (1)
    {
        // Si todo sale bien, mando lectura
        if (bmp180_read(&dev, &pressure, &temperature))
        {
            http_post(&pressure, &temperature);
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
