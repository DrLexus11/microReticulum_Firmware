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

// The main loop's first phases as services (PR F step F3b, R3 of
// docs/IoTPlatform.md). What the rest of the firmware sees: start them, poll
// them, read them. The adapters themselves are in LoopServicesImpl.h, which is
// included at the end of RNode_Firmware.ino because they call the functions and
// read the state defined there.

#include <cstddef>

#include "ServiceRunner.h"

// Registers the services and starts them, in loop order. Called once, at the
// end of setup(). Prints one line per service that is not running.
void loop_services_start();

// One pass over the running services -- heap_watch through reticulum.loop --
// in the order loop() used to call them. Each poll sets the TASK_WDT
// breadcrumb (LoopPhase.h) before it runs.
void loop_services_poll();

// The runner, for reading: health, stage and timing per service.
const ServiceRunner& loop_services();
