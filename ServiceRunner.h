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

// Runs IService instances in their declared order and measures them (R3 of
// docs/IoTPlatform.md). Pure and allocation-free: a fixed table, a clock and an
// optional hook passed in, so it is tested on Linux with a fake clock and runs
// unchanged on the boards.
//
// What it measures, per service, is what LoopPhase.h measures per loop phase on
// the ESP32 -- worst poll time ever, worst in the current window -- plus the
// count of polls over the service's own budget. The hook runs before each poll
// with the service's index, which is how the firmware keeps its TASK_WDT
// breadcrumb: the phase that was running when the watchdog fired.

#include <cstddef>
#include <cstdint>

#include "IService.h"

#ifndef SERVICE_RUNNER_CAPACITY
#define SERVICE_RUNNER_CAPACITY 24
#endif

struct ServiceTiming {
  uint32_t polls = 0;
  uint32_t overruns = 0;      // polls longer than the service's budget
  uint32_t worst_ms = 0;      // since boot
  uint32_t last_ms = 0;
};

class ServiceRunner {
public:
  using Clock = uint32_t (*)();
  using Hook = void (*)(size_t index);

  explicit ServiceRunner(Clock clock, Hook before_poll = nullptr)
    : _clock(clock), _before(before_poll) {}

  // False when the table is full: a configuration error, reported by the
  // caller in one line, never a silent drop.
  bool add(IService* service) {
    if (service == nullptr || _count >= SERVICE_RUNNER_CAPACITY) return false;
    _services[_count] = service;
    _started[_count] = false;
    _timing[_count] = ServiceTiming{};
    ++_count;
    return true;
  }

  // init then start, in order. A service that fails either is left out of the
  // poll and reported Failed; the rest carry on. Returns how many are running.
  size_t start_all(const AppContext& context) {
    size_t running = 0;
    for (size_t i = 0; i < _count; ++i) {
      _started[i] = _services[i]->init(context) && _services[i]->start();
      if (_started[i]) ++running;
    }
    return running;
  }

  // One pass over every running service, in order.
  void poll_all() {
    for (size_t i = 0; i < _count; ++i) {
      if (!_started[i]) continue;
      if (_before) _before(i);
      const uint32_t begin = _clock();
      _services[i]->poll(begin);
      const uint32_t took = _clock() - begin;   // wraps correctly in uint32
      ServiceTiming& t = _timing[i];
      ++t.polls;
      t.last_ms = took;
      if (took > t.worst_ms) t.worst_ms = took;
      if (took > _services[i]->budget_ms()) ++t.overruns;
    }
  }

  void stop_all() {
    for (size_t i = _count; i > 0; --i) {       // reverse order of start
      if (_started[i - 1]) _services[i - 1]->stop();
      _started[i - 1] = false;
    }
  }

  // The board's health report, as the sum of every service's own account. The
  // caller fills what no service owns (sender, uptime, reset reason).
  void collect(NodeTelemetry& report) const {
    for (size_t i = 0; i < _count; ++i) {
      if (_started[i]) _services[i]->telemetry(report);
    }
  }

  // A service that did not start is Failed, whatever it would say itself.
  Health health(size_t index) const {
    if (index >= _count) return Health{ServiceState::Failed, "no such service"};
    if (!_started[index]) return Health{ServiceState::Failed, "did not start"};
    return _services[index]->health();
  }

  size_t count() const { return _count; }
  IService* service(size_t index) const { return index < _count ? _services[index] : nullptr; }
  const ServiceTiming& timing(size_t index) const { return _timing[index < _count ? index : 0]; }
  bool running(size_t index) const { return index < _count && _started[index]; }

private:
  Clock _clock;
  Hook _before;
  IService* _services[SERVICE_RUNNER_CAPACITY] = {};
  bool _started[SERVICE_RUNNER_CAPACITY] = {};
  ServiceTiming _timing[SERVICE_RUNNER_CAPACITY] = {};
  size_t _count = 0;
};
