#include "esp_now_button.h"

LOG_MODULE_REGISTER(esp_now_button, LOG_LEVEL_INF);

// Beacon thread
K_THREAD_STACK_DEFINE(s_beacon_stack, BEACON_THREAD_STACK_SIZE);
struct k_thread s_beacon_tid;

uint8_t curr_mac_addr[ESP_NOW_ETH_ALEN] = {0};

// ESP-NOW broadcast peer
esp_now_peer_info_t peer = {};


/* Loggin received data */
void recv_log_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
	if (!info || !data || len <= 0) {
		return;
	}

	const uint8_t *mac = info->src_addr;
	const uint8_t *dst = info->des_addr;
	int8_t rssi = info->rx_ctrl->rssi;
	bool bcast = (memcmp(dst, bcast_mac, ESP_NOW_ETH_ALEN) == 0);

	LOG_INF("RX [%s] from %02x:%02x:%02x:%02x:%02x:%02x  rssi=%d  len=%d", bcast ? "BCAST" : "UNICAST", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], rssi, len);
	
    LOG_HEXDUMP_INF(data, MIN(len, 32), "  payload:");
}

/* Loggin sent data */
void send_log_cb(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
	if (!tx_info) {
		return;
	}

	const uint8_t *mac = tx_info->des_addr;

	if (status == ESP_NOW_SEND_SUCCESS) {
		LOG_INF("TX OK -> %02x:%02x:%02x:%02x:%02x:%02x, len=%u", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], tx_info->data_len);
	} else {
		LOG_ERR("TX FAIL-> %02x:%02x:%02x:%02x:%02x:%02x, len=%u", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], tx_info->data_len);
	}
}

/*
 * Beacon thread.
 * 
 * Sends a heartbeat broadcast every BEACON_INTERVAL_S.
 */
void beacon_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	k_thread_name_set(k_current_get(), "beacon");

	uint32_t seq = 0;
	esp_err_t err;

	while (true) {
		seq = seq + 1;

		uint8_t payload[9];

		payload[0] = (uint8_t)ESPNOW_MSG_HEARTBEAT;
		sys_put_le32(seq, &payload[1]);
		sys_put_le32(k_uptime_get_32(), &payload[5]);

		esp_err_t err = esp_now_send(bcast_mac, payload, sizeof(payload));

		if (err != ESP_OK) {
			LOG_ERR("esp_now_send failed: 0x%x", err);
		}

		k_sleep(K_SECONDS(CONFIG_ESPNOW_BEACON_INTERVAL_S));
	}
}

esp_err_t esp_now_button_init() {
	esp_err_t rc;
	wifi_mode_t cur_mode;
	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

	rc = esp_wifi_get_mode(&cur_mode);

	if (rc == ESP_ERR_WIFI_NOT_INIT) {
		rc = esp_wifi_init(&cfg);
		if (rc != ESP_OK) {
			LOG_ERR("esp_wifi_init failed: 0x%x", rc);
			return -EIO;
		}
	}
	rc = esp_wifi_set_storage(WIFI_STORAGE_RAM);
	if (rc != ESP_OK) {
		LOG_ERR("esp_wifi_set_storage failed: 0x%x", rc);
		return -EIO;
	}
	rc = esp_wifi_set_mode(ESP32_WIFI_MODE_STA);
	if (rc != ESP_OK) {
		LOG_ERR("esp_wifi_set_mode failed: 0x%x", rc);
		return -EIO;
	}
	rc = esp_wifi_start();
	if (rc != ESP_OK) {
		LOG_ERR("esp_wifi_start failed: 0x%x", rc);
		return -EIO;
	}

	// Disable power saving
	rc = esp_wifi_set_ps(WIFI_PS_NONE);
	if (rc != ESP_OK) {
		LOG_ERR("esp_wifi_set_ps failed: 0x%x", (unsigned)rc);
		return rc;
	}

	/* 2. Lock to configured channel -- no AP needed */
	rc = esp_wifi_set_channel(CONFIG_APP_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
	if (rc != ESP_OK) {
		LOG_ERR("esp_wifi_set_channel failed: 0x%x", rc);
		return -EIO;
	}

	/* 3. ESP-NOW init + callbacks */
	rc = esp_now_init();
	if (rc != ESP_OK) {
		LOG_ERR("esp_now_init failed: 0x%x", rc);
		return -EIO;
	}

	memcpy(peer.peer_addr, bcast_mac, ESP_NOW_ETH_ALEN);
	peer.channel = CONFIG_APP_ESPNOW_CHANNEL;
	peer.ifidx = WIFI_IF_STA;
	peer.encrypt = false;

	rc = esp_now_add_peer(&peer);
	if (rc != ESP_OK) {
		LOG_ERR("esp_now_add_peer failed: 0x%x", rc);
		return -EIO;
	}

	uint32_t espnow_ver = 0;

	rc = esp_now_get_version(&espnow_ver);
	if (rc != ESP_OK) {
		LOG_ERR("esp_now_get_version failed: 0x%x", rc);
		return -EIO;
	}
	
	rc = esp_wifi_get_mac(WIFI_IF_STA, curr_mac_addr);
	if (rc != ESP_OK) {
		LOG_ERR("esp_wifi_get_mac failed: 0x%x", rc);
		return -EIO;
	}

	LOG_INF("ESP-NOW v%u ready  ch=%u", (unsigned int)espnow_ver, CONFIG_APP_ESPNOW_CHANNEL);
	
	return rc;
}