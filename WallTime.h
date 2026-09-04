// Trustworthy UTC adoption for ESP32 station-mode builds.
//
// This never adjusts millis() or microReticulum's logical clock. NTP updates
// only the separate wall clock exposed by OS::wall_time_*().
#pragma once

#if defined(HAS_RNS) && MCU_VARIANT == MCU_ESP32 && HAS_WIFI == true

#include <sys/time.h>
#include <time.h>

#ifndef WALL_TIME_NTP_SERVER_1
#define WALL_TIME_NTP_SERVER_1 "pool.ntp.org"
#endif
#ifndef WALL_TIME_NTP_SERVER_2
#define WALL_TIME_NTP_SERVER_2 "time.cloudflare.com"
#endif
#ifndef WALL_TIME_NTP_SERVER_3
#define WALL_TIME_NTP_SERVER_3 "time.google.com"
#endif
#ifndef WALL_TIME_MAX_NTP_STEP_MS
#define WALL_TIME_MAX_NTP_STEP_MS 604800000ULL // seven days once time is known
#endif
#ifndef WALL_TIME_NTP_ACQUIRE_POLL_MS
#define WALL_TIME_NTP_ACQUIRE_POLL_MS 1000UL
#endif
#ifndef WALL_TIME_NTP_RESYNC_POLL_MS
#define WALL_TIME_NTP_RESYNC_POLL_MS 21600000UL // six hours
#endif
#ifndef WALL_TIME_NTP_MIN_CORRECTION_MS
#define WALL_TIME_NTP_MIN_CORRECTION_MS 1000ULL
#endif

static bool wall_time_ntp_started = false;
static uint32_t wall_time_last_poll_ms = 0;
static wl_status_t wall_time_last_wifi_status = WL_IDLE_STATUS;

inline const char* wall_time_result_name(RNS::Utilities::OS::WallTimeResult result) {
  using Result = RNS::Utilities::OS::WallTimeResult;
  switch (result) {
    case Result::ACCEPTED: return "accepted";
    case Result::BACKWARDS: return "backwards";
    case Result::JUMP_TOO_LARGE: return "jump-too-large";
    default: return "invalid";
  }
}

inline void wall_time_update() {
  const wl_status_t status = WiFi.status();
  const bool station_online = wifi_mode == WR_WIFI_STA &&
      !wifi_ap_fallback_active && status == WL_CONNECTED;

  if (!station_online) {
    if (wall_time_last_wifi_status == WL_CONNECTED) wall_time_ntp_started = false;
    wall_time_last_wifi_status = status;
    return;
  }
  wall_time_last_wifi_status = status;

  if (!wall_time_ntp_started) {
    // UTC epoch is timezone-independent. configTime starts the ESP-IDF SNTP
    // client asynchronously; the loop below adopts only a plausible result.
    configTime(0, 0, WALL_TIME_NTP_SERVER_1, WALL_TIME_NTP_SERVER_2,
               WALL_TIME_NTP_SERVER_3);
    wall_time_ntp_started = true;
    wall_time_last_poll_ms = 0;
    printf("[time] NTP requested on station WiFi\n");
  }

  const uint32_t now_ms = millis();
  const bool clock_needs_source = !RNS::Utilities::OS::wall_time_known() ||
      RNS::Utilities::OS::wall_time_source() ==
          RNS::Utilities::OS::WallTimeSource::PERSISTED;
  const uint32_t poll_interval = clock_needs_source
      ? WALL_TIME_NTP_ACQUIRE_POLL_MS : WALL_TIME_NTP_RESYNC_POLL_MS;
  if (wall_time_last_poll_ms != 0 &&
      now_ms - wall_time_last_poll_ms < poll_interval) return;
  wall_time_last_poll_ms = now_ms;

  timeval tv{};
  gettimeofday(&tv, nullptr);
  const uint64_t candidate = (uint64_t)tv.tv_sec * 1000ULL +
                             (uint64_t)tv.tv_usec / 1000ULL;
  // The library performs the authoritative plausibility check. This early
  // gate avoids logging the ESP32's 1970 startup clock once per minute.
  if (candidate < 946684800000ULL) return;

  // Never attempt a backwards correction. Small negative NTP phase errors are
  // normal and the monotonic-derived wall clock will catch up naturally.
  //
  // Returning here is the *healthy* path -- NTP checked and agreed closely
  // enough that no correction was worth applying -- so record that it happened.
  // Without it "sync age" counts from the last adoption, and a node whose
  // checks keep agreeing looks progressively more stale the better its clock
  // is. Rev 1 reported 11 hours that way while its NTP was working perfectly.
  if (RNS::Utilities::OS::wall_time_known()) {
    const uint64_t current = RNS::Utilities::OS::wall_time_millis();
    if (candidate <= current || candidate - current < WALL_TIME_NTP_MIN_CORRECTION_MS) {
      RNS::Utilities::OS::note_wall_time_verified();
      return;
    }
  }

  // A restored value is only a monotonic lower bound: it cannot account for
  // time spent powered off. A trusted network source may therefore advance it
  // without the live-clock step limit used after synchronization.
  const uint64_t max_step =
      RNS::Utilities::OS::wall_time_source() ==
          RNS::Utilities::OS::WallTimeSource::PERSISTED
      ? UINT64_MAX : WALL_TIME_MAX_NTP_STEP_MS;
  const auto result = RNS::Reticulum::adopt_wall_time(
      candidate, RNS::Utilities::OS::WallTimeSource::NTP,
      max_step);
  if (result == RNS::Utilities::OS::WallTimeResult::ACCEPTED) {
    printf("[time] adopted NTP UTC %llu ms\n", candidate);
  } else {
    printf("[time] rejected NTP UTC %llu ms: %s\n", candidate,
           wall_time_result_name(result));
  }
}

#else

inline void wall_time_update() {}

#endif
