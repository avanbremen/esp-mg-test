/**
 * \file       mg_test_main.c
 *
 * \brief      Main application entry point.
 *
 * \details    Demonstrates mg_broadcast never returning and locking op the calling task, thus making multi-threading impossible.
 *
 *             - Initializes nvs flash
 *             - Initializes Wi-Fi
 *                - Registers the ESP32 event handler
 *             - On Wi-Fi AP connect (got ip event), creates tasks mg_task and timer_task
 *                - Both tasks are created with a stack size of 16384 bytes (16k), as Mongoose ctl message size is defined as 8192 bytes
 *             - mg_task initializes WebSocket server (port 8000)
 *                - struct mg_mgr mgr; has file-scope so that mg_broadcast can access it via its own task (no need to pass reference)
 *                - Registers the Mongoose event handler
 *             - On Mongoose WebSocket frame received (mg_ev_handler), sends text "ws_frame_reply"
 *                - nc->user_data is set to 1, to differentiate user socket from (what is assumed to be) loopback socket
 *                - mg_broadcast callback on_work_complete is called 1 + N times, where N is the number of active user sockets
 *             - On timer_task execute (every 10 seconds), calls mg_broadcast with callback function on_work_complete and event data "timer_task" (text)
 *             - On on_work_complete (called from within mg_task), calls mg_send_websocket_frame to send event data from timer_task ("timer_task")
 *             - ERROR: mg_broadcast never returns, thus locking up timer_task (will only run once)
 *             - Note that task mg_task will continue to run just fine, therefore sending text (a ws frame) will still result in "ws_frame_reply"
 *
 *             The following is a console log example with 1 active user socket (after mg_broadcast never reached):
 *
 *             I (13566) mg_test_main: timer_task run
 *             I (13566) mg_test_main: before mg_broadcast
 *             I (13566) mg_test_main: on_work_complete
 *             I (13566) mg_test_main: testStr=timer_task
 *             I (13566) mg_test_main: before mg_send
 *             I (13576) mg_test_main: after mg_send
 *             I (13576) mg_test_main: on_work_complete
 *             I (13586) mg_test_main: user_data != 1
 *
 *             Based on Mongoose and ESP-IDF example projects, with minimal changes for recognizability.
 *             Tested with Mongoose 6.11 (latest release) and ESP-IDF master as well as Pre-release 3.0-rc1.
 *
 * \note       Configure Serial flasher config > Default serial port using make menuconfig (set to COM3).
 *             Configure Wi-Fi SSID and password using defines \c EXAMPLE_WIFI_SSID and \c EXAMPLE_WIFI_PASS (below).
 *             Define MG_ENABLE_BROADCAST (1) is set in project Makefile.
 *
 * \see        https://github.com/cesanta/mongoose
 *             https://github.com/cesanta/mongoose/tree/master/examples/websocket_chat
 *             https://github.com/cesanta/mongoose/tree/master/examples/multithreaded/multithreaded.c
 *             https://github.com/espressif/esp-idf
 *             https://github.com/espressif/esp-idf/blob/master/examples/protocols/http_request/main/http_request_example_main.c
 *
 * \author     J.A.T. van Bremen
 * \version    0.1
 * \date       15-02-2018
 *
 */

// includes
#include "esp_log.h"          // esp
#include "esp_system.h"       // -
#include "esp_wifi.h"         // -
#include "nvs_flash.h"        // -
#include "esp_event_loop.h"   // -

#include "freertos/FreeRTOS.h"   // FreeRTOS
#include "freertos/task.h"       // -

#include "mongoose.h"   // mongoose

#include <string.h>  // C std

// defines
#define EXAMPLE_WIFI_SSID  "your_wifi_ssid"
#define EXAMPLE_WIFI_PASS  "your_wifi_pass"

// local const data
static const char* TAG = "mg_test_main";
static const char *s_http_port = "8000";

// local variables
struct mg_mgr mgr;

// local function prototypes
static void initialise_wifi(void);
static esp_err_t event_handler(void *ctx, system_event_t *event);
static void mg_task(void *param);
static void mg_ev_handler(struct mg_connection *nc, int ev, void *ev_data);
static void timer_task(void *param);
static void on_work_complete(struct mg_connection *nc, int ev, void *ev_data);

// global functions


void app_main()
{
   ESP_ERROR_CHECK( nvs_flash_init() );
   initialise_wifi();
}

// local functions

static void initialise_wifi(void)
{
   tcpip_adapter_init();
   ESP_ERROR_CHECK( esp_event_loop_init(event_handler, NULL) );
   wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
   ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
   ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
   wifi_config_t wifi_config = {
      .sta = {
         .ssid = EXAMPLE_WIFI_SSID,
         .password = EXAMPLE_WIFI_PASS,
      },
   };
   ESP_LOGI(TAG, "Setting WiFi configuration SSID %s...", wifi_config.sta.ssid);
   ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
   ESP_ERROR_CHECK( esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
   ESP_ERROR_CHECK( esp_wifi_start() );
}

static esp_err_t event_handler(void *ctx, system_event_t *event)
{
   switch(event->event_id)
   {
      case SYSTEM_EVENT_STA_START:
         esp_wifi_connect();
         break;

      case SYSTEM_EVENT_STA_GOT_IP:
         ESP_LOGI(TAG, "Got ip, create mg_task and timer_task");
         xTaskCreate(mg_task, "mg_task", (16384 / sizeof(portSTACK_TYPE)), NULL, (tskIDLE_PRIORITY + 10), NULL);
         xTaskCreate(timer_task, "timer_task", (16384 / sizeof(portSTACK_TYPE)), NULL, (tskIDLE_PRIORITY + 10), NULL);
         break;

      case SYSTEM_EVENT_STA_DISCONNECTED:
         /* This is a workaround as ESP32 WiFi libs don't currently
         auto-reassociate. */
         esp_wifi_connect();
         break;

      default:
         break;
   }

   return ESP_OK;
}

static void mg_task(void *param)
{
   (void)param;

   struct mg_connection *nc;

   ESP_LOGI(TAG, "Starting task %s", pcTaskGetTaskName(NULL));

   mg_mgr_init(&mgr, NULL);

   nc = mg_bind(&mgr, s_http_port, mg_ev_handler);
   if (nc == NULL) {
     ESP_LOGE(TAG, "Failed to create listener");
     return;
   }

   mg_set_protocol_http_websocket(nc);

   ESP_LOGI(TAG, "Started on port %s", s_http_port);

   while (1) {
     mg_mgr_poll(&mgr, 200);
   }

   mg_mgr_free(&mgr);
}

static void mg_ev_handler(struct mg_connection *nc, int ev, void *ev_data)
{
   (void) ev_data;

   char ip[32];
   mg_sock_addr_to_str(&nc->sa, ip, sizeof(ip), MG_SOCK_STRINGIFY_IP | MG_SOCK_STRINGIFY_PORT);

   switch(ev)
   {
      case MG_EV_WEBSOCKET_HANDSHAKE_REQUEST:
         ESP_LOGI(TAG, "Ws handshake request ip=%s", ip);
         break;

      case MG_EV_WEBSOCKET_HANDSHAKE_DONE:
         ESP_LOGI(TAG, "Ws handshake done ip=%s", ip);
         nc->user_data = (void *)1;
         break;

      case MG_EV_WEBSOCKET_FRAME:
      {
         ESP_LOGI(TAG, "Ws frame ip=%s", ip);

         struct websocket_message* wm = (struct websocket_message *)ev_data;

         if(wm->size > 0)
         {
            ESP_LOGI(TAG, "Ws frame=%.*s", wm->size, wm->data);

            const char *testStr = "ws_frame_reply";
            mg_send_websocket_frame(nc, WEBSOCKET_OP_TEXT, testStr, strlen(testStr));
         }

         break;
      }

      case MG_EV_CLOSE:
         ESP_LOGI(TAG, "Connection closed ip=%s", ip);
         break;

      default:
         break;
   }
}

static void timer_task(void *param)
{
   (void) param;

   ESP_LOGI(TAG, "Starting task %s", pcTaskGetTaskName(NULL));

   const char *testStr = "timer_task";

   while(1)
   {
      ESP_LOGI(TAG, "%s sleep", pcTaskGetTaskName(NULL));
      vTaskDelay(10000 / portTICK_PERIOD_MS);
      ESP_LOGI(TAG, "%s run", pcTaskGetTaskName(NULL));

      ESP_LOGI(TAG, "before mg_broadcast");
      mg_broadcast(&mgr, on_work_complete, (void *)testStr, strlen(testStr) + 1);   // ERROR: never returns!? timer_task blocks (only executes once)
      ESP_LOGI(TAG, "after mg_broadcast");
   }
}

static void on_work_complete(struct mg_connection *nc, int ev, void *ev_data)
{
   (void) ev;

   ESP_LOGI(TAG, "on_work_complete");

   if((int)nc->user_data == 1)
   {
      if(ev_data != NULL)
      {
         const char *testStr = (const char *)ev_data;
         ESP_LOGI(TAG, "testStr=%s", testStr);

         ESP_LOGI(TAG, "before mg_send");
         mg_send_websocket_frame(nc, WEBSOCKET_OP_TEXT, testStr, strlen(testStr));  // should send text "timer_task" (without null-char)
         ESP_LOGI(TAG, "after mg_send");
      }
      else
      {
         ESP_LOGI(TAG, "ev_data == NULL");
      }
   }
   else
   {
      ESP_LOGI(TAG, "user_data != 1");
   }
}
