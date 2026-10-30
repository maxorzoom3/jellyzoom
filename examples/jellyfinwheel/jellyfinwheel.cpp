/*
 * Jellyfin PS5 - its program: the kit's Cover Wheel, showing your Jellyfin
 * libraries as tabs.
 *
 * The Jellyfin client runs on a thread of its own (jf_gate.cpp): it logs in,
 * lists the libraries, and fetches the titles and decoded posters around
 * whatever the wheel is looking at. This program keeps one jf::List per
 * library, fills its items in as pages arrive, turns each poster into a
 * texture of the kit's renderer (and takes the texture back when the wheel
 * moves on: only the posters near the wheel are kept), takes a palette from
 * the picture for the design to colour its backdrop with, and creates the
 * design (kit/screen.cpp, this title's copy of the kit's "carousel") once the
 * first library has its first covers. Until then it shows a loading screen.
 * Holding OPTIONS for a second leaves; a press is the design's.
 *
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "kit.hpp"
#include <type_traits>
#include "jf_hdrlab.h"
#include "jf_hdrpass.h"
#include "jf_backdrop.h"
#include "jf_gate.h"
#include "jf_playback_features.h"
#include "jf_setup.hpp"
#include "jf_profile.hpp"
#include "jf_seerr.hpp"
#include "jf_brand.hpp"

// The design on screen: this title's own, in kit/screen.cpp
std::unique_ptr<hui::app::Concept> makeScreen(hui::app::Context &context)
{
	return ps5ui::make_screen(context);
}

// ---- the film's picture ----
// One Vulkan image the player's frames are copied into; the kit draws it like any texture
// (import_texture). It rests in SHADER_READ_ONLY_OPTIMAL, as the kit requires whenever a frame draws it.
struct VideoSurface
{
	static constexpr int kWidth = 1920;
	static constexpr int kHeight = 1080;

	VkDevice dev{ VK_NULL_HANDLE };
	VkQueue queue{ VK_NULL_HANDLE };
	VkImage image{ VK_NULL_HANDLE };
	VkDeviceMemory imageMemory{ VK_NULL_HANDLE };
	VkImageView view{ VK_NULL_HANDLE };
	VkBuffer staging{ VK_NULL_HANDLE };
	VkDeviceMemory stagingMemory{ VK_NULL_HANDLE };
	void *mapped{ nullptr };
	VkCommandPool pool{ VK_NULL_HANDLE };
	VkCommandBuffer cmd{ VK_NULL_HANDLE };
	VkFence fence{ VK_NULL_HANDLE };

	static uint32_t findMemory(const VkPhysicalDeviceMemoryProperties &props, uint32_t bits, VkMemoryPropertyFlags want)
	{
		for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
			if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want) {
				return i;
			}
		}
		return UINT32_MAX;
	}

	void beginCommands()
	{
		vkResetCommandBuffer(cmd, 0);
		VkCommandBufferBeginInfo begin{};
		begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer(cmd, &begin);
	}

	void barrier(VkImageLayout from, VkImageLayout to, VkAccessFlags srcAccess, VkAccessFlags dstAccess,
		VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage)
	{
		VkImageMemoryBarrier b{};
		b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		b.srcAccessMask = srcAccess;
		b.dstAccessMask = dstAccess;
		b.oldLayout = from;
		b.newLayout = to;
		b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.image = image;
		b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
	}

	bool submitAndWait()
	{
		vkEndCommandBuffer(cmd);
		VkSubmitInfo submit{};
		submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submit.commandBufferCount = 1;
		submit.pCommandBuffers = &cmd;
		if (vkQueueSubmit(queue, 1, &submit, fence) != VK_SUCCESS) {
			return false;
		}
		vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
		vkResetFences(dev, 1, &fence);
		return true;
	}

	bool create(VkPhysicalDevice physical, VkDevice device, VkQueue q, uint32_t family)
	{
		dev = device;
		queue = q;
		VkPhysicalDeviceMemoryProperties props;
		vkGetPhysicalDeviceMemoryProperties(physical, &props);

		VkImageCreateInfo ici{};
		ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		ici.imageType = VK_IMAGE_TYPE_2D;
		ici.format = VK_FORMAT_R8G8B8A8_UNORM;
		ici.extent = { static_cast<uint32_t>(kWidth), static_cast<uint32_t>(kHeight), 1 };
		ici.mipLevels = 1;
		ici.arrayLayers = 1;
		ici.samples = VK_SAMPLE_COUNT_1_BIT;
		ici.tiling = VK_IMAGE_TILING_OPTIMAL;
		ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		if (vkCreateImage(dev, &ici, nullptr, &image) != VK_SUCCESS) {
			return false;
		}
		VkMemoryRequirements req;
		vkGetImageMemoryRequirements(dev, image, &req);
		VkMemoryAllocateInfo alloc{};
		alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		alloc.allocationSize = req.size;
		alloc.memoryTypeIndex = findMemory(props, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		if (alloc.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(dev, &alloc, nullptr, &imageMemory) != VK_SUCCESS ||
			vkBindImageMemory(dev, image, imageMemory, 0) != VK_SUCCESS) {
			return false;
		}
		VkImageViewCreateInfo vci{};
		vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		vci.image = image;
		vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vci.format = ici.format;
		vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		if (vkCreateImageView(dev, &vci, nullptr, &view) != VK_SUCCESS) {
			return false;
		}

		const VkDeviceSize bytes = static_cast<VkDeviceSize>(kWidth) * kHeight * 4;
		VkBufferCreateInfo bci{};
		bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bci.size = bytes;
		bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		if (vkCreateBuffer(dev, &bci, nullptr, &staging) != VK_SUCCESS) {
			return false;
		}
		vkGetBufferMemoryRequirements(dev, staging, &req);
		alloc.allocationSize = req.size;
		alloc.memoryTypeIndex = findMemory(props, req.memoryTypeBits,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		if (alloc.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(dev, &alloc, nullptr, &stagingMemory) != VK_SUCCESS ||
			vkBindBufferMemory(dev, staging, stagingMemory, 0) != VK_SUCCESS ||
			vkMapMemory(dev, stagingMemory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
			return false;
		}

		VkCommandPoolCreateInfo pci{};
		pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		pci.queueFamilyIndex = family;
		VkCommandBufferAllocateInfo cai{};
		cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cai.commandBufferCount = 1;
		VkFenceCreateInfo fci{};
		fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		if (vkCreateCommandPool(dev, &pci, nullptr, &pool) != VK_SUCCESS) {
			return false;
		}
		cai.commandPool = pool;
		if (vkAllocateCommandBuffers(dev, &cai, &cmd) != VK_SUCCESS || vkCreateFence(dev, &fci, nullptr, &fence) != VK_SUCCESS) {
			return false;
		}

		// black, and in the layout the kit expects
		beginCommands();
		barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
			VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		VkClearColorValue black{};
		VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
		barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
		return submitAndWait();
	}

	// Copies a w x h RGBA picture (w <= kWidth, h <= kHeight) into the image's top left corner. Waits for
	// the copy, so the pixels may change as soon as it returns.
	void upload(const uint8_t *rgba, int w, int h)
	{
		if (!view || w <= 0 || h <= 0 || w > kWidth || h > kHeight) {
			return;
		}
		std::memcpy(mapped, rgba, static_cast<size_t>(w) * h * 4);
		beginCommands();
		barrier(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT);
		VkBufferImageCopy region{};
		region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		region.imageExtent = { static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1 };
		vkCmdCopyBufferToImage(cmd, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
		barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
		submitAndWait();
	}

	// The caller has waited for the device to be idle, and has dropped the kit's handle for the view.
	void destroy()
	{
		if (!dev) {
			return;
		}
		if (fence) vkDestroyFence(dev, fence, nullptr);
		if (pool) vkDestroyCommandPool(dev, pool, nullptr);
		if (mapped) vkUnmapMemory(dev, stagingMemory);
		if (staging) vkDestroyBuffer(dev, staging, nullptr);
		if (stagingMemory) vkFreeMemory(dev, stagingMemory, nullptr);
		if (view) vkDestroyImageView(dev, view, nullptr);
		if (image) vkDestroyImage(dev, image, nullptr);
		if (imageMemory) vkFreeMemory(dev, imageMemory, nullptr);
		*this = VideoSurface();
	}
};

class VulkanExample : public ps5ui::KitExample
{
public:
	hui::Settings choices;
	hui::app::Telemetry telemetry;
	std::unique_ptr<hui::app::Context> context;
	std::unique_ptr<hui::app::Concept> screen;
	hui::app::Frame frame;
	hui::ui::Feedback feedback;
	ps5ui::HoldToLeave leaving;
	hui::gfx::DrawList leavingList;
	hui::gfx::DrawList loadingList;
	jfsetup::Screen signIn;
	jfprofile::Menu profileMenu;
	jfseerr::Screen seerr;
	bool seerrBound{ false };
	hui::gfx::DrawList profileList;
	jfbrand::Logo brand;           // the Jellyzoom logo: the splash at the start, then the corner
	hui::gfx::DrawList brandList;
	int pendingAccount{ 0 };
	int avatarTick{ 0 };
	double fpsSeconds{ 0.0 };
	int fpsFrames{ 0 };
	float loadingClock{ 0.0f };
	bool drained{ false };

	// the film
	jfp::Player player;
	VideoSurface video;
	std::uint32_t videoTex{ 0 };
	// HDR10 playback (Stage 12)
	jfhp::Pass hdrPass;
	int hdrStep{ -1 }; // Stage 19: HDR brightness, steps of 5% above as mastered, 0..20
	bool hdrOn{ false };    // the console's output is in HDR10
	bool hdrTried{ false }; // this film has been looked at
	int hdrHandle{ -1 };
	int videoMode{ 0 };     // what the picture on screen is: 1 the SDR surface, 2 the HDR surface
	hui::ui::MediaControls controls;
	hui::gfx::DrawList playerList;
	bool playerActive{ false };
	int playingItem{ -1 };
	int videoW{ 0 }, videoH{ 0 };
	bool chaptersSet{ false };
	float playerClock{ 0.0f };
	jfp::SegmentPrompt introPrompt;
	jfp::IntroPromptTimer introTimer;
	jfp::SegmentPrompt outroPrompt;
	jfp::SeekPreview seekPreview;
	JfGatePlaybackExtras playbackExtras{};
	bool playbackExtrasKnown{ false };
	struct TrickplayTexture {
		int sheetIndex;
		int columns;
		int rows;
		std::uint32_t tex;
		std::uint64_t lastUsed;
	};
	std::vector<TrickplayTexture> trickplayTextures;
	std::uint64_t trickplayUseClock{ 0 };

	// the player's panels, subtitles and badges
	enum class Panel { none, tracks, info };
	Panel panel{ Panel::none };
	JfGateDetails playDet{};
	int playDetState{ 0 }; // 0 asked, 1 here, -1 failed
	float playDetWait{ 0.0f };
	std::vector<JfGateTrack> audioTracks, subTracks;
	int trackCol{ 0 }, trackRow{ 0 };
	std::string noteText;
	float noteClock{ 0.0f };
	std::string wheelNotice;
	float wheelNoticeClock{ 0.0f };
	bool muteFocus{ false }; // Stage 20: the pad is on the mute button
	// Stage 35: the title logo in place of the poster, and the chapter marks for the glass scrubber
	std::uint32_t logoTex{ 0 };
	int logoW{ 0 }, logoH{ 0 };
	std::vector<float> chapterTimes;
	int subFont{ 42 }; // Stage 36: the subtitle size chosen in Settings
	struct PicTex
	{
		std::uint64_t id;
		std::uint32_t tex;
	};
	std::vector<PicTex> picTex; // Stage 37: picture subtitles that are on the GPU

	// the libraries
	struct ListState {
		bool built{ false };           // its items exist
		int pagesDone{ 0 };
		std::vector<unsigned char> pageDone;
		std::vector<int> textured;     // positions holding a poster texture of their own
		double idle{ 0.0 };            // seconds since it was last the list on screen
		int lettersFrom{ 0 };          // 1 the server's letter offsets, 2 counted in the gate
		int gen{ 0 };                  // the gate's generation this list was built from
		double age{ 0.0 };             // seconds since it was built
		bool dirty{ false };           // the server's list may have changed: ask again when it is next shown
	};
	std::vector<ListState> states;
	std::uint32_t placeholderTex{ 0 };
	std::vector<std::uint32_t> retired; // textures to destroy once the device is idle
	// the backdrop behind a title's page, and the player's pad state (Stage 14)
	std::uint32_t backdropTex{ 0 };
	int backdropFor{ -1 };
	int backdropWant{ -1 };
	int backdropW{ 0 };
	int backdropH{ 0 };
	float backdropFade{ 0.0f };
	hui::gfx::DrawList backdropList;
	bool uiHiddenByPad{ false };
	int autoNext{ 0 };      // 1 while the next episode is being asked for
	double autoWait{ 0.0 };
	bool outroCompleted{ false };
	bool scrubbing{ false };
	double scrubTarget{ 0.0 };
	double retireAge{ 0.0 };
	double evictClock{ 0.0 };
	double lettersClock{ 0.0 };
	int lastCategory{ -1 };
	int seenSearch{ 0 };
	static constexpr int kKeep = 14; // posters kept each side of the wheel

	VulkanExample() : KitExample()
	{
		title = "Jellyzoom";
		name = "jellyfinwheel";
		// The kit draws the whole screen and owns the pad
		settings.overlay = false;
		ps5.ownOverlay = true;
		defaultClearColor = { { 0.0f, 0.0f, 0.0f, 1.0f } };
	}

	void prepare() override
	{
		VulkanExampleBase::prepare();
		prepareKit();
		choices.light_bar = true;
		choices.music_volume = 0;
		// The kit plays its sounds through the console's one output; give that output the film's sound too
		if (kit.audio) {
			audio_stop();
			kit.audio = audio_start(fillAll, this);
			audio_set_surround(fillSurround, wantChannels, this);
		}
		// a dark card stands in for a cover whose poster has not arrived
		{
			const int w = 16, h = 24;
			std::vector<unsigned char> pixels(static_cast<size_t>(w) * h * 4);
			for (int y = 0; y < h; y++) {
				const float v = static_cast<float>(y) / static_cast<float>(h - 1);
				for (int x = 0; x < w; x++) {
					unsigned char *p = &pixels[(static_cast<size_t>(y) * w + x) * 4];
					p[0] = static_cast<unsigned char>(48 - 22 * v);
					p[1] = static_cast<unsigned char>(52 - 24 * v);
					p[2] = static_cast<unsigned char>(70 - 30 * v);
					p[3] = 255;
				}
			}
			placeholderTex = kit.renderer.create_texture(w, h, pixels.data());
		}
		brand.init([this](int w, int h, const unsigned char *rgba) -> std::uint32_t { return kit.renderer.create_texture(w, h, rgba); });
		controls.style.theme = filmTheme();
		controls.style.layout = hui::ui::MediaLayout::compact; // Stage 20: one low row; the poster is drawn beside it
		controls.style.panel = false;
		controls.style.show_art = false;
		controls.style.auto_hide = 5.0f;
		controls.style.show_shuffle = false;
		controls.style.show_repeat = false;
		controls.style.show_volume = true;
		controls.style.show_skip = false; // Stage 35: no rewind / fast forward (L2 / R2 scrub)
		controls.style.padding = 8.0f;
		controls.style.button_size = 60.0f;
		controls.style.play_size = 76.0f;
		controls.style.button_gap = 18.0f;
		controls.style.title_size = 32.0f;
		controls.style.artist_size = 22.0f;
		controls.style.time_size = 22.0f;
		controls.style.thumb_size = 28.0f;
		controls.art = [this](hui::ui::Canvas &canvas, const hui::gfx::Rect &box, float radius) {
			const auto *it = itemFor(playingItem);
			const std::uint32_t tex = it && it->cover ? it->cover : placeholderTex;
			const float w = box.h * 2.0f / 3.0f;
			canvas.list.image(tex, { box.x + (box.w - w) * 0.5f, box.y, w, box.h }, hui::gfx::kFullUv,
				hui::gfx::Color{ 1.0f, 1.0f, 1.0f, 1.0f }, radius);
		};
		const float controlsHeight = controls.preferred_height();
		// poster on the left, the mute button on the right (both drawn by the program); the controls between them
		controls.set_bounds({ 150.0f, 1080.0f - 130.0f - controlsHeight, 1502.0f, controlsHeight }); // Stage 35: the glass controls start at the left margin, the logo above them
		context = std::make_unique<hui::app::Context>(hui::app::Context{ kit.fonts, kit.catalog, telemetry, choices });
		jf_gate_start();
		prepared = true;
	}

	// "1 h 58 min", or "47 min"; empty when the server did not say
	static std::string runtimeText(long long ticks)
	{
		const long long minutes = ticks / 600000000LL;
		if (minutes <= 0) {
			return std::string();
		}
		char text[32];
		if (minutes >= 60) {
			std::snprintf(text, sizeof(text), "%lld h %lld min", minutes / 60, minutes % 60);
		} else {
			std::snprintf(text, sizeof(text), "%lld min", minutes);
		}
		return text;
	}

	static std::string kindText(const std::string &type)
	{
		if (type == "Movie") {
			return "Movie";
		}
		if (type == "Series") {
			return "Series";
		}
		if (type == "BoxSet") {
			return "Collection";
		}
		return type.empty() ? "Film" : type;
	}

	// The picture's average colour is the cover's body tone; the design eases its backdrop toward it
	static void paletteFrom(const JfGatePoster &poster, jf::Item &item)
	{
		double r = 0.0, g = 0.0, b = 0.0;
		long samples = 0;
		for (int y = 0; y < poster.height; y += 8) {
			for (int x = 0; x < poster.width; x += 8) {
				const unsigned char *pixel = poster.rgba + 4 * (static_cast<size_t>(y) * poster.width + x);
				r += pixel[0];
				g += pixel[1];
				b += pixel[2];
				samples++;
			}
		}
		if (samples == 0) {
			return;
		}
		const hui::gfx::Color mid{ static_cast<float>(r / samples / 255.0), static_cast<float>(g / samples / 255.0),
			static_cast<float>(b / samples / 255.0), 1.0f };
		const float peak = std::max({ mid.r, mid.g, mid.b, 0.05f });
		item.mid = mid;
		item.dark = hui::gfx::mix(mid, hui::gfx::Color{ 0.02f, 0.02f, 0.03f, 1.0f }, 0.65f);
		item.accent = hui::gfx::mix(hui::gfx::Color{ mid.r / peak, mid.g / peak, mid.b / peak, 1.0f },
			hui::gfx::Color{ 1.0f, 1.0f, 1.0f, 1.0f }, 0.15f);
	}

	static int distance(int a, int b)
	{
		return a < b ? b - a : a - b;
	}

	// A title's details, from what the server said
	static void applyMeta(jf::Item &item, const JfGateMeta &meta)
	{
		item.id = meta.id ? meta.id : "";
		item.titleText = meta.name ? meta.name : "";
		item.kindRaw = meta.kind ? meta.kind : "";
		item.kindText = (meta.genre && meta.genre[0]) ? std::string(meta.genre) : kindText(meta.kind ? meta.kind : "");
		// a series says how many seasons it has, a season its episodes, a collection its titles
		item.countText.clear();
		if (meta.child_count > 0) {
			const std::string n = std::to_string(meta.child_count);
			if (item.kindRaw == "Series") {
				item.countText = n + (meta.child_count == 1 ? " season" : " seasons");
			} else if (item.kindRaw == "Season") {
				item.countText = n + (meta.child_count == 1 ? " episode" : " episodes");
			} else if (item.kindRaw == "BoxSet") {
				item.countText = n + (meta.child_count == 1 ? " title" : " titles");
			}
		}
		if (!item.countText.empty()) {
			item.kindText += "  \xC2\xB7  " + item.countText;
		}
		item.overviewText = meta.overview ? meta.overview : "";
		item.runtimeText = runtimeText(meta.runtime_ticks);
		item.year = meta.year;
		item.rating = meta.rating > 0.0f ? meta.rating / 2.0f : 0.0f; // the stars are out of five
		item.durationSeconds = static_cast<float>(static_cast<double>(meta.runtime_ticks) / 10000000.0);
		item.resumeSeconds = static_cast<float>(static_cast<double>(meta.position_ticks) / 10000000.0);
		item.progress = 0.0f;
		if (meta.runtime_ticks > 0 && meta.position_ticks > 0) {
			item.progress = std::clamp(static_cast<float>(static_cast<double>(meta.position_ticks) /
				                                           static_cast<double>(meta.runtime_ticks)), 0.0f, 1.0f);
		}
		const std::string kind = meta.kind ? meta.kind : "";
		item.playable = kind == "Movie" || kind == "Episode" || kind == "Video" || kind == "MusicVideo";
		const std::string &sortKey = (meta.sort_name && meta.sort_name[0]) ? std::string(meta.sort_name) : item.titleText;
		char first = sortKey.empty() ? '#' : sortKey[0];
		if (first >= 'a' && first <= 'z') {
			first = static_cast<char>(first - 'a' + 'A');
		}
		item.letter = (first >= 'A' && first <= 'Z') ? first : '#';
		item.favorite = meta.favorite != 0;
		item.watched = meta.watched != 0;
		item.watchedPending = meta.watched_pending != 0;
		item.fresh = meta.added_days >= 0 && meta.added_days <= 14;
		item.ready = true;
		item.bind();
	}

	// One jf::List for every library the gate has found
	void makeLists()
	{
		const int count = jf_gate_category_count();
		while (static_cast<int>(jf::lists().size()) < count) {
			std::unique_ptr<jf::List> list(new jf::List());
			list->category = static_cast<int>(jf::lists().size());
			list->name = jf_gate_category_name(list->category);
			list->kind = jf_gate_category_kind(list->category);
			jf::lists().push_back(std::move(list));
			states.emplace_back();
		}
	}

	// Throws the list's items away (their poster textures go back) so it can be made again
	void resetList(int category)
	{
		jf::List &list = *jf::lists()[static_cast<size_t>(category)];
		ListState &state = states[static_cast<size_t>(category)];
		for (const auto &item : list.items) {
			if (item->hasPoster) {
				retired.push_back(item->cover);
			}
		}
		list.items.clear();
		list.known = false;
		list.lettersKnown = false;
		list.epoch++;
		state.built = false;
		state.pagesDone = 0;
		state.pageDone.clear();
		state.textured.clear();
		state.lettersFrom = 0;
		state.age = 0.0;
		state.dirty = false;
	}

	// What an empty or waiting wheel says
	void setMessage(jf::List &list, const ListState &state)
	{
		std::string text;
		const bool empty = state.built && list.items.empty();
		switch (list.kind) {
		case 1:
			if (empty) {
				text = "Nothing to continue yet";
			}
			break;
		case 2:
			if (empty) {
				text = "No next episodes yet";
			}
			break;
		case 4:
			if (empty) {
				text = "No favourites yet: press Triangle on a title";
			}
			break;
		case 5:
			if (jf::searchText().empty()) {
				text = "Press Cross to search your libraries";
			} else if (empty) {
				text = "No titles match \"" + jf::searchText() + "\"";
			} else if (!state.built) {
				text = "Searching ...";
			}
			break;
		default:
			if (empty) {
				text = list.filter != 0 ? std::string("Nothing matches this filter") : "Nothing in " + list.name;
			}
			break;
		}
		list.message = text;
	}

	// A list gets its items once the server has said how many titles there are, and their details
	// as the pages arrive
	void syncList(int category)
	{
		jf::List &list = *jf::lists()[static_cast<size_t>(category)];
		ListState &state = states[static_cast<size_t>(category)];
		// The gate started the category over (sorted, filtered, searched, asked again): make the list again
		const int generation = jf_gate_generation(category);
		if (generation != state.gen) {
			resetList(category);
			state.gen = generation;
			list.name = jf_gate_category_name(category); // a browsed title names its list
		}
		list.lettered = jf_gate_category_lettered(category) != 0;
		list.sortable = jf_gate_category_sortable(category) != 0;
		list.sort = jf_gate_sort(category);
		list.filter = jf_gate_filter(category);
		if (!state.built) {
			const int total = jf_gate_category_total(category);
			if (total < 0) {
				setMessage(list, state);
				return;
			}
			list.items.reserve(static_cast<size_t>(total));
			for (int i = 0; i < total; i++) {
				std::unique_ptr<jf::Item> item(new jf::Item());
				item->cover = placeholderTex;
				item->gateIndex = category * jf::kStride + i;
				item->bind();
				list.items.push_back(std::move(item));
			}
			state.pageDone.assign(static_cast<size_t>((total + JF_GATE_PAGE - 1) / JF_GATE_PAGE), 0);
			list.known = true;
			state.built = true;
			state.age = 0.0;
			std::cerr << "JFGATE: \"" << list.name << "\" has " << total << " titles on the wheel" << std::endl;
		}
		setMessage(list, state);
		if (state.pagesDone < static_cast<int>(state.pageDone.size())) {
			for (size_t page = 0; page < state.pageDone.size(); page++) {
				if (state.pageDone[page] || !jf_gate_page_loaded(category, static_cast<int>(page))) {
					continue;
				}
				state.pageDone[page] = 1;
				state.pagesDone++;
				const size_t from = page * JF_GATE_PAGE;
				const size_t to = std::min(list.items.size(), from + JF_GATE_PAGE);
				for (size_t pos = from; pos < to; pos++) {
					JfGateMeta meta{};
					if (jf_gate_item_meta(category * jf::kStride + static_cast<int>(pos), &meta)) {
						applyMeta(*list.items[pos], meta);
					}
				}
			}
		}
	}

	// A-Z offsets, polled twice a second: the server's answer first, then the exact count
	void syncLetters()
	{
		for (size_t category = 0; category < jf::lists().size(); category++) {
			ListState &state = states[category];
			if (!state.built || state.lettersFrom >= 2) {
				continue;
			}
			int starts[27];
			const int from = jf_gate_letters(static_cast<int>(category), starts);
			if (from > state.lettersFrom) {
				jf::List &list = *jf::lists()[category];
				std::memcpy(list.letters, starts, sizeof(starts));
				list.lettersKnown = true;
				state.lettersFrom = from;
			}
		}
	}

	// Posters the worker has decoded become covers, three a frame
	void takePosters()
	{
		const jf::View &view = jf::view();
		for (int taken = 0; taken < 3; taken++) {
			JfGatePoster poster;
			if (!jf_gate_take_poster(&poster)) {
				return;
			}
			const int category = poster.category;
			// A poster the gate fetched for a list that has started over since is of no use
			if (category < 0 || category >= static_cast<int>(jf::lists().size()) ||
				poster.generation != jf_gate_generation(category)) {
				free(poster.rgba);
				continue;
			}
			if (poster.generation != states[static_cast<size_t>(category)].gen) {
				// the list has not caught up with the gate yet: the gate may fetch it again later
				jf_gate_poster_released(category, poster.position);
				free(poster.rgba);
				continue;
			}
			const bool known = category >= 0 && category < static_cast<int>(jf::lists().size()) &&
				states[static_cast<size_t>(category)].built && poster.position >= 0 &&
				poster.position < static_cast<int>(jf::lists()[static_cast<size_t>(category)]->items.size());
			// One that arrived after the wheel moved on is not worth a texture
			const bool wanted = known && category == view.category && distance(poster.position, view.centre) <= kKeep;
			if (!wanted || jf::lists()[static_cast<size_t>(category)]->items[static_cast<size_t>(poster.position)]->hasPoster) {
				if (known && !wanted) {
					jf_gate_poster_released(category, poster.position);
				}
				free(poster.rgba);
				continue;
			}
			jf::Item &item = *jf::lists()[static_cast<size_t>(category)]->items[static_cast<size_t>(poster.position)];
			item.cover = kit.renderer.create_texture(poster.width, poster.height, poster.rgba);
			item.hasPoster = true;
			item.aspect = static_cast<float>(poster.height) / static_cast<float>(poster.width);
			paletteFrom(poster, item);
			states[static_cast<size_t>(category)].textured.push_back(poster.position);
			free(poster.rgba);
		}
	}

	// Textures the wheel has moved away from go back; they are destroyed in batches, once the device is idle
	void flushRetired(bool force)
	{
		if (retired.empty() || (!force && retired.size() < 6 && retireAge < 1.0)) {
			return;
		}
		if (device) {
			vkDeviceWaitIdle(device);
		}
		for (const std::uint32_t id : retired) {
			kit.renderer.destroy_texture(id);
		}
		retired.clear();
		retireAge = 0.0;
	}

	void evictPosters(double dt)
	{
		evictClock += dt;
		if (evictClock < 0.25) {
			return;
		}
		const double step = evictClock;
		evictClock = 0.0;
		if (!retired.empty()) {
			retireAge += step;
		}
		const jf::View &view = jf::view();
		for (size_t category = 0; category < jf::lists().size(); category++) {
			ListState &state = states[category];
			const bool active = static_cast<int>(category) == view.category;
			state.idle = active ? 0.0 : state.idle + step;
			if (state.textured.empty()) {
				continue;
			}
			std::vector<int> keep;
			for (const int pos : state.textured) {
				const bool far = active ? distance(pos, view.centre) > kKeep : state.idle > 1.5;
				if (!far) {
					keep.push_back(pos);
					continue;
				}
				jf::Item &item = *jf::lists()[category]->items[static_cast<size_t>(pos)];
				retired.push_back(item.cover);
				item.cover = placeholderTex;
				item.hasPoster = false;
				item.aspect = 1.5f;
				jf_gate_poster_released(static_cast<int>(category), pos);
			}
			state.textured.swap(keep);
		}
		flushRetired(false);
	}

	// What the design asked for since the last frame: a new sort, a favourite, a search
	void handleRequests()
	{
		if (!jf::actionNotice().empty()) {
			wheelNotice = jf::actionNotice();
			wheelNoticeClock = 6.0f;
			jf::actionNotice().clear();
		}
		JfGateWatchedResult watched{};
		while (jf_gate_take_watched_result(&watched)) {
			if (watched.success)
				++jf::watchedRevision();
			for (size_t category = 0; category < jf::lists().size(); ++category) {
				jf::List &list = *jf::lists()[category];
				for (const auto &item : list.items) {
					if (item->id != watched.item_id)
						continue;
					item->watchedPending = false;
					if (watched.success) {
						item->watched = watched.watched != 0;
						item->progress = 0.0f;
						item->resumeSeconds = 0.0f;
					}
				}
				if (watched.success && ((list.kind >= 1 && list.kind <= 4) ||
				    (list.kind == 0 && list.filter == 1)))
					states[category].dirty = true;
			}
			if (!watched.success) {
				wheelNotice = "Could not update watched status on the server";
				wheelNoticeClock = 6.0f;
			}
		}
		for (const jf::ViewRequest &request : jf::viewRequests()) {
			jf_gate_set_view(request.category, request.sort, request.filter);
		}
		jf::viewRequests().clear();
		for (const jf::FavouriteRequest &request : jf::favouriteRequests()) {
			jf_gate_set_favorite(request.gateIndex, request.on ? 1 : 0);
			// the Favourites list is out of date now; it is asked for again when it is next shown
			for (size_t category = 0; category < jf::lists().size(); category++) {
				if (jf::lists()[category]->kind == 4) {
					states[category].dirty = true;
				}
			}
		}
		jf::favouriteRequests().clear();
		if (jf::searchSerial() != seenSearch) {
			seenSearch = jf::searchSerial();
			jf_gate_search(jf::searchText().c_str());
		}
	}

	// Continue Watching and the like change as films are watched: ask again when they are shown
	void refreshStale()
	{
		const jf::View &view = jf::view();
		const bool arrived = view.category != lastCategory;
		lastCategory = view.category;
		for (size_t category = 0; category < jf::lists().size(); category++) {
			ListState &state = states[category];
			const int kind = jf::lists()[category]->kind;
			const bool refreshable = (kind >= 1 && kind <= 4) ||
			                         (kind == 0 && jf::lists()[category]->filter == 1 && state.dirty);
			if (!state.built || !refreshable) {
				continue;
			}
			const bool shown = static_cast<int>(category) == view.category;
			const bool old = shown && arrived && state.age > 60.0;
			if ((state.dirty && (!shown || arrived)) || old) {
				jf_gate_refresh(static_cast<int>(category));
				state.dirty = false;
				state.age = 0.0;
			}
		}
	}

	// What the worker thread has brought since the last frame, and where the wheel is looking
	void housekeeping(float dt)
	{
		makeLists();
		handleRequests();
		for (size_t category = 0; category < jf::lists().size(); category++) {
			syncList(static_cast<int>(category));
			if (states[category].built) {
				states[category].age += dt;
			}
		}
		refreshStale();
		lettersClock += dt;
		if (lettersClock > 0.5) {
			lettersClock = 0.0;
			syncLetters();
		}
		takePosters();
		evictPosters(dt);
		jf_gate_want(jf::view().category, jf::view().centre);
		backdropUpdate(dt);
		seasonUpdate();
		logoUpdate();
	}

	// ---- Stage 60: a season's poster on a show's page, and the show's logo on its episode page ----
	std::vector<long long> seasonOrder;
	int seasonAsked{ -1 }, seasonAskedFor{ -1 };
	int logoWanted{ -2 };

	void seasonUpdate()
	{
		const jf::SeasonWant &want = jf::seasonWant();
		if (want.gate >= 0 && (want.gate != seasonAsked || want.season != seasonAskedFor)) {
			seasonAsked = want.gate;
			seasonAskedFor = want.season;
			if (!jf::seasonCovers().count(jf::seasonKey(want.gate, want.season))) {
				jf_gate_season_poster_want(want.gate, want.season);
			}
		}
		JfGatePoster poster;
		while (jf_gate_season_poster_take(&poster)) {
			const long long key = jf::seasonKey(poster.category, poster.position);
			auto &covers = jf::seasonCovers();
			auto old = covers.find(key);
			if (old != covers.end() && old->second.tex) {
				retired.push_back(old->second.tex);
			}
			jf::SeasonCover cover;
			cover.tex = kit.renderer.create_texture(poster.width, poster.height, poster.rgba);
			cover.w = poster.width;
			cover.h = poster.height;
			covers[key] = cover;
			seasonOrder.push_back(key);
			std::free(poster.rgba);
			while (seasonOrder.size() > 60) {
				const long long drop = seasonOrder.front();
				seasonOrder.erase(seasonOrder.begin());
				auto found = covers.find(drop);
				if (found != covers.end()) {
					if (found->second.tex) {
						retired.push_back(found->second.tex);
					}
					covers.erase(found);
				}
			}
		}
	}

	void logoUpdate()
	{
		jf::LogoInfo &info = jf::logoInfo();
		if (playingItem >= 0) {
			info.gate = -1; // the player has the logo for now
			return;
		}
		const int want = jf::logoWant();
		if (want != logoWanted) {
			logoWanted = want;
			jf_logo_want(want);
			dropLogo();
		}
		takeLogo();
		info.gate = logoWanted;
		info.tex = logoTex;
		info.w = logoW;
		info.h = logoH;
		info.state = jf_logo_state();
	}

	// ---- Stage 14: the backdrop behind a title's page ----
	// The page of a title that was opened from the wheel gets the server's own big picture of it behind the page.
	void backdropUpdate(float dt)
	{
		const int wanted = ::jf::openedTitle();
		if (wanted != backdropWant) {
			backdropWant = wanted;
			jf_backdrop_want(wanted);
		}
		JfBackdrop got;
		if (jf_backdrop_take(&got)) {
			if (got.gate_index == backdropWant) {
				const std::uint32_t tex = kit.renderer.create_texture(got.width, got.height, got.rgba);
				if (backdropTex) {
					retired.push_back(backdropTex);
				}
				backdropTex = tex;
				backdropFor = got.gate_index;
				backdropW = got.width;
				backdropH = got.height;
			}
			std::free(got.rgba);
		}
		const bool show = backdropTex != 0 && backdropWant >= 0 && backdropFor == backdropWant;
		const float target = show ? 1.0f : 0.0f;
		const float step = dt * 2.5f;
		backdropFade += std::max(-step, std::min(step, target - backdropFade));
		::jf::backdropShown() = backdropFade;
	}

	// The picture, covering the screen; the design's own dimming and the opaque page go over it
	void backdropDraw()
	{
		backdropList.clear();
		if (backdropFade > 0.005f && backdropTex != 0) {
			const float screenAspect = 1920.0f / 1080.0f;
			const float aspect = static_cast<float>(backdropW) / static_cast<float>(std::max(1, backdropH));
			hui::gfx::Rect uv{ 0.0f, 0.0f, 1.0f, 1.0f };
			if (aspect > screenAspect) {
				uv.w = screenAspect / aspect;
				uv.x = (1.0f - uv.w) * 0.5f;
			} else {
				uv.h = aspect / screenAspect;
				uv.y = (1.0f - uv.h) * 0.5f;
			}
			backdropList.image(backdropTex, { 0.0f, 0.0f, 1920.0f, 1080.0f }, uv,
				hui::gfx::Color{ 1.0f, 1.0f, 1.0f, backdropFade }, 0.0f);
		}
		kit.renderer.draw(backdropList);
	}

	// The title a gate index names
	jf::Item *itemFor(int gateIndex)
	{
		if (gateIndex < 0) {
			return nullptr;
		}
		const size_t category = static_cast<size_t>(gateIndex / jf::kStride);
		const size_t pos = static_cast<size_t>(gateIndex % jf::kStride);
		if (category >= jf::lists().size() || pos >= jf::lists()[category]->items.size()) {
			return nullptr;
		}
		return jf::lists()[category]->items[pos].get();
	}

	// Holding OPTIONS for a second leaves; a press is the design's
	void holdOptions(float dt)
	{
		leaving.label = ps5.optionsEnds ? "Back to the menu" : "Close";
		if (!ps5.frameBudget && leaving.update(dt, (ps5_pad().held & PAD_OPTIONS) != 0)) {
			quit = true;
		}
	}

	void updateTelemetry()
	{
		if (benchmark.active) {
			// A test run's pictures must not depend on the console's timing
			telemetry.push(16.7f);
			telemetry.fps = 59.9f;
			telemetry.average_ms = 16.7f;
		} else {
			telemetry.push(frameTimer * 1000.0f);
			fpsSeconds += frameTimer;
			fpsFrames++;
			if (fpsSeconds >= 0.5) {
				telemetry.fps = static_cast<float>(fpsFrames / fpsSeconds);
				telemetry.average_ms = static_cast<float>(fpsSeconds * 1000.0 / fpsFrames);
				fpsSeconds = 0.0;
				fpsFrames = 0;
			}
			telemetry.voices = kit.mixer.active_voices();
		}
		telemetry.draw_calls = kit.renderer.last_draw_calls();
		telemetry.instances = kit.renderer.last_instances();
	}

	// Until the first library has its first covers: a message, and the posters arriving
	void renderLoading()
	{
		prepareFrame();
		const float dt = std::min(frameTimer, 0.05f);
		loadingClock += dt;
		const int start = jf_gate_category_count() > 0 ? jf_gate_start_category() : 0;
		jf::view() = jf::View{ start, 3 };
		housekeeping(dt);
		holdOptions(dt);

		const bool failed = jf_gate_finished() == 2;
		const size_t first = static_cast<size_t>(start);
		const bool built = first < jf::lists().size() && states[first].built;
		const size_t covers = built ? states[first].textured.size() : 0;
		const size_t total = built ? jf::lists()[first]->items.size() : 0;
		// Seven covers fill the screen; a short library, or a slow server, does not wait for them
		const bool enough = built && (total == 0 || covers >= std::min<size_t>(total, 7) || loadingClock > 6.0f);
		if (enough && loadingClock > 0.5f && !failed) {
			jf::startCategory() = start;
			screen = makeScreen(*context);
			screen->enter();
		choices.music_volume = 0;
			kit.apply(choices);
			feedback.play(hui::audio::Cue::welcome);
			std::cerr << "JFGATE: the wheel is up, " << jf::lists().size() << " libraries" << std::endl;
		}

		char message[96];
		if (failed) {
			std::snprintf(message, sizeof(message), "Could not load your library");
		} else {
			message[0] = 0; // nothing is written while it loads: the logo and the covers say it
		}
		loadingList.clear();
		const int authNow = jf_gate_auth_state();
		if (authNow == 1 || authNow == 2) {
			// No session yet: the sign-in pages (server, profile, password or Quick Connect)
			signIn.update(kit.input(), dt, feedback);
			signIn.draw(loadingList, kit.fonts);
		} else {
			if (message[0]) {
				hui::ui::text(loadingList, kit.fonts.display, message, 960.0f, 540.0f, 52,
					hui::gfx::Color{ 1.0f, 1.0f, 1.0f, 1.0f }, hui::gfx::Align::center);
			}
		}
		leavingList.clear();
		leaving.draw(leavingList, kit.fonts, ps5ui::active_theme());
		brandList.clear();
		brand.draw(brandList);
		kit.renderer.begin();
		kit.renderer.draw(loadingList);
		kit.renderer.draw(brandList);
		kit.renderer.draw(leavingList);
		buildKitCommandBuffer();
		submitFrame();
	}

	// ---- the film ----

	// The kit's sound and the film's, on the console's one output
	static void fillAll(int16_t *out, int frames, void *user)
	{
		VulkanExample *self = static_cast<VulkanExample *>(user);
		const jfp::State s = self->player.state();
		const bool film = s == jfp::State::buffering || s == jfp::State::playing || s == jfp::State::paused;
		if (film) {
			for (int i = 0; i < frames * 2; i++) {
				out[i] = 0;
			}
		} else {
			self->kit.mixer.render(out, frames);
		}
		self->player.mixInto(out, frames);
	}

	// 7.1 sound for a film whose track has more than two channels
	static void fillSurround(int16_t *out, int frames, void *user)
	{
		for (int i = 0; i < frames * 8; i++) {
			out[i] = 0;
		}
		static_cast<VulkanExample *>(user)->player.mixInto8(out, frames);
	}

	// How many channels the console's output should have right now
	static int wantChannels(void *user)
	{
		VulkanExample *self = static_cast<VulkanExample *>(user);
		const jfp::State s = self->player.state();
		const bool film = s == jfp::State::buffering || s == jfp::State::playing || s == jfp::State::paused;
		return film ? self->player.outputChannels() : 2;
	}

	// ---- Stage 11: the HDR lab (flag file /data/jellyzoom-hdr-lab, once) ----
	// Puts the output in HDR10 for a few seconds and copies test patterns onto the swapchain image. CIRCLE leaves early.
	struct HdrLab {
		int state{ 0 }; // 0 not looked at, 1 running, 2 over
		int variant{ 0 };
		int shown{ -1 };
		int handle{ -1 };
		float clock{ 0.0f };
		bool hdr{ false };
		bool statusLogged{ false };
		vks::Buffer staging;
	} hdrLab;
	static constexpr float kLabSeconds = 4.0f;

	void hdrLabEnd(const char *why)
	{
		if (hdrLab.hdr) {
			vkDeviceWaitIdle(device);
			jfhdr::setOutput(hdrLab.handle, false, width, height);
			hdrLab.hdr = false;
			jfhdr::logStatus(hdrLab.handle, "back in SDR");
		}
		if (hdrLab.staging.buffer != VK_NULL_HANDLE) {
			vkDeviceWaitIdle(device);
			hdrLab.staging.destroy();
		}
		hdrLab.state = 2;
		say("HDRLAB: over (%s)", why);
	}

	// true: this frame belonged to the lab
	bool hdrLabFrame()
	{
		if (hdrLab.state == 2) {
			return false;
		}
		if (hdrLab.state == 0) {
			if (!jfhdr::wanted()) {
				hdrLab.state = 2;
				return false;
			}
			say("HDRLAB: starting, swapchain %ux%u", width, height);
			hdrLab.handle = jfhdr::findHandle();
			if (hdrLab.handle < 0) {
				hdrLab.state = 2;
				return false;
			}
			jfhdr::logStatus(hdrLab.handle, "before");
			const VkDeviceSize size = static_cast<VkDeviceSize>(width) * height * 4;
			if (vulkanDevice->createBuffer(VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
					VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &hdrLab.staging, size) != VK_SUCCESS ||
				hdrLab.staging.map() != VK_SUCCESS) {
				say("HDRLAB: no staging buffer");
				hdrLab.state = 2;
				return false;
			}
			hdrLab.hdr = jfhdr::setOutput(hdrLab.handle, true, width, height);
			if (!hdrLab.hdr) {
				hdrLabEnd("the display refused HDR10");
				return false;
			}
			hdrLab.state = 1;
			hdrLab.clock = 0.0f;
		}

		const float dt = std::min(frameTimer, 0.05f);
		hui::InputFrame input = kit.input();
		hdrLab.clock += dt;
		hdrLab.variant = static_cast<int>(hdrLab.clock / kLabSeconds);
		if (hdrLab.variant >= jfhdr::kVariants || input.is_pressed(hui::Action::back)) {
			// the normal screen draws this very frame
			hdrLabEnd(hdrLab.variant >= jfhdr::kVariants ? "finished" : "CIRCLE");
			return false;
		}
		prepareFrame();
		kit.tick(dt);
		if (!hdrLab.statusLogged && hdrLab.clock > 1.5f) {
			hdrLab.statusLogged = true;
			jfhdr::logStatus(hdrLab.handle, "HDR on screen");
		}
		if (hdrLab.shown != hdrLab.variant) {
			hdrLab.shown = hdrLab.variant;
			jfhdr::fill(static_cast<std::uint32_t *>(hdrLab.staging.mapped), width, height, hdrLab.variant);
			say("HDRLAB: pattern %s", jfhdr::variantName(hdrLab.variant));
		}
		VkCommandBuffer cmd = drawCmdBuffers[currentBuffer];
		const VkCommandBufferBeginInfo begin{ .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		VK_CHECK_RESULT(vkBeginCommandBuffer(cmd, &begin));
		const VkImage image = swapChain.images[currentImageIndex];
		const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		vks::tools::insertImageMemoryBarrier(cmd, image, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, range);
		const VkBufferImageCopy region{
			.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
			.imageExtent = { width, height, 1 },
		};
		vkCmdCopyBufferToImage(cmd, hdrLab.staging.buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
		vks::tools::insertImageMemoryBarrier(cmd, image, VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, range);
		VK_CHECK_RESULT(vkEndCommandBuffer(cmd));
		submitFrame();
		return true;
	}

	void startPlayback(int gateIndex)
	{
		const jf::Item *found = itemFor(gateIndex);
		if (!found || !found->ready || !found->playable) {
			return;
		}
		jf_gate_playback_extras_clear();
		resetPlaybackExtras();
		const jf::Item &item = *found;
		if (!video.view) {
			if (!video.create(physicalDevice, device, queue, vulkanDevice->queueFamilyIndices.graphics)) {
				std::cerr << "PLAYER: could not make the video image" << std::endl;
				return;
			}
			videoTex = kit.renderer.import_texture(video.view);
		}
		controls.style.theme = filmTheme();
		controls.title.clear(); // Stage 20: no title and kind over the picture, the poster stays
		controls.artist.clear();
		muteFocus = false;
		controls.set_duration(item.durationSeconds);
		const jf::PlayOptions options = ::jf::playOptions();
		::jf::playOptions() = jf::PlayOptions();
		const float resume = options.fromStart ? 0.0f : item.resumeSeconds;
		controls.set_position(resume, true);
		controls.set_buffered(0.0f);
		controls.set_chapters({});
		controls.set_playing(true);
		controls.wake();
		videoW = videoH = 0;
		chaptersSet = false;
		playingItem = gateIndex;
		jf_logo_want(gateIndex); // Stage 35
		dropLogo();
		chapterTimes.clear();
		subFont = jfprofile::readText("/data/jellyzoom-sub-size") == "s" ? 34 : jfprofile::readText("/data/jellyzoom-sub-size") == "l" ? 54 : 42; // Stage 36
		playerActive = true;
		outroCompleted = false;
		jf_gate_pause(1); // the stream has the network
		panel = Panel::none;
		playDetState = 0;
		playDetWait = 0.0f;
		audioTracks.clear();
		subTracks.clear();
		noteText.clear();
		noteClock = 0.0f;
		playDet = JfGateDetails();
		std::cerr << "PLAYER: play \"" << item.titleText << "\" from " << resume << " s, sound track " << options.audio
				  << ", subtitles " << options.subtitle << std::endl;
		player.open(item.gateIndex, resume, options.audio, options.subtitle, true);
	}

	// ---- Stage 12: HDR10 playback ----
	// An HDR10 film puts the console's output in HDR10 (BT.2100 PQ, 10:10:10:2) for as long as it plays. From then on
	// every frame is drawn by hdrPass: the kit's UI into an image of its own, then one pass that lays it over the film
	// in linear light and packs the words. Files on the console: /data/jellyzoom-hdr-off (never switch), /data/jellyzoom-hdr-rb (red
	// in the low bits instead of blue), /data/jellyzoom-hdr-ui (a number: the nits of UI white, 203 by default).
	static bool flagFile(const char *path)
	{
		struct stat st;
		return ::stat(path, &st) == 0;
	}

	void hdrEnter()
	{
		hdrTried = true;
		if (flagFile("/data/jellyzoom-hdr-off")) {
			say("HDR: /data/jellyzoom-hdr-off exists, the film is tone-mapped to SDR");
			return;
		}
		if (useDynamicRendering || renderPass == VK_NULL_HANDLE || !video.view) {
			say("HDR: this build draws without a render pass, the film is tone-mapped to SDR");
			return;
		}
		if (!hdrPass.ready()) {
			jfhp::Config cfg;
			cfg.physical = physicalDevice;
			cfg.device = device;
			cfg.queue = queue;
			cfg.family = vulkanDevice->queueFamilyIndices.graphics;
			cfg.baseRenderPass = renderPass;
			cfg.colorFormat = swapChain.colorFormat;
			cfg.depthFormat = depthFormat;
			cfg.depthView = depthStencil.view;
			cfg.sdrView = video.view;
			cfg.width = width;
			cfg.height = height;
			if (!hdrPass.create(cfg)) {
				hdrPass.destroy();
				say("HDR: could not make the HDR frame, the film is tone-mapped to SDR");
				return;
			}
		}
		vkDeviceWaitIdle(device);
		hdrHandle = jfhdr::findHandle();
		if (hdrHandle < 0 || !jfhdr::setOutput(hdrHandle, true, width, height)) {
			say("HDR: the display did not take HDR10, the film is tone-mapped to SDR");
			return;
		}
		jfhdr::logStatus(hdrHandle, "HDR playback");
		hdrOn = true;
		videoMode = 0;
		videoW = videoH = 0;
		player.setHdrOutput(true);
		say("HDR: the output is HDR10 for this film (%s in the low bits)", flagFile("/data/jellyzoom-hdr-rb") ? "red" : "blue");
	}

	// Called before every frame of a film: the first time the film turns out to be HDR10 the output is switched
	void hdrUpdate()
	{
		if (hdrOn || hdrTried) {
			return;
		}
		const jfp::State s = player.state();
		if (s != jfp::State::opening && s != jfp::State::buffering && s != jfp::State::playing && s != jfp::State::paused) {
			return;
		}
		const jfp::Info info = player.info();
		if (info.width <= 0) {
			return;
		}
		if (info.range != "HDR10") {
			hdrTried = true; // an SDR film (or HLG, which stays tone-mapped)
			player.setPresentationReady();
			return;
		}
		hdrEnter();
		player.setPresentationReady();
	}

	// Back to SDR (and the 119.88 Hz mode) when the film is over
	void hdrLeave()
	{
		hdrTried = false;
		if (!hdrOn) {
			return;
		}
		player.setHdrOutput(false);
		vkDeviceWaitIdle(device);
		jfhdr::setOutput(hdrHandle, false, width, height);
		jfhdr::logStatus(hdrHandle, "back in SDR");
		hdrOn = false;
		videoMode = 0;
		videoW = videoH = 0;
		say("HDR: the output is SDR again");
	}

	// The frame in HDR10: the kit draws into hdrPass's image, then the pass packs film + UI onto the swapchain image
	void buildHdrCommandBuffer()
	{
		VkCommandBuffer cmd = drawCmdBuffers[currentBuffer];
		const VkCommandBufferBeginInfo begin{ .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		VK_CHECK_RESULT(vkBeginCommandBuffer(cmd, &begin));
		kit.renderer.prepare(cmd, currentBuffer);
		hdrPass.beginUi(cmd);
		kit.renderer.draw(cmd, static_cast<int>(width), static_cast<int>(height));
		hdrPass.endUi(cmd);
		jfhp::Look look;
		look.film = videoW > 0 ? videoMode : 0;
		look.frameW = videoW;
		look.frameH = videoH;
		look.x = (1920.0f - static_cast<float>(videoW)) * 0.5f / 1920.0f;
		look.y = (1080.0f - static_cast<float>(videoH)) * 0.5f / 1080.0f;
		look.w = static_cast<float>(videoW) / 1920.0f;
		look.h = static_cast<float>(videoH) / 1080.0f;
		look.redLow = flagFile("/data/jellyzoom-hdr-rb");
		static float uiNits = 0.0f;
		if (uiNits <= 0.0f) {
			uiNits = 203.0f;
			if (FILE *f = std::fopen("/data/jellyzoom-hdr-ui", "r")) {
				int n = 0;
				if (std::fscanf(f, "%d", &n) == 1 && n >= 50 && n <= 1000) {
					uiNits = static_cast<float>(n);
				}
				std::fclose(f);
			}
		}
		look.uiNits = uiNits;
		hdrPass.compose(cmd, frameBuffers[currentImageIndex], look);
		VK_CHECK_RESULT(vkEndCommandBuffer(cmd));
	}

	bool markOutroWatched()
	{
		const bool marked = player.closeAndMarkWatched();
		outroCompleted = true;
		if (!marked) {
			wheelNotice = "Could not mark this item watched on the server";
			wheelNoticeClock = 6.0f;
		} else if (jf::Item *item = itemFor(playingItem)) {
			item->watched = true;
			item->progress = 0.0f;
			item->resumeSeconds = 0.0f;
		}
		return marked;
	}

	void stopPlayback()
	{
		const double position = player.position();
		const double duration = player.duration();
		player.close();
		jf_gate_playback_extras_clear();
		resetPlaybackExtras();
		hdrLeave();
		jf::Item *item = itemFor(playingItem);
		if (item && !outroCompleted && duration > 0.0) {
			const double fraction = position / duration;
			item->watched = item->watched || fraction > 0.95;
			item->progress = fraction > 0.95 ? 0.0f : static_cast<float>(std::max(0.0, fraction));
			item->resumeSeconds = fraction > 0.95 ? 0.0f : static_cast<float>(position);
		}
		outroCompleted = false;
		playerActive = false;
		playingItem = -1;
		jf_logo_want(-1);
		dropLogo();
		logoWanted = -2;
		dropPics();
		panel = Panel::none;
		jf_gate_pause(0);
		// what was just watched changes Continue Watching and Next Up
		for (size_t category = 0; category < jf::lists().size(); category++) {
			const int kind = jf::lists()[category]->kind;
			if (kind == 1 || kind == 2) {
				states[category].dirty = true;
			}
		}
	}

	// ---- what the player shows besides the picture ----
	static std::string upperText(std::string s)
	{
		for (char &c : s) {
			c = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c;
		}
		return s;
	}

	static std::string channelName(int channels)
	{
		switch (channels) {
		case 1: return "1.0";
		case 2: return "2.0";
		case 6: return "5.1";
		case 8: return "7.1";
		default: return channels > 0 ? std::to_string(channels) + " ch" : std::string();
		}
	}

	static std::string trackName(const JfGateTrack &t)
	{
		if (t.title[0]) {
			return t.title;
		}
		if (t.lang[0]) {
			return upperText(t.lang);
		}
		return "Track " + std::to_string(t.index);
	}

	static std::string trackNote(const JfGateTrack &t)
	{
		std::string n;
		const auto add = [&n](const std::string &s) {
			if (s.empty()) {
				return;
			}
			if (!n.empty()) {
				n += "  \xC2\xB7  ";
			}
			n += s;
		};
		if (t.lang[0]) {
			add(upperText(t.lang));
		}
		if (t.codec[0]) {
			add(upperText(t.codec));
		}
		if (t.kind == 1) {
			add(channelName(t.channels));
		}
		if (t.atmos) {
			add("Atmos");
		}
		if (t.is_default) {
			add("Default");
		}
		if (t.forced) {
			add("Forced");
		}
		if (t.external) {
			add("External");
		}
		if (t.kind == 2 && !t.text) {
			add("Picture subtitles: not supported yet");
		}
		return n;
	}

	// A small tag whose right edge is at x; returns its left edge
	float drawTag(hui::gfx::DrawList &list, float right, float cy, const char *label, hui::gfx::Color fill,
		hui::gfx::Color ink, float alpha)
	{
		const float w = kit.fonts.semibold.measure(label, 20) + 28.0f;
		const float x = right - w;
		list.rounded_rect({ x, cy - 20.0f, w, 40.0f }, 10.0f, fill.with_alpha(fill.a * alpha));
		hui::ui::text(list, kit.fonts.semibold, label, x + 14.0f, cy + 7.0f, 20, ink.with_alpha(alpha));
		return x - 10.0f;
	}

	void pollPlayDetails(float dt)
	{
		if (playDetState != 0 || playingItem < 0) {
			return;
		}
		playDetWait -= dt;
		if (playDetWait > 0.0f) {
			return;
		}
		playDetWait = 0.3f;
		const int state = jf_gate_details_request(playingItem);
		if (state == 1 && jf_gate_details_get(playingItem, &playDet)) {
			playDetState = 1;
			for (int i = 0; i < playDet.track_count; i++) {
				(playDet.tracks[i].kind == 1 ? audioTracks : subTracks).push_back(playDet.tracks[i]);
			}
		} else if (state < 0) {
			playDetState = -1;
		}
	}

	void openPanel(Panel which)
	{
		panel = which;
		if (which == Panel::tracks) {
			trackCol = 0;
			trackRow = 0;
			const int now = player.audioStream();
			for (size_t i = 0; i < audioTracks.size(); i++) {
				if (audioTracks[i].index == now) {
					trackRow = static_cast<int>(i);
				}
			}
		}
	}

	// Triangle: sound and subtitles. OPTIONS: the facts. Returns true while a panel has the pad.
	void panelInput(const hui::InputFrame &input)
	{
		if (input.is_pressed(hui::Action::north)) {
			if (panel == Panel::tracks) {
				panel = Panel::none;
			} else {
				openPanel(Panel::tracks);
			}
			return;
		}
		if (input.is_pressed(hui::Action::menu)) {
			if (panel == Panel::info) {
				panel = Panel::none;
			} else {
				openPanel(Panel::info);
			}
			return;
		}
		if (panel != Panel::tracks) {
			return;
		}
		const int audioRows = static_cast<int>(audioTracks.size());
		const int subRows = static_cast<int>(subTracks.size()) + 1; // "Off" first
		if (input.nav == hui::Direction::left || input.nav == hui::Direction::right) {
			const int col = input.nav == hui::Direction::right ? 1 : 0;
			if (col != trackCol) {
				trackCol = col;
				const int now = col == 0 ? player.audioStream() : player.subtitleStream();
				trackRow = 0;
				if (col == 0) {
					for (int i = 0; i < audioRows; i++) {
						if (audioTracks[static_cast<size_t>(i)].index == now) {
							trackRow = i;
						}
					}
				} else {
					for (int i = 0; i < static_cast<int>(subTracks.size()); i++) {
						if (subTracks[static_cast<size_t>(i)].index == now) {
							trackRow = i + 1;
						}
					}
				}
			}
		}
		const int rows = trackCol == 0 ? audioRows : subRows;
		if (input.nav == hui::Direction::up && trackRow > 0) {
			trackRow--;
		} else if (input.nav == hui::Direction::down && trackRow + 1 < rows) {
			trackRow++;
		}
		if (input.is_pressed(hui::Action::confirm) && rows > 0) {
			if (trackCol == 0) {
				player.setAudio(audioTracks[static_cast<size_t>(trackRow)].index);
			} else if (trackRow == 0) {
				player.setSubtitle(-1);
			} else {
				const JfGateTrack &t = subTracks[static_cast<size_t>(trackRow - 1)];
				if (t.text) {
					player.setSubtitle(t.index);
				} else {
					noteText = "Picture subtitles are not supported yet";
					noteClock = 3.0f;
				}
			}
		}
	}

	static std::string megabits(long long bitsPerSecond)
	{
		char text[32];
		std::snprintf(text, sizeof(text), "%.1f Mbps", static_cast<double>(bitsPerSecond) / 1.0e6);
		return text;
	}

	void drawPanelFrame(hui::gfx::DrawList &list, const hui::gfx::Rect &r, const char *title)
	{
		list.shadow({ r.x, r.y + 14.0f, r.w, r.h }, 30.0f, 40.0f, hui::gfx::Color{ 0, 0, 0, 0.5f });
		list.rounded_rect(r, 30.0f, hui::gfx::Color{ 0.035f, 0.04f, 0.075f, 0.94f });
		list.bordered_rect(r, 30.0f, hui::gfx::Color{ 0, 0, 0, 0 }, 1.5f, hui::gfx::Color{ 1, 1, 1, 0.2f });
		hui::ui::text(list, kit.fonts.display, title, r.x + 40.0f, r.y + 72.0f, 40, hui::gfx::Color{ 1, 1, 1, 1 });
	}

	void drawTracksPanel(hui::gfx::DrawList &list)
	{
		const hui::gfx::Color white{ 1, 1, 1, 1 };
		const hui::gfx::Rect r{ 1040.0f, 150.0f, 760.0f, 640.0f };
		drawPanelFrame(list, r, "Audio and subtitles");
		const char *heads[2] = { "AUDIO", "SUBTITLES" };
		const int audioRows = static_cast<int>(audioTracks.size());
		const int subRows = static_cast<int>(subTracks.size()) + 1;
		for (int col = 0; col < 2; col++) {
			const float x = r.x + 32.0f + static_cast<float>(col) * 350.0f;
			const float w = 330.0f;
			hui::ui::text(list, kit.fonts.semibold, heads[col], x + 8.0f, r.y + 128.0f, 16,
				white.with_alpha(trackCol == col ? 0.9f : 0.45f), hui::gfx::Align::left, 3.0f);
			const int rows = col == 0 ? audioRows : subRows;
			if (playDetState == 0) {
				hui::ui::text(list, kit.fonts.regular, "Loading ...", x + 8.0f, r.y + 190.0f, 22, white.with_alpha(0.6f));
				continue;
			}
			if (rows == 0) {
				hui::ui::text(list, kit.fonts.regular, "None", x + 8.0f, r.y + 190.0f, 22, white.with_alpha(0.6f));
				continue;
			}
			const int current = col == 0 ? player.audioStream() : player.subtitleStream();
			const int visible = 7;
			const int cursor = trackCol == col ? trackRow : 0;
			const int first = std::clamp(cursor - 3, 0, std::max(0, rows - visible));
			for (int i = 0; i < visible && first + i < rows; i++) {
				const int index = first + i;
				const hui::gfx::Rect box{ x, r.y + 148.0f + 68.0f * static_cast<float>(i), w, 60.0f };
				const bool on = trackCol == col && trackRow == index;
				std::string name, note;
				int stream = -1;
				bool usable = true;
				if (col == 1 && index == 0) {
					name = "Off";
				} else {
					const JfGateTrack &t = col == 0 ? audioTracks[static_cast<size_t>(index)] : subTracks[static_cast<size_t>(index - 1)];
					name = trackName(t);
					note = trackNote(t);
					stream = t.index;
					usable = col == 0 || t.text != 0;
				}
				const float a = usable ? 1.0f : 0.45f;
				if (on) {
					list.rounded_rect(box, 30.0f, white.with_alpha(a));
				} else {
					list.rounded_rect(box, 30.0f, white.with_alpha(0.07f));
				}
				const hui::gfx::Color ink = on ? hui::gfx::Color{ 0.04f, 0.05f, 0.09f, 1.0f } : white;
				const std::string shown(kit.fonts.semibold.font->fit(name.c_str(), 22, w - 80.0f));
				hui::ui::text(list, kit.fonts.semibold, shown.c_str(), box.x + 24.0f, box.y + 26.0f, 22, ink.with_alpha(a));
				if (!note.empty()) {
					const std::string line(kit.fonts.regular.font->fit(note.c_str(), 15, w - 80.0f));
					hui::ui::text(list, kit.fonts.regular, line.c_str(), box.x + 24.0f, box.y + 48.0f, 15, ink.with_alpha(0.6f * a));
				}
				const bool isCurrent = (col == 1 && index == 0) ? current < 0 : stream == current;
				if (isCurrent) {
					list.circle(box.x + box.w - 30.0f, box.y + 30.0f, 9.0f, on ? hui::gfx::Color{ 0.2f, 0.5f, 1.0f, 1.0f } : white);
				}
			}
		}
	}

	void drawInfoPanel(hui::gfx::DrawList &list)
	{
		const hui::gfx::Color white{ 1, 1, 1, 1 };
		const hui::gfx::Rect r{ 1040.0f, 150.0f, 760.0f, 720.0f };
		drawPanelFrame(list, r, "Playback");
		const jfp::Info info = player.info();
		const bool hdr = info.range != "SDR" && !info.range.empty();
		std::vector<std::pair<std::string, std::string>> rows;
		rows.push_back({ "Method", "Direct Play" });
		rows.push_back({ "Container", info.container.empty() ? "-" : upperText(info.container) });
		{
			char text[96];
			std::snprintf(text, sizeof(text), "%s  %dx%d  %.3g fps  %d-bit", upperText(info.videoCodec).c_str(), info.width,
				info.height, info.fps, info.bitDepth);
			rows.push_back({ "Video", info.width > 0 ? text : "-" });
		}
		{
			std::string range = info.range.empty() ? "-" : info.range;
			if (info.range == "HDR10") {
				range = "HDR10 (PQ, BT.2020)";
			} else if (info.range == "HLG") {
				range = "HLG (BT.2020)";
			}
			if (info.dovi) {
				range = "Dolby Vision  Profile " + std::to_string(info.doviProfile) + "." + std::to_string(info.doviLevel);
				if (info.doviEnhancement) {
					range += "  (enhancement layer)";
				}
				if (info.range == "HDR10") {
					range += "  +  HDR10";
				}
			}
			rows.push_back({ "Range", range });
		}
		{
			std::string out = "SDR";
			if (info.hdrOutput) {
				out = "HDR10 to the display, as it is (peak " + std::to_string(info.peakNits) + " nits)";
			} else if (info.toneMapped) {
				out = "SDR, tone-mapped from " + std::to_string(info.peakNits) + " nits";
			}
			rows.push_back({ "Output", out });
		}
		rows.push_back({ "Audio", info.audioCodec.empty() ? "-" : info.audioText() });
		rows.push_back({ "Bitrate", info.bitrate > 0 ? megabits(info.bitrate) : "-" });
		rows.push_back({ "Dropped", std::to_string(info.dropped) + " pictures" });
		if (playDetState == 1) {
			if (playDet.file_size > 0) {
				char text[32];
				std::snprintf(text, sizeof(text), "%.1f GB", static_cast<double>(playDet.file_size) / 1.0e9);
				rows.push_back({ "File", text });
			}
			if (playDet.critic > 0.0f) {
				rows.push_back({ "Critics", std::to_string(static_cast<int>(playDet.critic + 0.5f)) + "%" });
			}
			if (playDet.community > 0.0f) {
				char text[16];
				std::snprintf(text, sizeof(text), "%.1f / 10", static_cast<double>(playDet.community));
				rows.push_back({ "Viewers", text });
			}
		}
		{
			const std::string status = player.subtitleStatus();
			const int sub = player.subtitleStream();
			rows.push_back({ "Subtitles", sub < 0 ? "Off" : (status.empty() ? "On" : status) });
		}
		for (size_t i = 0; i < rows.size(); i++) {
			const float y = r.y + 130.0f + 46.0f * static_cast<float>(i);
			hui::ui::text(list, kit.fonts.semibold, upperText(rows[i].first).c_str(), r.x + 40.0f, y, 15,
				white.with_alpha(0.5f), hui::gfx::Align::left, 3.0f);
			const std::string value(kit.fonts.regular.font->fit(rows[i].second.c_str(), 24, 470.0f));
			hui::ui::text(list, kit.fonts.regular, value.c_str(), r.x + 210.0f, y + 2.0f, 24, white.with_alpha(0.92f));
		}
	}

	// Stage 37: picture subtitles. They keep the place the disc gave them, except that on a wide film they move into the
	// black bar when they fit (and lift clear of the controls); true when one is showing
	bool drawSubtitlePictures(hui::gfx::DrawList &list, float controlsShown, const hui::gfx::Rect *skip)
	{
		const std::vector<jfp::SubPicture> pics = player.subtitlePictures(player.position());
		for (size_t i = 0; i < picTex.size();) {
			bool wanted = false;
			for (const jfp::SubPicture &p : pics) {
				wanted = wanted || p.id == picTex[i].id;
			}
			if (wanted) {
				i++;
			} else {
				retired.push_back(picTex[i].tex);
				picTex.erase(picTex.begin() + static_cast<std::ptrdiff_t>(i));
			}
		}
		if (pics.empty()) {
			return false;
		}
		float picture = 1080.0f, bar = 0.0f, videoAspect = 16.0f / 9.0f;
		if (videoW > 0 && videoH > 0) {
			videoAspect = static_cast<float>(videoW) / static_cast<float>(videoH);
			picture = std::min(1080.0f, 1920.0f / videoAspect);
			bar = (1080.0f - picture) * 0.5f;
		}
		struct Placed
		{
			std::uint32_t tex;
			hui::gfx::Rect r;
		};
		std::vector<Placed> placed;
		float top = 100000.0f, bottom = -100000.0f;
		for (const jfp::SubPicture &p : pics) {
			if (p.w <= 0 || p.h <= 0 || p.cw <= 0 || p.ch <= 0 || !p.rgba) {
				continue;
			}
			std::uint32_t tex = 0;
			for (const PicTex &t : picTex) {
				if (t.id == p.id) {
					tex = t.tex;
				}
			}
			if (!tex) {
				tex = kit.renderer.create_texture(p.w, p.h, p.rgba->data());
				picTex.push_back({ p.id, tex });
			}
			// a canvas made for the cropped picture lands on the picture; any other canvas is the whole screen
			const float canvasAspect = static_cast<float>(p.cw) / static_cast<float>(p.ch);
			const float off = canvasAspect > videoAspect ? canvasAspect - videoAspect : videoAspect - canvasAspect;
			const bool onPicture = bar > 0.0f && off < 0.03f * videoAspect;
			const float sx = 1920.0f / static_cast<float>(p.cw);
			const float sy = (onPicture ? picture : 1080.0f) / static_cast<float>(p.ch);
			const float oy = onPicture ? bar : 0.0f;
			const hui::gfx::Rect r{ static_cast<float>(p.x) * sx, oy + static_cast<float>(p.y) * sy,
				static_cast<float>(p.w) * sx, static_cast<float>(p.h) * sy };
			placed.push_back({ tex, r });
			top = std::min(top, r.y);
			bottom = std::max(bottom, r.y + r.h);
		}
		if (placed.empty()) {
			return false;
		}
		const float away = 1.0f - std::min(1.0f, controlsShown * 1.4f);
		const float lift = std::min(0.0f, 770.0f - bottom); // with the controls up, clear of them
		float dy = lift * controlsShown;
		if (bar >= (bottom - top) + 24.0f) {
			const float barTop = 1080.0f - bar * 0.5f - (bottom - top) * 0.5f; // centred in the lower bar
			dy = (barTop - top) * away + lift * (1.0f - away);
		}
		if (skip) {
			float left = 1920.0f, right = 0.0f;
			for (const Placed &p : placed) {
				left = std::min(left, p.r.x);
				right = std::max(right, p.r.x + p.r.w);
			}
			dy -= jfp::subtitleLiftForPrompt(left, top + dy, right, bottom + dy,
				skip->x, skip->y, skip->x + skip->w, skip->y + skip->h);
		}
		for (const Placed &p : placed) {
			list.image(p.tex, { p.r.x, p.r.y + dy, p.r.w, p.r.h }, hui::gfx::kFullUv, hui::gfx::Color{ 1, 1, 1, 1 }, 0.0f);
		}
		return true;
	}

	// The line the file says now, white with a dark edge, near the bottom
	void drawSubtitle(hui::gfx::DrawList &list, float controlsShown, const hui::gfx::Rect *skip)
	{
		if (drawSubtitlePictures(list, controlsShown, skip)) {
			return;
		}
		const std::string text = player.subtitleAt(player.position());
		if (text.empty()) {
			return;
		}
		std::vector<std::string> lines;
		size_t from = 0;
		while (from <= text.size()) {
			const size_t end = text.find('\n', from);
			lines.push_back(text.substr(from, end == std::string::npos ? std::string::npos : end - from));
			if (end == std::string::npos) {
				break;
			}
			from = end + 1;
		}
		// Stage 35: a film wider than the screen has black bars; the words go in the lower one when they fit and the
		// controls are away (with the controls up they lift clear of them, as before)
		const float lineGap = static_cast<float>(subFont) * 1.19f;
		float baseline = 1010.0f - 260.0f * controlsShown;
		if (videoW > 0 && videoH > 0) {
			const float picture = std::min(1080.0f, 1920.0f * static_cast<float>(videoH) / static_cast<float>(videoW));
			const float bar = (1080.0f - picture) * 0.5f;
			const float block = lineGap * static_cast<float>(lines.size());
			if (bar >= block + 24.0f) {
				// centred in the bar: the last line's baseline sits a little below the middle
				const float inBar = 1080.0f - bar * 0.5f + block * 0.5f - 18.0f;
				const float away = 1.0f - std::min(1.0f, controlsShown * 1.4f);
				baseline = inBar * away + (1010.0f - 260.0f) * (1.0f - away);
			}
		}
		if (skip) {
			float width = 0.0f;
			for (const std::string &line : lines)
				width = std::max(width, kit.fonts.semibold.measure(line, subFont));
			const float top = baseline - lineGap * static_cast<float>(lines.size() - 1) -
				kit.fonts.semibold.font->ascent(static_cast<float>(subFont));
			const float bottom = baseline + kit.fonts.semibold.font->descent(static_cast<float>(subFont));
			baseline -= jfp::subtitleLiftForPrompt(960.0f - width * 0.5f, top, 960.0f + width * 0.5f, bottom,
				skip->x, skip->y, skip->x + skip->w, skip->y + skip->h);
		}
		for (size_t i = 0; i < lines.size(); i++) {
			const float y = baseline - lineGap * static_cast<float>(lines.size() - 1 - i);
			const float offsets[][2] = {
				{ -1.5f, 0.0f }, { 1.5f, 0.0f }, { 0.0f, -1.5f }, { 0.0f, 1.5f },
				{ -1.06f, -1.06f }, { 1.06f, -1.06f }, { -1.06f, 1.06f }, { 1.06f, 1.06f }
			};
			for (const auto &offset : offsets) {
				hui::ui::text(list, kit.fonts.semibold, lines[i].c_str(), 960.0f + offset[0], y + offset[1], subFont,
					hui::gfx::Color{ 0, 0, 0, 0.9f }, hui::gfx::Align::center);
			}
			hui::ui::text(list, kit.fonts.semibold, lines[i].c_str(), 960.0f, y, subFont, hui::gfx::Color{ 1, 1, 1, 1 },
				hui::gfx::Align::center);
		}
	}

	// Top right: size, range and sound of what plays; under the title: what the critics and viewers say
	void drawBadges(hui::gfx::DrawList &list, float shown)
	{
		const jfp::Info info = player.info();
		if (info.width <= 0) {
			return;
		}
		float x = 1800.0f;
		const float cy = 100.0f;
		const hui::gfx::Color ink{ 0.04f, 0.05f, 0.09f, 1.0f };
		const hui::gfx::Color soft{ 1, 1, 1, 0.18f };
		const char *size = info.width >= 3000 ? "4K" : info.height >= 1000 || info.width >= 1800 ? "HD 1080p" : info.height >= 700 ? "HD 720p" : "SD";
		x = drawTag(list, x, cy, size, hui::gfx::Color{ 1, 1, 1, 0.92f }, ink, shown);
		// What reaches the display: a Dolby Vision file plays as its HDR10 picture, a tone-mapped one as SDR,
		// and every sound is decoded to PCM for the console (so it is the channels that count, not "Atmos")
		if (info.hdrOutput) {
			x = drawTag(list, x, cy, "HDR10", hui::gfx::Color{ 1.0f, 0.82f, 0.4f, 1.0f }, ink, shown);
		} else if (info.toneMapped) {
			x = drawTag(list, x, cy, "SDR", soft, hui::gfx::Color{ 1, 1, 1, 1 }, shown);
		}
		if (!info.bitstream.empty()) {
			// the soundbar decodes this one: say so, with the channels the file has
			const char *ch = info.audioChannels <= 2 ? "STEREO" : info.audioChannels <= 6 ? "5.1" : "7.1";
			x = drawTag(list, x, cy, (info.bitstream + " " + ch).c_str(), hui::gfx::Color{ 0.62f, 0.86f, 1.0f, 1.0f }, ink, shown);
		} else if (info.outChannels > 0) {
			const char *layout = info.outChannels == 2 ? "STEREO" : info.outChannels == 6 ? "5.1" : info.outChannels == 7 ? "6.1" : "7.1";
			x = drawTag(list, x, cy, (std::string("PCM ") + layout).c_str(), soft, hui::gfx::Color{ 1, 1, 1, 1 }, shown);
		}
		// ratings, and the keys
		float rx = 150.0f;
		const float ry = 112.0f;
		char text[48];
		if (playDetState == 1 && playDet.critic > 0.0f) {
			list.circle(rx + 10.0f, ry - 8.0f, 10.0f, hui::gfx::Color{ 0.98f, 0.35f, 0.24f, shown });
			std::snprintf(text, sizeof(text), "%d%% critics", static_cast<int>(playDet.critic + 0.5f));
			hui::ui::text(list, kit.fonts.semibold, text, rx + 30.0f, ry, 24, hui::gfx::Color{ 1, 1, 1, 0.85f * shown });
			rx += 30.0f + kit.fonts.semibold.measure(text, 24) + 34.0f;
		}
		if (playDetState == 1 && playDet.community > 0.0f) {
			list.star(rx + 10.0f, ry - 8.0f, 11.0f, hui::gfx::Color{ 1.0f, 0.82f, 0.4f, shown });
			std::snprintf(text, sizeof(text), "%.1f viewers", static_cast<double>(playDet.community));
			hui::ui::text(list, kit.fonts.semibold, text, rx + 30.0f, ry, 24, hui::gfx::Color{ 1, 1, 1, 0.85f * shown });
		}
	}

	// ---- Stage 13: the player's look ----
	// The controller hints under the controls: the DualSense glyphs, in the theme's glyph colours, fading with the controls
	void drawPlayerHints(hui::gfx::DrawList &list, float shown)
	{
		hui::ui::GlyphStyle style = hui::ui::GlyphStyle::dark();
		for (hui::gfx::Color *c : { &style.body, &style.edge, &style.ink, &style.label }) {
			c->a *= shown;
		}
		const hui::ui::Hint hints[] = {
			{ hui::ui::Button::cross, "Select" },
			{ hui::ui::Button::l2, "Scrub", hui::ui::Button::r2 },
			{ hui::ui::Button::triangle, "Audio and subtitles" },
			{ hui::ui::Button::options, "Info" },
			{ hui::ui::Button::circle, "Hide" },
			{ hui::ui::Button::l1, "Brightness (HDR)", hui::ui::Button::r1 },
		};
		const int hintCount = hdrOn ? 6 : 5;
		hui::ui::HintLayout layout;
		layout.size = 38.0f;
		layout.text_size = 24.0f;
		layout.cy = 1034.0f;
		layout.item_gap = 52.0f;
		layout.font = &kit.fonts.regular;
		const float width = hui::ui::measure_hints(kit.fonts, hints, hintCount, layout);
		hui::ui::draw_hints(list, kit.fonts, style, hints, hintCount, 960.0f - width * 0.5f, false, layout);
	}

	// Stage 20: the film's overlay. The theme with the play button in its own colour (/data/jellyzoom-play-colour: six hex digits)
	std::decay_t<decltype(ps5ui::active_theme())> filmTheme() const
	{
		auto t = ps5ui::active_theme();
		float r = 0.67f, g = 0.36f, b = 0.76f; // Jellyfin purple
		if (FILE *f = std::fopen("/data/jellyzoom-play-colour", "r")) {
			unsigned v = 0;
			if (std::fscanf(f, "%x", &v) == 1 && v <= 0xFFFFFFu) {
				r = static_cast<float>((v >> 16) & 0xFF) / 255.0f;
				g = static_cast<float>((v >> 8) & 0xFF) / 255.0f;
				b = static_cast<float>(v & 0xFF) / 255.0f;
			}
			std::fclose(f);
		}
		t.primary = hui::gfx::Color{ r, g, b, 1.0f };
		t.on_primary = hui::gfx::Color{ 1.0f, 1.0f, 1.0f, 1.0f };
		return t;
	}

	static constexpr float kPosterW = 110.0f, kPosterH = 165.0f;

	// The poster, bottom-aligned with the controls and to their left
	void drawFilmPoster(hui::gfx::DrawList &list, float shown)
	{
		const jf::Item *it = itemFor(playingItem);
		const std::uint32_t tex = it && it->cover ? it->cover : placeholderTex;
		list.image(tex, { 150.0f, 1080.0f - 130.0f - kPosterH, kPosterW, kPosterH }, hui::gfx::kFullUv,
			hui::gfx::Color{ 1.0f, 1.0f, 1.0f, shown }, 10.0f);
	}

	// ---- Stage 35: the player's glass controls ----
	// The title logo takes the poster's place, above the controls on the left (the title in words when the server has no logo)
	void dropLogo()
	{
		if (logoTex) {
			retired.push_back(logoTex);
			logoTex = 0;
		}
		logoW = logoH = 0;
	}

	void dropPics()
	{
		for (const PicTex &t : picTex) {
			retired.push_back(t.tex);
		}
		picTex.clear();
	}

	void clearTrickplayTextures()
	{
		for (const TrickplayTexture &texture : trickplayTextures) {
			retired.push_back(texture.tex);
		}
		trickplayTextures.clear();
		trickplayUseClock = 0;
	}

	void resetPlaybackExtras()
	{
		introPrompt.reset();
		introTimer.reset();
		outroPrompt.reset();
		seekPreview.reset();
		playbackExtras = {};
		playbackExtrasKnown = false;
		clearTrickplayTextures();
	}

	void pollPlaybackExtras()
	{
		if (playbackExtrasKnown || playingItem < 0) {
			return;
		}
		JfGatePlaybackExtras result{};
		if (!jf_gate_playback_extras_get(playingItem, &result) || result.state != 1) {
			return;
		}
		playbackExtras = result;
		playbackExtrasKnown = true;
		if (result.has_intro) {
			introPrompt.setSegment({ result.intro_start, result.intro_end });
		}
		if (result.has_outro) {
			outroPrompt.setSegment({ result.outro_start, result.outro_end });
		}
	}

	void takeTrickplayImages()
	{
		JfGateTrickplayImage image{};
		while (jf_gate_trickplay_take(&image)) {
			if (image.gate_index != playingItem || !image.rgba || image.width <= 0 || image.height <= 0) {
				std::free(image.rgba);
				continue;
			}
			if (!playbackExtrasKnown || playbackExtras.trickplay_width <= 0 || playbackExtras.trickplay_height <= 0 ||
				image.width % playbackExtras.trickplay_width != 0 || image.height % playbackExtras.trickplay_height != 0) {
				std::free(image.rgba);
				continue;
			}
			const int columns = image.width / playbackExtras.trickplay_width;
			const int rows = image.height / playbackExtras.trickplay_height;
			if (columns < 1 || columns > playbackExtras.trickplay_tile_width ||
				rows < 1 || rows > playbackExtras.trickplay_tile_height) {
				std::free(image.rgba);
				continue;
			}
			const std::uint32_t texture = kit.renderer.create_texture(image.width, image.height, image.rgba);
			std::free(image.rgba);
			if (!texture) {
				continue;
			}
			for (size_t i = 0; i < trickplayTextures.size();) {
				if (trickplayTextures[i].sheetIndex == image.sheet_index) {
					retired.push_back(trickplayTextures[i].tex);
					trickplayTextures.erase(trickplayTextures.begin() + static_cast<std::ptrdiff_t>(i));
				} else {
					++i;
				}
			}
			if (trickplayTextures.size() >= 2) {
				const auto oldest = std::min_element(trickplayTextures.begin(), trickplayTextures.end(),
					[](const TrickplayTexture &a, const TrickplayTexture &b) { return a.lastUsed < b.lastUsed; });
				jf_gate_trickplay_forget(playingItem, oldest->sheetIndex);
				retired.push_back(oldest->tex);
				trickplayTextures.erase(oldest);
			}
			trickplayTextures.push_back({ image.sheet_index, columns, rows, texture, ++trickplayUseClock });
		}
	}

	void takeLogo()
	{
		JfBackdrop got;
		if (jf_logo_take(&got)) {
			dropLogo();
			logoTex = kit.renderer.create_texture(got.width, got.height, got.rgba);
			logoW = got.width;
			logoH = got.height;
			std::free(got.rgba);
		}
	}

	void drawFilmLogo(hui::gfx::DrawList &list, float shown)
	{
		const float bottom = controls.bounds().y - 10.0f;
		if (logoTex && logoW > 0 && logoH > 0) {
			const float scale = std::min(560.0f / static_cast<float>(logoW), 170.0f / static_cast<float>(logoH));
			const float w = static_cast<float>(logoW) * scale, h = static_cast<float>(logoH) * scale;
			const hui::gfx::Rect r{ 150.0f, bottom - h, w, h };
			list.image(logoTex, { r.x + 2.0f, r.y + 4.0f, r.w, r.h }, hui::gfx::kFullUv, hui::gfx::Color{ 0, 0, 0, 0.45f * shown }, 0.0f);
			list.image(logoTex, r, hui::gfx::kFullUv, hui::gfx::Color{ 1, 1, 1, shown }, 0.0f);
		} else if (jf_logo_state() < 0) {
			const jf::Item *it = itemFor(playingItem);
			if (it) {
				const std::string title(kit.fonts.display.font->fit(it->titleText.c_str(), 64, 900.0f));
				hui::ui::text(list, kit.fonts.display, title.c_str(), 150.0f, bottom - 12.0f, 64,
					hui::gfx::Color{ 0, 0, 0, 0.4f * shown });
				hui::ui::text(list, kit.fonts.display, title.c_str(), 150.0f, bottom - 14.0f, 64,
					hui::gfx::Color{ 1, 1, 1, shown });
			}
		}
	}

	static void glassTriangle(hui::gfx::DrawList &list, float cx, float cy, float size, float dir, hui::gfx::Color c)
	{
		list.triangle({ cx - size * 0.5f, cy - size * 0.5f, size, size }, c, 0.0f, 1.5707963f * dir);
	}

	// One frosted pill, in the look of the wheel's detail buttons: a faint white fill and a hairline, white and glowing when
	// the pad is on it; the play button carries the play colour
	void glassPill(hui::gfx::DrawList &list, const hui::gfx::Rect &r, bool focus, bool primary, float breath)
	{
		const hui::gfx::Color white{ 1, 1, 1, 1 }, clear{ 0, 0, 0, 0 };
		const hui::gfx::Color accent = controls.style.theme.primary;
		const float round = r.h * 0.5f;
		if (focus) {
			list.glow(r, round, 22.0f, accent.with_alpha(0.30f + 0.14f * breath));
			list.rounded_rect(r, round, white);
		} else if (primary) {
			list.rounded_rect(r, round, accent.with_alpha(0.55f));
			list.gradient_rect(r, round, white.with_alpha(0.18f), white.with_alpha(0.0f));
			list.bordered_rect(r, round, clear, 1.5f, white.with_alpha(0.34f));
		} else {
			list.rounded_rect(r, round, white.with_alpha(0.12f));
			list.gradient_rect(r, round, white.with_alpha(0.14f), white.with_alpha(0.0f));
			list.bordered_rect(r, round, clear, 1.5f, white.with_alpha(0.22f));
		}
	}

	void drawGlassControls(hui::gfx::DrawList &list, float shown)
	{
		if (shown <= 0.01f) {
			return;
		}
		using hui::gfx::Color;
		using hui::gfx::Rect;
		using hui::ui::MediaControl;
		const Color white{ 1, 1, 1, 1 };
		const Color ink{ 0.04f, 0.05f, 0.09f, 1.0f };
		const Color accent = controls.style.theme.primary;
		const float breath = 0.5f + 0.5f * std::sin(playerClock * 2.4f);
		const Rect b = controls.bounds();
		const float pad = controls.style.padding;
		const float inX = b.x + pad, inW = b.w - 2.0f * pad;
		const float duration = controls.duration();
		const float position = controls.position();

		list.push_opacity(shown);
		// the scrubber
		const Rect thumb = controls.control_rect(MediaControl::scrubber);
		const float ty = thumb.y + thumb.h * 0.5f;
		const bool scrubFocus = controls.focus() == MediaControl::scrubber;
		const float thick = scrubFocus || scrubbing ? 8.0f : 6.0f;
		list.rounded_rect({ inX, ty - thick * 0.5f, inW, thick }, thick * 0.5f, white.with_alpha(0.16f));
		if (duration > 0.0f) {
			const float have = std::clamp((static_cast<float>(player.position() + player.buffered())) / duration, 0.0f, 1.0f);
			list.rounded_rect({ inX, ty - thick * 0.5f, inW * have, thick }, thick * 0.5f, white.with_alpha(0.22f));
			for (const float t : chapterTimes) {
				if (t > 1.0f && t < duration - 1.0f) {
					list.rounded_rect({ inX + inW * t / duration - 1.5f, ty - thick * 0.5f - 3.0f, 3.0f, thick + 6.0f }, 1.5f,
						white.with_alpha(0.5f));
				}
			}
		}
		const float playedEnd = std::clamp(thumb.x + thumb.w * 0.5f, inX, inX + inW);
		list.rounded_rect({ inX, ty - thick * 0.5f, std::max(0.0f, playedEnd - inX), thick }, thick * 0.5f, white.with_alpha(0.95f));
		{
			const float r = (scrubFocus || scrubbing ? 14.0f : 10.0f);
			const Rect knob{ playedEnd - r, ty - r, r * 2.0f, r * 2.0f };
			if (scrubFocus || scrubbing) {
				list.glow(knob, r, 18.0f, accent.with_alpha(0.55f));
			}
			list.rounded_rect(knob, r, white);
		}
		// the buttons: previous / next chapter and play
		const MediaControl row[3] = { MediaControl::previous, MediaControl::play, MediaControl::next };
		for (const MediaControl c : row) {
			const Rect r = controls.control_rect(c);
			if (r.w <= 0.0f) {
				continue;
			}
			const bool focus = controls.focus() == c && !muteFocus;
			glassPill(list, r, focus, c == MediaControl::play, breath);
			const Color icon = focus ? ink : white;
			const float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f, k = r.w / 56.0f;
			if (c == MediaControl::play) {
				if (controls.playing()) {
					list.rounded_rect({ cx - 9.5f * k, cy - 11.0f * k, 7.0f * k, 22.0f * k }, 2.5f * k, icon);
					list.rounded_rect({ cx + 2.5f * k, cy - 11.0f * k, 7.0f * k, 22.0f * k }, 2.5f * k, icon);
				} else {
					glassTriangle(list, cx + 2.0f * k, cy, 24.0f * k, 1.0f, icon);
				}
			} else {
				const float dir = c == MediaControl::previous ? -1.0f : 1.0f;
				glassTriangle(list, cx - dir * 3.0f * k, cy, 18.0f * k, dir, icon);
				list.rounded_rect({ cx + dir * 8.0f * k - 2.0f * k, cy - 9.0f * k, 4.0f * k, 18.0f * k }, 2.0f * k, icon);
			}
		}
		const Rect nextRect = controls.control_rect(MediaControl::next);
		const Rect playRect = controls.control_rect(MediaControl::play);
		const float rowCy = playRect.y + playRect.h * 0.5f;
		// the clock: where the film is, and how long it is
		{
			auto clockText = [](float seconds, bool hours) {
				char t[24];
				const int whole = std::max(0, static_cast<int>(seconds));
				if (hours) {
					std::snprintf(t, sizeof(t), "%d:%02d:%02d", whole / 3600, (whole / 60) % 60, whole % 60);
				} else {
					std::snprintf(t, sizeof(t), "%d:%02d", whole / 60, whole % 60);
				}
				return std::string(t);
			};
			const bool hours = duration >= 3600.0f;
			const float x = (nextRect.w > 0.0f ? nextRect.x + nextRect.w : playRect.x + playRect.w) + 24.0f;
			const std::string elapsed = clockText(position, hours);
			hui::ui::text(list, kit.fonts.mono, elapsed.c_str(), x, rowCy + 8.0f, 24, white.with_alpha(0.96f));
			const float used = kit.fonts.mono.measure(elapsed.c_str(), 24);
			const std::string total = " / " + clockText(duration, hours);
			hui::ui::text(list, kit.fonts.mono, total.c_str(), x + used, rowCy + 8.0f, 24, white.with_alpha(0.52f));
		}
		// the volume
		{
			const Rect v = controls.control_rect(MediaControl::volume);
			if (v.w > 0.0f) {
				const bool focus = controls.focus() == MediaControl::volume && !muteFocus;
				glassPill(list, v, focus, false, breath);
				const Color icon = focus ? ink : white;
				const float cx = v.x + v.w * 0.5f, cy = v.y + v.h * 0.5f, k = v.w / 56.0f;
				const bool muted = controls.muted();
				const float level = controls.volume();
				list.rounded_rect({ cx - 13.0f * k, cy - 4.5f * k, 7.0f * k, 9.0f * k }, 1.5f * k, icon);
				glassTriangle(list, cx - 6.0f * k, cy, 18.0f * k, -1.0f, icon);
				const float t = 2.8f * k;
				if (muted || level <= 0.001f) {
					list.line(cx + 5.0f * k, cy - 4.5f * k, cx + 13.0f * k, cy + 4.5f * k, t, icon);
					list.line(cx + 13.0f * k, cy - 4.5f * k, cx + 5.0f * k, cy + 4.5f * k, t, icon);
				} else {
					list.arc(cx + 1.0f * k, cy, 7.0f * k, t, 0.75f, 1.64f, icon);
					if (level > 0.5f) {
						list.arc(cx + 1.0f * k, cy, 12.5f * k, t, 0.75f, 1.64f, icon);
					}
				}
				const float vw = controls.style.volume_width;
				const Rect bar{ inX + inW - vw, rowCy - 3.0f, vw, 6.0f };
				const float fill = muted ? 0.0f : std::clamp(level, 0.0f, 1.0f);
				list.rounded_rect(bar, 3.0f, white.with_alpha(0.16f));
				list.rounded_rect({ bar.x, bar.y, bar.w * fill, bar.h }, 3.0f, white.with_alpha(0.9f));
				const float vr = controls.adjusting() ? 10.0f : 7.0f;
				const Rect knob{ bar.x + bar.w * fill - vr, rowCy - vr, vr * 2.0f, vr * 2.0f };
				if (controls.adjusting()) {
					list.glow(knob, vr, 14.0f, accent.with_alpha(0.55f));
				}
				list.rounded_rect(knob, vr, white);
			}
		}
		list.pop_opacity();
	}

	// A mute button right of the volume slider, reached with RIGHT from the volume button (LEFT comes back)
	void drawMuteButton(hui::gfx::DrawList &list, float shown)
	{
		const hui::gfx::Rect v = controls.control_rect(hui::ui::MediaControl::volume);
		const float cy = v.y + v.h * 0.5f;
		const hui::gfx::Rect r{ 1770.0f - 100.0f, cy - 24.0f, 100.0f, 48.0f };
		const bool muted = controls.muted();
		// Stage 35: a glass pill like the other controls (red-tinted while muted)
		list.push_opacity(shown);
		if (muteFocus) {
			list.glow(r, 24.0f, 20.0f, controls.style.theme.primary.with_alpha(0.4f));
			list.rounded_rect(r, 24.0f, hui::gfx::Color{ 1, 1, 1, 1 });
		} else {
			list.rounded_rect(r, 24.0f, muted ? hui::gfx::Color{ 0.9f, 0.3f, 0.25f, 0.55f } : hui::gfx::Color{ 1, 1, 1, 0.12f });
			list.gradient_rect(r, 24.0f, hui::gfx::Color{ 1, 1, 1, 0.14f }, hui::gfx::Color{ 1, 1, 1, 0.0f });
			list.bordered_rect(r, 24.0f, hui::gfx::Color{ 0, 0, 0, 0 }, 1.5f, hui::gfx::Color{ 1, 1, 1, muted ? 0.4f : 0.22f });
		}
		hui::ui::text(list, kit.fonts.semibold, muted ? "Unmute" : "Mute", r.x + r.w * 0.5f, r.y + 32.0f, 22,
			muteFocus ? hui::gfx::Color{ 0.04f, 0.05f, 0.09f, 1.0f } : hui::gfx::Color{ 1, 1, 1, 1 }, hui::gfx::Align::center);
		list.pop_opacity();
	}

	// The scrims over the picture: two smooth gradients (the stacked bands showed as lines), and no plate under the controls
	void drawPlayerScrims(hui::gfx::DrawList &list, float shown)
	{
		list.gradient_rect({ 0.0f, 0.0f, 1920.0f, 280.0f }, 0.0f, hui::gfx::Color{ 0.0f, 0.0f, 0.0f, 0.55f * shown },
			hui::gfx::Color{ 0.0f, 0.0f, 0.0f, 0.0f });
		list.gradient_rect({ 0.0f, 1080.0f - 420.0f, 1920.0f, 420.0f }, 0.0f, hui::gfx::Color{ 0.0f, 0.0f, 0.0f, 0.0f },
			hui::gfx::Color{ 0.0f, 0.0f, 0.0f, 0.7f * shown });
	}

	hui::gfx::Rect skipPromptRect() const
	{
		return { 1390.0f, jfp::skipPromptTop(controls.bounds().y, controls.visibility()), 420.0f, 82.0f };
	}

	void drawSkipPrompt(hui::gfx::DrawList &list, const char *label, float progress = -1.0f)
	{
		using hui::gfx::Color;
		using hui::gfx::Rect;
		const Rect r = skipPromptRect();
		const Color white{ 1, 1, 1, 1 };
		const Color accent = controls.style.theme.primary;
		list.glow(r, 18.0f, 22.0f, accent.with_alpha(0.28f));
		list.rounded_rect(r, 18.0f, Color{ 0.025f, 0.035f, 0.06f, 0.91f });
		if (progress >= 0.0f && progress < 1.0f) {
			list.push_clip({ r.x, r.y, r.w * std::clamp(progress, 0.0f, 1.0f), r.h });
			list.rounded_rect(r, 18.0f, hui::gfx::mix(Color{ 0.025f, 0.035f, 0.06f, 1.0f }, accent, 0.65f));
			list.pop_clip();
		}
		list.bordered_rect(r, 18.0f, Color{ 0, 0, 0, 0 }, 1.5f, white.with_alpha(0.34f));
		hui::ui::text(list, kit.fonts.semibold, label, r.x + 125.0f, r.y + 35.0f, 25, white,
			hui::gfx::Align::center);
		hui::ui::text(list, kit.fonts.regular, "TRIANGLE / REMOTE OK", r.x + 308.0f, r.y + 51.0f, 14,
			white.with_alpha(0.72f), hui::gfx::Align::center);
	}

	void drawTrickplayPreview(hui::gfx::DrawList &list, double seconds, bool linger = false)
	{
		if (!playbackExtrasKnown || playbackExtras.state != 1 || !std::isfinite(seconds)) {
			return;
		}
		jfp::TrickplayInfo info;
		info.width = playbackExtras.trickplay_width;
		info.height = playbackExtras.trickplay_height;
		info.tileWidth = playbackExtras.trickplay_tile_width;
		info.tileHeight = playbackExtras.trickplay_tile_height;
		info.thumbnailCount = playbackExtras.trickplay_count;
		info.interval = playbackExtras.trickplay_interval;
		if (!info.valid()) {
			return;
		}
		jfp::ThumbnailLocation location{};
		if (!jfp::mapThumbnail(info, seconds, &location)) {
			return;
		}
		jf_gate_trickplay_request(playingItem, location.sheet);

		using hui::gfx::Color;
		using hui::gfx::Rect;
		const Rect frame{ 760.0f, 342.0f, 400.0f, 258.0f };
		const Color white{ 1, 1, 1, 1 };
		list.rounded_rect(frame, 14.0f, Color{ 0.025f, 0.035f, 0.06f, 0.94f });
		list.bordered_rect(frame, 14.0f, Color{ 0, 0, 0, 0 }, 1.5f, white.with_alpha(0.25f));

		const float previewWidth = frame.w - 24.0f;
		const float previewHeight = frame.h - 58.0f;
		const float aspect = static_cast<float>(info.width) / static_cast<float>(info.height);
		float imageWidth = previewWidth;
		float imageHeight = imageWidth / aspect;
		if (imageHeight > previewHeight) {
			imageHeight = previewHeight;
			imageWidth = imageHeight * aspect;
		}
		const Rect imageRect{ frame.x + (frame.w - imageWidth) * 0.5f, frame.y + 10.0f, imageWidth, imageHeight };

		TrickplayTexture *found = nullptr;
		for (TrickplayTexture &texture : trickplayTextures) {
			if (texture.sheetIndex == location.sheet) {
				found = &texture;
				break;
			}
		}
		if (found) {
			if (linger) {
				seekPreview.ready();
			}
			found->lastUsed = ++trickplayUseClock;
			const Rect uv{ static_cast<float>(location.x) / static_cast<float>(found->columns),
				static_cast<float>(location.y) / static_cast<float>(found->rows),
				1.0f / static_cast<float>(found->columns), 1.0f / static_cast<float>(found->rows) };
			list.image(found->tex, imageRect, uv, white, 8.0f);
		} else {
			list.rounded_rect(imageRect, 8.0f, Color{ 1, 1, 1, 0.08f });
			hui::ui::text(list, kit.fonts.semibold, "LOADING PREVIEW", imageRect.x + imageRect.w * 0.5f,
				imageRect.y + imageRect.h * 0.5f + 8.0f, 19, white.with_alpha(0.72f), hui::gfx::Align::center);
		}

		const int wholeSeconds = static_cast<int>(std::clamp(seconds, 0.0, 359999.0));
		char stamp[24];
		if (wholeSeconds >= 3600) {
			std::snprintf(stamp, sizeof(stamp), "%d:%02d:%02d", wholeSeconds / 3600, (wholeSeconds / 60) % 60, wholeSeconds % 60);
		} else {
			std::snprintf(stamp, sizeof(stamp), "%d:%02d", wholeSeconds / 60, wholeSeconds % 60);
		}
		hui::ui::text(list, kit.fonts.semibold, stamp, frame.x + frame.w * 0.5f, frame.y + frame.h - 10.0f, 18,
			white.with_alpha(0.9f), hui::gfx::Align::center);
	}

	void seekFromControls(double seconds, bool showPreview = true)
	{
		const double duration = player.duration();
		introPrompt.observeSeek(seconds, duration);
		introTimer.reset();
		outroPrompt.observeSeek(seconds, duration);
		if (showPreview && std::isfinite(seconds) && duration > 0.0) {
			seekPreview.show(std::clamp(seconds, 0.0, std::max(0.0, duration - 2.0)));
		} else if (!showPreview) {
			seekPreview.reset();
		}
		player.seek(seconds);
	}

	void renderPlayer()
	{
		hdrUpdate();
		prepareFrame();
		const float dt = std::min(frameTimer, 0.05f);
		playerClock += dt;
		seekPreview.tick(dt);
		hui::InputFrame input = kit.input();
		holdOptions(dt);
		updateTelemetry();
		pollPlayDetails(dt);
		{
			const std::string note = player.takeNote();
			if (!note.empty()) {
				noteText = note;
				noteClock = 3.0f;
			}
			noteClock = std::max(0.0f, noteClock - dt);
		}
		// While a panel is up the pad belongs to it: the controls and the player see an idle pad
		const hui::InputFrame padInput = input;
		const bool panelWasOpen = panel != Panel::none;
		if (panelWasOpen) {
			input = hui::InputFrame{};
			input.connected = true;
		}

		jfp::Frame picture;
		if (player.present(&picture)) {
			bool shown = false;
			if (picture.hdr) {
				if (hdrOn && hdrPass.uploadHdr(reinterpret_cast<const std::uint16_t *>(picture.rgba), picture.width, picture.height)) {
					if (videoMode != 2) {
						say("HDR: the first HDR10 picture, %dx%d", picture.width, picture.height);
					}
					videoMode = 2;
					shown = true;
				}
			} else {
				video.upload(picture.rgba, picture.width, picture.height);
				videoMode = 1;
				shown = true;
			}
			if (shown) {
				videoW = picture.width;
				videoH = picture.height;
			}
		}
		takeLogo();
		pollPlaybackExtras();
		takeTrickplayImages();
		const jfp::State state = player.state();
		const bool running = state == jfp::State::buffering || state == jfp::State::playing || state == jfp::State::paused;
		const double duration = player.duration();
		const bool introEligible = running && introPrompt.visible(player.position(), duration);
		introTimer.update(introEligible, state == jfp::State::playing, dt);
		const bool introPromptVisible = introEligible && panel == Panel::none &&
			introTimer.visible(!controls.hidden() && !uiHiddenByPad);
		const bool outroEligible = running && outroPrompt.visible(player.position(), duration);
		const bool outroPromptVisible = outroEligible && panel == Panel::none && !introEligible;
		if (running) {
			if (!chaptersSet) {
				std::vector<hui::ui::MediaChapter> list;
				for (const jfp::Chapter &c : player.chapters()) {
					hui::ui::MediaChapter m;
					m.time = c.time;
					m.title = c.title;
					list.push_back(m);
				}
				controls.set_chapters(list);
				chapterTimes.clear();
				for (const hui::ui::MediaChapter &m : list) {
					chapterTimes.push_back(m.time);
				}
				chaptersSet = true;
			}
			if (player.duration() > 0.0) {
				controls.set_duration(static_cast<float>(player.duration()));
			}
		}

		if (running) {
			if (panelWasOpen && padInput.is_pressed(hui::Action::back)) {
				panel = Panel::none;
			} else if (!((introPromptVisible || outroPromptVisible) && padInput.is_pressed(hui::Action::north))) {
				panelInput(padInput);
			}
		}
		// CIRCLE with the controls up hides them (the film plays on); with them hidden it leaves the film.
		// Any other button brings hidden controls back and is not passed on.
		const bool backPress = input.is_pressed(hui::Action::back);
		const bool uiWasHidden = controls.hidden();
		const bool hideNow = backPress && !uiWasHidden && running;
		const bool introWake = jfp::wakeIntroControls(introEligible, introTimer.expired(), uiWasHidden,
			input.is_pressed(hui::Action::confirm));
		const bool swallow = (uiHiddenByPad || introWake) && !backPress &&
			(input.pressed != 0 || input.nav != hui::Direction::none);
		const bool skipHandled = jfp::skipPromptPressed(introPromptVisible || outroPromptVisible, panelWasOpen, hideNow,
			input.is_pressed(hui::Action::north), kit.remoteConfirmPressed());
		const bool inputActivity = input.pressed != 0 || input.nav != hui::Direction::none ||
			std::fabs(input.stick_x) > 0.3f || std::fabs(input.stick_y) > 0.3f;
		const jfp::PlaybackControlWake wake = jfp::playbackControlWake(skipHandled, swallow, inputActivity);
		if (wake.clearManualHide) {
			uiHiddenByPad = false;
		}
		bool skipToEnd = false;
		if (skipHandled) {
			scrubbing = false;
			jfp::SegmentPrompt &prompt = introPromptVisible ? introPrompt : outroPrompt;
			skipToEnd = outroPromptVisible && jfp::segmentEndsAtDuration(playbackExtras.outro_end, duration);
			if (!skipToEnd) {
				const double target = prompt.skipTarget(duration);
				if (target > player.position()) {
					seekFromControls(target, false);
				} else {
					seekPreview.reset();
				}
				prompt.dismiss();
			}
		}
		controls.style.auto_hide = uiHiddenByPad ? 0.001f : 5.0f;
		controls.style.hide_paused = uiHiddenByPad;
		if (wake.wake) {
			controls.wake();
		}
		// Stage 20: the mute button right of the volume slider. RIGHT from the volume button reaches it, CROSS mutes or
		// unmutes, LEFT goes back, UP / DOWN leave it for the controls. The controls see an idle pad while it has the pad.
		if (controls.hidden() || !running) {
			muteFocus = false;
		}
		bool muteHandled = false;
		if (running && !swallow && !hideNow && !skipHandled) {
			if (muteFocus) {
				if (input.is_pressed(hui::Action::confirm)) {
					controls.toggle_mute(feedback);
					player.setVolume(controls.volume(), controls.muted());
					muteHandled = true;
				} else if (input.is_pressed(hui::Action::left)) {
					muteFocus = false;
					muteHandled = true;
				} else if (input.is_pressed(hui::Action::up) || input.is_pressed(hui::Action::down)) {
					muteFocus = false;
				}
			} else if (controls.focus() == hui::ui::MediaControl::volume && !controls.adjusting() && input.is_pressed(hui::Action::right)) {
				muteFocus = true;
				muteHandled = true;
			}
		}
		hui::InputFrame quiet{};
		quiet.connected = true;
		const hui::ui::Event event = controls.handle(
			swallow || hideNow || muteHandled || muteFocus || skipHandled ? quiet : input, feedback);
		if (hideNow) {
			uiHiddenByPad = true;
			controls.style.auto_hide = 0.001f;
			controls.style.hide_paused = true;
		}
		bool leave = backPress && !hideNow;
		int pendingNext = -1;
		if (skipToEnd) {
			const jf::Item *playing = itemFor(playingItem);
			const bool episode = playing && playing->kindRaw == "Episode";
			if (jfp::finishOutro(episode, !flagFile("/data/jellyzoom-no-autoplay"),
					[this] { return markOutroWatched(); },
					[this] { jf_gate_next_request(playingItem); })) {
				autoNext = 1;
				autoWait = 0.0;
			} else {
				leave = true;
			}
		}
		if (!skipToEnd && state == jfp::State::ended && autoNext == 0) {
			// an episode that has played to its end goes on to the next one, when the series has one
			const jf::Item *playing = itemFor(playingItem);
			if (playing && playing->kindRaw == "Episode" && !flagFile("/data/jellyzoom-no-autoplay")) {
				jf_gate_next_request(playingItem);
				autoNext = 1;
				autoWait = 0.0;
			} else {
				leave = true;
			}
		}
		if (autoNext == 1) {
			autoWait += static_cast<double>(dt);
			const int found = jf_gate_next_state();
			if (leave || found < 0 || autoWait > 12.0) {
				if (!leave && (found < -1 || autoWait > 12.0)) {
					wheelNotice = "Could not load the next episode";
					wheelNoticeClock = 6.0f;
				}
				jf_gate_next_cancel();
				autoNext = 0;
				leave = true;
			} else if (found == 2) {
				const int queued = jf_gate_next_index();
				syncList(queued / jf::kStride);
				const jf::Item *next = itemFor(queued);
				if (next && next->ready && next->playable) {
					pendingNext = queued;
				}
			}
		}
		if (leave || pendingNext >= 0) {
			uiHiddenByPad = false;
			scrubbing = false;
		}
		if (event != hui::ui::Event::none) {
			switch (controls.command()) {
			case hui::ui::MediaCommand::play:
				player.setPaused(false);
				break;
			case hui::ui::MediaCommand::pause:
				player.setPaused(true);
				break;
			case hui::ui::MediaCommand::seek:
			case hui::ui::MediaCommand::rewind:
			case hui::ui::MediaCommand::forward:
				seekFromControls(controls.seek_position());
				break;
			case hui::ui::MediaCommand::previous:
			case hui::ui::MediaCommand::next: {
				// the previous or next chapter; with none, ten seconds back or thirty on
				const bool back = controls.command() == hui::ui::MediaCommand::previous;
				const double now = player.position();
				double target = back ? now - 10.0 : now + 30.0;
				const std::vector<jfp::Chapter> chapters = player.chapters();
				for (size_t i = 0; i < chapters.size(); i++) {
					if (!back && chapters[i].time > now + 1.0) {
						target = chapters[i].time;
						break;
					}
					if (back && chapters[chapters.size() - 1 - i].time < now - 3.0) {
						target = chapters[chapters.size() - 1 - i].time;
						break;
					}
				}
				seekFromControls(target);
				break;
			}
			case hui::ui::MediaCommand::volume:
			case hui::ui::MediaCommand::mute:
				player.setVolume(controls.volume(), controls.muted());
				break;
			default:
				break;
			}
		}
		if (running && !skipToEnd) {
			const uint32_t media = panelWasOpen ? 0 : remote_media_pressed();
			if (media & REMOTE_PLAY) {
				player.setPaused(false);
			} else if (media & REMOTE_PAUSE) {
				player.setPaused(true);
			} else if (media & REMOTE_PLAY_PAUSE) {
				player.setPaused(!player.paused());
			}
			if (input.is_pressed(hui::Action::west)) {
				player.setPaused(!player.paused());
			}
			// Stage 19: L1 / R1 make an HDR film darker / brighter, 5% a press, up to 100% brighter; the level is kept
			if (hdrOn) {
				if (hdrStep < 0) {
					hdrStep = 0;
					if (FILE *f = std::fopen("/data/jellyzoom-hdr-gain", "r")) {
						int n = 0;
						if (std::fscanf(f, "%d", &n) == 1 && n >= 100) {
							hdrStep = std::min(20, (n - 100 + 2) / 5);
						}
						std::fclose(f);
					}
					jfhp_set_gain(100 + 5 * hdrStep);
				}
				const int d = input.is_pressed(hui::Action::page_next) ? 1 : input.is_pressed(hui::Action::page_prev) ? -1 : 0;
				if (d != 0) {
					hdrStep = std::max(0, std::min(20, hdrStep + d));
					const int percent = 100 + 5 * hdrStep;
					jfhp_set_gain(percent);
					if (FILE *f = std::fopen("/data/jellyzoom-hdr-gain", "w")) {
						std::fprintf(f, "%d\n", percent);
						std::fclose(f);
					}
					noteText = hdrStep == 0 ? std::string("HDR brightness: as mastered") : "HDR brightness: +" + std::to_string(hdrStep * 5) + "%";
					noteClock = 2.0f;
					say("HDR: brightness %d%% (step %d of 20)", percent, hdrStep);
				}
			}
			// L2 / R2 scrub, faster the harder the trigger is pulled; the film jumps there when it is let go
			const float pull = input.trigger_r - input.trigger_l;
			const float level = std::fabs(pull);
			if (!skipHandled && level > 0.08f && player.duration() > 0.0) {
				if (!scrubbing) {
					scrubbing = true;
					scrubTarget = player.position();
				}
				const float u = std::min(1.0f, (level - 0.08f) / 0.87f);
				const double speed = 4.0 + 236.0 * static_cast<double>(u) * static_cast<double>(u); // film seconds a second
				scrubTarget += (pull > 0.0f ? 1.0 : -1.0) * speed * static_cast<double>(dt);
				scrubTarget = std::max(0.0, std::min(player.duration() - 2.0, scrubTarget));
				uiHiddenByPad = false; // the bar shows where the scrub is
				controls.style.auto_hide = 5.0f;
				controls.style.hide_paused = false;
				controls.wake();
			} else if (scrubbing) {
				scrubbing = false;
				if (!skipHandled)
					seekFromControls(scrubTarget);
			}
		}
		controls.set_playing(!player.paused());
		controls.set_position(static_cast<float>(scrubbing ? scrubTarget : player.position()));
		controls.set_buffered(static_cast<float>(player.position() + player.buffered()));
		controls.update(dt);
		kit.play(feedback, screen->info().sounds, choices.haptics);
		feedback.clear();
		kit.tick(dt);

		playerList.clear();
		if (videoW > 0 && !hdrOn) {
			const float w = static_cast<float>(videoW), h = static_cast<float>(videoH);
			const hui::gfx::Rect rect{ (1920.0f - w) * 0.5f, (1080.0f - h) * 0.5f, w, h };
			const hui::gfx::Rect uv{ 0.0f, 0.0f, w / VideoSurface::kWidth, h / VideoSurface::kHeight };
			playerList.image(videoTex, rect, uv, hui::gfx::Color{ 1.0f, 1.0f, 1.0f, 1.0f }, 0.0f);
		}
		const float shown = controls.visibility();
		const bool introDraw = running && panel == Panel::none && introPrompt.visible(player.position(), duration) &&
			introTimer.visible(!controls.hidden() && !uiHiddenByPad);
		const bool outroDraw = running && panel == Panel::none && !introEligible &&
			outroPrompt.visible(player.position(), duration);
		const hui::gfx::Rect skipRect = skipPromptRect();
		if (shown > 0.01f) {
			drawPlayerScrims(playerList, shown);
			drawFilmLogo(playerList, shown);
			drawMuteButton(playerList, shown);
			drawBadges(playerList, shown);
			drawPlayerHints(playerList, shown);
		}
		if (running) {
			drawSubtitle(playerList, shown, introDraw || outroDraw ? &skipRect : nullptr);
			const std::string status = player.subtitleStatus();
			const std::string toast = noteClock > 0.0f ? noteText : status;
			if (!toast.empty()) {
				const float a = noteClock > 0.0f ? std::min(1.0f, noteClock) : 0.8f;
				const float w = kit.fonts.semibold.measure(toast.c_str(), 24) + 56.0f;
				playerList.rounded_rect({ 960.0f - w * 0.5f, 215.0f, w, 52.0f }, 26.0f, hui::gfx::Color{ 0.03f, 0.04f, 0.07f, 0.85f * a });
				hui::ui::text(playerList, kit.fonts.semibold, toast.c_str(), 960.0f, 249.0f, 24,
					hui::gfx::Color{ 1, 1, 1, a }, hui::gfx::Align::center);
			}
			if (panel == Panel::tracks) {
				drawTracksPanel(playerList);
			} else if (panel == Panel::info) {
				drawInfoPanel(playerList);
			}
		}
		if (introDraw) {
			drawSkipPrompt(playerList, "SKIP INTRO",
				introTimer.expired() ? -1.0f : static_cast<float>(introTimer.progress()));
		} else if (outroDraw) {
			drawSkipPrompt(playerList, "SKIP OUTRO");
		}
		if (running && panel == Panel::none) {
			if (scrubbing) {
				drawTrickplayPreview(playerList, scrubTarget);
			} else if (seekPreview.visible()) {
				drawTrickplayPreview(playerList, seekPreview.target(), true);
			}
		}
		if (state == jfp::State::opening || state == jfp::State::buffering) {
			hui::ui::text(playerList, kit.fonts.display, state == jfp::State::opening ? "Starting..." : "Buffering...",
				960.0f, 540.0f, 52, hui::gfx::Color{ 1.0f, 1.0f, 1.0f, 0.55f + 0.35f * std::sin(playerClock * 3.0f) },
				hui::gfx::Align::center);
		} else if (state == jfp::State::failed) {
			const std::string why = player.error();
			hui::ui::text(playerList, kit.fonts.display, why.c_str(), 960.0f, 520.0f, 44,
				hui::gfx::Color{ 1.0f, 0.8f, 0.8f, 1.0f }, hui::gfx::Align::center);
			hui::ui::text(playerList, kit.fonts.regular, "Press Back to return", 960.0f, 590.0f, 30,
				hui::gfx::Color{ 1.0f, 1.0f, 1.0f, 0.6f }, hui::gfx::Align::center);
		}
		if (autoNext == 1) {
			hui::ui::text(playerList, kit.fonts.display, "Up next ...", 960.0f, 540.0f, 52,
				hui::gfx::Color{ 1.0f, 1.0f, 1.0f, 0.55f + 0.35f * std::sin(playerClock * 3.0f) }, hui::gfx::Align::center);
		}
		drawGlassControls(playerList, controls.visibility()); // Stage 35: the controls are drawn here, as glass

		leavingList.clear();
		leaving.draw(leavingList, kit.fonts, ps5ui::active_theme());
		kit.renderer.begin();
		kit.renderer.draw(playerList);
		kit.renderer.draw(leavingList);
		if (hdrOn) {
			buildHdrCommandBuffer();
		} else {
			buildKitCommandBuffer();
		}
		submitFrame();
		if (pendingNext >= 0) {
			stopPlayback();
			autoNext = 0;
			jf_gate_next_cancel();
			startPlayback(pendingNext);
		} else if (leave) {
			autoNext = 0;
			stopPlayback();
		}
	}

	// The profile picture arrives from the gate as pixels: it becomes a texture here
	void takeAvatar()
	{
		JfGatePoster pic;
		if (jf_gate_take_avatar(&pic)) {
			if (::jf::profileTexture()) {
				retired.push_back(::jf::profileTexture());
			}
			::jf::profileTexture() = kit.renderer.create_texture(pic.width, pic.height, pic.rgba);
			::jf::profileAspect() = pic.width > 0 ? static_cast<float>(pic.height) / static_cast<float>(pic.width) : 1.0f;
			free(pic.rgba);
		}
		if (++avatarTick % 120 == 1) {
			char name[128];
			jf_gate_auth_text(2, name, sizeof(name));
			::jf::profileName() = name;
		}
	}

	// Sign out (1) or switch server (2): everything of the account goes, the worker starts over at the sign-in pages
	void changeAccount(int account)
	{
		if (device) {
			vkDeviceWaitIdle(device);
		}
		screen.reset();
		seerr.deactivate();
		jfseerr::Screen::forgetSession();
		for (const auto &list : jf::lists()) {
			for (const auto &item : list->items) {
				if (item->hasPoster) {
					retired.push_back(item->cover);
				}
			}
		}
		if (::jf::profileTexture()) {
			retired.push_back(::jf::profileTexture());
			::jf::profileTexture() = 0;
		}
		flushRetired(true);
		jf::lists().clear();
		states.clear();
		jf::searchText().clear();
		jf::view() = jf::View{ 0, 3 };
		jf::startCategory() = 0;
		jf::openedTitle() = -1;
		jf::browseRequest() = -1;
		jf::playRequest() = -1;
		jf::favouriteRequests().clear();
		jf::viewRequests().clear();
		::jf::profileName().clear();
		backdropFor = -1;
		backdropWant = -1;
		// Switch server keeps the old server (the chooser has a Back row); only a successful new server replaces it
		jf_gate_signout(0);
		loadingClock = 0.0f;
		signIn = jfsetup::Screen();
		if (account == 2) {
			signIn.chooseServer();
		}
		std::cerr << "JFGATE: account changed (" << (account == 2 ? "switch server" : "sign out") << ")" << std::endl;
	}

	void render() override
	{
		if (!prepared) {
			return;
		}
		takeAvatar();
		if (pendingAccount != 0) {
			const int account = pendingAccount;
			pendingAccount = 0;
			changeAccount(account);
		}
		if (hdrLabFrame()) {
			return;
		}
		if (!screen) {
			renderLoading();
			return;
		}
		if (playerActive) {
			renderPlayer();
			return;
		}
		prepareFrame();
		const float dt = std::min(frameTimer, 0.05f);
		wheelNoticeClock = std::max(0.0f, wheelNoticeClock - dt);
		// A test run ignores the pad: the design shows its entrance and rests
		hui::InputFrame input = kit.input();
		if (ps5.frameBudget) {
			input = hui::InputFrame{};
			input.connected = true;
		}
		holdOptions(dt);
		updateTelemetry();
		housekeeping(dt);
		::jf::rightStickY() = (float)(ps5_pad().right_y);
		if (::jf::profileRequest()) {
			::jf::profileRequest() = false;
			profileMenu.open();
		}
		if (profileMenu.is_open()) {
			// the menu has the pad; the wheel sees nothing
			const int account = profileMenu.update(input, dt, feedback);
			if (account != 0) {
				pendingAccount = account;
			}
			input = hui::InputFrame{};
			input.connected = true;
		}
		// the Discover tab is part of the design: it runs Seerr's page through these hooks, inside its own update and draw
		::jf::discoverEnabled() = jfprofile::seerrEnabled();
		if (::jf::discoverGo() == 3) {
			::jf::discoverGo() = 0;
			seerr.deactivate();
			jfseerr::Screen::forgetSession(); // Settings > Seerr > Disconnect
		}
		if (!seerrBound) {
			seerrBound = true;
			seerr.bind(
				[this](int w, int h, const unsigned char *rgba) -> std::uint32_t { return kit.renderer.create_texture(w, h, rgba); },
				[this](std::uint32_t tex) { retired.push_back(tex); });
		}
		{
			using Self = std::remove_cv_t<std::remove_pointer_t<decltype(this)>>;
			using FeedbackType = std::remove_reference_t<decltype(feedback)>;
			::jf::DiscoverHooks &hooks = ::jf::discoverHooks();
			hooks.ctx = this;
			hooks.activate = [](void *ctx, bool on) {
				Self *self = static_cast<Self *>(ctx);
				if (on) {
					self->seerr.activate();
				} else {
					self->seerr.deactivate();
				}
			};
			hooks.update = [](void *ctx, const void *in, float dt, void *fb, bool pad) -> bool {
				Self *self = static_cast<Self *>(ctx);
				self->seerr.update(*static_cast<const hui::InputFrame *>(in), dt, *static_cast<FeedbackType *>(fb), pad);
				return self->seerr.takeTabsRequest();
			};
			hooks.draw = [](void *ctx, void *frameArg, const void *fonts) {
				Self *self = static_cast<Self *>(ctx);
				self->seerr.draw(*static_cast<hui::app::Frame *>(frameArg), *static_cast<const hui::ui::Fonts *>(fonts));
			};
		}
		screen->update(input, dt, feedback);
		// Play was pressed on a film
		if (::jf::playRequest() >= 0) {
			const int wanted = ::jf::playRequest();
			::jf::playRequest() = -1;
			startPlayback(wanted);
		}
		// CIRCLE the design did not take (it answers the CIRCLE it uses with a cue) leaves
		if (!ps5.frameBudget && input.is_pressed(hui::Action::back) && feedback.cues.empty()) {
			quit = true;
		}
		if (context->settings_changed) {
			context->settings_changed = false;
		choices.music_volume = 0;
			kit.apply(choices);
		}
		kit.play(feedback, screen->info().sounds, choices.haptics);
		feedback.clear();
		if (choices.light_bar) {
			// the spotlight colour of the covers, lifted so its strongest channel is full: the light bar is small
			const ::jf::LightColour &lc = ::jf::lightColour();
			const float peak = std::max(std::max(lc.r, lc.g), std::max(lc.b, 0.05f));
			kit.light_bar(hui::gfx::Color{ lc.r / peak, lc.g / peak, lc.b / peak, 1.0f });
		}
		kit.tick(dt);

		// backdrop, scene, [the glass copy], overlay, post: the order a design is drawn in
		frame.reset();
		frame.glass_texture = kit.renderer.glass_texture();
		screen->draw(frame);
		if (wheelNoticeClock > 0.0f) {
			const float w = kit.fonts.semibold.measure(wheelNotice.c_str(), 24) + 56.0f;
			frame.overlay.rounded_rect({ 960.0f - w * 0.5f, 215.0f, w, 52.0f }, 26.0f,
				hui::gfx::Color{ 0.03f, 0.04f, 0.07f, 0.92f });
			hui::ui::text(frame.overlay, kit.fonts.semibold, wheelNotice.c_str(), 960.0f, 249.0f, 24,
				hui::gfx::Color{ 1.0f, 0.8f, 0.8f, 1.0f }, hui::gfx::Align::center);
		}
		kit.renderer.begin();
		kit.renderer.backdrop(frame.backdrop);
		backdropDraw();
		kit.renderer.draw(frame.scene);
		if (frame.glass) {
			kit.renderer.glass();
		}
		kit.renderer.draw(frame.overlay);
		profileList.clear();
		profileMenu.draw(profileList, kit.fonts);
		kit.renderer.draw(profileList);
		brandList.clear();
		brand.draw(brandList);
		kit.renderer.draw(brandList);
		kit.renderer.backdrop(frame.post);
		leavingList.clear();
		leaving.draw(leavingList, kit.fonts, ps5ui::active_theme());
		kit.renderer.draw(leavingList);
		buildKitCommandBuffer();
		submitFrame();
	}

	~VulkanExample() override
	{
		if (hdrLab.state == 1) {
			hdrLabEnd("exit");
		}
		jf_backdrop_stop();
		// the combined sound callback reads the player: stop it first
		audio_stop();
		kit.audio = false;
		player.close();
		jf_gate_stop();
		if (device) {
			vkDeviceWaitIdle(device);
		}
		// The covers are the renderer's textures: free them before it goes
		screen.reset();
		for (const auto &list : jf::lists()) {
			for (const auto &item : list->items) {
				if (item->hasPoster) {
					kit.renderer.destroy_texture(item->cover);
				}
			}
		}
		for (auto &entry : jf::seasonCovers()) {
			if (entry.second.tex) {
				kit.renderer.destroy_texture(entry.second.tex);
			}
		}
		jf::seasonCovers().clear();
		if (::jf::profileTexture()) {
			kit.renderer.destroy_texture(::jf::profileTexture());
			::jf::profileTexture() = 0;
		}
		flushRetired(true);
		if (placeholderTex) {
			kit.renderer.destroy_texture(placeholderTex);
		}
		if (brand.texture()) {
			kit.renderer.destroy_texture(brand.texture());
			brand.forget();
		}
		if (videoTex) {
			kit.renderer.destroy_texture(videoTex);
		}
		hdrLeave();
		hdrPass.destroy();
		video.destroy();
		jf::lists().clear();
	}
};

VULKAN_EXAMPLE_MAIN()
