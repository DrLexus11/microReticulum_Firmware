// Which part of loop() was running when the task watchdog fired.
//
// `CarriedIssues.md` §1: this node resets with TASK_WDT at irregular intervals,
// measured from 4 minutes to over 10 hours, and environment is ruled out. The
// watchdog names the *task* -- loopTask -- which is not news, because loop() is
// where all of this firmware's work happens. What is missing is which call
// inside it stopped returning.
//
// esp_task_wdt_reset() is fed once, at the end of loop(). So any phase before it
// that blocks for longer than the watchdog period trips it, and the reset
// destroys the evidence.
//
// Two things here, and they answer different questions:
//
// - A breadcrumb in RTC memory, which survives a reset. On the next boot, if the
//   reset reason was TASK_WDT, the phase recorded there is the one that hung.
//   That converts "a task watchdog fired" into "it hung in <name>".
//
// - A worst-case duration per phase, in RAM, readable over provisioning. This
//   finds a phase that is getting slow *before* it crosses the threshold, which
//   is the difference between diagnosing this once and seeing it coming.
//
// Cost is a store to RTC memory and a millis() subtraction per phase, per loop.

#pragma once

#include <stdint.h>

#define LOOP_PHASE_NONE        0
#define LOOP_PHASE_HEAP        1
#define LOOP_PHASE_NOMAD_ANN   2
#define LOOP_PHASE_RRC         3
#define LOOP_PHASE_BLE_PEER    4
#define LOOP_PHASE_RADIO_WD    5
#define LOOP_PHASE_LORA_CFG    6
#define LOOP_PHASE_RADIO_CMT   7
#define LOOP_PHASE_LXMF_ANN    8
#define LOOP_PHASE_LXMF_SYNC   9
#define LOOP_PHASE_RETICULUM  10
#define LOOP_PHASE_RADIO_ON   11
#define LOOP_PHASE_TX_QUEUE   12
#define LOOP_PHASE_PERIPH     13
#define LOOP_PHASE_MEMORY     14
#define LOOP_PHASE_COUNT      15

inline const char* loop_phase_name(uint8_t phase) {
	switch (phase) {
		case LOOP_PHASE_HEAP:      return "heap_watch";
		case LOOP_PHASE_NOMAD_ANN: return "nomadnet_announce_watch";
		case LOOP_PHASE_RRC:       return "rrc_hub_loop";
		case LOOP_PHASE_BLE_PEER:  return "ble_peer_loop";
		case LOOP_PHASE_RADIO_WD:  return "radio_rx_watchdog";
		case LOOP_PHASE_LORA_CFG:  return "lora_config_consistency_watch";
		case LOOP_PHASE_RADIO_CMT: return "radio_commit_confirm_watch";
		case LOOP_PHASE_LXMF_ANN:  return "lxmf_propagation_announce_watch";
		case LOOP_PHASE_LXMF_SYNC: return "lxmf_peer_sync_watch";
		case LOOP_PHASE_RETICULUM: return "reticulum.loop";
		case LOOP_PHASE_RADIO_ON:  return "radio_online block";
		case LOOP_PHASE_TX_QUEUE:  return "tx_queue_handler";
		case LOOP_PHASE_PERIPH:    return "peripherals (display/pmu/bt/wifi/input)";
		case LOOP_PHASE_MEMORY:    return "memory_low handling";
		default:                   return "none";
	}
}

#if defined(ESP32)

#include <esp_attr.h>

// Deliberately NOINIT: it must survive a reset with its value intact, which is
// the entire point. A magic guards against reading uninitialised RTC memory
// after a genuine power loss, where the value means nothing.
#define LOOP_PHASE_MAGIC 0x4C50484Eu   // "LPHN"
extern RTC_NOINIT_ATTR uint32_t loop_phase_magic;
extern RTC_NOINIT_ATTR uint8_t  loop_phase_current;
extern RTC_NOINIT_ATTR uint8_t  loop_phase_at_reset;

inline uint32_t* loop_phase_worst() {
	static uint32_t worst[LOOP_PHASE_COUNT] = {0};
	return worst;
}

inline uint8_t& loop_phase_active() {
	static uint8_t active = LOOP_PHASE_NONE;
	return active;
}

inline uint32_t& loop_phase_started() {
	static uint32_t started = 0;
	return started;
}

// Close the previous phase, open this one. Called with LOOP_PHASE_NONE at the
// end of loop() so the last phase is measured too.
inline void loop_phase(uint8_t phase) {
	const uint32_t now = millis();
	const uint8_t prev = loop_phase_active();
	if (prev != LOOP_PHASE_NONE && prev < LOOP_PHASE_COUNT) {
		const uint32_t took = now - loop_phase_started();
		if (took > loop_phase_worst()[prev]) loop_phase_worst()[prev] = took;
	}
	loop_phase_active()  = phase;
	loop_phase_started() = now;
	loop_phase_current   = phase;   // the breadcrumb, in RTC memory
}

// Called once at boot, before loop() runs. Captures the breadcrumb left by the
// previous run and re-arms it.
inline void loop_phase_boot(bool was_task_wdt) {
	if (loop_phase_magic == LOOP_PHASE_MAGIC && was_task_wdt) {
		loop_phase_at_reset = loop_phase_current;
		printf("[wdt] previous reset was TASK_WDT while in phase %u (%s)\n",
		       (unsigned)loop_phase_at_reset, loop_phase_name(loop_phase_at_reset));
	}
	else {
		// Either the first boot after a power loss, so the breadcrumb is
		// meaningless, or a reset that was not the watchdog and does not
		// implicate a phase.
		loop_phase_at_reset = LOOP_PHASE_NONE;
	}
	loop_phase_magic   = LOOP_PHASE_MAGIC;
	loop_phase_current = LOOP_PHASE_NONE;
}

inline uint8_t loop_phase_last_wdt()  { return loop_phase_at_reset; }

// The slowest phase seen since boot, and how long it took.
inline uint8_t loop_phase_worst_id() {
	uint8_t worst = LOOP_PHASE_NONE; uint32_t best = 0;
	for (uint8_t i = 1; i < LOOP_PHASE_COUNT; i++) {
		if (loop_phase_worst()[i] > best) { best = loop_phase_worst()[i]; worst = i; }
	}
	return worst;
}
inline uint32_t loop_phase_worst_ms() {
	uint32_t best = 0;
	for (uint8_t i = 1; i < LOOP_PHASE_COUNT; i++) {
		if (loop_phase_worst()[i] > best) best = loop_phase_worst()[i];
	}
	return best;
}

#else   // not ESP32: no RTC memory, and no task watchdog to diagnose

inline void loop_phase(uint8_t phase) { (void)phase; }
inline void loop_phase_boot(bool was_task_wdt) { (void)was_task_wdt; }
inline uint8_t  loop_phase_last_wdt()  { return LOOP_PHASE_NONE; }
inline uint8_t  loop_phase_worst_id()  { return LOOP_PHASE_NONE; }
inline uint32_t loop_phase_worst_ms()  { return 0; }

#endif
