#ifndef _ESP_NOW_BUTTON_H_
#define _ESP_NOW_BUTTON_H_

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/byteorder.h>
#include <inttypes.h>
#include <string.h>

#include <esp_now.h>
#include <esp_wifi.h>

#include "button.h"

// Beacon thread settings
#define BEACON_THREAD_STACK_SIZE 2048
#define BEACON_THREAD_PRIORITY   5

K_THREAD_STACK_DECLARE(s_beacon_stack, BEACON_THREAD_STACK_SIZE);
extern struct k_thread s_beacon_tid;

void beacon_thread(void *p1, void *p2, void *p3);

/*
 * ESP-NOW broadcast MAC address
 */
static const uint8_t bcast_mac[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

extern uint8_t curr_mac_addr[ESP_NOW_ETH_ALEN];


enum espnow_message_type {
	ESPNOW_MSG_HEARTBEAT = 1,
    ESPNOW_MSG_BUTTON    = 2,
};

#define ESPNOW_HEADER_SIZE 1

/*
 * Heartbeat frame, fixed-layout, 14 bytes on the wire. seq and uptime_ms use little-endian (native ESP32).
 */
struct __packed espnow_button_heartbeat {
	// Header
	uint8_t type;

	// data
	uint32_t seq;
	uint32_t uptime_ms;
};

/*
 * Button state frame 
 */
struct __packed espnow_button_state {
	// Header
	uint8_t type;

	// data
	enum button_state state;
};

void recv_log_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len);

void send_log_cb(const esp_now_send_info_t *tx_info, esp_now_send_status_t status);

esp_err_t esp_now_button_init();

#endif