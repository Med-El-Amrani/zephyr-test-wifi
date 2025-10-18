# Zephyr WiFi/MQTT Test - ESP32-S3

Simple WiFi connection test for ESP32-S3 using Zephyr RTOS. Connects to WiFi, displays the assigned IP address and sends a counter data to a broker on a laptop connected to the same network.

## Features
- WiFi connection with WPA2-PSK
- MQTT connection to broker and publish
- DHCP client for automatic IP assignment
- IP address display via logging
- Event-driven WiFi management

## Hardware
- ESP32-S3 DevKitC
- 2.4GHz WiFi network