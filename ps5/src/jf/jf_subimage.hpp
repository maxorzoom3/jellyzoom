// Picture subtitles (Blu-ray PGS, DVD, DVB) burnt into a converted frame. Header only; no FFmpeg in here,
// so it can be tested on its own. A Bitmap is straight (not premultiplied) RGBA on a "canvas" of cw x ch
// pixels; blend() scales it to the frame it lands on.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace jfsub {

struct Bitmap
{
	double start{ 0.0 };
	double end{ 0.0 };
	int x{ 0 }, y{ 0 }, w{ 0 }, h{ 0 }; // place and size on the canvas
	int cw{ 0 }, ch{ 0 };               // the canvas the subtitle was authored for (often 1920x1080)
	std::vector<std::uint8_t> rgba;     // w * h * 4, straight alpha
};

// Draws `b` over an RGBA frame of dw x dh (stride dw * 4). The picture is scaled with the canvas: a 1080p
// subtitle on a 1080p frame is copied pixel for pixel; a smaller target is averaged over the source pixels
// it covers, a bigger one is interpolated.
inline void blend(std::uint8_t *dst, int dw, int dh, const Bitmap &b)
{
	if (dst == nullptr || dw <= 0 || dh <= 0 || b.w <= 0 || b.h <= 0 || b.cw <= 0 || b.ch <= 0 ||
	    b.rgba.size() < static_cast<size_t>(b.w) * b.h * 4) {
		return;
	}
	const double sx = static_cast<double>(dw) / b.cw;
	const double sy = static_cast<double>(dh) / b.ch;
	const int x0 = std::max(0, static_cast<int>(std::floor(b.x * sx)));
	const int y0 = std::max(0, static_cast<int>(std::floor(b.y * sy)));
	const int x1 = std::min(dw, static_cast<int>(std::ceil((b.x + b.w) * sx)));
	const int y1 = std::min(dh, static_cast<int>(std::ceil((b.y + b.h) * sy)));
	const double fx = 1.0 / sx; // source pixels per target pixel
	const double fy = 1.0 / sy;
	for (int py = y0; py < y1; py++) {
		// the source rows this target row covers
		const double v0 = (py - b.y * sy) * fy;
		const double v1 = v0 + fy;
		for (int px = x0; px < x1; px++) {
			const double u0 = (px - b.x * sx) * fx;
			const double u1 = u0 + fx;
			double r = 0, g = 0, a = 0, bl = 0, wsum = 0; // premultiplied sums
			if (fx <= 1.0 && fy <= 1.0) {
				// magnifying (or 1:1): bilinear at the pixel's centre
				const double cu = (u0 + u1) * 0.5 - 0.5;
				const double cv = (v0 + v1) * 0.5 - 0.5;
				const int iu = static_cast<int>(std::floor(cu));
				const int iv = static_cast<int>(std::floor(cv));
				const double tu = cu - iu;
				const double tv = cv - iv;
				for (int j = 0; j < 2; j++) {
					for (int i = 0; i < 2; i++) {
						const int su = iu + i;
						const int sv = iv + j;
						const double wgt = (i ? tu : 1.0 - tu) * (j ? tv : 1.0 - tv);
						if (su < 0 || sv < 0 || su >= b.w || sv >= b.h || wgt <= 0.0) {
							// outside the picture counts as transparent: weight without colour
							wsum += wgt;
							continue;
						}
						const std::uint8_t *s = &b.rgba[(static_cast<size_t>(sv) * b.w + su) * 4];
						const double al = s[3] / 255.0;
						r += wgt * al * s[0];
						g += wgt * al * s[1];
						bl += wgt * al * s[2];
						a += wgt * al;
						wsum += wgt;
					}
				}
			} else {
				// shrinking: the mean of every source pixel under this one
				const int su0 = std::max(0, static_cast<int>(std::floor(u0)));
				const int su1 = std::min(b.w, static_cast<int>(std::ceil(u1)));
				const int sv0 = std::max(0, static_cast<int>(std::floor(v0)));
				const int sv1 = std::min(b.h, static_cast<int>(std::ceil(v1)));
				for (int sv = sv0; sv < sv1; sv++) {
					const double wy = std::min<double>(sv + 1, v1) - std::max<double>(sv, v0);
					for (int su = su0; su < su1; su++) {
						const double wx = std::min<double>(su + 1, u1) - std::max<double>(su, u0);
						const double wgt = wx * wy;
						if (wgt <= 0.0) {
							continue;
						}
						const std::uint8_t *s = &b.rgba[(static_cast<size_t>(sv) * b.w + su) * 4];
						const double al = s[3] / 255.0;
						r += wgt * al * s[0];
						g += wgt * al * s[1];
						bl += wgt * al * s[2];
						a += wgt * al;
						wsum += wgt;
					}
				}
			}
			if (wsum <= 0.0 || a <= 0.0) {
				continue;
			}
			// r,g,bl are colour * alpha, summed with weights; a is the weighted alpha. Normalise by the weights.
			const double alpha = std::min(1.0, a / wsum);
			const double pr = r / wsum, pg = g / wsum, pb = bl / wsum; // premultiplied colour
			std::uint8_t *d = dst + (static_cast<size_t>(py) * dw + px) * 4;
			const double inv = 1.0 - alpha;
			d[0] = static_cast<std::uint8_t>(std::min(255.0, pr + d[0] * inv + 0.5));
			d[1] = static_cast<std::uint8_t>(std::min(255.0, pg + d[1] * inv + 0.5));
			d[2] = static_cast<std::uint8_t>(std::min(255.0, pb + d[2] * inv + 0.5));
			// d[3] stays opaque
		}
	}
}

// One 0xAARRGGBB palette entry (FFmpeg's AVSubtitleRect palette) as straight RGBA bytes
inline void paletteEntry(std::uint32_t c, std::uint8_t out[4])
{
	out[0] = static_cast<std::uint8_t>((c >> 16) & 255);
	out[1] = static_cast<std::uint8_t>((c >> 8) & 255);
	out[2] = static_cast<std::uint8_t>(c & 255);
	out[3] = static_cast<std::uint8_t>((c >> 24) & 255);
}

} // namespace jfsub
