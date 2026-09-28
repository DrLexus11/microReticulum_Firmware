// BootLog.h -- keeping bootlog.txt as a ring of its newest lines.
//
// The bootlog records every boot's reason and the previous run's uptime, and is
// echoed at boot so a board that died in the field can be read after it is
// replugged. It used to be deleted whole once past 4 KB, which lost all of its
// history at once -- exactly the record the heap-leak and watchdog work in
// CarriedIssues #1 reads. Past the cap it now keeps the newest lines.
//
// Pure: no I/O, no allocation. Moves into the protocol/utility layer of
// docs/IoTPlatform.md as it arrives.

#pragma once

#include <cstddef>

// Past this size the log is trimmed before the next line is appended.
#define BOOTLOG_CAP_BYTES 4096
// How much of the newest history a trim keeps.
#define BOOTLOG_KEEP_BYTES 2048

// Where the newest `keep` bytes of whole lines begin in buf[0..n): an offset at
// a line start, so no half line survives a trim. 0 when everything fits.
inline size_t bootlog_tail_start(const char* buf, size_t n, size_t keep) {
	if (n <= keep) return 0;
	size_t start = n - keep;
	while (start < n && buf[start - 1] != '\n') start++;
	return start;
}
