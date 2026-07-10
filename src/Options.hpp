#pragma once

// Conversion options, threaded from the command line down through
// Csar -> Cgrp -> Cbnk. Grouped rather than passed as loose flags so that
// adding one does not touch every constructor signature again.
struct Options
{
	// Do not ignore pan values of stereo samples (-p).
	bool Pan = false;

	// Seconds to hold a release-127 voice open, or 0 for the hardware
	// behaviour (--pad-sustain).
	//
	// Byte 127 is the envelope's fastest-rate sentinel: the NW4C rate
	// conversion is the Wii NW4R EnvGenerator::CalcRelease verbatim, including
	// `if (x == 127) return 65535.0f;`, so on hardware such a voice stops
	// essentially instantly at note-off. The seconds-long tail heard on console
	// comes from the DSP reverb, which the sequence carries as a CC91 send.
	//
	// A soundfont cannot carry that reverb, and a General MIDI player's own
	// reverb is nothing like the 3DS DSP's, so dry renders of release-127 pads
	// sound abruptly cut. This option trades accuracy for that: it fakes the
	// missing tail by over-holding the note. It is named for the deviation, not
	// for correctness, and defaults to off.
	double PadSustainSeconds = 0.0;
};
