// See wb_mqtt_net.h.
#include "wb_mqtt_net.h"

#include "wb_log.h"

size_t WbMqttNetClient::write(const uint8_t* buf, size_t size) {
    if (size == 0) return 0;
    const uint32_t t0 = millis();
    const size_t n = WiFiClient::write(buf, size);
    if (n < size && connected()) {
        _stalls++;
        Log.printf("[MQTT] write stalled (%u of %u bytes in %u ms), closing the connection to reconnect\n",
                   (unsigned)n, (unsigned)size, (unsigned)(millis() - t0));
        stop();
    }
    return n;
}
