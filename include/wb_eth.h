#pragma once
#include <cstdint>

// Optional W5500 SPI Ethernet support (community request — Waveshare ESP32-S3-ETH
// & friends). ESP32-S3 has no built-in Ethernet MAC, so these boards bridge a
// WIZnet W5500 over SPI. Built ONLY when -D WB_ETH_W5500 is set (the esp32s3-eth
// env); the normal WiFi builds get the no-op stubs so callers stay #ifdef-free.
//
// It attaches the W5500 to lwIP via the IDF esp_eth SPI driver, so the existing
// WiFiClient / WebServer / MQTT / OTA all ride over Ethernet transparently — no
// separate network stack. Pins come from -D WB_ETH_PIN_* (per-board; see
// platformio.ini). Rationale: Ethernet frees the single 2.4 GHz radio for BLE
// only, which is the usual cure for flaky WiFi on this BLE-heavy gateway.
namespace wb_eth {

// Bring up the W5500 (SPI bus + esp_eth driver + DHCP netif). Returns true once
// the driver is installed and started (link/IP come asynchronously — poll isUp).
bool begin();

// Link is up AND a DHCP lease has been obtained.
bool isUp();

// IPv4 address as a raw 32-bit value (0 until DHCP completes).
uint32_t localIP();

// The Ethernet MAC (derived from the chip's eFuse — W5500 has none of its own).
void macAddress(uint8_t out[6]);

}  // namespace wb_eth
