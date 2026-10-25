#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <inttypes.h>
#include <string.h>

#include <esp_now.h>
#include <esp_wifi.h>

#include <button.h>
#include <esp_now_button.h>

LOG_MODULE_REGISTER(party_reserver, LOG_LEVEL_INF);

// IO specs
static const struct gpio_dt_spec user_led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec reservation_button = GPIO_DT_SPEC_GET(DT_ALIAS(reservationbutton), gpios);

static const struct device *const serial_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

static int serial_write_line(const char *data, size_t len)
{
	if (!device_is_ready(serial_dev))
	{
		return -ENODEV;
	}

	for (size_t i = 0; i < len; ++i)
	{
		uart_poll_out(serial_dev, (unsigned char)data[i]);
	}

	return 0;
}

#define RX_MAX_PAYLOAD_SIZE 9 // Heatboar has the Header (1 byte) and 2*4bytes of data

#define RX_QUEUE_DEPTH 50

struct received_packet
{
	// Header
	enum espnow_message_type type;

	// metadata
	uint8_t src_mac[ESP_NOW_ETH_ALEN];
	uint32_t received_at_ms;

	// Actual data
	uint8_t data[RX_MAX_PAYLOAD_SIZE];
};

K_MSGQ_DEFINE(rx_queue, sizeof(struct received_packet), RX_QUEUE_DEPTH, 4);

/* Loggin received data */
void receiver_recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
	recv_log_cb(info, data, len);

	if (!info || !data || len <= 0)
	{
		return;
	}

	if (len < ESPNOW_HEADER_SIZE || len > RX_MAX_PAYLOAD_SIZE)
	{
		return;
	}

	struct received_packet packet = {0};

	memcpy(packet.src_mac, info->src_addr, sizeof(packet.src_mac));

	// The first byte is the Header->type field
	packet.type = (enum espnow_message_type)data[0];
	memcpy(packet.data, data + ESPNOW_HEADER_SIZE, (size_t)len - ESPNOW_HEADER_SIZE);

	packet.received_at_ms = k_uptime_get_32();

	if (k_msgq_put(&rx_queue, &packet, K_NO_WAIT) != 0)
	{
		// atomic_inc(&rx_dropped);
	}
}

static void process_packet(const struct received_packet *packet)
{
	char line[192];
	char mac[18];

	snprintf(mac, sizeof(mac),
			 "%02x:%02x:%02x:%02x:%02x:%02x",
			 (unsigned)packet->src_mac[0],
			 (unsigned)packet->src_mac[1],
			 (unsigned)packet->src_mac[2],
			 (unsigned)packet->src_mac[3],
			 (unsigned)packet->src_mac[4],
			 (unsigned)packet->src_mac[5]);

	int len;

	switch (packet->type)
	{
	case ESPNOW_MSG_BUTTON:
	{
		uint8_t state = packet->data[0];

		if (state != RELEASED && state != PRESSED)
		{
			return;
		}

		len = snprintf(
			line, sizeof(line),
			"{\"type\":\"button\",\"mac\":\"%s\","
			"\"state\":%u,\"received_at_ms\":%" PRIu32 "}\n",
			mac,
			(unsigned)state,
			packet->received_at_ms);
		break;
	}

	case ESPNOW_MSG_HEARTBEAT:
	{
		uint32_t sequence = sys_get_le32(&packet->data[0]);
		uint32_t uptime_ms = sys_get_le32(&packet->data[4]);

		len = snprintf(
			line, sizeof(line),
			"{\"type\":\"heartbeat\",\"mac\":\"%s\","
			"\"seq\":%" PRIu32 ",\"uptime_ms\":%" PRIu32 ","
			"\"received_at_ms\":%" PRIu32 "}\n",
			mac,
			sequence,
			uptime_ms,
			packet->received_at_ms);
		break;
	}

	default:
		return;
	}

	/* Never transmit truncated JSON. */
	if (len < 0 || (size_t)len >= sizeof(line))
	{
		return;
	}

	int ret = serial_write_line(line, (size_t)len);
	if (ret < 0)
	{
		/* Record a failure counter if needed; logs are disabled. */
	}
}

K_THREAD_STACK_DEFINE(s_receiver_stack, BEACON_THREAD_STACK_SIZE);
struct k_thread s_receiver_tid;

static void receiver_thread(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	struct received_packet packet;

	while (true)
	{
		if (k_msgq_get(&rx_queue, &packet, K_FOREVER) == 0)
		{
			process_packet(&packet);
		}
	}
}

/*
 * Sender-only callback, on button state changes, broadcasts the updated state.
 */
void on_button_change_cb(enum button_state state)
{
	// For debug purses we sync the board LED with the button state
	gpio_pin_set_dt(&user_led, state == RELEASED ? 0 : 1);

	const uint8_t payload[2] = {
		(uint8_t)ESPNOW_MSG_BUTTON,
		(uint8_t)state,
	};

	int err = esp_now_send(bcast_mac, payload, sizeof(payload));

	if (err != ESP_OK)
	{
		LOG_ERR("esp_now_send failed: 0x%x", err);
	}

	return;
}

int main()
{
	// global return status code
	int ret;

	k_msleep(1000);

	// Common ESP-NOW initialization
	ret = esp_now_button_init();

	if (ret != 0)
	{
		LOG_ERR("ESP-NOW initialization failed");
		return ret;
	}

	// GPIO setup
	if (!gpio_is_ready_dt(&user_led))
	{
		return -EIO;
	}

	ret = gpio_pin_configure_dt(&user_led, GPIO_OUTPUT_ACTIVE);

	if (ret < 0)
	{
		LOG_ERR("User LED configuration failed");
		return ret;
	}

	if (IS_ENABLED(CONFIG_BUTTON_SENDER))
	{
		if (!gpio_is_ready_dt(&reservation_button))
		{
			return -EIO;
		}

		ret = gpio_pin_configure_dt(&reservation_button, GPIO_INPUT | GPIO_PULL_UP);

		if (ret != 0)
		{
			LOG_ERR("Reservation button configuration failed");
			return ret;
		}
	}

	// Loggin for sent payloads via ESP-NOW is enabled for both device modes
	int esp_err = esp_now_register_send_cb(send_log_cb);
	if (esp_err != ESP_OK)
	{
		LOG_ERR("esp_now_register_send_cb failed: 0x%x", esp_err);
		return -EIO;
	}

	if (IS_ENABLED(CONFIG_BUTTON_SENDER))
	{
		// ESP-NOW loggin callbacks
		esp_err = esp_now_register_recv_cb(recv_log_cb);
		if (esp_err != ESP_OK)
		{
			LOG_ERR("esp_now_register_recv_cb failed: 0x%x", esp_err);
			return -EIO;
		}

		// Button-side ESP32 board
		// Here we want to broadcast the button state
		ret = register_button(&reservation_button, &on_button_change_cb);

		on_button_change_cb(RELEASED);

		// Start the heartbeat thread
		k_thread_create(&s_beacon_tid, s_beacon_stack, K_THREAD_STACK_SIZEOF(s_beacon_stack), &beacon_thread, NULL, NULL, NULL, BEACON_THREAD_PRIORITY, 0, K_NO_WAIT);
		LOG_INF("Beacon thread started, interval=%ds", CONFIG_ESPNOW_BEACON_INTERVAL_S);
	}
	else if (IS_ENABLED(CONFIG_RECEIVER))
	{
		if (!device_is_ready(serial_dev))
		{
			return -ENODEV;
		}

		// ESP-NOW loggin callbacks
		esp_err = esp_now_register_recv_cb(receiver_recv_cb);
		if (esp_err != ESP_OK)
		{
			LOG_ERR("esp_now_register_recv_cb failed: 0x%x", esp_err);
			return -EIO;
		}

		k_thread_create(&s_receiver_tid, s_receiver_stack, K_THREAD_STACK_SIZEOF(s_receiver_stack), &receiver_thread, NULL, NULL, NULL, BEACON_THREAD_PRIORITY, 0, K_NO_WAIT);
		LOG_INF("Receiver thread started");
	}

	while (1)
	{
		k_msleep(1000);
	}

	return 0;
}
