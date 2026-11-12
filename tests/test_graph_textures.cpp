#include "RenderGraphInternal.h"

#include <doctest/doctest.h>
#include <vulkan/vk_enum_string_helper.h>

#include <algorithm>
#include <optional>
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

	using InferFn = Realization (*)(Dimensions, const TextureUses&, bool, bool);

	std::vector<std::string> check(InferFn infer, uint32_t subset, VkFormat format, bool exported) {
		const GraphTextureSpec spec = {.size = Dimensions{64, 32}, .format = format};
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
		if (exported)
			required |= VK_IMAGE_USAGE_SAMPLED_BIT;

		const Realization realization = infer(std::get<Dimensions>(spec.size), uses, exported, false);
		const Realization confinedRealization = infer(std::get<Dimensions>(spec.size), uses, exported, true);
		const TextureSpec created = ToTextureSpec(spec, realization);
		std::vector<std::string> failures;
		if (confinedRealization.usage != realization.usage || confinedRealization.dimensions != realization.dimensions)
			failures.emplace_back("confinement changed more than storage");
		if (confinedRealization.storage == StorageType::Memoryless) {
			if (const VkImageUsageFlags missing = required & ~vkutil::ToVkImageUsage(confinedRealization.usage, StorageType::Memoryless, format))
				failures.push_back("memoryless drops " + string_VkImageUsageFlags(missing));
		}

		const VkImageUsageFlags usage = vkutil::ToVkImageUsage(created.usage, created.storage, created.format);

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
		Realization superset(const Dimensions dimensions, const TextureUses&, bool, bool) {
			return {.dimensions = dimensions, .usage = TextureUsageBits_Attachment | TextureUsageBits_Sampled | TextureUsageBits_Storage};
		}
		Realization exportNotSampled(const Dimensions dimensions, const TextureUses& uses, bool, const bool confined) {
			return InferRealization(dimensions, uses, false, confined);
		}
	}

	bool anyFailureContains(InferFn infer, const char* needle) {
		std::vector<std::string> failures;
		checkEverySubset(infer, &failures);
		return std::ranges::any_of(failures, [&](const std::string& failure) { return failure.find(needle) != std::string::npos; });
	}
}

TEST_CASE("inferred graph texture usage covers every declared access and nothing else") {
	CHECK(checkEverySubset(InferRealization) == kUseSubsets * std::size(kFormats) * 2);
}

TEST_CASE("usage checker catches hand picked and dropped usage") {
	CHECK(anyFailureContains(known_bad::superset, "unjustified VK_IMAGE_USAGE_STORAGE_BIT"));
	CHECK(anyFailureContains(known_bad::exportNotSampled, "missing VK_IMAGE_USAGE_SAMPLED_BIT"));
}

TEST_CASE("an exported texture keeps its last writer alive with no reader in the graph") {
	RenderGraph graph;
	const GraphTexture preview = graph.createTexture({.size = Dimensions{16, 16}, .format = VK_FORMAT_D16_UNORM, .debugName = "preview"});
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
		textures[0].source = DeclaredTexture{.spec = {.size = Dimensions{16, 16}, .format = VK_FORMAT_D16_UNORM}, .exported = exported};
		textures[1].source = SwapchainTexture{};
		const std::vector<uint32_t> kept = cull_unused_passes(order, predecessors, passes, textures);
		CHECK_MESSAGE(kept == (exported ? std::vector<uint32_t>{0, 1} : std::vector<uint32_t>{1}), "exported " << exported);
	}
}

TEST_CASE("relative sizes floor each axis and never reach zero") {
	constexpr Dimensions kExtents[] = {{1280, 720, 1}, {2560, 1440, 1}, {1, 1, 1}, {333, 7, 1}};
	size_t checked = 0;
	for (const Dimensions extent: kExtents) {
		for (uint32_t i = 0; i < 8; ++i) {
			++checked;
			const Dimensions scaled = ScaleExtent(extent, 1.f / static_cast<float>(1u << i));
			CHECK(scaled.width == std::max(1u, extent.width >> i));
			CHECK(scaled.height == std::max(1u, extent.height >> i));
			CHECK(scaled.depth == 1);
		}
	}
	CHECK(checked == std::size(kExtents) * 8);
	CHECK(ScaleExtent({1280, 720, 1}, 0.75f) == Dimensions{960, 540, 1});
}

namespace {
	AttachmentInfo Attachment(const GraphTexture texture, const VkAttachmentLoadOp loadOp, const VkAttachmentStoreOp storeOp, const std::optional<GraphTexture> resolve = std::nullopt) {
		AttachmentInfo attachment{};
		attachment.target.resource = texture;
		attachment.loadOp = loadOp;
		attachment.storeOp = storeOp;
		if (resolve.has_value())
			attachment.resolveTarget = AttachmentTarget{.resource = *resolve};
		return attachment;
	}

	CompiledPass Group(std::vector<AttachmentInfo> colors, std::vector<GraphTexture> alsoDeclared = {}) {
		CompiledPass group;
		std::vector<GraphTexture> declared = std::move(alsoDeclared);
		for (const AttachmentInfo& color: colors) {
			declared.push_back(color.target.resource);
			if (color.resolveTarget.has_value())
				declared.push_back(color.resolveTarget->resource);
		}
		std::ranges::sort(declared, {}, GraphTextureAccess::index);
		declared.erase(std::ranges::unique(declared).begin(), declared.end());
		group.members.push_back({.name = "member", .sortedDeclaredTextures = std::move(declared)});
		group.colorAttachments = std::move(colors);
		return group;
	}

	constexpr VkAttachmentLoadOp kClear = VK_ATTACHMENT_LOAD_OP_CLEAR;
	constexpr VkAttachmentLoadOp kLoad = VK_ATTACHMENT_LOAD_OP_LOAD;
	constexpr VkAttachmentStoreOp kStore = VK_ATTACHMENT_STORE_OP_STORE;
	constexpr VkAttachmentStoreOp kDiscard = VK_ATTACHMENT_STORE_OP_DONT_CARE;

	struct ConfinedFixture {
		RenderGraph graph;
		GraphTexture msaa = graph.createTexture({.size = Dimensions{16, 16}, .format = VK_FORMAT_R8G8B8A8_UNORM, .debugName = "msaa"});
		GraphTexture resolve = graph.createTexture({.size = Dimensions{16, 16}, .format = VK_FORMAT_R8G8B8A8_UNORM, .debugName = "resolve"});

		std::vector<bool> run(const std::vector<CompiledPass>& groups) const { return ConfinedToOneScope(groups, 2); }
		static bool isConfined(const std::vector<bool>& confined, const GraphTexture texture) { return confined[GraphTextureAccess::index(texture)]; }
	};
}

TEST_CASE("an attachment cleared and discarded inside one scope is confined") {
	ConfinedFixture f;
	std::vector<CompiledPass> groups;
	groups.push_back(Group({Attachment(f.msaa, kClear, kDiscard)}));
	CHECK(ConfinedFixture::isConfined(f.run(groups), f.msaa));
}

TEST_CASE("an attachment stored in one scope and loaded in the next is not confined") {
	ConfinedFixture f;
	std::vector<CompiledPass> groups;
	groups.push_back(Group({Attachment(f.msaa, kClear, kStore)}));
	groups.push_back(Group({Attachment(f.msaa, kLoad, kDiscard)}));
	CHECK_FALSE(ConfinedFixture::isConfined(f.run(groups), f.msaa));
}

TEST_CASE("a resolve target is never confined") {
	ConfinedFixture f;
	std::vector<CompiledPass> groups;
	groups.push_back(Group({Attachment(f.msaa, kClear, kDiscard, f.resolve)}));
	const std::vector<bool> confined = f.run(groups);
	CHECK(ConfinedFixture::isConfined(confined, f.msaa));
	CHECK_FALSE(ConfinedFixture::isConfined(confined, f.resolve));
}

TEST_CASE("an attachment another scope declares as a dependency is not confined") {
	ConfinedFixture f;
	std::vector<CompiledPass> groups;
	groups.push_back(Group({Attachment(f.msaa, kClear, kDiscard)}));
	groups.push_back(Group({Attachment(f.resolve, kClear, kDiscard)}, {f.msaa}));
	const std::vector<bool> confined = f.run(groups);
	CHECK_FALSE(ConfinedFixture::isConfined(confined, f.msaa));
	CHECK(ConfinedFixture::isConfined(confined, f.resolve));
}

TEST_CASE("an attachment whose contents are stored or loaded is not confined") {
	ConfinedFixture f;
	for (const auto [loadOp, storeOp]: {std::pair{kClear, kStore}, std::pair{kLoad, kDiscard}, std::pair{kLoad, kStore}}) {
		std::vector<CompiledPass> groups;
		groups.push_back(Group({Attachment(f.msaa, loadOp, storeOp)}));
		CHECK_FALSE(ConfinedFixture::isConfined(f.run(groups), f.msaa));
	}
}

TEST_CASE("only an attachment-only confined texture is inferred memoryless") {
	TextureUses attachmentOnly;
	attachmentOnly.addAttachment();
	TextureUses sampled = attachmentOnly;
	sampled.add(TextureAccess::Sampled);
	constexpr Dimensions kSize{16, 16};
	CHECK(InferRealization(kSize, attachmentOnly, false, true).storage == StorageType::Memoryless);
	CHECK(InferRealization(kSize, attachmentOnly, false, false).storage == StorageType::Device);
	CHECK(InferRealization(kSize, attachmentOnly, true, true).storage == StorageType::Device);
	CHECK(InferRealization(kSize, sampled, false, true).storage == StorageType::Device);
}
