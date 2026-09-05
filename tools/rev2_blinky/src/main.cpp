#include <Arduino.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>

// Rev2 D2: 3V3 -> 330 ohm -> LED -> GPIO4 (active low).
constexpr uint8_t LED_PIN = 4;
constexpr size_t TEST_WORDS = 16384; // Exercise 64 KiB of external RAM.
HardwareSerial debugUART(0);
volatile uint32_t *testRAM = nullptr;
uint32_t passes = 0;
uint32_t errors = 0;
uint32_t nextReport = 0;
const char *resetName(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON: return "POWERON";
        case ESP_RST_EXT: return "EXTERNAL";
        case ESP_RST_SW: return "SOFTWARE";
        case ESP_RST_PANIC: return "PANIC";
        case ESP_RST_INT_WDT: return "INT_WDT";
        case ESP_RST_TASK_WDT: return "TASK_WDT";
        case ESP_RST_WDT: return "WDT";
        case ESP_RST_BROWNOUT: return "BROWNOUT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        default: return "OTHER";
    }
}
void output(const char *line) {
    if (Serial) Serial.println(line);
    debugUART.println(line);
}
void setup() {
    digitalWrite(LED_PIN, HIGH);
    pinMode(LED_PIN, OUTPUT);
    Serial.begin(115200);
    debugUART.begin(115200, SERIAL_8N1, 44, 43);
    testRAM = static_cast<uint32_t *>(heap_caps_malloc(
        TEST_WORDS * sizeof(uint32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    output("BOOT rev2-n16r2-blinky-v1; D2=1Hz; USB+UART0=115200; WiFi/LoRa off");
}
void loop() {
    const uint32_t now = millis();
    digitalWrite(LED_PIN, (now % 1000 < 500) ? LOW : HIGH);
    if (static_cast<int32_t>(now - nextReport) >= 0) {
        nextReport = now + 1000;
        if (testRAM) {
            const uint32_t seed = 0xA5A55A5Au ^ passes;
            for (size_t i = 0; i < TEST_WORDS; ++i) testRAM[i] = seed ^ (i * 2654435761u);
            for (size_t i = 0; i < TEST_WORDS; ++i)
                if (testRAM[i] != (seed ^ (i * 2654435761u))) ++errors;
            ++passes;
        }
        char line[320];
        const auto reason = esp_reset_reason();
        snprintf(line, sizeof(line),
            "UP=%llus reset=%s(%d) heap=%u min_heap=%u flash=%u psram=%u ram_test=%s passes=%lu errors=%lu",
            static_cast<unsigned long long>(esp_timer_get_time() / 1000000),
            resetName(reason), static_cast<int>(reason), ESP.getFreeHeap(),
            ESP.getMinFreeHeap(), ESP.getFlashChipSize(), ESP.getPsramSize(),
            testRAM ? (errors ? "FAIL" : "OK") : "ALLOC_FAIL",
            static_cast<unsigned long>(passes), static_cast<unsigned long>(errors));
        output(line);
    }
    delay(5);
}
