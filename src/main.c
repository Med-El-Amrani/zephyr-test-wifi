#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_core.h>
#include <zephyr/net/net_context.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/random/random.h>

LOG_MODULE_REGISTER(wifi_mqtt_example, LOG_LEVEL_INF);

/* MQTT Configuration - CHANGE TO YOUR LAPTOP'S IP */
#define MQTT_BROKER_ADDR    "172.20.10.7"  // ⚠️ CHANGE THIS TO YOUR LAPTOP'S IP!
#define MQTT_BROKER_PORT    1883
#define MQTT_CLIENT_ID      "esp32s3"
#define MQTT_PUB_TOPIC      "esp32/counter"

/* WiFi callbacks */
static struct net_mgmt_event_callback wifi_cb;
static struct net_mgmt_event_callback ipv4_cb;

static K_SEM_DEFINE(wifi_connected, 0, 1);
static K_SEM_DEFINE(ipv4_assigned, 0, 1);

/* MQTT client context */
static struct mqtt_client client;
static uint8_t rx_buffer[128];
static uint8_t tx_buffer[128];
static struct sockaddr_storage broker;
static bool mqtt_connected = false;

/* Client ID and topic */
static struct mqtt_utf8 client_id_utf8;
static struct mqtt_utf8 pub_topic_utf8;

/* ===== WiFi Event Handlers ===== */
static void handle_wifi_connect_result(struct net_mgmt_event_callback *cb)
{
	const struct wifi_status *status = (const struct wifi_status *)cb->info;

	if (status->status) {
		LOG_ERR("WiFi connection failed (status: %d)", status->status);
	} else {
		LOG_INF("WiFi connected successfully");
		k_sem_give(&wifi_connected);
	}
}

static void handle_wifi_disconnect_result(struct net_mgmt_event_callback *cb)
{
	const struct wifi_status *status = (const struct wifi_status *)cb->info;

	LOG_WRN("WiFi disconnected (reason: %d)", status->status);
	k_sem_reset(&wifi_connected);
	k_sem_reset(&ipv4_assigned);
	mqtt_connected = false;
}

static void handle_ipv4_result(struct net_if *iface)
{
	char ip_addr[NET_IPV4_ADDR_LEN];

	for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
		struct net_if_addr *if_addr = &iface->config.ip.ipv4->unicast[i];

		if (if_addr->addr_type != NET_ADDR_DHCP || !if_addr->is_used) {
			continue;
		}
		
		if (net_addr_ntop(AF_INET, &if_addr->address.in_addr, ip_addr, sizeof(ip_addr))) {
			LOG_INF("========================================");
			LOG_INF("ESP32-S3 IP Address: %s", ip_addr);
			LOG_INF("========================================");
			k_sem_give(&ipv4_assigned);
			return;
		}
	}
}

static void wifi_mgmt_event_handler(struct net_mgmt_event_callback *cb,
				     uint64_t mgmt_event, struct net_if *iface)
{
	switch (mgmt_event) {
	case NET_EVENT_WIFI_CONNECT_RESULT:
		handle_wifi_connect_result(cb);
		break;
	case NET_EVENT_WIFI_DISCONNECT_RESULT:
		handle_wifi_disconnect_result(cb);
		break;
	default:
		break;
	}
}

static void ipv4_mgmt_event_handler(struct net_mgmt_event_callback *cb,
				     uint64_t mgmt_event, struct net_if *iface)
{
	if (mgmt_event == NET_EVENT_IPV4_ADDR_ADD) {
		handle_ipv4_result(iface);
	}
}

static int wifi_connect(void)
{
	struct net_if *iface = net_if_get_default();
	struct wifi_connect_req_params params = {0};

	if (!iface) {
		LOG_ERR("No default network interface found");
		return -1;
	}

	params.ssid = CONFIG_WIFI_SSID;
	params.ssid_length = strlen(CONFIG_WIFI_SSID);
	params.psk = CONFIG_WIFI_PASSWORD;
	params.psk_length = strlen(CONFIG_WIFI_PASSWORD);
	params.channel = WIFI_CHANNEL_ANY;
	params.security = WIFI_SECURITY_TYPE_PSK;
	params.band = WIFI_FREQ_BAND_2_4_GHZ;
	params.mfp = WIFI_MFP_OPTIONAL;

	LOG_INF("Connecting to WiFi SSID: %s", params.ssid);

	if (net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params, sizeof(params))) {
		LOG_ERR("WiFi connection request failed");
		return -1;
	}

	return 0;
}

/* ===== MQTT Event Handler ===== */
static void mqtt_evt_handler(struct mqtt_client *mqtt_client,
			      const struct mqtt_evt *evt)
{
	switch (evt->type) {
	case MQTT_EVT_CONNACK:
		if (evt->result) {
			LOG_ERR("MQTT connect failed: %d", evt->result);
			mqtt_connected = false;
		} else {
			LOG_INF("✓ MQTT connected to broker!");
			mqtt_connected = true;
		}
		break;

	case MQTT_EVT_DISCONNECT:
		LOG_WRN("MQTT disconnected");
		mqtt_connected = false;
		break;

	case MQTT_EVT_PUBACK:
		LOG_DBG("MQTT PUBACK received");
		break;

	default:
		LOG_DBG("MQTT event: %d", evt->type);
		break;
	}
}

/* ===== MQTT Setup ===== */
static int broker_init(void)
{
	struct sockaddr_in *broker_addr = (struct sockaddr_in *)&broker;

	broker_addr->sin_family = AF_INET;
	broker_addr->sin_port = htons(MQTT_BROKER_PORT);

	/* Convert broker IP address */
	if (zsock_inet_pton(AF_INET, MQTT_BROKER_ADDR, 
			    &broker_addr->sin_addr) != 1) {
		LOG_ERR("Invalid broker IP address: %s", MQTT_BROKER_ADDR);
		return -EINVAL;
	}

	LOG_INF("MQTT Broker: %s:%d", MQTT_BROKER_ADDR, MQTT_BROKER_PORT);
	return 0;
}

static int setup_mqtt_client(void)
{
	/* Initialize MQTT client */
	mqtt_client_init(&client);

	/* Setup broker */
	if (broker_init() != 0) {
		return -1;
	}

	client.broker = &broker;
	client.evt_cb = mqtt_evt_handler;
	client.protocol_version = MQTT_VERSION_3_1_1;

	/* Buffers */
	client.rx_buf = rx_buffer;
	client.rx_buf_size = sizeof(rx_buffer);
	client.tx_buf = tx_buffer;
	client.tx_buf_size = sizeof(tx_buffer);

	/* Client ID with random suffix */
	static char cid[32];
	snprintf(cid, sizeof(cid), "%s_%08x", MQTT_CLIENT_ID, sys_rand32_get());
	client_id_utf8.utf8 = (uint8_t *)cid;
	client_id_utf8.size = strlen(cid);
	client.client_id = client_id_utf8;

	/* Publish topic */
	pub_topic_utf8.utf8 = (uint8_t *)MQTT_PUB_TOPIC;
	pub_topic_utf8.size = strlen(MQTT_PUB_TOPIC);

	LOG_INF("MQTT Client ID: %s", cid);
	LOG_INF("MQTT Topic: %s", MQTT_PUB_TOPIC);

	return 0;
}

static int mqtt_connect_to_broker(void)
{
	int ret = mqtt_connect(&client);
	if (ret != 0) {
		LOG_ERR("mqtt_connect failed: %d", ret);
		return ret;
	}

	/* Wait for CONNACK */
	for (int i = 0; i < 50 && !mqtt_connected; i++) {
		mqtt_input(&client);
		k_msleep(100);
	}

	if (!mqtt_connected) {
		LOG_ERR("MQTT connection timeout");
		return -ETIMEDOUT;
	}

	return 0;
}

/* ===== MQTT Publish ===== */
static int mqtt_publish_counter(int counter)
{
	if (!mqtt_connected) {
		LOG_WRN("MQTT not connected, skipping publish");
		return -ENOTCONN;
	}

	/* Create JSON payload */
	char payload[64];
	int len = snprintf(payload, sizeof(payload), 
			   "{\"counter\":%d,\"device\":\"esp32s3\"}", counter);

	/* Setup publish parameters */
	struct mqtt_publish_param param = {
		.message.topic = pub_topic_utf8,
		.message.payload.data = payload,
		.message.payload.len = len,
		.message_id = sys_rand32_get() & 0xFFFF,
		.dup_flag = 0,
		.retain_flag = 0,
	};

	/* Publish */
	int ret = mqtt_publish(&client, &param);
	if (ret != 0) {
		LOG_ERR("mqtt_publish failed: %d", ret);
		return ret;
	}

	LOG_INF("✓ Published: %s", payload);
	return 0;
}

/* ===== Main Application ===== */
int main(void)
{
	LOG_INF("========================================");
	LOG_INF("ESP32-S3 WiFi + MQTT Example");
	LOG_INF("========================================");

	/* Setup WiFi callbacks */
	net_mgmt_init_event_callback(&wifi_cb, wifi_mgmt_event_handler,
				      NET_EVENT_WIFI_CONNECT_RESULT |
				      NET_EVENT_WIFI_DISCONNECT_RESULT);
	
	net_mgmt_init_event_callback(&ipv4_cb, ipv4_mgmt_event_handler,
				      NET_EVENT_IPV4_ADDR_ADD);

	net_mgmt_add_event_callback(&wifi_cb);
	net_mgmt_add_event_callback(&ipv4_cb);

	/* Give the system some time to initialize */
	k_sleep(K_SECONDS(1));

	/* Connect to WiFi */
	LOG_INF("Step 1/3: Connecting to WiFi...");
	if (wifi_connect() != 0) {
		LOG_ERR("Failed to initiate WiFi connection");
		return -1;
	}

	/* Wait for WiFi connection */
	if (k_sem_take(&wifi_connected, K_SECONDS(30)) != 0) {
		LOG_ERR("WiFi connection timeout");
		return -1;
	}

	/* Wait for IP address */
	if (k_sem_take(&ipv4_assigned, K_SECONDS(30)) != 0) {
		LOG_ERR("DHCP timeout - no IP address assigned");
		return -1;
	}

	LOG_INF("✓ WiFi connected and IP assigned!");

	/* Initialize MQTT client */
	LOG_INF("Step 2/3: Initializing MQTT client...");
	if (setup_mqtt_client() != 0) {
		LOG_ERR("MQTT client init failed");
		return -1;
	}

	/* Connect to MQTT broker */
	LOG_INF("Step 3/3: Connecting to MQTT broker...");
	if (mqtt_connect_to_broker() != 0) {
		LOG_ERR("MQTT connection failed");
		return -1;
	}

	LOG_INF("========================================");
	LOG_INF("✓ All systems ready!");
	LOG_INF("Publishing counter every 5 seconds...");
	LOG_INF("========================================");

	/* Main loop - publish counter */
	int counter = 0;
	while (1) {
		/* Publish counter */
		if (mqtt_publish_counter(counter) == 0) {
			LOG_INF("Counter: %d", counter);
			counter++;
		}

		/* Process MQTT events */
		if (mqtt_connected) {
			mqtt_input(&client);
			mqtt_live(&client);
		}

		/* Sleep 5 seconds */
		k_sleep(K_SECONDS(5));
	}

	return 0;
}