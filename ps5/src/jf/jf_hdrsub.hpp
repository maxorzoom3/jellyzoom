// Picture subtitles (jfsub::Bitmap) burnt into an HDR10 picture: RGBA, 16 bits a channel, BT.2020, PQ coded.
// The subtitle's colour is an SDR one (gamma 2.2); its white becomes kSubtitleNits, which is easier on the eyes than
// the 203 nits of reference white, and each channel is PQ coded on its own (white stays white, a colour is close enough).
// Header only; jf_subimage.hpp comes first.
#pragma once
#include "jf_subimage.hpp"

namespace jfsub {

constexpr double kSubtitleNits = 150.0;

inline const std::uint16_t *pqSubtitleTable()
{
	static std::uint16_t table[256];
	static bool done = false;
	if (!done) {
		const double m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
		for (int i = 0; i < 256; i++) {
			const double nits = std::pow(i / 255.0, 2.2) * kSubtitleNits;
			const double y = std::pow(nits / 10000.0, m1);
			const double e = std::pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
			table[i] = static_cast<std::uint16_t>(std::min(65535.0, std::max(0.0, e * 65535.0 + 0.5)));
		}
		done = true;
	}
	return table;
}

// Draws `b` over an RGBA64 frame of dw x dh (stride dw * 4 elements). Scaled as blend() scales it.
inline void blendWide(std::uint16_t *dst, int dw, int dh, const Bitmap &b)
{
	if (dst == nullptr || dw <= 0 || dh <= 0 || b.w <= 0 || b.h <= 0 || b.cw <= 0 || b.ch <= 0 ||
	    b.rgba.size() < static_cast<size_t>(b.w) * b.h * 4) {
		return;
	}
	const std::uint16_t *lut = pqSubtitleTable();
	const double sx = static_cast<double>(dw) / b.cw;
	const double sy = static_cast<double>(dh) / b.ch;
	const int x0 = std::max(0, static_cast<int>(std::floor(b.x * sx)));
	const int y0 = std::max(0, static_cast<int>(std::floor(b.y * sy)));
	const int x1 = std::min(dw, static_cast<int>(std::ceil((b.x + b.w) * sx)));
	const int y1 = std::min(dh, static_cast<int>(std::ceil((b.y + b.h) * sy)));
	const double fx = 1.0 / sx;
	const double fy = 1.0 / sy;
	for (int py = y0; py < y1; py++) {
		const double v0 = (py - b.y * sy) * fy;
		const double v1 = v0 + fy;
		for (int px = x0; px < x1; px++) {
			const double u0 = (px - b.x * sx) * fx;
			const double u1 = u0 + fx;
			double r = 0, g = 0, a = 0, bl = 0, wsum = 0;
			if (fx <= 1.0 && fy <= 1.0) {
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
			// r, g, bl are colour * alpha weighted sums, a the weighted alpha
			const double alpha = std::min(1.0, a / wsum);
			if (alpha <= 0.0) {
				continue;
			}
			const double cr = std::min(255.0, r / a), cg = std::min(255.0, g / a), cb = std::min(255.0, bl / a); // straight colour
			const double col[3] = { cr, cg, cb };
			std::uint16_t *d = dst + (static_cast<size_t>(py) * dw + px) * 4;
			for (int k = 0; k < 3; k++) {
				const double code = lut[static_cast<int>(col[k] + 0.5)];
				d[k] = static_cast<std::uint16_t>(std::min(65535.0, d[k] * (1.0 - alpha) + code * alpha + 0.5));
			}
			// d[3] stays opaque
		}
	}
}

} // namespace jfsub
