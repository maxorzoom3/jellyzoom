/* Dolby TrueHD for the console's compressed port: MAT frames (IEC 61937-9 style), experimental.
 * Twenty-four TrueHD access units (40 samples each at 48 kHz, 20 ms in all) go into one 61424-byte MAT payload,
 * each in a 2560-byte slot, with a start code at the front, a middle code at slot 12 and an end code at the back.
 * The burst around it is 61440 bytes: Pa Pb Pc Pd (data type 0x16, length in bytes), the payload with its byte pairs
 * swapped, zeros. Fed to the port as S16 stereo that is 15360 frames, 4096 bytes (1024 frames) at a time.
 */
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

namespace jfmat
{
constexpr int kPayload = 61424;
constexpr int kSlot = 2560;
constexpr int kBurst = 61440;

struct Packer
{
	std::vector<std::uint8_t> hd = std::vector<std::uint8_t>(kBurst, 0);
	int count = 0; // access units in the frame so far

	void reset() { count = 0; }

	// Adds one access unit. Returns 0 while the frame fills, kBurst when `out` holds a complete burst, -1 if the unit does not fit a slot.
	int add(const std::uint8_t *d, int n, std::vector<std::uint8_t> &out)
	{
		static const std::uint8_t start[20] = { 0x07, 0x9E, 0x00, 0x03, 0x84, 0x01, 0x01, 0x01, 0x80, 0x00,
			0x56, 0xA5, 0x3B, 0xF4, 0x81, 0x83, 0x49, 0x80, 0x77, 0xE0 };
		static const std::uint8_t middle[12] = { 0xC3, 0xC1, 0x42, 0x49, 0x3B, 0xFA, 0x82, 0x83, 0x49, 0x80, 0x77, 0xE0 };
		static const std::uint8_t end[16] = { 0xC3, 0xC2, 0xC0, 0xC4, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x97, 0x11 };
		if (!d || n <= 0) {
			return -1;
		}
		int codeLen = 0;
		if (count == 0) {
			std::memcpy(hd.data(), start, sizeof(start));
			codeLen = 28; // the start code (20) plus the 8 bytes the burst header takes from the first slot
		} else if (count == 12) {
			std::memcpy(&hd[12 * kSlot - 8 - 4], middle, sizeof(middle));
			codeLen = 8;
		}
		if (n > kSlot - codeLen) {
			return -1;
		}
		const int at = count * kSlot - 8 + codeLen;
		std::memcpy(&hd[static_cast<size_t>(at)], d, static_cast<size_t>(n));
		std::memset(&hd[static_cast<size_t>(at + n)], 0, static_cast<size_t>(kSlot - n - codeLen));
		if (++count < 24) {
			return 0;
		}
		count = 0;
		std::memcpy(&hd[kPayload - sizeof(end)], end, sizeof(end));
		out.assign(kBurst, 0);
		out[0] = 0x72; out[1] = 0xF8; // Pa 0xF872
		out[2] = 0x1F; out[3] = 0x4E; // Pb 0x4E1F
		out[4] = 0x16; out[5] = 0x00; // Pc: data type 22
		out[6] = static_cast<std::uint8_t>(kPayload & 0xFF);
		out[7] = static_cast<std::uint8_t>(kPayload >> 8);
		for (int i = 0; i + 1 < kPayload; i += 2) {
			out[8 + static_cast<size_t>(i)] = hd[static_cast<size_t>(i) + 1];
			out[9 + static_cast<size_t>(i)] = hd[static_cast<size_t>(i)];
		}
		return kBurst;
	}
};
} // namespace jfmat
