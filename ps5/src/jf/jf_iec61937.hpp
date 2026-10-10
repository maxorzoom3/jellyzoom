/* IEC 61937 bursts for the console's compressed-audio ports: Dolby Digital (AC-3), Dolby Digital Plus
 * (E-AC-3) and DTS core. Written from the published layout (a 4-word preamble, then the frame, padded
 * with zeros to the burst period); no code from any other project.
 *
 * A burst is what the port is fed as S16 stereo: 16-bit words, little-endian in memory, the payload's
 * byte pairs swapped (the standard carries 16-bit big-endian words).
 *   AC-3    period 1536 frames at 48 kHz  (6144 bytes)   Pc 0x01 | bsmod << 8, Pd = bits
 *   E-AC-3  period 6144 frames at 192 kHz (24576 bytes)  Pc 0x15,               Pd = bytes
 *   DTS     period 512 / 1024 / 2048 frames at 48 kHz    Pc 0x0B / 0x0C / 0x0D, Pd = bits
 */
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

namespace jfiec
{
enum Kind { None = 0, Ac3 = 1, Eac3 = 2, Dts = 3 };

inline int portMode(int kind) { return kind == Ac3 ? 0 : kind == Dts ? 2 : 3; } // sceAudioOutEx modes
inline int grainFrames(int kind) { return kind == Eac3 ? 1024 : 256; }
inline int portMul(int kind) { return kind == Eac3 ? 4 : 1; } // port frames per 48 kHz frame of sound
inline const char *kindName(int kind) { return kind == Ac3 ? "Dolby Digital" : kind == Eac3 ? "Dolby Digital Plus" : kind == Dts ? "DTS" : "PCM"; }

// Builds one burst from one frame (a packet). Returns the burst's size in bytes, 0 if the packet cannot be sent as it is.
// seconds48 gets the sound's length in 48 kHz frames.
inline size_t pack(int kind, const std::uint8_t *d, int n, std::vector<std::uint8_t> &out, int *seconds48)
{
	if (!d || n < 10) {
		return 0;
	}
	size_t period = 0;
	unsigned pc = 0, pd = 0;
	switch (kind) {
	case Ac3: {
		if (d[0] != 0x0B || d[1] != 0x77 || (d[5] >> 3) > 10 || (d[4] >> 6) != 0) {
			return 0; // not AC-3, or not 48 kHz
		}
		period = 6144;
		pc = 0x01u | (static_cast<unsigned>(d[5] & 7) << 8);
		pd = static_cast<unsigned>(n) * 8u;
		if (seconds48) {
			*seconds48 = 1536;
		}
		break;
	}
	case Eac3: {
		if (d[0] != 0x0B || d[1] != 0x77) {
			return 0;
		}
		if ((d[5] >> 3) > 10) {
			if ((d[4] >> 6) != 0 || ((d[4] >> 4) & 3) != 3) {
				return 0; // not 48 kHz, or not the 6-block frames this packer sends one to a burst
			}
		} else {
			// A Dolby Digital core (6 blocks, 48 kHz) with a second frame behind it: the form some 7.1 tracks come in.
			// ac3Words: frame size in 16-bit words for frmsizecod 0..37 at 48 kHz (one entry per pair of codes).
			static const unsigned short ac3Words[19] = { 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512, 640, 768, 896, 1024, 1152, 1280 };
			const unsigned code = d[4] & 0x3Fu;
			if ((d[4] >> 6) != 0 || code >= 38) {
				return 0;
			}
			const int core = ac3Words[code >> 1] * 2;
			if (n < core + 10 || d[core] != 0x0B || d[core + 1] != 0x77) {
				return 0;
			}
		}
		period = 24576;
		pc = 0x15;
		pd = static_cast<unsigned>(n);
		if (seconds48) {
			*seconds48 = 1536;
		}
		break;
	}
	case Dts: {
		if (d[0] != 0x7F || d[1] != 0xFE || d[2] != 0x80 || d[3] != 0x01) {
			return 0;
		}
		const int blocks = ((d[4] & 1) << 6) | (d[5] >> 2);
		const int fsize = (((d[5] & 3) << 12) | (d[6] << 4) | (d[7] >> 4)) + 1;
		const int samples = (blocks + 1) * 32;
		if (fsize > n || (samples != 512 && samples != 1024 && samples != 2048)) {
			return 0;
		}
		n = fsize; // the core only: whatever the HD extension adds after it stays behind
		period = static_cast<size_t>(samples) * 4;
		pc = samples == 512 ? 0x0Bu : samples == 1024 ? 0x0Cu : 0x0Du;
		pd = static_cast<unsigned>(n) * 8u;
		if (seconds48) {
			*seconds48 = samples;
		}
		break;
	}
	default:
		return 0;
	}
	if (static_cast<size_t>(n) + 8 > period || pd > 0xFFFFu) {
		return 0;
	}
	out.assign(period, 0);
	out[0] = 0x72; out[1] = 0xF8; // Pa 0xF872
	out[2] = 0x1F; out[3] = 0x4E; // Pb 0x4E1F
	out[4] = static_cast<std::uint8_t>(pc & 0xFF);
	out[5] = static_cast<std::uint8_t>(pc >> 8);
	out[6] = static_cast<std::uint8_t>(pd & 0xFF);
	out[7] = static_cast<std::uint8_t>(pd >> 8);
	for (int i = 0; i + 1 < n; i += 2) {
		out[8 + static_cast<size_t>(i)] = d[i + 1];
		out[9 + static_cast<size_t>(i)] = d[i];
	}
	if (n & 1) {
		out[8 + static_cast<size_t>(n - 1)] = 0;
		out[8 + static_cast<size_t>(n)] = d[n - 1];
	}
	return period;
}

// Which formats the HDMI sink says it decodes: the 0x180-byte block of sceAudioOutSysGetHdmiMonitorInfo(1, ...),
// whose 8-byte records start at 0x98: [CEA format][channels][..][0][rates][...]. 2 AC-3, 10 E-AC-3, 7 DTS.
inline void sinkFormats(const std::uint8_t *info, unsigned size, bool *ac3, bool *eac3, bool *dts)
{
	*ac3 = *eac3 = *dts = false;
	for (unsigned at = 0x98; at + 8 <= size && at < 0xF0; at += 8) {
		const unsigned fmt = info[at];
		if (fmt == 0) {
			break;
		}
		if (fmt == 2) {
			*ac3 = true;
		} else if (fmt == 10) {
			*eac3 = true;
		} else if (fmt == 7) {
			*dts = true;
		}
	}
}
} // namespace jfiec
