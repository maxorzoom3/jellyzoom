// The Jellyzoom logo: full size in the middle for a moment when the app starts, then it glides up into the top-left corner
// and stays there. The picture is a PNG inside the program (jf_brand_png.inc), decoded with the gate's image decoder.
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>

#include "jf_gate.h"

namespace jfbrand {

#include "jf_brand_png.inc"

class Logo
{
  public:
	using Create = std::function<std::uint32_t(int, int, const unsigned char *)>;

	// Makes the texture; false when the picture would not decode (nothing is drawn then)
	bool init(const Create &create)
	{
		int w = 0, h = 0;
		unsigned char *rgba = nullptr;
		if (!jf_gate_decode_image(kBrandPng, static_cast<int>(sizeof(kBrandPng)), &w, &h, &rgba) || !rgba) {
			return false;
		}
		tex_ = create(w, h, rgba);
		std::free(rgba);
		start_ = std::chrono::steady_clock::now();
		return tex_ != 0;
	}

	std::uint32_t texture() const { return tex_; }
	void forget() { tex_ = 0; }

	// Seconds the splash has been running (0 until init)
	float clock() const
	{
		if (!tex_) {
			return 0.0f;
		}
		return std::chrono::duration<float>(std::chrono::steady_clock::now() - start_).count();
	}

	// Draws over everything of the library and the sign-in pages: the splash (a dark veil, the logo large and glowing),
	// then the small logo in the corner
	void draw(hui::gfx::DrawList &list) const
	{
		if (!tex_) {
			return;
		}
		using hui::gfx::Color;
		using hui::gfx::Rect;
		const float t = clock();
		constexpr float kIn = 0.9f, kHold = 2.4f, kMove = 0.9f;
		const float move = clamp01((t - kHold) / kMove);
		const float e = move * move * (3.0f - 2.0f * move); // ease in and out
		const float fadeIn = clamp01(t / kIn);
		const float rise = 1.0f - (1.0f - fadeIn) * (1.0f - fadeIn);
		// the veil is there from the first frame and lifts as the logo leaves for the corner
		if (e < 1.0f) {
			list.rounded_rect(Rect{ 0.0f, 0.0f, 1920.0f, 1080.0f }, 0.0f, Color{ 0.012f, 0.014f, 0.024f, 0.96f * (1.0f - e) });
		}
		const float bigSize = 560.0f, smallSize = 70.0f;
		const float size = bigSize + (smallSize - bigSize) * e;
		const float cx = 960.0f + (48.0f + smallSize * 0.5f - 960.0f) * e;
		const float cy = 520.0f + (20.0f + smallSize * 0.5f - 520.0f) * e;
		const float grow = (0.88f + 0.12f * rise) * (1.0f + 0.015f * std::sin(t * 2.6f) * (1.0f - e));
		const float alpha = (e < 1.0f ? rise : 1.0f) * (e >= 1.0f ? 0.96f : 1.0f);
		const float s = size * (e < 1.0f ? grow : 1.0f);
		const Rect r{ cx - s * 0.5f, cy - s * 0.5f, s, s };
		if (e < 1.0f) {
			// a glow behind it that breathes, and falls away as it leaves
			const float pulse = 0.5f + 0.5f * std::sin(t * 3.0f);
			list.glow(r.inset(s * 0.12f), s * 0.3f, s * 0.35f, Color{ 0.18f, 0.65f, 1.0f, (0.30f + 0.12f * pulse) * rise * (1.0f - e) });
		}
		list.image(tex_, r, hui::gfx::kFullUv, Color{ 1.0f, 1.0f, 1.0f, alpha }, 0.0f);
	}

  private:
	static float clamp01(float v) { return std::max(0.0f, std::min(1.0f, v)); }
	std::uint32_t tex_{ 0 };
	std::chrono::steady_clock::time_point start_;
};

} // namespace jfbrand
