// Jellyfin Wheel - the HDR frame (Stage 12). While the console's output is in HDR10, every frame goes through here:
//   1. the kit draws the UI into an image of its own (a render pass compatible with the program's, cleared to transparent),
//   2. one full-screen pass lays the UI over the film in linear light and packs the result as 10:10:10:2 words (PQ).
// The film is either an SDR picture (the program's RGBA8 surface) or an HDR10 picture held here as RGBA16 (BT.2020, PQ).
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

namespace jfhp
{

struct Config
{
	VkPhysicalDevice physical{ VK_NULL_HANDLE };
	VkDevice device{ VK_NULL_HANDLE };
	VkQueue queue{ VK_NULL_HANDLE };
	std::uint32_t family{ 0 };
	VkRenderPass baseRenderPass{ VK_NULL_HANDLE }; // the program's pass on the swapchain (colour + depth)
	VkFormat colorFormat{ VK_FORMAT_UNDEFINED };   // the swapchain's (B8G8R8A8_UNORM)
	VkFormat depthFormat{ VK_FORMAT_UNDEFINED };
	VkImageView depthView{ VK_NULL_HANDLE };
	VkImageView sdrView{ VK_NULL_HANDLE }; // the program's RGBA8 film surface, resting in SHADER_READ_ONLY_OPTIMAL
	std::uint32_t width{ 0 };
	std::uint32_t height{ 0 };
};

struct Look
{
	int film{ 0 };            // 0 nothing, 1 the SDR surface, 2 the HDR surface
	float x{ 0 }, y{ 0 }, w{ 1 }, h{ 1 }; // the film's rectangle on the screen, 0..1
	int frameW{ 0 }, frameH{ 0 };         // the picture's size in pixels
	float uiNits{ 203.0f };
	float sdrNits{ 203.0f };
	bool redLow{ false };     // false: blue in the low bits of the 10:10:10:2 word
};

} // namespace jfhp

extern "C" {
// C linkage because the program includes this header inside its own namespace (like jf_hdrlab.h)
void *jfhp_create(VkPhysicalDevice physical, VkDevice device, VkQueue queue, std::uint32_t family, VkRenderPass baseRenderPass,
	VkFormat colorFormat, VkFormat depthFormat, VkImageView depthView, VkImageView sdrView, std::uint32_t width, std::uint32_t height);
void jfhp_destroy(void *pass);
void jfhp_set_gain(int percent); // the film's brightness at once: 25..400, 100 = as mastered
bool jfhp_uploadHdr(void *pass, const std::uint16_t *rgba, int w, int h);
void jfhp_beginUi(void *pass, VkCommandBuffer cmd);
void jfhp_endUi(void *pass, VkCommandBuffer cmd);
void jfhp_compose(void *pass, VkCommandBuffer cmd, VkFramebuffer framebuffer, int film, float x, float y, float w, float h,
	int frameW, int frameH, float uiNits, float sdrNits, bool redLow);
}

namespace jfhp
{

class Pass
{
public:
	Pass() = default;
	Pass(const Pass &) = delete;
	Pass &operator=(const Pass &) = delete;
	~Pass() { destroy(); }

	static constexpr int kSurfaceW = 1920;
	static constexpr int kSurfaceH = 1080;

	bool create(const Config &c)
	{
		destroy();
		p_ = jfhp_create(c.physical, c.device, c.queue, c.family, c.baseRenderPass, c.colorFormat, c.depthFormat, c.depthView,
			c.sdrView, c.width, c.height);
		return p_ != nullptr;
	}
	// The caller has waited for the device to be idle.
	void destroy()
	{
		if (p_) {
			jfhp_destroy(p_);
			p_ = nullptr;
		}
	}
	bool ready() const { return p_ != nullptr; }
	// An HDR10 picture: w x h RGBA, 16 bits a channel, little endian (w <= 1920, h <= 1080). Waits for the copy.
	bool uploadHdr(const std::uint16_t *rgba, int w, int h) { return p_ && jfhp_uploadHdr(p_, rgba, w, h); }
	// Recorded into the frame's command buffer: the kit's prepare(), beginUi, the kit's draw(), endUi, compose.
	void beginUi(VkCommandBuffer cmd) { jfhp_beginUi(p_, cmd); }
	void endUi(VkCommandBuffer cmd) { jfhp_endUi(p_, cmd); }
	void compose(VkCommandBuffer cmd, VkFramebuffer fb, const Look &l)
	{
		jfhp_compose(p_, cmd, fb, l.film, l.x, l.y, l.w, l.h, l.frameW, l.frameH, l.uiNits, l.sdrNits, l.redLow);
	}

private:
	void *p_{ nullptr };
};

} // namespace jfhp
