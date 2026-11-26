#include "RenderGraphInternal.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <numeric>
#include <vector>

using namespace mythril;

namespace {
	GraphTexture Version(const uint32_t id, const uint32_t version) { return GraphTextureAccess::make(id, version); }

	std::vector<PassUses> UsesOfAll(const std::vector<PassDesc>& passes) {
		std::vector<PassUses> uses;
		for (const PassDesc& pass: passes)
			uses.push_back(UsesOf(pass));
		return uses;
	}

	struct Picked {
		std::vector<uint32_t> order;
		std::vector<size_t> groupOf;
	};
	Picked Flatten(const ScheduleResult& result, const size_t nodeCount) {
		Picked picked{{}, std::vector<size_t>(nodeCount, SIZE_MAX)};
		for (size_t g = 0; g < result.groups.size(); ++g) {
			for (const uint32_t node: result.groups[g]) {
				picked.order.push_back(node);
				picked.groupOf[node] = g;
			}
		}
		return picked;
	}
	bool IsTopological(const std::vector<ScheduleNode>& nodes, const Picked& picked) {
		std::vector<size_t> position(nodes.size());
		for (size_t i = 0; i < picked.order.size(); ++i)
			position[picked.order[i]] = i;
		for (uint32_t node = 0; node < nodes.size(); ++node) {
			for (const uint32_t pred: nodes[node].predecessors) {
				if (position[pred] >= position[node])
					return false;
			}
		}
		return picked.order.size() == nodes.size();
	}
}

TEST_CASE("versions order readers of an old version before the next writer") {
	std::vector<PassDesc> passes(4);
	passes[0].textureDependencyOperations.emplace_back(Version(0, 0), TextureAccess::StorageWrite);
	passes[1].textureDependencyOperations.emplace_back(Version(0, 1), TextureAccess::Sampled);
	passes[2].textureDependencyOperations.emplace_back(Version(0, 1), TextureAccess::StorageWrite);
	passes[3].textureDependencyOperations.emplace_back(Version(0, 1), TextureAccess::Sampled);
	const PassEdges edges = BuildPassEdges(UsesOfAll(passes), 1, 0);

	CHECK(edges.data[0].empty());
	CHECK(edges.data[1] == std::unordered_set<uint32_t>{0});
	CHECK(edges.data[2] == std::unordered_set<uint32_t>{0});
	CHECK(edges.data[3] == std::unordered_set<uint32_t>{0});
	CHECK(edges.order[2] == std::unordered_set<uint32_t>{0, 1, 3});
	CHECK(edges.order[3] == std::unordered_set<uint32_t>{0});
}

TEST_CASE("culling follows data edges, so a reader ordered before a kept writer is still culled") {
	std::vector<GraphResource> textures(2);
	textures[0].source = DeclaredTexture{.spec = {.size = Dimensions{16, 16}, .format = VK_FORMAT_R8G8B8A8_UNORM}};
	textures[1].source = ImportedTexture{};
	std::vector<PassDesc> passes(3);
	for (PassDesc& pass: passes)
		pass.type = PassDesc::Type::Compute;
	passes[0].textureDependencyOperations.emplace_back(Version(0, 0), TextureAccess::StorageWrite);
	passes[1].textureDependencyOperations.emplace_back(Version(0, 1), TextureAccess::Sampled);
	passes[2].textureDependencyOperations.emplace_back(Version(0, 1), TextureAccess::StorageReadWrite);
	passes[2].textureDependencyOperations.emplace_back(Version(1, 0), TextureAccess::StorageWrite);
	const PassEdges edges = BuildPassEdges(UsesOfAll(passes), textures.size(), 0);
	REQUIRE(edges.order[2].contains(1));
	CHECK(cull_unused_passes({0, 1, 2}, edges.data, passes, textures) == std::vector<uint32_t>{0, 2});
}

TEST_CASE("the scheduler keeps a merge chain whole in every declaration order") {
	enum : uint32_t { Upload,
		Sky,
		Opaque,
		Transparent,
		Particles,
		kCount };
	const std::vector<std::vector<uint32_t>> preds = {{}, {}, {Sky}, {Opaque}, {Transparent, Upload}};
	std::vector<uint32_t> label(kCount);
	std::iota(label.begin(), label.end(), 0u);
	size_t orders = 0;
	do {
		std::vector<ScheduleNode> nodes(kCount);
		for (uint32_t role = 0; role < kCount; ++role) {
			for (const uint32_t pred: preds[role])
				nodes[label[role]].predecessors.push_back(label[pred]);
			nodes[label[role]].renders = role != Upload;
		}
		const auto tryJoin = [&](const std::span<const uint32_t> group, const uint32_t candidate) { return nodes[group.front()].renders && nodes[candidate].renders; };
		const ScheduleResult result = SchedulePasses(std::span<const ScheduleNode>(nodes), false, true, tryJoin);
		const Picked picked = Flatten(result, kCount);
		CHECK(result.blocked.empty());
		CHECK(IsTopological(nodes, picked));
		CHECK(result.groups.size() == 2);
		CHECK(picked.groupOf[label[Upload]] == 0);
		CHECK(picked.groupOf[label[Sky]] == picked.groupOf[label[Particles]]);
		CHECK(SchedulePasses(std::span<const ScheduleNode>(nodes), false, true, tryJoin).groups == result.groups);
		++orders;
	} while (std::ranges::next_permutation(label).found);
	CHECK(orders == 120);
}

TEST_CASE("the scheduler starts async work first and runs its consumers last") {
	std::vector<ScheduleNode> nodes(4);
	nodes[1].async = true;
	nodes[2].predecessors = {1};
	nodes[2].readsAsync = true;
	const auto never = [](std::span<const uint32_t>, uint32_t) { return false; };
	const ScheduleResult result = SchedulePasses(std::span<const ScheduleNode>(nodes), false, true, never);
	CHECK(Flatten(result, nodes.size()).order == std::vector<uint32_t>{1, 0, 3, 2});
}

TEST_CASE("the scheduler reports cycles and declared order violations instead of dropping passes") {
	const auto never = [](std::span<const uint32_t>, uint32_t) { return false; };
	std::vector<ScheduleNode> cycle(3);
	cycle[0].predecessors = {2};
	cycle[2].predecessors = {0};
	const ScheduleResult cyclic = SchedulePasses(std::span<const ScheduleNode>(cycle), false, true, never);
	CHECK(cyclic.blocked == std::vector<uint32_t>{0, 2});
	CHECK(Flatten(cyclic, cycle.size()).order == std::vector<uint32_t>{1});

	std::vector<ScheduleNode> backwards(2);
	backwards[0].predecessors = {1};
	CHECK(SchedulePasses(std::span<const ScheduleNode>(backwards), true, true, never).blocked == std::vector<uint32_t>{0});
	CHECK(SchedulePasses(std::span<const ScheduleNode>(backwards), false, true, never).blocked.empty());
}

TEST_CASE("async picks its queue from the pass kind and falls back to main") {
	constexpr AsyncQueues kAll{.compute = true, .transfer = true};
	constexpr AsyncQueues kComputeOnly{.compute = true};
	CHECK(QueueOf(PassDesc::Type::Compute, false, kAll) == QueueAffinity::Graphics);
	CHECK(QueueOf(PassDesc::Type::Transfer, false, kAll) == QueueAffinity::Graphics);
	CHECK(QueueOf(PassDesc::Type::Compute, true, kAll) == QueueAffinity::AsyncCompute);
	CHECK(QueueOf(PassDesc::Type::RayTracing, true, kAll) == QueueAffinity::AsyncCompute);
	CHECK(QueueOf(PassDesc::Type::Transfer, true, kAll) == QueueAffinity::AsyncTransfer);
	CHECK(QueueOf(PassDesc::Type::Transfer, true, kComputeOnly) == QueueAffinity::AsyncCompute);
	CHECK(QueueOf(PassDesc::Type::Compute, true, {}) == QueueAffinity::Graphics);
	CHECK(QueueOf(PassDesc::Type::Transfer, true, {}) == QueueAffinity::Graphics);
}

TEST_CASE("an async transfer pass that builds a structure runs on async compute") {
	constexpr AsyncQueues kAll{.compute = true, .transfer = true};
	PassDesc copy{.name = "copy", .type = PassDesc::Type::Transfer, .transferOps = {BufferCopyOp{}}};
	PassDesc build{.name = "build", .type = PassDesc::Type::Transfer, .transferOps = {BufferCopyOp{}, AccelerationStructureBuildOp{}}};
	CHECK(QueueOf(QueueNeedOf(copy, {}).type, true, kAll) == QueueAffinity::AsyncTransfer);
	CHECK(QueueOf(QueueNeedOf(build, {}).type, true, kAll) == QueueAffinity::AsyncCompute);
	CHECK(QueueOf(QueueNeedOf(build, {}).type, true, {.transfer = true}) == QueueAffinity::Graphics);
}

namespace {
	std::vector<uint32_t> BatchesOf(const std::vector<QueueAffinity>& queues, const std::vector<std::vector<uint32_t>>& preds) {
		std::vector<uint32_t> batches;
		for (const BatchSlot& slot: SplitBatches(queues, preds))
			batches.push_back(slot.batch);
		return batches;
	}
}

TEST_CASE("batches split only where a queue waits on another") {
	constexpr auto G = QueueAffinity::Graphics;
	constexpr auto C = QueueAffinity::AsyncCompute;
	SUBCASE("one queue is one batch") {
		const std::vector queues{G, G, G};
		const std::vector<std::vector<uint32_t>> preds{{}, {0}, {1}};
		CHECK(BatchesOf(queues, preds) == std::vector<uint32_t>{0, 0, 0});
	}
	SUBCASE("main reads async work, so main splits once and async stays whole") {
		// 0 main, 1 async, 2 main independent, 3 main reads 1
		const std::vector queues{G, C, G, G};
		const std::vector<std::vector<uint32_t>> preds{{}, {}, {0}, {1, 2}};
		CHECK(BatchesOf(queues, preds) == std::vector<uint32_t>{0, 0, 0, 1});
		const std::vector<BatchSlot> slots = SplitBatches(queues, preds);
		CHECK(slots[3].waits[static_cast<size_t>(C)] == 0);
		CHECK(slots[0].waits[static_cast<size_t>(C)] == BatchSlot::kNone);
	}
	SUBCASE("a producer batch closes when another queue waits on it") {
		// 0 async, 1 main reads 0, 2 async again, 3 main reads 2
		const std::vector queues{C, G, C, G};
		const std::vector<std::vector<uint32_t>> preds{{}, {0}, {}, {2}};
		CHECK(BatchesOf(queues, preds) == std::vector<uint32_t>{0, 0, 1, 1});
	}
	SUBCASE("a second wait on the same batch does not split again") {
		const std::vector queues{C, G, G};
		const std::vector<std::vector<uint32_t>> preds{{}, {0}, {0, 1}};
		CHECK(BatchesOf(queues, preds) == std::vector<uint32_t>{0, 0, 0});
	}
}

TEST_CASE("an async pass overlaps only main work it does not depend on") {
	constexpr auto G = QueueAffinity::Graphics;
	constexpr auto C = QueueAffinity::AsyncCompute;
	CHECK(HasIndependentMainWork(std::vector{G, C, G}, std::vector<std::vector<uint32_t>>{{}, {0}, {1}})[1] == false);
	CHECK(HasIndependentMainWork(std::vector{G, C, G}, std::vector<std::vector<uint32_t>>{{}, {}, {0, 1}})[1] == true);
}

TEST_CASE("exclusive images move between families with matching releases and acquires") {
	constexpr uint32_t G = 0, C = 1;
	constexpr uint32_t kStart = OwnershipTransfer::kFrameStart;
	constexpr uint32_t kEnd = OwnershipTransfer::kFrameEnd;
	const auto pass = [](const uint32_t family, const bool keeps, const SubresourceRange range = SubresourceRange{0, 1, 0, 1}) { return OwnershipPass{family, {{0, range, 0, keeps}}}; };
	const auto ends = [](const OwnershipTransfer& t) { return std::pair{t.releasePass, t.acquirePass}; };

	SUBCASE("main to async and back inside the frame") {
		const OwnershipPlan plan = PlanOwnership(std::vector{pass(G, true), pass(C, true), pass(G, true)}, G);
		REQUIRE(plan.transfers.size() == 2);
		CHECK(ends(plan.transfers[0]) == std::pair{0u, 1u});
		CHECK((plan.transfers[0].srcFamily == G && plan.transfers[0].dstFamily == C));
		CHECK(ends(plan.transfers[1]) == std::pair{1u, 2u});
		CHECK(plan.discards.empty());
	}
	SUBCASE("an image first used on async is released at frame start and returned at the join") {
		const OwnershipPlan plan = PlanOwnership(std::vector{pass(C, true)}, G);
		REQUIRE(plan.transfers.size() == 2);
		CHECK(ends(plan.transfers[0]) == std::pair{kStart, 0u});
		CHECK(ends(plan.transfers[1]) == std::pair{0u, kEnd});
		CHECK((plan.transfers[1].srcFamily == C && plan.transfers[1].dstFamily == G));
	}
	SUBCASE("dropping the contents skips the transfer but still returns the image") {
		const OwnershipPlan plan = PlanOwnership(std::vector{pass(C, false)}, G);
		CHECK(plan.discards == std::vector<std::pair<uint32_t, uint32_t>>{{0, 0}});
		REQUIRE(plan.transfers.size() == 1);
		CHECK(ends(plan.transfers[0]) == std::pair{0u, kEnd});
	}
	SUBCASE("one family never transfers") {
		CHECK(PlanOwnership(std::vector{pass(G, true), pass(G, true)}, G).transfers.empty());
	}
	SUBCASE("a differently shaped overlap across families is a conflict") {
		const OwnershipPlan plan = PlanOwnership(std::vector{pass(G, true, {0, 4, 0, 1}), pass(C, true, {0, 1, 0, 1})}, G);
		CHECK(plan.conflicts == std::vector<std::pair<uint32_t, uint32_t>>{{1, 0}});
		CHECK(plan.transfers.empty());
	}
}
