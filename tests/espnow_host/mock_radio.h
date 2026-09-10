#pragma once
// Host fakes for exercising the real ESPNowInterface queue/recovery code.
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <stdexcept>
#define BOARDS_H
#define HAS_WIFI true
#define MCU_ESP32 0x81
#define MCU_VARIANT MCU_ESP32
#define ERROR(...) ((void)0)
#define ERRORF(...) ((void)0)
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
inline uint32_t tick = 10000;
inline uint32_t millis() { return tick; }
inline void delay(uint32_t ms) { tick += ms; }
inline uint32_t esp_random() { return 17; }
using esp_err_t = int;
constexpr int ESP_OK=0, ESP_ERR_ESPNOW_EXIST=1;
using wifi_interface_t = int;
using wifi_mode_t = int;
using wifi_second_chan_t = int;
constexpr int WIFI_IF_STA=0, WIFI_IF_AP=1, WIFI_MODE_STA=1, WIFI_MODE_AP=2,
    WIFI_MODE_APSTA=3, WIFI_SECOND_CHAN_NONE=0, WL_CONNECTED=3;
struct MockWiFi {
    int mode=WIFI_MODE_STA, status_value=6;
    int getMode() { return mode; }
    int status() { return status_value; }
    void disconnect(bool, bool) {}
};
inline MockWiFi WiFi;
struct wifi_country_t { uint8_t schan, nchan; };
inline uint8_t channel = 9;
inline int esp_wifi_get_country(wifi_country_t* c) { *c={1,11}; return 0; }
inline int esp_wifi_set_channel(uint8_t c, int) { channel=c; return 0; }
inline int esp_wifi_get_channel(uint8_t* c, int*) { *c=channel; return 0; }
inline int esp_wifi_get_mac(int, uint8_t* m) { memset(m,1,6); return 0; }
using esp_now_send_status_t = int;
constexpr int ESP_NOW_SEND_SUCCESS=0;
struct esp_now_peer_info_t { uint8_t peer_addr[6]; uint8_t channel; int ifidx; bool encrypt; };
inline void (*send_cb)(const uint8_t*, int) = nullptr;
inline int esp_now_init() { return 0; }
inline int esp_now_deinit() { return 0; }
inline int esp_now_register_recv_cb(void(*)(const uint8_t*,const uint8_t*,int)) { return 0; }
inline int esp_now_register_send_cb(void(*cb)(const uint8_t*,int)) { send_cb=cb; return 0; }
inline void esp_now_unregister_recv_cb() {}
inline void esp_now_unregister_send_cb() { send_cb=nullptr; }
inline bool esp_now_is_peer_exist(const uint8_t*) { return true; }
inline int esp_now_add_peer(const esp_now_peer_info_t*) { return 0; }
struct Sent { std::array<uint8_t,6> mac; std::vector<uint8_t> wire; };
inline std::vector<Sent> sent;
inline int fail_mac = -1;
inline int esp_now_send(const uint8_t* m, const uint8_t* d, size_t n) {
    Sent s; memcpy(s.mac.data(),m,6); s.wire.assign(d,d+n); sent.push_back(s);
    send_cb(m, m[0]==fail_mac ? 1 : 0); return 0;
}
namespace RNS {
class Bytes {
    std::vector<uint8_t> v;
public:
    Bytes() = default;
    Bytes(const uint8_t* p, size_t n):v(p,p+n) {}
    size_t size() const { return v.size(); }
    const uint8_t* data() const { return v.data(); }
};
class Reticulum {
public:
    inline static bool enabled=true;
    static bool transport_enabled() { return enabled; }
    static void transport_enabled(bool v) { enabled=v; }
};
class Transport {
public:
    inline static uint32_t received=0;
    static uint32_t packets_received() { return received; }
};
class InterfaceImpl {
protected:
    bool _IN=false,_OUT=false,_online=false,_ifac_required=false;
    size_t _HW_MTU=0; uint32_t _bitrate=0; std::string _name;
    Bytes _ifac_key;
    virtual bool send_outgoing(const Bytes&) { return true; }
    void handle_incoming(const Bytes&) { ++Transport::received; }
    void handle_outgoing(const Bytes&) {}
public:
    explicit InterfaceImpl(const char* n):_name(n) {}
    virtual ~InterfaceImpl()=default;
    virtual bool start() { return true; }
    virtual void stop() {}
    virtual void detach() {}
    virtual void loop() {}
};
namespace Cryptography {
inline Bytes hkdf(size_t n, const Bytes&, const Bytes&) {
    std::vector<uint8_t> zero(n); return Bytes(zero.data(),n);
}
}
}
