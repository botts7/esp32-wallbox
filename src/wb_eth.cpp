#include "wb_eth.h"
#include "wb_log.h"

#ifdef WB_ETH_W5500
// ── W5500 SPI Ethernet via the IDF esp_eth driver (core 2.0.17 / IDF 4.4) ─────
// The Arduino core's built-in ETH library is RMII-only on 2.x (no W5500), so we
// drive the WIZnet part directly through esp_eth and glue it to lwIP. Once the
// eth netif is up, lwIP routes sockets over it → WiFiClient/WebServer/MQTT/OTA
// work unchanged.
#include <cstring>
#include "esp_eth.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"

// Board pins (defaults = Waveshare ESP32-S3-ETH base; override per board in the
// build env). SPI host: 1 = SPI2_HOST (FSPI) on the S3.
#ifndef WB_ETH_SPI_HOST
#define WB_ETH_SPI_HOST 1
#endif
#ifndef WB_ETH_SPI_MHZ
#define WB_ETH_SPI_MHZ 20
#endif
#ifndef WB_ETH_PIN_RST
#define WB_ETH_PIN_RST -1
#endif

namespace {
esp_eth_handle_t s_eth = nullptr;
esp_netif_t*     s_netif = nullptr;
volatile bool    s_linkUp = false;
volatile uint32_t s_ip = 0;
uint8_t          s_mac[6] = {0};

void onEth(void*, esp_event_base_t base, int32_t id, void*) {
    if (base != ETH_EVENT) return;
    if (id == ETHERNET_EVENT_CONNECTED)         s_linkUp = true;
    else if (id == ETHERNET_EVENT_DISCONNECTED) { s_linkUp = false; s_ip = 0; }
}
void onIp(void*, esp_event_base_t, int32_t id, void* data) {
    if (id == IP_EVENT_ETH_GOT_IP) {
        auto* ev = static_cast<ip_event_got_ip_t*>(data);
        s_ip = ev->ip_info.ip.addr;
        Log.printf("[ETH] DHCP lease " IPSTR "\n", IP2STR(&ev->ip_info.ip));
    }
}
}  // namespace

namespace wb_eth {

bool begin() {
    // Idempotent core init (Arduino may not have done these on an ETH-only build).
    esp_netif_init();
    esp_event_loop_create_default();
    gpio_install_isr_service(0);   // for the W5500 INT line (harmless if present)

    spi_host_device_t host = static_cast<spi_host_device_t>(WB_ETH_SPI_HOST);
    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num = WB_ETH_PIN_MOSI;
    buscfg.miso_io_num = WB_ETH_PIN_MISO;
    buscfg.sclk_io_num = WB_ETH_PIN_SCLK;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    if (spi_bus_initialize(host, &buscfg, SPI_DMA_CH_AUTO) != ESP_OK) {
        Log.println("[ETH] spi_bus_initialize failed");
        return false;
    }
    spi_device_interface_config_t devcfg = {};
    devcfg.command_bits = 16;   // W5500 framing: 16-bit address + 8-bit control
    devcfg.address_bits = 8;
    devcfg.mode = 0;
    devcfg.clock_speed_hz = WB_ETH_SPI_MHZ * 1000 * 1000;
    devcfg.spics_io_num = WB_ETH_PIN_CS;
    devcfg.queue_size = 20;
    spi_device_handle_t spi = nullptr;
    if (spi_bus_add_device(host, &devcfg, &spi) != ESP_OK) {
        Log.println("[ETH] spi_bus_add_device failed");
        return false;
    }

    eth_w5500_config_t w5500 = ETH_W5500_DEFAULT_CONFIG(spi);
    w5500.int_gpio_num = WB_ETH_PIN_INT;
    eth_mac_config_t macc = ETH_MAC_DEFAULT_CONFIG();
    esp_eth_mac_t* mac = esp_eth_mac_new_w5500(&w5500, &macc);

    eth_phy_config_t phyc = ETH_PHY_DEFAULT_CONFIG();
    phyc.phy_addr = 1;
    phyc.reset_gpio_num = WB_ETH_PIN_RST;
    esp_eth_phy_t* phy = esp_eth_phy_new_w5500(&phyc);

    esp_eth_config_t cfg = ETH_DEFAULT_CONFIG(mac, phy);
    if (esp_eth_driver_install(&cfg, &s_eth) != ESP_OK || !s_eth) {
        Log.println("[ETH] esp_eth_driver_install failed");
        return false;
    }

    // W5500 has no factory MAC — give it a unique one from the chip eFuse.
    esp_read_mac(s_mac, ESP_MAC_ETH);
    esp_eth_ioctl(s_eth, ETH_CMD_S_MAC_ADDR, s_mac);

    // Glue to a DHCP netif so lwIP carries all sockets over Ethernet.
    esp_netif_config_t ncfg = ESP_NETIF_DEFAULT_ETH();
    s_netif = esp_netif_new(&ncfg);
    esp_netif_attach(s_netif, esp_eth_new_netif_glue(s_eth));

    esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &onEth, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &onIp, nullptr);

    if (esp_eth_start(s_eth) != ESP_OK) {
        Log.println("[ETH] esp_eth_start failed");
        return false;
    }
    Log.printf("[ETH] W5500 up (host=%d CS=%d INT=%d RST=%d SCLK=%d MOSI=%d MISO=%d @%dMHz)\n",
               WB_ETH_SPI_HOST, WB_ETH_PIN_CS, WB_ETH_PIN_INT, WB_ETH_PIN_RST,
               WB_ETH_PIN_SCLK, WB_ETH_PIN_MOSI, WB_ETH_PIN_MISO, WB_ETH_SPI_MHZ);
    return true;
}

bool isUp() { return s_linkUp && s_ip != 0; }
uint32_t localIP() { return s_ip; }
void macAddress(uint8_t out[6]) { memcpy(out, s_mac, 6); }

}  // namespace wb_eth

#else  // ── WiFi builds: no-op stubs so wb_net stays #ifdef-free ───────────────
namespace wb_eth {
bool begin() { return false; }
bool isUp() { return false; }
uint32_t localIP() { return 0; }
void macAddress(uint8_t out[6]) { for (int i = 0; i < 6; i++) out[i] = 0; }
}  // namespace wb_eth
#endif
