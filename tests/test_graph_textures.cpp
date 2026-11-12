#include "RenderGraphInternal.h"

#include <doctest/doctest.h>
#include <vulkan/vk_enum_string_helper.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace mythril;

namespace {
	constexpr TextureAccess kAccesses[] = {
	        TextureAccess::Sampled,
	        TextureAccess::StorageRead,
	        TextureAccess::StorageWrite,
	        TextureAccess::StorageReadWrite,
	        TextureAccess::TransferRead,
	        TextureAccess::TransferWrite,
	};
	constexpr uint32_t kAttachmentBit = 1u << std::size(kAccesses);
	constexpr uint32_t kResolveBit = kAttachmentBit << 1;
	constexpr uint32_t kUseSubsets = kResolveBit << 1;
	constexpr VkFormat kFormats[] = {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D32_SFLOAT};

	// the usage a layout demands, from the VkImageMemoryBarrier2 newLayout and VkRenderingAttachmentInfo VUIDs
	VkImageUsageFlags RequiredBy(TextureAccess access) {
		switch (access) {
			case TextureAccess::Sampled:
				return VK_IMAGE_USAGE_SAMPLED_BIT;
			case TextureAccess::StorageRead:
			case TextureAccess::StorageWrite:
			case TextureAccess::StorageReadWrite:
				return VK_IMAGE_USAGE_STORAGE_BIT;
			case TextureAccess::TransferRead:
				return VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
			case TextureAccess::TransferWrite:
				return VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		}
		return 0;
	}

	using InferFn = Realization (*)(const GraphTextureSpec&, const TextureUses&, bool);

	std::vector<std::string> check(InferFn infer, uint32_t subset, VkFormat format, bool exported) {
		const GraphTextureSpec spec = {.size = {64, 32}, .format = format};
		TextureUses uses;
		VkImageUsageFlags required = 0;
		for (size_t i = 0; i < std::size(kAccesses); ++i) {
			if (subset & (1u << i)) {
				uses.add(kAccesses[i]);
				required |= RequiredBy(kAccesses[i]);
			}
		}
		const VkImageUsageFlags attachmentBit = vkutil::IsFormatDepthOrStencil(format) ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		if (subset & (kAttachmentBit | kResolveBit)) {
			uses.addAttachment();
			required |= attachmentBit;
		}
		// sampled outside the graph, and the exit barrier leaves it in SHADER_READ_ONLY
		if (exported)
			required |= VK_IMAGE_USAGE_SAMPLED_BIT;

		const Realization realization = infer(spec, uses, exported);
		const TextureSpec created = ToTextureSpec(spec, realization);
		const VkImageUsageFlags usage = vkutil::ToVkImageUsage(created.usage, created.storage, created.format);

		std::vector<std::string> failures;
		// Device storage always carries both transfer bits so the texture can be uploaded to and read back
		constexpr VkImageUsageFlags kPolicy = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		if ((required & ~kPolicy) == 0) {
			if (realization.usage != 0)
				failures.emplace_back("a texture only transfers touch must infer no usage so compile asserts");
			return failures;
		}
		if (realization.storage != StorageType::Device)
			failures.emplace_back("storage is not Device");
		if (created.dimension.width != 64 || created.dimension.height != 32)
			failures.emplace_back("dimensions do not match the spec");
		if (const VkImageUsageFlags missing = required & ~usage)
			failures.push_back("missing " + string_VkImageUsageFlags(missing));
		if (const VkImageUsageFlags unjustified = usage & ~required & ~kPolicy)
			failures.push_back("unjustified " + string_VkImageUsageFlags(unjustified));
		return failures;
	}

	size_t checkEverySubset(InferFn infer, std::vector<std::string>* allFailures = nullptr) {
		size_t checked = 0;
		for (uint32_t subset = 0; subset < kUseSubsets; ++subset) {
			for (const VkFormat format: kFormats) {
				for (const bool exported: {false, true}) {
					++checked;
					const std::vector<std::string> failures = check(infer, subset, format, exported);
					if (failures.empty())
						continue;
					if (allFailures) {
						allFailures->insert(allFailures->end(), failures.begin(), failures.end());
						continue;
					}
					std::string all;
					for (const std::string& failure: failures)
						all += "\n  " + failure;
					FAIL_CHECK("subset " << subset << " format " << string_VkFormat(format) << " exported " << exported << all);
				}
			}
		}
		return checked;
	}

	namespace known_bad {
		// what 07 picked by hand for its offscreen color target, Storage it never uses
		Realization superset(const GraphTextureSpec& spec, const TextureUses&, bool) {
			return {.dimensions = spec.size, .usage = TextureUsageBits_Attachment | TextureUsageBits_Sampled | TextureUsageBits_Storage};
		}
		Realization exportNotSampled(const GraphTextureSpec& spec, const TextureUses& uses, bool) {
			return InferRealization(spec, uses, false);
		}
	} // namespace known_bad

	bool anyFailureContains(InferFn infer, const char* needle) {
		std::vector<std::string> failures;
		checkEverySubset(infer, &failures);
		return std::ranges::any_of(failures, [&](const std::string& failure) { return failure.find(needle) != std::string::npos; });
	}
} // namespace

TEST_CASE("inferred graph texture usage covers every declared access and nothing else") {
	CHECK(checkEverySubset(InferRealization) == kUseSubsets * std::size(kFormats) * 2);
}

TEST_CASE("usage checker catches hand picked and dropped usage") {
	CHECK(anyFailureContains(known_bad::superset, "unjustified VK_IMAGE_USAGE_STORAGE_BIT"));
	CHECK(anyFailureContains(known_bad::exportNotSampled, "missing VK_IMAGE_USAGE_SAMPLED_BIT"));
}

TEST_CASE("an exported texture keeps its last writer alive with no reader in the graph") {
	RenderGraph graph;
	const GraphTexture preview = graph.createTexture({.size = {16, 16}, .format = VK_FORMAT_D16_UNORM, .debugName = "preview"});
	const GraphTexture backBuffer = graph.backBuffer();

	std::vector<PassDesc> passes(2);
	passes[0].name = "writes preview";
	passes[0].type = PassDesc::Type::Graphics;
	passes[0].attachmentOperations.push_back({.texDesc = preview});
	passes[1].name = "writes back buffer";
	passes[1].type = PassDesc::Type::Graphics;
	passes[1].attachmentOperations.push_back({.texDesc = backBuffer});
	const std::vector<uint32_t> order = {0, 1};
	const std::vector<std::unordered_set<uint32_t>> predecessors(2);

	for (const bool exported: {false, true}) {
		std::vector<GraphResource> textures(2);
		textures[0].source = DeclaredTexture{.spec = {.size = {16, 16}, .format = VK_FORMAT_D16_UNORM}, .exported = exported};
		textures[1].source = SwapchainTexture{};
		const std::vector<uint32_t> kept = cull_unused_passes(order, predecessors, passes, textures);
		CHECK_MESSAGE(kept == (exported ? std::vector<uint32_t>{0, 1} : std::vector<uint32_t>{1}), "exported " << exported);
	}
}
