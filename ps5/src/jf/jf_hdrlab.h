// Jellyfin Wheel - HDR lab (Stage 11). Switches the console's output to HDR10 (BT.2100 PQ, 10:10:10:2) for a few
// seconds and draws test patterns in four bit layouts, so the right one can be found on the real console.
// Everything here is CPU-side and Vulkan-free; the program copies the pattern onto the swapchain image.
#pragma once

#include <cstdint>

// The program includes this inside its own namespace, so the functions have C linkage (no namespace in their names);
// jfhdr:: below only forwards to them.
extern "C" {
bool jfhdr_wanted(void);
int jfhdr_findHandle(void);
bool jfhdr_setOutput(int handle, bool hdr, std::uint32_t width, std::uint32_t height);
void jfhdr_logStatus(int handle, const char *when);
void jfhdr_fill(std::uint32_t *px, std::uint32_t width, std::uint32_t height, int variant);
const char *jfhdr_variantName(int variant);
double jfhdr_pqEncode(double nits);
}

namespace jfhdr {

constexpr int kVariants = 4;

// True once if the flag file /data/jellyzoom-hdr-lab exists (the file is removed so the lab runs for one start only).
inline bool wanted() { return jfhdr_wanted(); }
// The VideoOut handle the swapchain's buffers were registered on (-1 if none is found). Logged.
inline int findHandle() { return jfhdr_findHandle(); }
// Retype the registered buffers to HDR10 (hdr true) or back to SDR. Returns true if VideoOut accepted it.
inline bool setOutput(int handle, bool hdr, std::uint32_t w, std::uint32_t hgt) { return jfhdr_setOutput(handle, hdr, w, hgt); }
// Logs the output status (dynamic range, flags) with a label.
inline void logStatus(int handle, const char *when) { jfhdr_logStatus(handle, when); }
// Test pattern for a variant (0..3) as 32-bit words. Variant bit 0: 0 = red in the low bits, 1 = blue in the low bits;
// bit 1: 0 = PQ-coded values, 1 = gamma-coded values. White squares at the top left count the variant (1..4).
inline void fill(std::uint32_t *px, std::uint32_t w, std::uint32_t hgt, int variant) { jfhdr_fill(px, w, hgt, variant); }
inline const char *variantName(int variant) { return jfhdr_variantName(variant); }
// SMPTE ST 2084 inverse EOTF: nits (0..10000) to the signal 0..1.
inline double pqEncode(double nits) { return jfhdr_pqEncode(nits); }

} // namespace jfhdr
