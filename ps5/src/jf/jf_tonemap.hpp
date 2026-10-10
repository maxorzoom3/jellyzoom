/* HDR to SDR on the CPU: PQ (HDR10, Dolby Vision's base layer) and HLG pictures, as 16-bit R'G'B' in BT.2020,
 * become the 8-bit BT.709 picture the screen shows.
 *
 * Per pixel: the code values become linear light (a table each), BT.2020 becomes BT.709, the luminance is
 * squeezed with a "mobius" curve (straight below the knee, smoothly bent to white above it) and the colour
 * follows it so hues stay; what is still too bright is desaturated towards the grey instead of clipped per
 * channel; last the SDR gamma, and an ordered dither so the dark gradients do not band.
 * Header only, so it can be tested on a computer. */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace jftm
{

constexpr float kWhiteNits = 203.0f; // the level of reference (diffuse) white in HDR10 and HLG, which becomes SDR white

struct Mapper
{
	int transfer{ 0 };   // 1 PQ, 2 HLG
	float peak{ 0.0f };  // the brightest level of the picture, in reference whites
	std::vector<float> linear;   // 4096 entries: code value to linear light (reference whites for PQ, scene light for HLG)
	std::vector<float> oetf;     // 4096 entries: linear 0..1 to 0..255
	std::vector<float> hlgGain;  // 1024 entries: luminance to luminance^0.2
	float knee{ 0.6f };
	float ma{ 0.0f }, mb{ 1.0f };

	static float pqEotf(float v)
	{
		const double m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
		const double np = std::pow(std::max(static_cast<double>(v), 0.0), 1.0 / m2);
		return static_cast<float>(std::pow(std::max(np - c1, 0.0) / (c2 - c3 * np), 1.0 / m1) * 10000.0);
	}

	static float hlgInverse(float e)
	{
		const double a = 0.17883277, b = 0.28466892, c = 0.55991073;
		return static_cast<float>(e <= 0.5f ? e * e / 3.0 : (std::exp((e - c) / a) + b) / 12.0);
	}

	// The tone curve: input and output in reference whites, 1.0 is SDR white
	float curve(float x) const
	{
		if (x <= knee)
			return x;
		return (mb * mb + 2.0f * mb * knee + knee * knee) / (mb - ma) * (x + ma) / (x + mb);
	}

	void prepare(int newTransfer, float peakNits)
	{
		const float newPeak = std::max(1.2f, peakNits / kWhiteNits);
		if (newTransfer == transfer && std::fabs(newPeak - peak) < 0.01f && !linear.empty())
			return;
		transfer = newTransfer;
		peak = newPeak;
		linear.resize(4096);
		for (int i = 0; i < 4096; i++) {
			const float v = static_cast<float>(i) / 4095.0f;
			linear[static_cast<size_t>(i)] = transfer == 2 ? hlgInverse(v) : pqEotf(v) / kWhiteNits;
		}
		hlgGain.resize(1024);
		for (int i = 0; i < 1024; i++)
			hlgGain[static_cast<size_t>(i)] = std::pow(std::max(static_cast<float>(i) / 1023.0f, 1e-4f), 0.2f);
		oetf.resize(4096);
		for (int i = 0; i < 4096; i++)
			oetf[static_cast<size_t>(i)] = std::pow(static_cast<float>(i) / 4095.0f, 1.0f / 2.2f) * 255.0f;
		// the mobius curve through (knee, knee) and (peak, 1)
		const float j = knee, p = peak;
		ma = -j * j * (p - 1.0f) / (j * j - 2.0f * j + p);
		mb = (j * j - 2.0f * j * p + p) / std::max(p - 1.0f, 1e-6f);
	}

	// Maps the rows [from, to) of an image `w` wide. rgb48 is R,G,B 16-bit, `stride` elements a row.
	void run(const std::uint16_t *rgb48, size_t stride, std::uint8_t *rgba, int w, int from, int to) const
	{
		static const float kDither[16] = { 0.5f / 16, 8.5f / 16, 2.5f / 16, 10.5f / 16, 12.5f / 16, 4.5f / 16, 14.5f / 16, 6.5f / 16,
			3.5f / 16, 11.5f / 16, 1.5f / 16, 9.5f / 16, 15.5f / 16, 7.5f / 16, 13.5f / 16, 5.5f / 16 };
		const float *lin = linear.data();
		const float *gain = hlgGain.data();
		const float *oe = oetf.data();
		const bool hlg = transfer == 2;
		for (int y = from; y < to; y++) {
			const std::uint16_t *src = rgb48 + static_cast<size_t>(y) * stride;
			std::uint8_t *dst = rgba + static_cast<size_t>(y) * static_cast<size_t>(w) * 4;
			for (int x = 0; x < w; x++, src += 3, dst += 4) {
				float r = lin[src[0] >> 4], g = lin[src[1] >> 4], b = lin[src[2] >> 4];
				if (hlg) {
					// the HLG system gamma: scene light becomes display light, in reference whites
					const float ys = 0.2627f * r + 0.6780f * g + 0.0593f * b;
					const float k = gain[std::min(1023, static_cast<int>(ys * 1023.0f + 0.5f))] * (1000.0f / kWhiteNits);
					r *= k;
					g *= k;
					b *= k;
				}
				// BT.2020 to BT.709 primaries
				float R = 1.6605f * r - 0.5876f * g - 0.0728f * b;
				float G = -0.1246f * r + 1.1329f * g - 0.0083f * b;
				float B = -0.0182f * r - 0.1006f * g + 1.1187f * b;
				const float lum = 0.2126f * R + 0.7152f * G + 0.0722f * B;
				if (lum > 1e-6f) {
					const float ym = curve(lum);
					const float k = ym / lum;
					R *= k;
					G *= k;
					B *= k;
					const float mx = std::max(R, std::max(G, B));
					if (mx > 1.0f && mx > ym) {
						// too bright in one channel: slide towards the grey of the same brightness
						const float t = (mx - 1.0f) / (mx - ym);
						R += (ym - R) * t;
						G += (ym - G) * t;
						B += (ym - B) * t;
					}
				} else {
					R = G = B = 0.0f;
				}
				const float d = kDither[((y & 3) << 2) | (x & 3)];
				const float vr = oe[std::min(4095, std::max(0, static_cast<int>(R * 4095.0f + 0.5f)))];
				const float vg = oe[std::min(4095, std::max(0, static_cast<int>(G * 4095.0f + 0.5f)))];
				const float vb = oe[std::min(4095, std::max(0, static_cast<int>(B * 4095.0f + 0.5f)))];
				dst[0] = static_cast<std::uint8_t>(std::min(255.0f, vr + d));
				dst[1] = static_cast<std::uint8_t>(std::min(255.0f, vg + d));
				dst[2] = static_cast<std::uint8_t>(std::min(255.0f, vb + d));
				dst[3] = 255;
			}
		}
	}
};

} // namespace jftm
