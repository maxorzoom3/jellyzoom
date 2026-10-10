// Jellyfin Wheel - the HDR frame (Stage 12). See jf_hdrpass.h.
#include "jf_hdrpass.h"

#include <algorithm>
#include <cstdio>
#include <atomic>
#include <cstring>

namespace jfhp_impl
{

// Stage 19: the film's brightness in percent, set from the pad (0: not set, the flag file or 100 decides)
std::atomic<int> gGainPercent{ 0 };

namespace
{
#include "jf_hdrpass_spv.inc"

struct Push
{
	float rect[4];
	float frame[4];
	float misc[4];
};
static_assert(sizeof(Push) == 48, "push constants");

VkShaderModule makeModule(VkDevice device, const std::uint32_t *code, size_t bytes)
{
	VkShaderModuleCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	info.codeSize = bytes;
	info.pCode = code;
	VkShaderModule module = VK_NULL_HANDLE;
	if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS) {
		return VK_NULL_HANDLE;
	}
	return module;
}

void say(const char *what)
{
	std::fprintf(stderr, "HDR: %s\n", what);
}
} // namespace

class Impl
{
public:
	Impl() = default;
	Impl(const Impl &) = delete;
	Impl &operator=(const Impl &) = delete;
	~Impl() { destroy(); }

	static constexpr int kSurfaceW = 1920;
	static constexpr int kSurfaceH = 1080;

	bool create(const jfhp::Config &config);
	// The caller has waited for the device to be idle.
	void destroy();
	bool ready() const { return ready_; }

	// An HDR10 picture: w x h RGBA, 16 bits a channel, little endian (w <= 1920, h <= 1080). Waits for the copy.
	bool uploadHdr(const std::uint16_t *rgba, int w, int h);

	// Recorded into the frame's command buffer, in this order, with the kit's prepare() before the first and its
	// draw() between the two:
	void beginUi(VkCommandBuffer cmd);   // the UI's pass, cleared to transparent
	void endUi(VkCommandBuffer cmd);
	// The swapchain's pass: film + UI packed. framebuffer is the swapchain image's.
	void compose(VkCommandBuffer cmd, VkFramebuffer framebuffer, const jfhp::Look &look);

private:
	bool makeImage(VkFormat format, std::uint32_t w, std::uint32_t h, VkImageUsageFlags usage, VkImage *image,
		VkDeviceMemory *memory, VkImageView *view);
	std::uint32_t findMemory(std::uint32_t bits, VkMemoryPropertyFlags want) const;
	void hdrBarrier(VkImageLayout from, VkImageLayout to, VkAccessFlags srcAccess, VkAccessFlags dstAccess,
		VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage);

	jfhp::Config c_{};
	bool ready_{ false };
	VkPhysicalDeviceMemoryProperties memory_{};

	VkSampler sampler_{ VK_NULL_HANDLE };
	VkDescriptorSetLayout setLayout_{ VK_NULL_HANDLE };
	VkDescriptorPool pool_{ VK_NULL_HANDLE };
	VkDescriptorSet set_{ VK_NULL_HANDLE };
	VkPipelineLayout layout_{ VK_NULL_HANDLE };
	VkPipeline pipeline_{ VK_NULL_HANDLE };

	// the UI's target
	VkRenderPass uiPass_{ VK_NULL_HANDLE };
	VkImage uiImage_{ VK_NULL_HANDLE };
	VkDeviceMemory uiMemory_{ VK_NULL_HANDLE };
	VkImageView uiView_{ VK_NULL_HANDLE };
	VkFramebuffer uiFramebuffer_{ VK_NULL_HANDLE };

	// the HDR picture and the way it gets there
	VkImage hdrImage_{ VK_NULL_HANDLE };
	VkDeviceMemory hdrMemory_{ VK_NULL_HANDLE };
	VkImageView hdrView_{ VK_NULL_HANDLE };
	VkBuffer staging_{ VK_NULL_HANDLE };
	VkDeviceMemory stagingMemory_{ VK_NULL_HANDLE };
	void *mapped_{ nullptr };
	VkCommandPool cmdPool_{ VK_NULL_HANDLE };
	VkCommandBuffer cmd_{ VK_NULL_HANDLE };
	VkFence fence_{ VK_NULL_HANDLE };
};


std::uint32_t Impl::findMemory(std::uint32_t bits, VkMemoryPropertyFlags want) const
{
	for (std::uint32_t i = 0; i < memory_.memoryTypeCount; i++) {
		if ((bits & (1u << i)) && (memory_.memoryTypes[i].propertyFlags & want) == want) {
			return i;
		}
	}
	return UINT32_MAX;
}

bool Impl::makeImage(VkFormat format, std::uint32_t w, std::uint32_t h, VkImageUsageFlags usage, VkImage *image,
	VkDeviceMemory *memory, VkImageView *view)
{
	VkImageCreateInfo ici{};
	ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = format;
	ici.extent = { w, h, 1 };
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = usage;
	ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (vkCreateImage(c_.device, &ici, nullptr, image) != VK_SUCCESS) {
		return false;
	}
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(c_.device, *image, &req);
	VkMemoryAllocateInfo alloc{};
	alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	alloc.allocationSize = req.size;
	alloc.memoryTypeIndex = findMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (alloc.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(c_.device, &alloc, nullptr, memory) != VK_SUCCESS ||
		vkBindImageMemory(c_.device, *image, *memory, 0) != VK_SUCCESS) {
		return false;
	}
	VkImageViewCreateInfo vci{};
	vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	vci.image = *image;
	vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vci.format = format;
	vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	return vkCreateImageView(c_.device, &vci, nullptr, view) == VK_SUCCESS;
}

void Impl::hdrBarrier(VkImageLayout from, VkImageLayout to, VkAccessFlags srcAccess, VkAccessFlags dstAccess,
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
	b.image = hdrImage_;
	b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	vkCmdPipelineBarrier(cmd_, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

bool Impl::create(const jfhp::Config &config)
{
	destroy();
	c_ = config;
	if (!c_.device || !c_.baseRenderPass || !c_.depthView || !c_.sdrView || !c_.width || !c_.height) {
		say("the pass is missing something to draw with");
		return false;
	}
	vkGetPhysicalDeviceMemoryProperties(c_.physical, &memory_);
	VkFormatProperties fp;
	vkGetPhysicalDeviceFormatProperties(c_.physical, VK_FORMAT_R16G16B16A16_UNORM, &fp);
	const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
	if ((fp.optimalTilingFeatures & need) != need) {
		say("RGBA16 pictures cannot be sampled here");
		return false;
	}

	VkSamplerCreateInfo sci{};
	sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sci.magFilter = VK_FILTER_LINEAR;
	sci.minFilter = VK_FILTER_LINEAR;
	sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sci.maxLod = 0.0f;
	if (vkCreateSampler(c_.device, &sci, nullptr, &sampler_) != VK_SUCCESS) {
		return false;
	}

	VkDescriptorSetLayoutBinding bindings[3]{};
	for (uint32_t i = 0; i < 3; i++) {
		bindings[i].binding = i;
		bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		bindings[i].descriptorCount = 1;
		bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	}
	VkDescriptorSetLayoutCreateInfo slci{};
	slci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	slci.bindingCount = 3;
	slci.pBindings = bindings;
	if (vkCreateDescriptorSetLayout(c_.device, &slci, nullptr, &setLayout_) != VK_SUCCESS) {
		return false;
	}
	const VkDescriptorPoolSize size{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 };
	VkDescriptorPoolCreateInfo dpci{};
	dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	dpci.maxSets = 1;
	dpci.poolSizeCount = 1;
	dpci.pPoolSizes = &size;
	if (vkCreateDescriptorPool(c_.device, &dpci, nullptr, &pool_) != VK_SUCCESS) {
		return false;
	}
	VkDescriptorSetAllocateInfo dsai{};
	dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	dsai.descriptorPool = pool_;
	dsai.descriptorSetCount = 1;
	dsai.pSetLayouts = &setLayout_;
	if (vkAllocateDescriptorSets(c_.device, &dsai, &set_) != VK_SUCCESS) {
		return false;
	}
	const VkPushConstantRange range{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push) };
	VkPipelineLayoutCreateInfo plci{};
	plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	plci.setLayoutCount = 1;
	plci.pSetLayouts = &setLayout_;
	plci.pushConstantRangeCount = 1;
	plci.pPushConstantRanges = &range;
	if (vkCreatePipelineLayout(c_.device, &plci, nullptr, &layout_) != VK_SUCCESS) {
		return false;
	}

	// The UI's pass: the same attachments as the program's (so the kit's pipelines fit it), but the colour is cleared
	// to transparent and left ready to be sampled.
	VkAttachmentDescription att[2]{};
	att[0].format = c_.colorFormat;
	att[0].samples = VK_SAMPLE_COUNT_1_BIT;
	att[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	att[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	att[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	att[1].format = c_.depthFormat;
	att[1].samples = VK_SAMPLE_COUNT_1_BIT;
	att[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	att[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	att[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	const VkAttachmentReference colorRef{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	const VkAttachmentReference depthRef{ 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
	VkSubpassDescription sub{};
	sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	sub.colorAttachmentCount = 1;
	sub.pColorAttachments = &colorRef;
	sub.pDepthStencilAttachment = &depthRef;
	const VkPipelineStageFlags tests = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	VkSubpassDependency dep[2]{};
	dep[0].srcSubpass = VK_SUBPASS_EXTERNAL;
	dep[0].dstSubpass = 0;
	dep[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | tests;
	dep[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | tests;
	dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
		VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	dep[1].srcSubpass = 0;
	dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
	dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	dep[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	dep[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	VkRenderPassCreateInfo rpci{};
	rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	rpci.attachmentCount = 2;
	rpci.pAttachments = att;
	rpci.subpassCount = 1;
	rpci.pSubpasses = &sub;
	rpci.dependencyCount = 2;
	rpci.pDependencies = dep;
	if (vkCreateRenderPass(c_.device, &rpci, nullptr, &uiPass_) != VK_SUCCESS) {
		return false;
	}
	if (!makeImage(c_.colorFormat, c_.width, c_.height, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
			&uiImage_, &uiMemory_, &uiView_)) {
		return false;
	}
	const VkImageView fbViews[2] = { uiView_, c_.depthView };
	VkFramebufferCreateInfo fci{};
	fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	fci.renderPass = uiPass_;
	fci.attachmentCount = 2;
	fci.pAttachments = fbViews;
	fci.width = c_.width;
	fci.height = c_.height;
	fci.layers = 1;
	if (vkCreateFramebuffer(c_.device, &fci, nullptr, &uiFramebuffer_) != VK_SUCCESS) {
		return false;
	}

	// the HDR picture, its staging buffer, and a command buffer to copy with
	if (!makeImage(VK_FORMAT_R16G16B16A16_UNORM, kSurfaceW, kSurfaceH, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			&hdrImage_, &hdrMemory_, &hdrView_)) {
		return false;
	}
	const VkDeviceSize bytes = static_cast<VkDeviceSize>(kSurfaceW) * kSurfaceH * 8;
	VkBufferCreateInfo bci{};
	bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bci.size = bytes;
	bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (vkCreateBuffer(c_.device, &bci, nullptr, &staging_) != VK_SUCCESS) {
		return false;
	}
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(c_.device, staging_, &req);
	VkMemoryAllocateInfo alloc{};
	alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	alloc.allocationSize = req.size;
	alloc.memoryTypeIndex = findMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (alloc.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(c_.device, &alloc, nullptr, &stagingMemory_) != VK_SUCCESS ||
		vkBindBufferMemory(c_.device, staging_, stagingMemory_, 0) != VK_SUCCESS ||
		vkMapMemory(c_.device, stagingMemory_, 0, VK_WHOLE_SIZE, 0, &mapped_) != VK_SUCCESS) {
		return false;
	}
	VkCommandPoolCreateInfo cpci{};
	cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	cpci.queueFamilyIndex = c_.family;
	if (vkCreateCommandPool(c_.device, &cpci, nullptr, &cmdPool_) != VK_SUCCESS) {
		return false;
	}
	VkCommandBufferAllocateInfo cbai{};
	cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	cbai.commandPool = cmdPool_;
	cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cbai.commandBufferCount = 1;
	VkFenceCreateInfo fence{};
	fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	if (vkAllocateCommandBuffers(c_.device, &cbai, &cmd_) != VK_SUCCESS || vkCreateFence(c_.device, &fence, nullptr, &fence_) != VK_SUCCESS) {
		return false;
	}
	// black, and in the layout the shader reads it in
	{
		VkCommandBufferBeginInfo begin{};
		begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer(cmd_, &begin);
		hdrBarrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
			VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		const VkClearColorValue black{};
		const VkImageSubresourceRange sr{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		vkCmdClearColorImage(cmd_, hdrImage_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &sr);
		hdrBarrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
			VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
		vkEndCommandBuffer(cmd_);
		VkSubmitInfo submit{};
		submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submit.commandBufferCount = 1;
		submit.pCommandBuffers = &cmd_;
		if (vkQueueSubmit(c_.queue, 1, &submit, fence_) != VK_SUCCESS) {
			return false;
		}
		vkWaitForFences(c_.device, 1, &fence_, VK_TRUE, UINT64_MAX);
		vkResetFences(c_.device, 1, &fence_);
	}

	// what the shader reads
	const VkDescriptorImageInfo images[3] = {
		{ sampler_, uiView_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
		{ sampler_, c_.sdrView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
		{ sampler_, hdrView_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
	};
	VkWriteDescriptorSet writes[3]{};
	for (uint32_t i = 0; i < 3; i++) {
		writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstSet = set_;
		writes[i].dstBinding = i;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[i].pImageInfo = &images[i];
	}
	vkUpdateDescriptorSets(c_.device, 3, writes, 0, nullptr);

	// the pipeline, into the program's own pass (one colour, one depth attachment)
	const VkShaderModule vs = makeModule(c_.device, kComposeVert, sizeof(kComposeVert));
	const VkShaderModule fs = makeModule(c_.device, kComposeFrag, sizeof(kComposeFrag));
	bool ok = vs != VK_NULL_HANDLE && fs != VK_NULL_HANDLE;
	if (ok) {
		VkPipelineShaderStageCreateInfo stages[2]{};
		stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
		stages[0].module = vs;
		stages[0].pName = "main";
		stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
		stages[1].module = fs;
		stages[1].pName = "main";
		VkPipelineVertexInputStateCreateInfo input{};
		input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
		VkPipelineInputAssemblyStateCreateInfo assembly{};
		assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
		assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		VkPipelineViewportStateCreateInfo viewport{};
		viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
		viewport.viewportCount = 1;
		viewport.scissorCount = 1;
		VkPipelineRasterizationStateCreateInfo raster{};
		raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
		raster.polygonMode = VK_POLYGON_MODE_FILL;
		raster.cullMode = VK_CULL_MODE_NONE;
		raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
		raster.lineWidth = 1.0f;
		VkPipelineMultisampleStateCreateInfo multisample{};
		multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
		multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
		VkPipelineDepthStencilStateCreateInfo depth{};
		depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
		VkPipelineColorBlendAttachmentState blend{};
		blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
		VkPipelineColorBlendStateCreateInfo colorBlend{};
		colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
		colorBlend.attachmentCount = 1;
		colorBlend.pAttachments = &blend;
		const VkDynamicState states[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
		VkPipelineDynamicStateCreateInfo dynamic{};
		dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
		dynamic.dynamicStateCount = 2;
		dynamic.pDynamicStates = states;
		VkGraphicsPipelineCreateInfo gpci{};
		gpci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
		gpci.stageCount = 2;
		gpci.pStages = stages;
		gpci.pVertexInputState = &input;
		gpci.pInputAssemblyState = &assembly;
		gpci.pViewportState = &viewport;
		gpci.pRasterizationState = &raster;
		gpci.pMultisampleState = &multisample;
		gpci.pDepthStencilState = &depth;
		gpci.pColorBlendState = &colorBlend;
		gpci.pDynamicState = &dynamic;
		gpci.layout = layout_;
		gpci.renderPass = c_.baseRenderPass;
		gpci.subpass = 0;
		ok = vkCreateGraphicsPipelines(c_.device, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipeline_) == VK_SUCCESS;
	}
	if (vs) vkDestroyShaderModule(c_.device, vs, nullptr);
	if (fs) vkDestroyShaderModule(c_.device, fs, nullptr);
	if (!ok) {
		say("the compose pipeline was refused");
		return false;
	}
	ready_ = true;
	return true;
}

void Impl::destroy()
{
	ready_ = false;
	if (!c_.device) {
		return;
	}
	VkDevice d = c_.device;
	if (pipeline_) vkDestroyPipeline(d, pipeline_, nullptr);
	if (layout_) vkDestroyPipelineLayout(d, layout_, nullptr);
	if (pool_) vkDestroyDescriptorPool(d, pool_, nullptr); // frees the set
	if (setLayout_) vkDestroyDescriptorSetLayout(d, setLayout_, nullptr);
	if (sampler_) vkDestroySampler(d, sampler_, nullptr);
	if (uiFramebuffer_) vkDestroyFramebuffer(d, uiFramebuffer_, nullptr);
	if (uiView_) vkDestroyImageView(d, uiView_, nullptr);
	if (uiImage_) vkDestroyImage(d, uiImage_, nullptr);
	if (uiMemory_) vkFreeMemory(d, uiMemory_, nullptr);
	if (uiPass_) vkDestroyRenderPass(d, uiPass_, nullptr);
	if (fence_) vkDestroyFence(d, fence_, nullptr);
	if (cmdPool_) vkDestroyCommandPool(d, cmdPool_, nullptr);
	if (mapped_) vkUnmapMemory(d, stagingMemory_);
	if (staging_) vkDestroyBuffer(d, staging_, nullptr);
	if (stagingMemory_) vkFreeMemory(d, stagingMemory_, nullptr);
	if (hdrView_) vkDestroyImageView(d, hdrView_, nullptr);
	if (hdrImage_) vkDestroyImage(d, hdrImage_, nullptr);
	if (hdrMemory_) vkFreeMemory(d, hdrMemory_, nullptr);
	pipeline_ = VK_NULL_HANDLE;
	layout_ = VK_NULL_HANDLE;
	pool_ = VK_NULL_HANDLE;
	set_ = VK_NULL_HANDLE;
	setLayout_ = VK_NULL_HANDLE;
	sampler_ = VK_NULL_HANDLE;
	uiFramebuffer_ = VK_NULL_HANDLE;
	uiView_ = VK_NULL_HANDLE;
	uiImage_ = VK_NULL_HANDLE;
	uiMemory_ = VK_NULL_HANDLE;
	uiPass_ = VK_NULL_HANDLE;
	fence_ = VK_NULL_HANDLE;
	cmdPool_ = VK_NULL_HANDLE;
	cmd_ = VK_NULL_HANDLE;
	mapped_ = nullptr;
	staging_ = VK_NULL_HANDLE;
	stagingMemory_ = VK_NULL_HANDLE;
	hdrView_ = VK_NULL_HANDLE;
	hdrImage_ = VK_NULL_HANDLE;
	hdrMemory_ = VK_NULL_HANDLE;
	c_ = jfhp::Config{};
}

bool Impl::uploadHdr(const std::uint16_t *rgba, int w, int h)
{
	if (!ready_ || !rgba || w <= 0 || h <= 0 || w > kSurfaceW || h > kSurfaceH) {
		return false;
	}
	std::memcpy(mapped_, rgba, static_cast<size_t>(w) * h * 8);
	vkResetCommandBuffer(cmd_, 0);
	VkCommandBufferBeginInfo begin{};
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd_, &begin);
	hdrBarrier(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
		VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	VkBufferImageCopy region{};
	region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	region.imageExtent = { static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1 };
	vkCmdCopyBufferToImage(cmd_, staging_, hdrImage_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
	hdrBarrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
		VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
	vkEndCommandBuffer(cmd_);
	VkSubmitInfo submit{};
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &cmd_;
	if (vkQueueSubmit(c_.queue, 1, &submit, fence_) != VK_SUCCESS) {
		return false;
	}
	vkWaitForFences(c_.device, 1, &fence_, VK_TRUE, UINT64_MAX);
	vkResetFences(c_.device, 1, &fence_);
	return true;
}

void Impl::beginUi(VkCommandBuffer cmd)
{
	VkClearValue clear[2]{};
	clear[1].depthStencil = { 1.0f, 0 };
	VkRenderPassBeginInfo info{};
	info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	info.renderPass = uiPass_;
	info.framebuffer = uiFramebuffer_;
	info.renderArea.extent = { c_.width, c_.height };
	info.clearValueCount = 2;
	info.pClearValues = clear;
	vkCmdBeginRenderPass(cmd, &info, VK_SUBPASS_CONTENTS_INLINE);
}

void Impl::endUi(VkCommandBuffer cmd)
{
	vkCmdEndRenderPass(cmd);
}

void Impl::compose(VkCommandBuffer cmd, VkFramebuffer framebuffer, const jfhp::Look &look)
{
	VkClearValue clear[2]{};
	clear[1].depthStencil = { 1.0f, 0 };
	VkRenderPassBeginInfo info{};
	info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	info.renderPass = c_.baseRenderPass;
	info.framebuffer = framebuffer;
	info.renderArea.extent = { c_.width, c_.height };
	info.clearValueCount = 2;
	info.pClearValues = clear;
	vkCmdBeginRenderPass(cmd, &info, VK_SUBPASS_CONTENTS_INLINE);
	const VkViewport viewport{ 0.0f, 0.0f, static_cast<float>(c_.width), static_cast<float>(c_.height), 0.0f, 1.0f };
	const VkRect2D scissor{ { 0, 0 }, { c_.width, c_.height } };
	vkCmdSetViewport(cmd, 0, 1, &viewport);
	vkCmdSetScissor(cmd, 0, 1, &scissor);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set_, 0, nullptr);
	Push push{};
	push.rect[0] = look.x;
	push.rect[1] = look.y;
	push.rect[2] = std::max(look.w, 1e-4f);
	push.rect[3] = std::max(look.h, 1e-4f);
	push.frame[0] = static_cast<float>(look.frameW);
	push.frame[1] = static_cast<float>(look.frameH);
	// /data/jf-hdr-gain: a number, the film's brightness in percent (25..400, 100 = as mastered)
	static float gain = 1.0f;
	static int countdown = 0;
	if (countdown-- <= 0) {
		countdown = 120;
		float g = 1.0f;
		if (FILE *f = std::fopen("/data/jf-hdr-gain", "r")) {
			int n = 0;
			if (std::fscanf(f, "%d", &n) == 1 && n >= 25 && n <= 400) {
				g = static_cast<float>(n) / 100.0f;
			}
			std::fclose(f);
		}
		gain = g;
	}
	if (gGainPercent.load() >= 25) {
		gain = static_cast<float>(gGainPercent.load()) / 100.0f;
	}
	push.frame[2] = gain;
	push.frame[3] = 0.0f;
	push.misc[0] = static_cast<float>(look.film);
	push.misc[1] = look.uiNits;
	push.misc[2] = look.sdrNits;
	push.misc[3] = look.redLow ? 1.0f : 0.0f;
	vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
	vkCmdDraw(cmd, 3, 1, 0, 0);
	vkCmdEndRenderPass(cmd);
}

} // namespace jfhp_impl

extern "C" {
void *jfhp_create(VkPhysicalDevice physical, VkDevice device, VkQueue queue, std::uint32_t family, VkRenderPass baseRenderPass,
	VkFormat colorFormat, VkFormat depthFormat, VkImageView depthView, VkImageView sdrView, std::uint32_t width, std::uint32_t height)
{
	jfhp::Config c;
	c.physical = physical;
	c.device = device;
	c.queue = queue;
	c.family = family;
	c.baseRenderPass = baseRenderPass;
	c.colorFormat = colorFormat;
	c.depthFormat = depthFormat;
	c.depthView = depthView;
	c.sdrView = sdrView;
	c.width = width;
	c.height = height;
	jfhp_impl::Impl *p = new jfhp_impl::Impl();
	if (!p->create(c)) {
		p->destroy();
		delete p;
		return nullptr;
	}
	return p;
}
void jfhp_set_gain(int percent)
{
	jfhp_impl::gGainPercent = percent;
}
void jfhp_destroy(void *pass)
{
	if (pass) {
		jfhp_impl::Impl *p = static_cast<jfhp_impl::Impl *>(pass);
		p->destroy();
		delete p;
	}
}
bool jfhp_uploadHdr(void *pass, const std::uint16_t *rgba, int w, int h)
{
	return pass && static_cast<jfhp_impl::Impl *>(pass)->uploadHdr(rgba, w, h);
}
void jfhp_beginUi(void *pass, VkCommandBuffer cmd) { static_cast<jfhp_impl::Impl *>(pass)->beginUi(cmd); }
void jfhp_endUi(void *pass, VkCommandBuffer cmd) { static_cast<jfhp_impl::Impl *>(pass)->endUi(cmd); }
void jfhp_compose(void *pass, VkCommandBuffer cmd, VkFramebuffer framebuffer, int film, float x, float y, float w, float h,
	int frameW, int frameH, float uiNits, float sdrNits, bool redLow)
{
	jfhp::Look l;
	l.film = film;
	l.x = x;
	l.y = y;
	l.w = w;
	l.h = h;
	l.frameW = frameW;
	l.frameH = frameH;
	l.uiNits = uiNits;
	l.sdrNits = sdrNits;
	l.redLow = redLow;
	static_cast<jfhp_impl::Impl *>(pass)->compose(cmd, framebuffer, l);
}
}
