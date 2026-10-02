// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#pragma once

// The service contract of docs/IoTPlatform.md (R3, PR F step F3). Everything
// that runs in the main loop becomes a service with one lifecycle, driven by
// the application: init -> start -> poll ... -> stop. Pure: no Arduino, no
// FreeRTOS, so it builds and is tested on Linux, as IMPR-Vox needs.
//
// The rules that bind a service (IoTPlatform.md, "Rules drawn from this week's
// failures"):
//   - poll() returns within its budget (default 20 ms). The runner measures
//     every poll and counts overruns; it does not trust the service to.
//   - poll() does not allocate in the steady state. init() allocates.
//   - health() comes from something measured -- traffic, peers, a successful
//     operation within a window -- never from a flag the service set itself.
//   - A service disabled by configuration says so, in one line, at boot and in
//     its health reason.

#include <cstdint>

#include "TelemetryCodec.h"

enum class ServiceState : uint8_t {
  Starting = 0,   // initialised, not yet shown to work
  Healthy  = 1,
  Degraded = 2,   // working, short of what it should be doing
  Failed   = 3,   // not working; the application may restart it
  Disabled = 4,   // off by configuration, not by fault
};

inline const char* service_state_name(ServiceState state) {
  switch (state) {
    case ServiceState::Starting: return "starting";
    case ServiceState::Healthy:  return "healthy";
    case ServiceState::Degraded: return "degraded";
    case ServiceState::Failed:   return "failed";
    case ServiceState::Disabled: return "disabled";
  }
  return "unknown";
}

// A state and a short reason, for a person: "no peer 2 min", "radio not
// responding". The reason is a string literal or a buffer the service owns; it
// must outlive the call.
struct Health {
  ServiceState state = ServiceState::Starting;
  const char* reason = "";
};

// What the application hands every service at init. Read-only, shared; it grows
// as services need it (identity, typed config), never as a channel between
// services -- those talk through mailboxes the application wires.
struct AppContext {
  uint32_t boot_ms = 0;
};

class IService {
public:
  virtual ~IService() = default;

  virtual const char* name() const = 0;
  // Allocate everything here. False means the service cannot run; the
  // application reports it and carries on without it.
  virtual bool init(const AppContext& context) = 0;
  virtual bool start() = 0;
  // Bounded by budget_ms(); never allocates in the steady state.
  virtual void poll(uint32_t now_ms) = 0;
  virtual Health health() const = 0;
  // Each service writes its own fields of the board's health report, so the
  // report is the sum of the services' own accounts.
  virtual void telemetry(NodeTelemetry& report) const = 0;
  virtual void stop() = 0;

  // A service that must take longer says so here, rather than overrunning.
  virtual uint32_t budget_ms() const { return 20; }
};
