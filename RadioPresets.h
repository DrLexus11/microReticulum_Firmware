// Named LoRa PHY presets.
//
// The PHY parameters below must be identical on every node that needs to hear
// every other node, and the failure mode when they are not is silent and
// total: strong RF arrives and nothing demodulates, in both directions, while
// every health indicator (radio_online, hw_ready, noise floor, the reported SF
// itself) looks perfect. That is exactly the 2026-08-22 outage, which cost a
// day to diagnose because bandwidth and spreading factor are individually
// settable and had drifted apart on one node.
//
// Presets exist so a fleet agrees by *name* -- "put everything on ShortFast" --
// rather than by three separately-settable numbers that can diverge one node at
// a time. A single enum is far harder to get half-right than three integers.
//
// The ladder is deliberately Meshtastic's: it is a well-travelled set of
// working points, and matching it keeps mental models portable for anyone who
// arrives from that stack. Frequency and TX power are intentionally NOT part of
// a preset -- those are regional/regulatory and per-installation concerns, not
// link-rate concerns, and bundling them would make a preset unsafe to apply
// across regions.
//
// Adding a preset is safe. CHANGING an existing one is not: nodes agree by
// index over the wire, so an edited row silently means different parameters on
// nodes running different firmware. Append; never renumber.

#ifndef RADIOPRESETS_H
#define RADIOPRESETS_H

#include <stdint.h>
#include <stddef.h>

struct RadioPreset {
	const char* name;
	uint32_t    bw;		// Hz
	uint8_t     sf;
	uint8_t     cr;		// 4/n denominator
};

static const RadioPreset RADIO_PRESETS[] = {
	{ "ShortTurbo",   500000,  7, 5 },	// fastest, least range
	{ "ShortFast",    250000,  7, 5 },
	{ "ShortSlow",    250000,  8, 5 },
	{ "MediumFast",   250000,  9, 5 },
	{ "MediumSlow",   250000, 10, 5 },
	{ "LongFast",     250000, 11, 5 },
	{ "LongModerate", 125000, 11, 8 },
	{ "LongSlow",     125000, 12, 8 },	// slowest, most range
};

#define RADIO_PRESET_COUNT ((uint8_t)(sizeof(RADIO_PRESETS)/sizeof(RADIO_PRESETS[0])))

// Reported when the live configuration matches no preset -- i.e. bandwidth, SF
// or coding rate were set individually. Readable, never settable: writing it
// would not describe any configuration.
#define RADIO_PRESET_CUSTOM ((uint8_t)255)

#endif
