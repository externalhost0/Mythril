//
// Created by Hayden Rivas on 12/2/25.
//

#include "mythril/CTXBuilder.h"
#include "mythril/CTX.h"
#include "mythril/Objects.h"
#include "mythril/CPUProfiler.h"
#include "mythril/ImGui.h"

#include "SDL3/SDL_events.h"

#include <vector>
#include <filesystem>

#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <imgui_impl_sdl3.h>
#include <ImGuizmo.h>

#include <glm/glm.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtc/packing.hpp>

#include <fastgltf/core.hpp>
#include <fastgltf/types.hpp>
#include <fastgltf/glm_element_traits.hpp>

#define STB_IMAGE_IMPLEMENTATION
#include <random>
#include <stb_image.h>
#include <SDL3/SDL_vulkan.h>

#include "Helpers.h"
#include "HostStructs.h"
#include "Primitives.h"

#include "../SDL3Usage.h"
#include "../SampleUtils.h"

#include <chrono>
#include <thread>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif
#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <cstdio>
#endif

// private memory owned by this process in MB, excludes shared library pages and includes swapped/compressed pages
static float GetMemoryUsageMB() {
	size_t bytes = 0;
#if defined(_WIN32)
	PROCESS_MEMORY_COUNTERS_EX counters{};
	if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) bytes = counters.PrivateUsage;
#elif defined(__APPLE__)
	task_vm_info_data_t info{};
	mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
	if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) bytes = info.phys_footprint;
#elif defined(__linux__)
	if (FILE* file = std::fopen("/proc/self/status", "r")) {
		char line[256];
		while (std::fgets(line, sizeof(line), file)) {
			unsigned long kb = 0;
			if (std::sscanf(line, "RssAnon: %lu kB", &kb) == 1 || std::sscanf(line, "VmSwap: %lu kB", &kb) == 1) bytes += static_cast<size_t>(kb) * 1024;
		}
		std::fclose(file);
	}
#endif
	return static_cast<float>(static_cast<double>(bytes) / (1024.0 * 1024.0));
}

static double GetProcessCPUSeconds() {
#if defined(_WIN32)
	FILETIME creation, exit, kernel, user;
	if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) return 0.0;
	auto toU64 = [](const FILETIME& ft) { return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime; };
	return static_cast<double>(toU64(kernel) + toU64(user)) * 1e-7;
#else
	rusage usage{};
	if (getrusage(RUSAGE_SELF, &usage) != 0) return 0.0;
	auto toSeconds = [](const timeval& tv) { return static_cast<double>(tv.tv_sec) + static_cast<double>(tv.tv_usec) * 1e-6; };
	return toSeconds(usage.ru_utime) + toSeconds(usage.ru_stime);
#endif
}

// process cpu usage in percent of all logical cores (0-100), resampled at most every 500ms
static float GetCPUUsage() {
	using Clock = std::chrono::steady_clock;
	static Clock::time_point lastWall = Clock::now();
	static double lastCPU = GetProcessCPUSeconds();
	static float usage = 0.0f;
	static const unsigned cores = std::max(1u, std::thread::hardware_concurrency());

	const Clock::time_point nowWall = Clock::now();
	const double wallSeconds = std::chrono::duration<double>(nowWall - lastWall).count();
	if (wallSeconds < 0.5) return usage;

	const double nowCPU = GetProcessCPUSeconds();
	usage = static_cast<float>((nowCPU - lastCPU) / (wallSeconds * cores) * 100.0);
	lastWall = nowWall;
	lastCPU = nowCPU;
	return usage;
}

struct Camera {
	glm::vec3 position;
	glm::vec3 forwardVector;
	glm::vec3 upVector;
	float aspectRatio;
	float fov;
	float nearPlane;
	float farPlane;
};
static glm::mat4 calculateViewMatrix(const Camera& camera) {
	return glm::lookAt(camera.position, camera.position + camera.forwardVector, camera.upVector);
}
static glm::mat4 calculateProjectionMatrix(const Camera& camera, bool isReverseDepth = false) {
	return glm::perspective(glm::radians(camera.fov), camera.aspectRatio,
		isReverseDepth ? camera.farPlane : camera.nearPlane,
		isReverseDepth ? camera.nearPlane : camera.farPlane);
}

struct MeshCompiled {
	mythril::BufferHandle vertexBufHandle;
	VkDeviceAddress vertexAddress{};
	uint32_t vertexCount{};
	mythril::BufferHandle indexBufHandle;
	uint32_t indexCount{};
	int materialIndex{};
};

struct MaterialData {
	glm::vec4 baseColorFactor = glm::vec4(1);
	size_t baseColorTextureIndex = -1;
	size_t normalTextureIndex = -1;
	size_t roughnessMetallicTextureIndex = -1;
	bool isTransparent;
};
struct AssetCompiled {
	std::vector<MeshCompiled> meshes;
	std::vector<mythril::TextureHandle> textureHandles;
	std::vector<MaterialData> materials;

	uint32_t getNumOfOpaqueMeshes() const {
		uint32_t n = 0;
		for (const MeshCompiled& mesh : meshes) {
			if (materials[mesh.materialIndex].isTransparent) continue;
			n++;
		}
		return n;
	}
	uint32_t getNumOfTransparentMeshes() const {
		uint32_t n = 0;
		for (const MeshCompiled& mesh : meshes) {
			if (!materials[mesh.materialIndex].isTransparent) continue;
			n++;
		}
		return n;
	}
};

struct TextureData {
	std::unique_ptr<uint8_t[], decltype(&stbi_image_free)> pixels{nullptr, stbi_image_free};
	uint32_t width;
	uint32_t height;
	uint32_t channels;
};
struct PrimitiveData {
	std::vector<GPU::GeneralVertex> vertexData;
	std::vector<uint32_t> indexData;
	int materialIndex;
};
struct MeshData {
	std::vector<PrimitiveData> primitives;
};
struct AssetData {
	std::vector<MeshData> meshData;
	std::vector<TextureData> textureData;
	std::vector<MaterialData> materialData;

	std::vector<MeshData> getOpaqueMeshData() const {
		std::vector<MeshData> outMesh;
		// reserve more than we need
		outMesh.reserve(meshData.size());
		for (const auto& mesh : meshData) {
			std::vector<PrimitiveData> outPrim;
			for (const auto& primitive : mesh.primitives) {
				if (materialData[primitive.materialIndex].isTransparent) continue;
				outPrim.push_back(primitive);
			}
			outMesh.push_back({outPrim});
		}
		return outMesh;
	}
	std::vector<MeshData> getTransparentMeshData() const {
		std::vector<MeshData> outMesh;
		// reserve more than we need
		outMesh.reserve(meshData.size());
		for (const auto& mesh : meshData) {
			std::vector<PrimitiveData> outPrim;
			for (const auto& primitive : mesh.primitives) {
				if (!materialData[primitive.materialIndex].isTransparent) continue;
				outPrim.push_back(primitive);
			}
			outMesh.push_back({outPrim});
		}
		return outMesh;
	}
};

static AssetData loadGLTFAsset(const std::filesystem::path& filepath) {
	// 1. load asset
	fastgltf::Parser parser;
	auto data_result = fastgltf::GltfDataBuffer::FromPath(filepath);
	SAMPLE_ASSERT_MSG(data_result.error() == fastgltf::Error::None, "Data load from path {} failed!", filepath.string().c_str());
	fastgltf::GltfDataBuffer data = std::move(data_result.get());

	constexpr fastgltf::Options options =
			fastgltf::Options::DontRequireValidAssetMember |
			fastgltf::Options::LoadExternalBuffers         |
			fastgltf::Options::LoadExternalImages          |
			fastgltf::Options::AllowDouble;

	auto asset_result = fastgltf::Expected<fastgltf::Asset>(fastgltf::Error::None);
	switch (determineGltfFileType(data)) {
		case fastgltf::GltfType::glTF:
			asset_result = parser.loadGltf(data, filepath.parent_path(), options);
			break;
		case fastgltf::GltfType::GLB:
			asset_result = parser.loadGltfBinary(data, filepath.parent_path(), fastgltf::Options::None);
			break;
		case fastgltf::GltfType::Invalid:
			assert(false);
	}
	SAMPLE_ASSERT_MSG(asset_result.error() == fastgltf::Error::None, "Error when validating {}", filepath.string().c_str());
	fastgltf::Asset gltf = std::move(asset_result.get());

	// 2. process asset

	// iterate materials
	// what images are associated to what meshes
	std::vector<MaterialData> materials = {};
	materials.reserve(gltf.materials.size());
	for (const fastgltf::Material& material : gltf.materials) {
		MaterialData material_data{};

		if (material.pbrData.baseColorTexture) {
			size_t texture_index = material.pbrData.baseColorTexture->textureIndex;
			material_data.baseColorTextureIndex = gltf.textures[texture_index].imageIndex.value();
		} else {
			material_data.baseColorTextureIndex = 0;
		}
		if (material.normalTexture) {
			size_t texture_index = material.normalTexture->textureIndex;
			material_data.normalTextureIndex = gltf.textures[texture_index].imageIndex.value();
		} else {
			material_data.normalTextureIndex = 0;
		}
		if (material.pbrData.metallicRoughnessTexture) {
			size_t texture_index = material.pbrData.metallicRoughnessTexture->textureIndex;
			material_data.roughnessMetallicTextureIndex = gltf.textures[texture_index].imageIndex.value();
		} else {
			material_data.roughnessMetallicTextureIndex = 0;
		}
		auto& base_color = material.pbrData.baseColorFactor;
		material_data.baseColorFactor = {base_color.x(), base_color.y(), base_color.z(), base_color.w()};
		material_data.isTransparent = material.alphaMode == fastgltf::AlphaMode::Blend || material.alphaMode == fastgltf::AlphaMode::Mask;
		materials.push_back(material_data);
	}

	// iterate meshes
	std::vector<MeshData> meshes = {};
	for (const fastgltf::Mesh& mesh : gltf.meshes) {
		std::vector<PrimitiveData> primitives = {};

		for (const fastgltf::Primitive& primitive : mesh.primitives) {
			std::vector<GPU::GeneralVertex> vertices;
			std::vector<uint32_t> indices;

			// initial vertex used by both
			size_t initial_vertex = vertices.size();
			// load indicies
			{
				const fastgltf::Accessor& indexAccessor = gltf.accessors[primitive.indicesAccessor.value()];
				indices.reserve(indices.size() + indexAccessor.count);
				fastgltf::iterateAccessor<uint32_t>(gltf, indexAccessor, [&indices, initial_vertex](const uint32_t idx) {
					indices.push_back(idx + initial_vertex);
				});
			}
			// load vertices
			{
				const fastgltf::Accessor& posAccessor = gltf.accessors[primitive.findAttribute("POSITION")->accessorIndex];
				vertices.resize(vertices.size() + posAccessor.count);

				// initialize vertex values
				// load position
				fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf, posAccessor, [&vertices, initial_vertex](const glm::vec3 v, const size_t index) {
					GPU::GeneralVertex vertex;
					vertex.position = v;
					vertex.uv_x = 0;
					vertex.normal = glm::vec3{ 1, 0, 0 };
					vertex.uv_y = 0;
					vertex.tangent = glm::vec4{ 1.f };
					vertices[initial_vertex + index] = vertex;
				});

				// load normals
				if (auto normals = primitive.findAttribute("NORMAL"); normals != primitive.attributes.end()) {
					fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf, gltf.accessors[normals->accessorIndex], [&vertices, initial_vertex](glm::vec3 v, size_t index) {
						vertices[initial_vertex + index].normal = v;
					});
				}

				// load UVs
				if (auto uv = primitive.findAttribute("TEXCOORD_0"); uv != primitive.attributes.end()) {
					fastgltf::iterateAccessorWithIndex<glm::vec2>(gltf, gltf.accessors[uv->accessorIndex], [&vertices, initial_vertex](glm::vec2 v, size_t index) {
						vertices[initial_vertex + index].uv_x = v.x;
						vertices[initial_vertex + index].uv_y = v.y;
					});
				}

				// load tangents
				if (auto tangents = primitive.findAttribute("TANGENT"); tangents != primitive.attributes.end()) {
					fastgltf::iterateAccessorWithIndex<glm::vec4>(gltf, gltf.accessors[tangents->accessorIndex], [&vertices, initial_vertex](glm::vec4 v, size_t index) {
						vertices[initial_vertex + index].tangent = v;
					});
				}
			}
			int material_index = primitive.materialIndex.has_value() ? static_cast<int>(primitive.materialIndex.value()) : 0;
			// store
			primitives.push_back(PrimitiveData{vertices, indices, material_index});
		}
		// store
		meshes.emplace_back(MeshData{primitives});
	}

	std::vector<TextureData> textures = {};
	textures.reserve(gltf.images.size());
	// the int is just for naming in debug
	for (const fastgltf::Image& image : gltf.images) {
		TextureData texture{};

		// we declare these variables at the top simply because we use it in all paths
		int w, h, nChannels;
		std::visit(fastgltf::visitor {
			[&](auto& args) {
					SAMPLE_ASSERT_MSG(false, "Unsupported image named: '{}' from gltf: '{}'", image.name, filepath.string().c_str());
				},
				[&](const fastgltf::sources::BufferView& view) {
					const fastgltf::BufferView& bufferView = gltf.bufferViews[view.bufferViewIndex];
					const fastgltf::Buffer& buffer = gltf.buffers[bufferView.bufferIndex];
					std::visit(fastgltf::visitor {
							[](auto& arg) {},
							[&](const fastgltf::sources::Vector& vector) {
								unsigned char* image_data = stbi_load_from_memory(
										reinterpret_cast<const stbi_uc*>(vector.bytes.data() + bufferView.byteOffset),
										static_cast<int>(bufferView.byteLength),
										&w, &h, &nChannels, 4);
								SAMPLE_ASSERT_MSG(image_data, "Failed to load data from image named: '{}' from gltf: '{}'", image.name, filepath.string().c_str());
								texture.width = w;
								texture.height = h;
								texture.channels = nChannels;
								texture.pixels.reset(image_data);
							}}, buffer.data);
				},
				// this is the codepath basically most of sponza takes
				[&](const fastgltf::sources::Array& array) {
					unsigned char* image_data = stbi_load_from_memory(
							reinterpret_cast<const stbi_uc*>(array.bytes.data()),
							static_cast<int>(array.bytes.size()),
							&w, &h, &nChannels, 4);
					SAMPLE_ASSERT_MSG(image_data, "Failed to load data from image named: '{}' from gltf: '{}'", image.name, filepath.string().c_str());
					texture.width = w;
					texture.height = h;
					texture.channels = nChannels;
					texture.pixels.reset(image_data);
				},
				[&](const fastgltf::sources::Vector& vector) {
					unsigned char* image_data = stbi_load_from_memory(
							reinterpret_cast<const stbi_uc*>(vector.bytes.data()),
							static_cast<int>(vector.bytes.size()),
							&w, &h, &nChannels, 4);
					SAMPLE_ASSERT_MSG(image_data, "Failed to load data from image named: '{}' from gltf: '{}'", image.name, filepath.string().c_str());
					texture.width = w;
					texture.height = h;
					texture.channels = nChannels;
					texture.pixels.reset(image_data);
				},
				[&](const fastgltf::sources::URI& uri_source) {
					SAMPLE_ASSERT(uri_source.fileByteOffset == 0);
					SAMPLE_ASSERT(uri_source.uri.isLocalPath());

					const std::filesystem::path image_path = uri_source.uri.path();
					unsigned char* image_data = stbi_load(image_path.string().c_str(), &w, &h, &nChannels, 4);
					SAMPLE_ASSERT_MSG(image_data, "Failed to load data from image named: '{}' from gltf: '{}'", image.name, filepath.string().c_str());
					texture.width = w;
					texture.height = h;
					texture.channels = nChannels;
					texture.pixels.reset(image_data);
				}
		}, image.data);
		textures.push_back(std::move(texture));
	}
	return AssetData{meshes, std::move(textures), std::move(materials)};
}

static mythril::Texture loadTexture(mythril::CTX& ctx, const std::filesystem::path& filepath) {
	assert(!filepath.empty());
	assert(std::filesystem::exists(filepath));

	int w, h, nChannels;
	unsigned char* image_data = stbi_load(filepath.string().c_str(), &w, &h, &nChannels, 4);
	assert(image_data);
	mythril::Texture texture = ctx.createTexture({
		.dimension = { static_cast<uint32_t>(w), static_cast<uint32_t>(h) },
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.usage = mythril::TextureUsageBits_Sampled,
		.storage = mythril::StorageType::Device,
		.numMipLevels = sample::CalcNumMipLevels(w, h),
		.initialData = image_data,
		.generateMipmaps = true,
		.debugName = filepath.filename().string().c_str()
	});
	stbi_image_free(image_data);
	return texture;
}

static AssetCompiled compileGLTFAsset(mythril::CTX& ctx, AssetData& data) {
	AssetCompiled asset;
	// resize all vectors that asset owns
	asset.meshes.reserve(data.meshData.size());
	asset.textureHandles.reserve(data.textureData.size());
	// and just transfer the data regarding materials
	asset.materials = data.materialData;

	unsigned int i = 0;
	for (const MeshData& mesh_data : data.meshData) {
		i++;
		unsigned int j = 0;
		for (const PrimitiveData& primitive_data : mesh_data.primitives) {
			j++;
			char vertex_name_buf[64];
			snprintf(vertex_name_buf, sizeof(vertex_name_buf), "Sponza Vertex Buf on Mesh %d Primitive %d", i, j);
			char index_name_buf[64];
			snprintf(index_name_buf, sizeof(index_name_buf), "Sponza Index Buf on Mesh %d Primitive %d", i, j);

			MeshCompiled mesh_compiled;
			mesh_compiled.materialIndex = primitive_data.materialIndex;

			const mythril::BufferHandle mesh_vertex_buffer = ctx.createBuffer({
				.size = sizeof(GPU::GeneralVertex) * primitive_data.vertexData.size(),
				.usage = mythril::BufferUsageBits::BufferUsageBits_Storage,
				.storage = mythril::StorageType::Device,
				.initialData = primitive_data.vertexData.data(),
				.debugName = vertex_name_buf
			}).release();
			mesh_compiled.vertexBufHandle = mesh_vertex_buffer;
			mesh_compiled.vertexAddress = ctx.gpuAddress(mesh_vertex_buffer);
			mesh_compiled.vertexCount = primitive_data.vertexData.size();

			const mythril::BufferHandle mesh_index_buffer = ctx.createBuffer({
				.size = sizeof(uint32_t) * primitive_data.indexData.size(),
				.usage = mythril::BufferUsageBits::BufferUsageBits_Index,
				.storage = mythril::StorageType::Device,
				.initialData = primitive_data.indexData.data(),
				.debugName = index_name_buf
			}).release();
			mesh_compiled.indexBufHandle = mesh_index_buffer;
			mesh_compiled.indexCount = primitive_data.indexData.size();

			asset.meshes.push_back(mesh_compiled);
		}

	}
	i = 0;
	for (TextureData& texture_data : data.textureData) {
		char texture_name_buf[64];
		snprintf(texture_name_buf, sizeof(texture_name_buf), "Sponza Image %d", i);
		i++;

		mythril::TextureHandle texture_handle;
		// our textures need to have mipmaps to prevent the shimmering
		if (texture_data.pixels) {
			texture_handle = ctx.createTexture({
				.dimension = { texture_data.width, texture_data.height },
				.format = VK_FORMAT_R8G8B8A8_UNORM,
				.usage = mythril::TextureUsageBits::TextureUsageBits_Sampled,
				.numMipLevels = sample::CalcNumMipLevels(texture_data.width, texture_data.height),
				.initialData = texture_data.pixels.get(),
				.generateMipmaps = true,
				.debugName = texture_name_buf
			}).release();
			// call stbi_image_free on our data now that its uploaded safely
			texture_data.pixels.reset(nullptr);
		} else {
			texture_handle = ctx.getNullTexture().handle();
		}
		asset.textureHandles.push_back(texture_handle);
	}
	return asset;
}


// one ring slot more than frames in flight, so the slot being rewritten belongs to a frame the CPU already waited on
struct GpuTimeline {
	static constexpr uint32_t kSlots = mythril::kNUM_FRAMES_IN_FLIGHT + 1;
	static constexpr uint32_t kMaxGroups = 64;
	struct Block {
		std::string name;
		mythril::PassDesc::Type type = mythril::PassDesc::Type::Graphics;
		mythril::QueueAffinity queue = mythril::QueueAffinity::Graphics;
		double startMs = 0.0;
		double endMs = 0.0;
	};
	mythril::QueryPool pool;
	std::array<std::vector<mythril::ScheduledPass>, kSlots> schedules;
	std::vector<Block> latest;
	std::vector<Block> averaged;
	uint64_t frame = 0;
	bool showAverage = true;

	uint32_t slot() const { return static_cast<uint32_t>(frame % kSlots); }
	uint32_t firstQuery() const { return slot() * 2 * kMaxGroups; }

	// call between compile and execute, it reads the slot this frame is about to overwrite
	void collect(const mythril::CTX& ctx, std::span<const mythril::ScheduledPass> schedule) {
		std::vector<Block> groups;
		for (const mythril::ScheduledPass& pass : schedules[slot()]) {
			if (pass.culled) continue;
			if (pass.mergeGroup >= groups.size()) groups.resize(pass.mergeGroup + 1);
			Block& block = groups[pass.mergeGroup];
			block.name += block.name.empty() ? pass.name : " + " + pass.name;
			block.type = pass.type;
			block.queue = pass.queue;
		}
		std::vector<Block> fresh;
		std::vector<std::array<uint64_t, 2>> ticks;
		uint64_t origin = UINT64_MAX;
		for (uint32_t g = 0; g < groups.size(); ++g) {
			std::array<uint64_t, 2> pair{};
			if (!ctx.getQueryPoolResults(pool.handle(), firstQuery() + 2 * g, pair)) continue;
			origin = std::min(origin, pair[0]);
			fresh.push_back(std::move(groups[g]));
			ticks.push_back(pair);
		}
		for (size_t i = 0; i < fresh.size(); ++i) {
			fresh[i].startMs = ctx.timestampTicksToNs(ticks[i][0] - origin) * 1e-6;
			fresh[i].endMs = ctx.timestampTicksToNs(ticks[i][1] - origin) * 1e-6;
		}
		if (fresh.empty()) {
			schedules[slot()].assign(schedule.begin(), schedule.end());
			return;
		}
		if (fresh.size() == averaged.size() && std::ranges::equal(fresh, averaged, {}, &Block::name, &Block::name)) {
			constexpr double kAlpha = 0.02;
			for (size_t i = 0; i < fresh.size(); ++i) {
				averaged[i].startMs += (fresh[i].startMs - averaged[i].startMs) * kAlpha;
				averaged[i].endMs += (fresh[i].endMs - averaged[i].endMs) * kAlpha;
			}
		} else {
			averaged = fresh;
		}
		latest = std::move(fresh);
		schedules[slot()].assign(schedule.begin(), schedule.end());
	}
	void advance() { ++frame; }
};

void DrawGpuTimeline(GpuTimeline& timeline) {
	ImGui::SetNextWindowSize({1400, 300}, ImGuiCond_FirstUseEver);
	const std::vector<GpuTimeline::Block>& blocks = timeline.showAverage ? timeline.averaged : timeline.latest;
	if (!ImGui::Begin("GPU Timeline") || blocks.empty()) {
		ImGui::End();
		return;
	}
	double frameMs = 0.0, busyMs = 0.0;
	std::array<bool, mythril::kQueueCount> usedQueue{};
	for (const GpuTimeline::Block& b : blocks) {
		frameMs = std::max(frameMs, b.endMs);
		busyMs += b.endMs - b.startMs;
		usedQueue[static_cast<size_t>(b.queue)] = true;
	}
	ImGui::Text("GPU frame %.3f ms, sum of passes %.3f ms", frameMs, busyMs);
	ImGui::SameLine(0.f, ImGui::GetStyle().ItemSpacing.x * 4.f);
	ImGui::Checkbox("Average (EMA)", &timeline.showAverage);

	// indexed by PassDesc::Type
	constexpr const char* kTypeNames[] = {"Graphics", "Compute", "Transfer", "Ray Tracing"};
	constexpr ImU32 kFill[] = {IM_COL32(147, 196, 125, 255), IM_COL32(255, 217, 102, 255), IM_COL32(183, 183, 183, 255), IM_COL32(224, 102, 102, 255)};

	constexpr const char* kQueueNames[] = {"main", "async compute", "async transfer"};
	const float lineH = ImGui::GetTextLineHeight();
	const float blockH = 3.f * lineH;
	constexpr int kLabelLines = 4;
	const float rowH = 2.f * kLabelLines * lineH + blockH + ImGui::GetStyle().ItemSpacing.y * 2.f;
	float gutter = 0.f;
	for (size_t q = 0; q < mythril::kQueueCount; ++q)
		if (usedQueue[q]) gutter = std::max(gutter, ImGui::CalcTextSize(kQueueNames[q]).x);
	gutter += ImGui::GetStyle().ItemSpacing.x;
	ImDrawList* draw = ImGui::GetWindowDrawList();
	const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);
	const ImU32 border = ImGui::GetColorU32(ImGuiCol_Border, 4.f);

	for (size_t q = 0; q < mythril::kQueueCount; ++q) {
		if (!usedQueue[q]) continue;
		const ImVec2 origin = ImGui::GetCursorScreenPos();
		const float width = std::max(ImGui::GetContentRegionAvail().x - gutter, 1.f);
		const float scale = width / static_cast<float>(std::max(frameMs, 1e-6));
		const float x0 = origin.x + gutter;
		const float top = origin.y + kLabelLines * lineH;
		const float bottom = top + blockH;
		draw->AddText({origin.x, top + (blockH - lineH) * 0.5f}, text, kQueueNames[q]);

		// a label takes the innermost line it does not overlap on, or the least crowded one
		std::array<float, kLabelLines> nameRight;
		std::array<float, kLabelLines> timeRight;
		nameRight.fill(-FLT_MAX);
		timeRight.fill(-FLT_MAX);
		const auto place = [&](std::array<float, kLabelLines>& right, const float center, const float textW) {
			const float left = std::max(std::min(center - textW * 0.5f, x0 + width - textW), x0);
			const auto free = std::ranges::find_if(right, [&](const float r) { return left >= r; });
			const int line = static_cast<int>((free != right.end() ? free : std::ranges::min_element(right)) - right.begin());
			right[line] = left + textW + ImGui::GetStyle().ItemSpacing.x;
			return std::pair{left, line};
		};
		for (const GpuTimeline::Block& b : blocks) {
			if (static_cast<size_t>(b.queue) != q) continue;
			const ImVec2 pMin{x0 + static_cast<float>(b.startMs) * scale, top};
			const ImVec2 pMax{std::max(x0 + static_cast<float>(b.endMs) * scale, pMin.x + 1.f), bottom};
			draw->AddRectFilled(pMin, pMax, kFill[static_cast<size_t>(b.type)]);
			draw->AddRect(pMin, pMax, border);
			const float center = (pMin.x + pMax.x) * 0.5f;

			const float nameW = ImGui::CalcTextSize(b.name.c_str()).x;
			const auto [nameX, nameLine] = place(nameRight, center, nameW);
			draw->AddText({nameX, top - (1.f + nameLine) * lineH}, text, b.name.c_str());

			char timeBuf[32];
			std::snprintf(timeBuf, sizeof(timeBuf), "%.3f ms", b.endMs - b.startMs);
			const float timeW = ImGui::CalcTextSize(timeBuf).x;
			const auto [timeX, timeLine] = place(timeRight, center, timeW);
			draw->AddText({timeX, bottom + timeLine * lineH}, text, timeBuf);

			if (ImGui::IsMouseHoveringRect(pMin, pMax))
				ImGui::SetTooltip("%s\n%s on %s\nstart %.3f ms\nduration %.3f ms", b.name.c_str(), kTypeNames[static_cast<size_t>(b.type)], kQueueNames[q], b.startMs, b.endMs - b.startMs);
		}
		ImGui::Dummy({width + gutter, rowH});
	}
	ImGui::End();
}

void DrawLightingUI(GPU::LightingData& lightingData) {
	if (ImGui::Begin("Lighting")) {
		if (ImGui::CollapsingHeader("Environment Light", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::ColorEdit3("Env Color",  &lightingData.environmentLight.color.x);
			ImGui::SliderFloat("Env Intensity", &lightingData.environmentLight.intensity, 0.0f, 1.0f);
		}

		if (ImGui::CollapsingHeader("Directional Light", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::ColorEdit3("Dir Color",  &lightingData.directionalLights.color.x);
			ImGui::SliderFloat("Dir Intensity", &lightingData.directionalLights.intensity, 0.0f, 10.0f);
			ImGui::DragFloat3("Dir Direction", &lightingData.directionalLights.direction.x, 0.1f);
		}

		if (ImGui::CollapsingHeader("Point Lights", ImGuiTreeNodeFlags_DefaultOpen)) {
			for (int i = 0; i < 4; i++) {
				std::string label = "Point Light " + std::to_string(i);
				if (ImGui::TreeNode(label.c_str())) {
					ImGui::ColorEdit3("Color",     &lightingData.pointLights[i].color.x);
					ImGui::SliderFloat("Intensity", &lightingData.pointLights[i].intensity, 0.0f, 1000.0f);
					ImGui::SliderFloat("Range", &lightingData.pointLights[i].range, 0.1f, 100.f);
					ImGui::DragFloat3("Position",  &lightingData.pointLights[i].position.x, 0.1f);

					ImGui::TreePop();
				}
			}
		}
	}
	ImGui::End();
}



struct ParticleData {
	glm::vec3 position;
	float size;
	glm::vec3 velocity;
	glm::vec3 color;
};
static std::vector<ParticleData> GenerateParticles(
	size_t count,
	glm::vec3 minPos,
	glm::vec3 maxPos,
	float minSize,
	float maxSize,
	glm::vec3 baseColor = glm::vec3(1.0f),
	float hueVariation = 0.f,
	float saturationVariation = 0.f,
	float brightnessVariation = 0.f,
	uint32_t seed = std::random_device{}()
)
{
	std::mt19937 rng(seed);

	std::uniform_real_distribution<float> posX(minPos.x, maxPos.x);
	std::uniform_real_distribution<float> posY(minPos.y, maxPos.y);
	std::uniform_real_distribution<float> posZ(minPos.z, maxPos.z);
	std::uniform_real_distribution<float> sizeDist(minSize, maxSize);

	glm::vec3 baseHSV = Helpers::RGBtoHSV(baseColor);
	std::uniform_real_distribution<float> hueDist(-hueVariation, hueVariation);
	std::uniform_real_distribution<float> satDist(-saturationVariation, saturationVariation);
	std::uniform_real_distribution<float> valDist(-brightnessVariation, brightnessVariation);

	std::uniform_real_distribution<float> velDist(-0.05f, 0.05f);
	std::vector<ParticleData> particles;
	particles.reserve(count);

	for (size_t i = 0; i < count; ++i)
	{
		ParticleData p{};
		p.position = glm::vec3(posX(rng),posY(rng),posZ(rng));
		p.size = sizeDist(rng);

		glm::vec3 particleHSV = baseHSV;
		particleHSV.x = fmod(particleHSV.x + hueDist(rng) + 360.0f, 360.0f);
		particleHSV.y = glm::clamp(particleHSV.y + satDist(rng), 0.0f, 1.0f);
		particleHSV.z = glm::clamp(particleHSV.z + valDist(rng), 0.0f, 1.0f);
		p.color = Helpers::HSVtoRGB(particleHSV);

		p.velocity = glm::vec3(velDist(rng),velDist(rng),velDist(rng));

		particles.emplace_back(p);
	}

	return particles;
}
static void UpdateParticles(std::vector<ParticleData>& particles, float deltaTime, float speed) {
	const float dtSpeed = deltaTime * speed;
	for (auto& p : particles) {
		p.position += p.velocity * dtSpeed;
		// for (int i = 0; i < 3; ++i) {
		// 	if (p.position[i] < -50.0f || p.position[i] > 100.0f)
		// 		p.velocity[i] = -p.velocity[i]; // reflect velocity
		// }
	}
}
static constexpr uint8_t kNumPointLights = 4;
static void UpdatePointLightShadowMatrices(
    const GPU::LightingData& lightingData,
    float modelScale,
    float nearPlane,
    float farPlane,
    glm::mat4 outShadowMatrices[kNumPointLights][6]) {
    const glm::mat4 shadowProj = glm::perspective(glm::radians(90.0f), 1.0f, nearPlane, farPlane);
    const glm::mat4 scaledModel = glm::scale(glm::mat4(1.0f), glm::vec3(modelScale));
    const glm::mat4 projModel = shadowProj;
    static const struct { glm::vec3 target; glm::vec3 up; } faceDirs[6] = {
        { { 1, 0, 0}, {0,-1, 0} }, // +X
        { {-1, 0, 0}, {0,-1, 0} }, // -X
        { { 0,-1, 0}, {0, 0,-1} }, // -Y
        { { 0, 1, 0}, {0, 0, 1} }, // +Y
        { { 0, 0, 1}, {0,-1, 0} }, // +Z
        { { 0, 0,-1}, {0,-1, 0} }  // -Z
    };
    for (int i = 0; i < kNumPointLights; ++i) {
        const glm::vec3& lightPos = lightingData.pointLights[i].position;
        for (int face = 0; face < 6; ++face) {
            outShadowMatrices[i][face] = projModel * glm::lookAt(lightPos, lightPos + faceDirs[face].target, faceDirs[face].up) * scaledModel;
        }
    }
}

int main() {
	static constexpr VkFormat kOffscreenFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
	static const std::filesystem::path kDataDir = std::filesystem::path(MYTH_SAMPLE_NAME).concat("_data/");
	static const std::vector<std::string> slang_searchpaths = {
		"../../include/",
		"../include/",
		(kDataDir / "shaders/").string()
	};
	SDL_Window* sdlWindow = BuildSDLWindow(true);
	const auto initialWindowSize = GetSDLWindowFramebufferSize(sdlWindow);
	{
		Uint32 sdlExtensionCount = 0;
		const char* const* sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&sdlExtensionCount);
		auto ctx = mythril::CTXBuilder{}
		.set_vulkan_cfg({
			.app_name = "Cool App Name",
			.engine_name = "Cool Engine Name",
			.app_version = {0, 0, 1},
			.engine_version = {0, 0, 1},
			.enableValidation = true,
			.deviceExtensionNames = {VK_EXT_MEMORY_BUDGET_EXTENSION_NAME},
		})
		.set_window_surface({
			.instanceExtensions = std::span(sdlExtensions, sdlExtensionCount),
			.createFunc = [sdlWindow](VkInstance instance) {
				VkSurfaceKHR surface;
				SDL_Vulkan_CreateSurface(sdlWindow, instance, nullptr, &surface);
				return surface;
			},
			.destroyFunc = [](VkInstance instance, VkSurfaceKHR surface) {
				SDL_Vulkan_DestroySurface(instance, surface, nullptr);
			}
		})
		.set_slang_cfg({
			.searchpaths = slang_searchpaths
		})
		.with_default_swapchain({
			.width = initialWindowSize.width,
			.height = initialWindowSize.height,
			.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR,
		})
		.with_ImGui({
			.format = kOffscreenFormat,
			.windowInitFunction = [sdlWindow] { ImGui_ImplSDL3_InitForVulkan(sdlWindow); },
			.windowDestroyFunction = [] { ImGui_ImplSDL3_Shutdown(); }
		})
		.build();

		static constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
		constexpr uint32_t shadow_map_size = 4096;
		static constexpr VkFormat kShadowFormat = VK_FORMAT_D16_UNORM;
		// we combine results of a single channel (red) into all in order to have a grayscale image
		constexpr mythril::ComponentMapping red_swizzle = {
			.r = mythril::Swizzle::Swizzle_R,
			.g = mythril::Swizzle::Swizzle_R,
			.b = mythril::Swizzle::Swizzle_R,
			.a = mythril::Swizzle::Swizzle_1
		};
		
		mythril::Sampler repeatSampler = ctx->createSampler({
			.magFilter = mythril::SamplerFilter::Linear,
			.minFilter = mythril::SamplerFilter::Linear,
			.mipMap = mythril::SamplerMipMap::Linear,
			.wrapU = mythril::SamplerWrap::Repeat,
			.wrapV = mythril::SamplerWrap::Repeat,
			.wrapW = mythril::SamplerWrap::Repeat,
			.debugName = "Repeating Linear Mipmap Sampler"
		});

		mythril::Shader shadowShader = ctx->createShader({
			.source = kDataDir / "shaders/DepthOnly.slang",
			.debugName = "Shadow Shader"
		});

		mythril::GraphicsPipeline shadowPipeline = ctx->createGraphicsPipeline({
			.vertexShader = {shadowShader},
			.fragmentShader = {shadowShader},
			.cull = mythril::CullMode::OFF,
			.debugName = "Shadow Graphics Pipeline"
		});

		mythril::GraphicsPipeline depthPrepassPipeline = ctx->createGraphicsPipeline({
			.vertexShader = {shadowShader},
			.fragmentShader = {shadowShader},
			.cull = mythril::CullMode::BACK,
			.multisample = mythril::SampleCount::X4,
			.debugName = "Depth Prepass Pipeline"
		});

		mythril::Shader redDebugShader = ctx->createShader({
			.source = kDataDir / "shaders/RedDebug.slang",
			.debugName = "Red Shader"
		});
		mythril::GraphicsPipeline redDebugPipeline = ctx->createGraphicsPipeline({
			.vertexShader = {redDebugShader},
			.fragmentShader = {redDebugShader},
			.debugName = "Red Graphics Pipeline"
		});

		mythril::Buffer frameDataHandle = ctx->createBuffer({
			.size = sizeof(GPU::FrameData),
			.usage = mythril::BufferUsageBits::BufferUsageBits_Storage,
			.storage = mythril::StorageType::Device,
			.debugName = "frameData Buffer"
		});

		mythril::Sampler clampSampler = ctx->createSampler({
			.magFilter = mythril::SamplerFilter::Linear,
			.minFilter = mythril::SamplerFilter::Linear,
			.wrapU = mythril::SamplerWrap::ClampEdge,
			.wrapV = mythril::SamplerWrap::ClampEdge,
			.debugName = "Clamp Sampler"
		});


		mythril::Shader fullscreenCompositeShader = ctx->createShader({
			.source = kDataDir / "shaders/FullscreenComposite.slang",
			.debugName = "Fullscreen Composite Shader"
		});
		mythril::GraphicsPipeline fullscreenCompositePipeline = ctx->createGraphicsPipeline({
			.vertexShader = {fullscreenCompositeShader},
			.fragmentShader = {fullscreenCompositeShader},
			.cull = mythril::CullMode::OFF,
			.debugName = "Fullscreen Composite Graphics Pipeline"
		});

		const uint16_t brightPixel = glm::packHalf1x16(0);
		const mythril::TextureSpec lumtexspec0 = {
			.dimension = {1, 1},
			.format = VK_FORMAT_R16_SFLOAT,
			.usage = mythril::TextureUsageBits::TextureUsageBits_Sampled | mythril::TextureUsageBits_Storage,
			.storage = mythril::StorageType::Device,
			.components = red_swizzle,
			.initialData = &brightPixel,
			.debugName = "Luminance Adaptation 0"
		};
		mythril::TextureSpec lumtexspec1 = lumtexspec0;
		lumtexspec1.debugName = "Luminance Adaptation 1";
		mythril::Texture adaptedLuminanceTextures[2] = {
			ctx->createTexture(lumtexspec0),
			ctx->createTexture(lumtexspec1)
		};

		// load gltf asset
		AssetData sponzaData = loadGLTFAsset(kDataDir / "meshes/sponza/Sponza.gltf");
		const AssetCompiled sponzaCompiled = compileGLTFAsset(*ctx, sponzaData);

		// load my arrow visualizer
		AssetData arrowData = loadGLTFAsset(kDataDir / "meshes/arrow.glb");
		const AssetCompiled arrowCompiled = compileGLTFAsset(*ctx, arrowData);

		// create an indirect buffer to draw sponza in one call for shadow passes
		// alot of the data here we can throw away once we make our objects
		mythril::Buffer opaqueSponzaVertexBuf;
		mythril::Buffer opaqueSponzaIndexBuf;
		mythril::Buffer opaqueSponzaIndirectBuf;
		uint32_t opaqueDrawCount;
		{
			const std::vector<MeshData>& sponzaOpaqueOnlyData = sponzaData.getOpaqueMeshData();
			// we are going to make two buffers, one with vertex and another for index that we index into at different points
			std::vector<GPU::GeneralVertex> allVertexData;
			std::vector<uint32_t> allIndexData;
			int32_t vertexCursor = 0;
			uint32_t indexCursor  = 0;
			struct IndirectMeshData {
				int32_t vertexOffset;
				uint32_t firstIndex;
				uint32_t indexCount;
			};
			std::vector<IndirectMeshData> indirect_mesh_data;
			for (const MeshData& mesh_data : sponzaOpaqueOnlyData) {
				for (const PrimitiveData& primitive_data : mesh_data.primitives) {
					// fill in data we need when building the VkIndexIndirectCommands
					IndirectMeshData indirect{};
					indirect.vertexOffset = vertexCursor;
					indirect.firstIndex = indexCursor;
					indirect.indexCount = primitive_data.indexData.size();
					// fill in the giant vertex & index data
					allVertexData.insert(allVertexData.end(), primitive_data.vertexData.begin(), primitive_data.vertexData.end());
					allIndexData.insert(allIndexData.end(), primitive_data.indexData.begin(), primitive_data.indexData.end());
					// increment our position throughout each primitive
					vertexCursor += static_cast<int32_t>(primitive_data.vertexData.size());
					indexCursor  += primitive_data.indexData.size();

					indirect_mesh_data.push_back(indirect);
				}
			}
			opaqueDrawCount = indirect_mesh_data.size();
			// once we have contigous data we can put them in a buffer like we normally do
			opaqueSponzaVertexBuf = ctx->createBuffer({
				.size = allVertexData.size() * sizeof(GPU::GeneralVertex),
				.usage = mythril::BufferUsageBits::BufferUsageBits_Storage,
				.storage = mythril::StorageType::Device,
				.initialData = allVertexData.data(),
				.debugName = "Sponza Opaque All Vertex Buf"
			});
			opaqueSponzaIndexBuf = ctx->createBuffer({
				.size = allIndexData.size() * sizeof(uint32_t),
				.usage = mythril::BufferUsageBits::BufferUsageBits_Index,
				.storage = mythril::StorageType::Device,
				.initialData = allIndexData.data(),
				.debugName = "Sponza Opaque All Index Buf"
			});

			// we are not dependant on the buffers we created so the order here doesnt matter
			uint32_t drawId = 0;
			std::vector<VkDrawIndexedIndirectCommand> indirectCmds;
			for (const IndirectMeshData& mesh : indirect_mesh_data) {
				indirectCmds.push_back({
					.indexCount = mesh.indexCount,
					.instanceCount = 1,
					.firstIndex = mesh.firstIndex,
					.vertexOffset = mesh.vertexOffset,
					.firstInstance = drawId
				});
				drawId++;
			}
			// make the indirect buffer object
			opaqueSponzaIndirectBuf = ctx->createBuffer({
				.size = sizeof(VkDrawIndexedIndirectCommand) * indirectCmds.size(),
				.usage = mythril::BufferUsageBits::BufferUsageBits_Indirect,
				.storage = mythril::StorageType::Device,
				.initialData = indirectCmds.data(),
				.debugName = "Sponza Indirect Buf"
			});
		}

		constexpr float kMODELSCALE = 0.05f;
		constexpr float kReverseZFarDepth = 0.f;
		mythril::RenderGraph graph;
		GpuTimeline gpuTimeline{.pool = ctx->createQueryPool({.count = GpuTimeline::kSlots * 2 * GpuTimeline::kMaxGroups, .debugName = "GPU Timeline Queries"})};
		const mythril::GraphTexture rgShadowMap = graph.createTexture({
			.size = mythril::Dimensions{shadow_map_size, shadow_map_size},
			.format = kShadowFormat,
			.debugName = "Shadow Map Texture"
		});
		const mythril::GraphTexture rgMsaaColorTarget = graph.createTexture({
			.size = mythril::RelativeSize{},
			.format = kOffscreenFormat,
			.samples = mythril::SampleCount::X4,
			.debugName = "MSAA Color Texture"
		});
		const mythril::GraphTexture rgMsaaDepthTarget = graph.createTexture({
			.size = mythril::RelativeSize{},
			.format = kDepthFormat,
			.samples = mythril::SampleCount::X4,
			.debugName = "MSAA Depth Texture"
		});
		const mythril::GraphTexture rgOffscreenColorTarget = graph.createTexture({.size = mythril::RelativeSize{}, .format = kOffscreenFormat, .debugName = "Offscreen Color Texture"});
		const mythril::GraphTexture rgDepthTarget = graph.createTexture({.size = mythril::RelativeSize{}, .format = kDepthFormat, .debugName = "Offscreen Depth Texture"});
		const mythril::GraphTexture rgFinalColorTarget = graph.createTexture({.size = mythril::RelativeSize{}, .format = kOffscreenFormat, .debugName = "Final Color Target (Takes in OffscreenColor)"});
		const mythril::GraphTexture rgAdaptedLuminanceTextures[2] = {
			graph.importTexture(adaptedLuminanceTextures[0]),
			graph.importTexture(adaptedLuminanceTextures[1]),
		};

		GPU::LightingData lightingData = {};
		lightingData.directionalLights.color     = {1.0f, 1.0f, 0.97f};
		lightingData.directionalLights.intensity = 8.0f;
		GPU::FrameData frameData{};

		float near = 1.f, far = 450.f;
		float depthBiasConstant = 1.25f, depthBiasSlope = 1.75f;

		glm::mat4 lightSpaceMatrix{};
		float orthoSize = 100.f;

		float distance_from_center = 330.f;

		// not to be chanegd by us, its calculated
		constexpr auto scene_center = glm::vec3(0, 30, 0);

		glm::vec3 eulerAngles = glm::radians(glm::vec3(-40.f, -60.f, 0.f));
		auto sun_quaternion = glm::quat(eulerAngles);


		static constexpr uint32_t pointLightShadowResolution = 512;
		mythril::Texture pointLightShadowTex[kNumPointLights];
		mythril::GraphTexture rgPointLightShadowTex[kNumPointLights];
		for (int i = 0; i < kNumPointLights; i++) {
			char debug_name[128];
			snprintf(debug_name, sizeof(debug_name), "Point Light Shadow Tex %d", i);
			pointLightShadowTex[i] = ctx->createTexture({
				.dimension = {pointLightShadowResolution, pointLightShadowResolution, 1},
				.type = mythril::TextureType::Type_Cube,
				.format = kShadowFormat,
				.usage = mythril::TextureUsageBits_Attachment | mythril::TextureUsageBits_Sampled,
				.debugName = debug_name
			});
			rgPointLightShadowTex[i] = graph.importTexture(pointLightShadowTex[i]);
		}
		mythril::Shader pointshadowShader = ctx->createShader({
			.source = kDataDir / "shaders/PointShadow.slang",
			.debugName = "Point Shadow Map Shader"
		});
		mythril::GraphicsPipeline pointShadowGraphicsPipeline = ctx->createGraphicsPipeline({
			.vertexShader = pointshadowShader,
			.fragmentShader = pointshadowShader,
			.cull = mythril::CullMode::BACK,
			.debugName = "Point Shadow Map Graphics Pipeline"
		});
		// we need position data
		lightingData.pointLights[0] = { {1.0f, 0.8f, 0.7f}, 10.0f, { 10.0f,  3.0f,  2.0f}, 20.f };
		lightingData.pointLights[1] = { {0.7f, 0.8f, 1.0f}, 9.0f, {-30.0f,  2.5f, -1.0f}, 50.f };
		lightingData.pointLights[2] = { {0.7f, 1.0f, 0.7f}, 12.0f, { 40.0f,  4.0f, -3.0f}, 14.f };
		lightingData.pointLights[3] = { {1.0f, 0.0f, 0.1f}, 11.0f, { -40.0f,  1.0f,  4.0f}, 25.f };

		const auto scaledModel = glm::scale(glm::mat4(1.0), glm::vec3(kMODELSCALE));

		// the sizeof 6 matrices is over the 128 min,
		// but also i just want to precalculate the matrices
		// we could push the perspective and model once and than only push the view matrix at a different offset for every face
		glm::mat4 shadowMatrices[kNumPointLights][6];
		float pointlight_nearplane = 0.1f;
		float pointlight_farplane = 60.f;
		UpdatePointLightShadowMatrices(
		lightingData,
		kMODELSCALE,
		pointlight_nearplane,
		pointlight_farplane,
		shadowMatrices);

		mythril::Buffer pointLightShadowMatrixBuf = ctx->createBuffer({
			.size = sizeof(glm::mat4) * kNumPointLights * 6,
			.usage = mythril::BufferUsageBits::BufferUsageBits_Storage,
			.storage = mythril::StorageType::Device,
			.initialData = &shadowMatrices[0][0],
			.debugName = "Point Light Shadow Matrices Buffer"
		});


		GPU::LightingData lastFramelightingData{};

		mythril::Sampler shadowSampler = ctx->createSampler({
			.wrapU = mythril::SamplerWrap::ClampBorder,
			.wrapV = mythril::SamplerWrap::ClampBorder,
			.depthCompareEnabled = true,
			.depthCompareOp = mythril::CompareOp::LessEqual,
			.debugName = "Shadow Sampler Comparison"
		});

		mythril::Shader standardShader = ctx->createShader({
			.source = kDataDir / "shaders/Standard.slang",
			.debugName = "Standard Shader"
		});
		mythril::GraphicsPipeline opaquePipeline = ctx->createGraphicsPipeline({
			.vertexShader = {standardShader},
			.fragmentShader = {standardShader},
			.blend = mythril::BlendingMode::OFF,
			.cull = mythril::CullMode::BACK,
			.multisample = mythril::SampleCount::X4,
			.debugName = "Opaque Graphics Pipeline"
		});
		// same shader as opaque, only blending and culling differ
		mythril::GraphicsPipeline transparentPipeline = ctx->createGraphicsPipeline({
			.vertexShader = {standardShader},
			.fragmentShader = {standardShader},
			.blend = mythril::BlendingMode::ALPHA_BLEND,
			.cull = mythril::CullMode::OFF,
			.multisample = mythril::SampleCount::X4,
			.debugName = "Transparent Graphics Pipeline"
		});
		mythril::Shader skyShader = ctx->createShader({
			.source = kDataDir / "shaders/Sky.slang",
			.debugName = "Sky Shader"
		});
		mythril::GraphicsPipeline skyPipeline = ctx->createGraphicsPipeline({
			.vertexShader   = {skyShader},
			.fragmentShader = {skyShader},
			.blend          = mythril::BlendingMode::OFF,
			.cull           = mythril::CullMode::OFF,
			.multisample    = mythril::SampleCount::X4,
			.debugName      = "Sky Graphics Pipeline"
		});

		mythril::Shader particleShader = ctx->createShader({
			.source = kDataDir / "shaders/Particles.slang",
			.debugName = "Ambient Particle Shader"
		});
		mythril::GraphicsPipeline particle_pipeline = ctx->createGraphicsPipeline({
			.vertexShader = particleShader,
			.fragmentShader = particleShader,
			.blend = mythril::BlendingMode::ALPHA_BLEND,
			.cull = mythril::CullMode::OFF,
			.multisample = mythril::SampleCount::X4,
			.debugName = "Ambient Particle Pipeline"
		});

		auto particles = GenerateParticles(
		5'000,
		glm::vec3(-50.0f),
		glm::vec3( 100.0f),
		0.01f,
		0.15f,
		{66.f/255.f, 60.f/255.f, 50.f/255.f},
		10.f,
		0.1f,
		0.1f
		);

		mythril::Buffer particles_buffer = ctx->createBuffer({
			.size = particles.size() * sizeof(ParticleData),
			.usage = mythril::BufferUsageBits::BufferUsageBits_Storage,
			.storage = mythril::StorageType::Device,
			.initialData = particles.data(),
			.debugName = "Particle Buffer"
		});

		mythril::Buffer particle_vertex_buffer = ctx->createBuffer({
			.size = Primitives::kHexVertices.size() * sizeof(Primitives::Vertex2D),
			.usage = mythril::BufferUsageBits::BufferUsageBits_Storage,
			.storage = mythril::StorageType::Device,
			.initialData = Primitives::kHexVertices.data(),
			.debugName = "Quad Vertex Buffer"
		});
		mythril::Buffer particle_index_buffer = ctx->createBuffer({
			.size = Primitives::kHexIndices.size() * sizeof(uint32_t),
			.usage = mythril::BufferUsageBits::BufferUsageBits_Index,
			.storage = mythril::StorageType::Device,
			.initialData = Primitives::kHexIndices.data(),
			.debugName = "Quad Index Buffer"
		});

		float particle_emission = 0.7f;
		float particle_speed = 4.f;


		constexpr unsigned int kNumColorMips = 7;
		mythril::GraphTexture rgOffscreenColorTexs[kNumColorMips];
		for (int i = 0; i < kNumColorMips; i++) {
			char namebuf[64];
			snprintf(namebuf, sizeof(namebuf), "Color Bloom Tex %i", i);
			rgOffscreenColorTexs[i] = graph.createTexture({
				.size = mythril::RelativeSize{.scale = 1.f / static_cast<float>(2u << i)},
				.format = VK_FORMAT_B10G11R11_UFLOAT_PACK32,
				.debugName = namebuf
			});
		}
		mythril::Shader downsampleComputeShader = ctx->createShader({
			.source = kDataDir / "shaders/Downsample.slang",
			.debugName = "Downsample Compute Shader"
		});
		mythril::ComputePipeline downsampleComputePipeline = ctx->createComputePipeline({
			.shader = downsampleComputeShader.handle(),
			.debugName = "Downsample Compute Pipeline"
		});


		mythril::Shader luminanceConversionShader = ctx->createShader({
			.source = kDataDir /  "shaders/ConversionLuminance.slang",
			.debugName = "Luminance Conversion Compute Shader"
		});
		mythril::ComputePipeline conversionLuminancePipeline = ctx->createComputePipeline({
			.shader = luminanceConversionShader.handle(),
			.debugName = "Luminance Conversion Pipeline"
		});
		const mythril::GraphTexture rgCurrentLuminanceTex = graph.createTexture({
			.size = mythril::Dimensions{1, 1, 1},
			.format = VK_FORMAT_R8_UNORM,
			.components = red_swizzle,
			.debugName = "Luminance Current Texture"
		});

		mythril::Shader upsampleComputeShader = ctx->createShader({
			.source = kDataDir / "shaders/Upsample.slang",
			.debugName = "Upsample Compute Shader"
		});
		mythril::ComputePipeline upsampleComputePipeline = ctx->createComputePipeline({
			.shader = upsampleComputeShader.handle(),
			.debugName = "Upsample Compute Pipeline"
		});
		float filter_radius = 1.f;
		float blend = 1.f;

		mythril::Shader adaptationShader = ctx->createShader({
			.source = kDataDir / "shaders/Adaptation.slang",
			.debugName = "Adaptation Compute Shader"
		});
		mythril::ComputePipeline adaptationComputePipeline = ctx->createComputePipeline({
			.shader = adaptationShader.handle(),
			.debugName = "Adaptation Compute Pipeline"
		});

		
		float adaptationSpeed = 1.5f;
		bool lumFlip = false;
		auto st = std::chrono::steady_clock::now();


		// should be pretty low we crrently dont handle this well
		float bloom_strength = 0.055;
		float exposure_final = 1.f;
		enum class ToneMappingMode : int {
			None = 0,
			Reinhard = 1,
			Uchimira = 2,
			KhronosPBR = 3,
			AgX = 4
		} tone_mapping_mode = ToneMappingMode::KhronosPBR;
		float reinhardMaxWhite = 1.5f;
		float uchimuraP = 1.f;
		float uchimuraA = 1.05f;
		float uchimuraM = 0.1f;
		float uchimuraL = 0.8f;
		float uchimuraC = 3.0f;
		float uchimuraB = 0.0f;
		float khronosStartCompression = 0.15f;
		float khronosDesaturation = 0.15f;
		glm::vec3 agxSlope = {1.f, 1.f, 1.f};
		glm::vec3 agxOffset = {0.f, 0.f, 0.f};
		glm::vec3 agxPower = {1.f, 1.f, 1.f};
		float agxSaturation = 1.f;

		const mythril::GraphBuffer rgFrameData = graph.importBuffer(frameDataHandle);
		const mythril::GraphBuffer rgPointLightShadowMatrices = graph.importBuffer(pointLightShadowMatrixBuf);
		const mythril::GraphBuffer rgOpaqueSponzaVertices = graph.importBuffer(opaqueSponzaVertexBuf);
		const mythril::GraphBuffer rgOpaqueSponzaIndices = graph.importBuffer(opaqueSponzaIndexBuf);
		const mythril::GraphBuffer rgOpaqueSponzaIndirect = graph.importBuffer(opaqueSponzaIndirectBuf);
		const mythril::GraphBuffer rgParticles = graph.importBuffer(particles_buffer);
		const mythril::GraphBuffer rgParticleVertices = graph.importBuffer(particle_vertex_buffer);
		const mythril::GraphBuffer rgParticleIndices = graph.importBuffer(particle_index_buffer);

		struct SponzaDraw {
			const AssetCompiled* asset = nullptr;
			mythril::GraphicsPipelineHandle pipeline;
			bool transparent = false;
			mythril::GraphTexture shadowMap;
			mythril::GraphTexture pointShadowMaps[kNumPointLights];
			GPU::GeometryPushConstants push{};

			static void draw(const SponzaDraw& d, mythril::GraphicsCmd& cmd) {
				cmd.cmdBindGraphicsPipeline(d.pipeline);
				// opaque depth comes from depth_prepass, so only shade the surface that won
				cmd.cmdBindDepthState(d.transparent ? mythril::DepthState{mythril::CompareOp::Greater, true} : mythril::DepthState{mythril::CompareOp::GreaterEqual, false});
				GPU::GeometryPushConstants push = d.push;
				push.shadowTexture = cmd.texture(d.shadowMap).index();
				for (int i = 0; i < kNumPointLights; i++)
					push.pointLightShadowTextures[i] = cmd.texture(d.pointShadowMaps[i]).index();
				// what we call a mesh is really a primitive for fastgltf
				for (const MeshCompiled& mesh : d.asset->meshes) {
					const MaterialData& material = d.asset->materials[mesh.materialIndex];
					if (material.isTransparent != d.transparent) continue;
					push.vba = mesh.vertexAddress;
					push.baseColorTexture = d.asset->textureHandles[material.baseColorTextureIndex].index();
					push.normalTexture = d.asset->textureHandles[material.normalTextureIndex].index();
					push.roughnessMetallicTexture = d.asset->textureHandles[material.roughnessMetallicTextureIndex].index();
					cmd.cmdPushConstants(push);
					cmd.cmdBindIndexBuffer(mesh.indexBufHandle);
					cmd.cmdDrawIndexed(mesh.indexCount);
				}
			}
		};
		glm::mat4 projViewFromCamera;
		const auto setupSponzaDraw = [&](SponzaDraw& d, const mythril::GraphicsPipeline& pipeline, bool transparent) {
			d.asset = &sponzaCompiled;
			d.pipeline = pipeline.handle();
			d.transparent = transparent;
			d.shadowMap = rgShadowMap;
			std::ranges::copy(rgPointLightShadowTex, d.pointShadowMaps);
			d.push = {
				.model = scaledModel,
				.mvp = projViewFromCamera * scaledModel,
				.frame = ctx->gpuAddress(frameDataHandle),
				.tintColor = {1, 1, 1, 1},
				.samplerState = repeatSampler.index(),
				.shadowSampler = shadowSampler.index(),
				.lightSpaceMatrix = lightSpaceMatrix,
				.pointLightFarPlane = pointlight_farplane
			};
		};
		struct DepthOnlyDraw {
			mythril::GraphicsPipelineHandle pipeline;
			float depthBiasConstant = 0.f;
			float depthBiasSlope = 0.f;
			mythril::BufferHandle indexBuffer;
			mythril::Buffer* indirectBuffer = nullptr;
			uint32_t drawCount = 0;
		};
		struct PointShadowPush {
			VkDeviceAddress shadowMVPs;
			VkDeviceAddress vba;
			glm::mat4 model;
			uint32_t nLight;
			glm::vec3 lightPos;
			float farPlane;
		};
		struct BloomChain {
			mythril::ComputePipelineHandle pipeline;
			mythril::GraphTexture source;
			mythril::GraphTexture mips[kNumColorMips];
			uint64_t sampler = 0;
			float filterRadius = 0.f;
		};

		auto buildGraph = [&] {
			mythril::GraphTexture shadowMap = rgShadowMap;
			mythril::GraphTexture pointShadowMaps[kNumPointLights];
			std::ranges::copy(rgPointLightShadowTex, pointShadowMaps);
			mythril::GraphTexture msaaColor = rgMsaaColorTarget;
			mythril::GraphTexture msaaDepth = rgMsaaDepthTarget;
			mythril::GraphTexture offscreenColor = rgOffscreenColorTarget;
			mythril::GraphTexture depthResolve = rgDepthTarget;
			mythril::GraphTexture bloomMips[kNumColorMips];
			std::ranges::copy(rgOffscreenColorTexs, bloomMips);
			mythril::GraphTexture currentLuminance = rgCurrentLuminanceTex;
			mythril::GraphTexture adaptedLuminance[2] = {rgAdaptedLuminanceTextures[0], rgAdaptedLuminanceTextures[1]};
			mythril::GraphTexture finalColor = rgFinalColorTarget;
			mythril::GraphBuffer frameBuffer = rgFrameData;
			mythril::GraphBuffer shadowMatrixBuffer = rgPointLightShadowMatrices;
			mythril::GraphBuffer particleBuffer = rgParticles;

			struct DepthPrepassData {
				DepthOnlyDraw draw;
				struct {
					glm::mat4 mvp;
					VkDeviceAddress vba;
				} push;
			};
			graph.addGraphicsPass<DepthPrepassData>("depth_prepass", [&](mythril::GraphicsPassBuilder& b, DepthPrepassData& d) {
				msaaDepth = b.attachment({
					.texDesc = msaaDepth,
					.clearValue = mythril::ClearValue::depth(kReverseZFarDepth, 0),
					.loadOp = mythril::LoadOp::CLEAR,
					.storeOp = mythril::StoreOp::STORE,
				}).texture;
				b.dependency(rgOpaqueSponzaVertices, mythril::BufferAccess::ShaderRead);
				b.dependency(rgOpaqueSponzaIndices, mythril::BufferAccess::IndexRead);
				b.dependency(rgOpaqueSponzaIndirect, mythril::BufferAccess::IndirectRead);
				d.draw = {.pipeline = depthPrepassPipeline.handle(), .indexBuffer = opaqueSponzaIndexBuf.handle(), .indirectBuffer = &opaqueSponzaIndirectBuf, .drawCount = opaqueDrawCount};
				d.push = {
					.mvp = projViewFromCamera * scaledModel,
					.vba = ctx->gpuAddress(opaqueSponzaVertexBuf)
				};
			}, [](const DepthPrepassData& d, mythril::GraphicsCmd& cmd) {
				cmd.cmdBindGraphicsPipeline(d.draw.pipeline);
				cmd.cmdBindDepthState({mythril::CompareOp::Greater, true});
				cmd.cmdPushConstants(d.push);
				cmd.cmdBindIndexBuffer(d.draw.indexBuffer);
				cmd.cmdDrawIndexedIndirect(*d.draw.indirectBuffer, 0, d.draw.drawCount);
			});

			{
				const glm::vec3 lightDir = glm::normalize(lightingData.directionalLights.direction);
				const glm::vec3 lightPos = scene_center - lightDir * distance_from_center;
				if (far < near) {
					far = near;
				}
				assert(far >= near);
				const glm::mat4 lightView = glm::lookAt(lightPos, scene_center, glm::vec3(0, 1, 0));
				const glm::mat4 lightProj = glm::ortho(-orthoSize, orthoSize, -orthoSize, orthoSize, near, far);
				lightSpaceMatrix = lightProj * lightView;
			}
			struct ShadowMapData {
				DepthOnlyDraw draw;
				struct {
					glm::mat4 mvp;
					VkDeviceAddress vba;
				} push;
			};
			graph.addGraphicsPass<ShadowMapData>("shadow_map", [&](mythril::GraphicsPassBuilder& b, ShadowMapData& d) {
				shadowMap = b.attachment({
					.texDesc = {shadowMap},
					.clearValue = mythril::ClearValue::depth(1.f, 0),
					.loadOp = mythril::LoadOp::CLEAR,
					.storeOp = mythril::StoreOp::STORE
				}).texture;
				b.dependency(rgOpaqueSponzaVertices, mythril::BufferAccess::ShaderRead);
				b.dependency(rgOpaqueSponzaIndices, mythril::BufferAccess::IndexRead);
				b.dependency(rgOpaqueSponzaIndirect, mythril::BufferAccess::IndirectRead);
				d.draw = {
					shadowPipeline.handle(),
					depthBiasConstant,
					depthBiasSlope,
					opaqueSponzaIndexBuf.handle(),
					&opaqueSponzaIndirectBuf,
					opaqueDrawCount
				};
				d.push = {
					.mvp = lightSpaceMatrix * scaledModel,
					.vba = ctx->gpuAddress(opaqueSponzaVertexBuf)
				};
			}, [](const ShadowMapData& d, mythril::GraphicsCmd& cmd) {
				cmd.cmdBindGraphicsPipeline(d.draw.pipeline);
				cmd.cmdBindDepthState({mythril::CompareOp::LessEqual, true});
				cmd.cmdSetDepthBiasEnable(true);
				cmd.cmdSetDepthBias(d.draw.depthBiasConstant, d.draw.depthBiasSlope, 0.f);
				cmd.cmdPushConstants(d.push);
				cmd.cmdBindIndexBuffer(d.draw.indexBuffer);
				cmd.cmdDrawIndexedIndirect(*d.draw.indirectBuffer, 0, d.draw.drawCount);
			});
			graph.addTransferPass("per_frame_scene_uploads", [&](mythril::TransferBuilder& b) {
				frameBuffer = b.update(frameBuffer, frameData);
				shadowMatrixBuffer = b.update(shadowMatrixBuffer, shadowMatrices);
			});
			struct PointShadow {
				DepthOnlyDraw draw;
				PointShadowPush push;
			};
			for (int i = 0; i < kNumPointLights; i++) {
				if (lightingData.pointLights[i].position == lastFramelightingData.pointLights[i].position)
					continue;
				graph.addGraphicsPass<PointShadow>(std::format("pointlight_shadow_{}", i).c_str(), [&](mythril::GraphicsPassBuilder& b, PointShadow& d) {
					pointShadowMaps[i] = b.attachment({
						.texDesc = pointShadowMaps[i],
						.clearValue = mythril::ClearValue::depth(1.f, 1),
						.loadOp = mythril::LoadOp::CLEAR,
						.storeOp = mythril::StoreOp::STORE,
					}).texture;
					b.multiview(6, 0b111111);
					b.dependency(shadowMatrixBuffer, mythril::BufferAccess::ShaderRead);
					b.dependency(rgOpaqueSponzaVertices, mythril::BufferAccess::ShaderRead);
					b.dependency(rgOpaqueSponzaIndices, mythril::BufferAccess::IndexRead);
					b.dependency(rgOpaqueSponzaIndirect, mythril::BufferAccess::IndirectRead);
					d.draw = {.pipeline = pointShadowGraphicsPipeline.handle(), .indexBuffer = opaqueSponzaIndexBuf.handle(), .indirectBuffer = &opaqueSponzaIndirectBuf, .drawCount = opaqueDrawCount};
					d.push = {
						.shadowMVPs = ctx->gpuAddress(pointLightShadowMatrixBuf),
						.vba = ctx->gpuAddress(opaqueSponzaVertexBuf),
						.model = scaledModel,
						.nLight = static_cast<uint32_t>(i),
						.lightPos = lightingData.pointLights[i].position,
						.farPlane = pointlight_farplane,
					};
				}, [](const PointShadow& d, mythril::GraphicsCmd& cmd) {
					cmd.cmdBindGraphicsPipeline(d.draw.pipeline);
					cmd.cmdBindDepthState({mythril::CompareOp::LessEqual, true});
					cmd.cmdPushConstants(d.push);
					cmd.cmdBindIndexBuffer(d.draw.indexBuffer);
					cmd.cmdDrawIndexedIndirect(*d.draw.indirectBuffer, 0, d.draw.drawCount);
				});
			}
			struct Sky {
				mythril::GraphicsPipelineHandle pipeline;
				struct {
					VkDeviceAddress frame;
					glm::vec3 sunDirection;
					float _pad;
				} push;
			};
			graph.addGraphicsPass<Sky>("sky", [&](mythril::GraphicsPassBuilder& b, Sky& d) {
				msaaColor = b.attachment({
					.texDesc   = msaaColor,
					.clearValue = mythril::ClearValue::color(0.f, 0.f, 0.f, 1.f),
					.loadOp    = mythril::LoadOp::CLEAR,
					.storeOp   = mythril::StoreOp::STORE,
				}).texture;
				msaaDepth = b.attachment({
					.texDesc = msaaDepth,
					.loadOp = mythril::LoadOp::LOAD,
					.storeOp = mythril::StoreOp::STORE,
				}).texture;
				b.dependency(frameBuffer, mythril::BufferAccess::ShaderRead);
				d.pipeline = skyPipeline.handle();
				d.push = {.frame = ctx->gpuAddress(frameDataHandle), .sunDirection = sun_quaternion * glm::vec3(0.f, 0.f, -1.f)};
			}, [](const Sky& d, mythril::GraphicsCmd& cmd) {
				cmd.cmdBindGraphicsPipeline(d.pipeline);
				// sky sits at the reverse-Z far plane, so it only shades pixels the prepass left empty
				cmd.cmdBindDepthState({mythril::CompareOp::Equal, false});
				cmd.cmdPushConstants(d.push);
				cmd.cmdDraw(3);
			});
			const auto sponzaPass = [&](const char* name, const mythril::GraphicsPipeline& pipeline, bool transparent) {
				graph.addGraphicsPass<SponzaDraw>(name, [&](mythril::GraphicsPassBuilder& b, SponzaDraw& d) {
					msaaColor = b.attachment({
						.texDesc = msaaColor,
						.loadOp  = mythril::LoadOp::LOAD,
						.storeOp = mythril::StoreOp::STORE,
					}).texture;
					msaaDepth = b.attachment({
						.texDesc = msaaDepth,
						.loadOp = mythril::LoadOp::LOAD,
						.storeOp = mythril::StoreOp::STORE,
					}).texture;
					b.dependency(shadowMap, mythril::TextureAccess::Sampled);
					for (const mythril::GraphTexture tex : pointShadowMaps)
						b.dependency(tex, mythril::TextureAccess::Sampled);
					b.dependency(frameBuffer, mythril::BufferAccess::ShaderRead);
					setupSponzaDraw(d, pipeline, transparent);
				}, &SponzaDraw::draw);
			};
			sponzaPass("geometry_opaque", opaquePipeline, false);
			sponzaPass("geometry_transparent", transparentPipeline, true);
			graph.addTransferPass("per_frame_particle_uploads", [&](mythril::TransferBuilder& b) {
				particleBuffer = b.upload(particleBuffer, particles);
			});
			struct Particles {
				mythril::GraphicsPipelineHandle pipeline;
				mythril::BufferHandle indexBuffer;
				uint32_t instanceCount = 0;
				struct {
					VkDeviceAddress vba;
					VkDeviceAddress bufferAddr;
					VkDeviceAddress frame;
					float brightness;
					float speed;
				} push;
			};
			graph.addGraphicsPass<Particles>("Ambient Particles", [&](mythril::GraphicsPassBuilder& b, Particles& d) {
				const mythril::AttachmentOutput color = b.attachment({
					.texDesc = msaaColor,
					.loadOp = mythril::LoadOp::LOAD,
					.storeOp = mythril::StoreOp::DONT_CARE,
					.resolveTexDesc = offscreenColor
				});
				const mythril::AttachmentOutput depth = b.attachment({
					.texDesc = msaaDepth,
					.loadOp = mythril::LoadOp::LOAD,
					.storeOp = mythril::StoreOp::DONT_CARE,
					.resolveTexDesc = depthResolve
				});
				msaaColor = color.texture;
				offscreenColor = color.resolve;
				msaaDepth = depth.texture;
				depthResolve = depth.resolve;
				b.dependency(particleBuffer, mythril::BufferAccess::ShaderRead);
				b.dependency(frameBuffer, mythril::BufferAccess::ShaderRead);
				b.dependency(rgParticleVertices, mythril::BufferAccess::ShaderRead);
				b.dependency(rgParticleIndices, mythril::BufferAccess::IndexRead);
				d.pipeline = particle_pipeline.handle();
				d.indexBuffer = particle_index_buffer.handle();
				d.instanceCount = static_cast<uint32_t>(particles.size());
				d.push = {
					.vba = ctx->gpuAddress(particle_vertex_buffer),
					.bufferAddr = ctx->gpuAddress(particles_buffer),
					.frame = ctx->gpuAddress(frameDataHandle),
					.brightness = particle_emission,
					.speed = particle_speed
				};
			}, [](const Particles& d, mythril::GraphicsCmd& cmd) {
				cmd.cmdBindGraphicsPipeline(d.pipeline);
				cmd.cmdBindDepthState({mythril::CompareOp::Greater, true});
				cmd.cmdPushConstants(d.push);
				cmd.cmdBindIndexBuffer(d.indexBuffer);
				cmd.cmdDrawIndexed(Primitives::kHexIndices.size(), d.instanceCount);
			});
			const auto setupBloomChain = [&](mythril::ComputePassBuilder& b, BloomChain& d, const mythril::ComputePipeline& pipeline, std::optional<uint32_t> readOnlyMip, std::optional<uint32_t> writeOnlyMip) {
				for (uint32_t i = 0; i < kNumColorMips; i++) {
					const mythril::TextureAccess access = (i == readOnlyMip) ? mythril::TextureAccess::Sampled
					: (i == writeOnlyMip) ? mythril::TextureAccess::StorageWrite : mythril::TextureAccess::StorageReadWrite;
					bloomMips[i] = b.dependency(bloomMips[i], access);
				}
				b.executeCmdsInOrder();
				d.pipeline = pipeline.handle();
				std::ranges::copy(bloomMips, d.mips);
				d.sampler = clampSampler.index();
				d.filterRadius = filter_radius;
			};
			graph.addComputePass<BloomChain>("Downsampling", [&](mythril::ComputePassBuilder& b, BloomChain& d) {
				b.dependency(offscreenColor, mythril::TextureAccess::Sampled);
				setupBloomChain(b, d, downsampleComputePipeline, std::nullopt, kNumColorMips - 1);
				d.source = offscreenColor;
			}, [](const BloomChain& d, mythril::ComputeCmd& cmd) {
				cmd.cmdBindComputePipeline(d.pipeline);
				for (int i = 0; i < kNumColorMips; i++) {
					const mythril::ResolvedTexture out = cmd.texture(d.mips[i]);
					struct PushConstants {
						uint64_t inTex;
						uint64_t outTex;
						uint64_t sampler;
						uint32_t mipLevel;
					} push {
						.inTex = cmd.texture(i == 0 ? d.source : d.mips[i-1]).index(),
						.outTex = out.index(),
						.sampler = d.sampler,
						.mipLevel = static_cast<uint32_t>(i)
					};
					cmd.cmdPushConstants(push);
					static constexpr uint32_t threadNum = 8;
					const mythril::Dimensions texDims = out.dimensions;
					const mythril::Dimensions roundedDims = {
						(texDims.width + threadNum - 1) / threadNum,
						(texDims.height + threadNum - 1) / threadNum,
						1};
					cmd.cmdDispatchThreadGroup(roundedDims);
				}
			});
			struct LuminanceConvert {
				mythril::ComputePipelineHandle pipeline;
				mythril::GraphTexture lowResColor;
				mythril::GraphTexture currentLuminance;
			};
			graph.addComputePass<LuminanceConvert>("convert into current lum", [&](mythril::ComputePassBuilder& b, LuminanceConvert& d) {
				b.dependency(bloomMips[kNumColorMips - 1], mythril::TextureAccess::Sampled);
				currentLuminance = b.dependency(currentLuminance, mythril::TextureAccess::StorageReadWrite);
				d = {conversionLuminancePipeline.handle(), bloomMips[kNumColorMips - 1], currentLuminance};
			}, [](const LuminanceConvert& d, mythril::ComputeCmd& cmd) {
				cmd.cmdBindComputePipeline(d.pipeline);
				const mythril::ResolvedTexture lowResColor = cmd.texture(d.lowResColor);
				const struct PushConstant {
					uint64_t lowResColorTex;
					uint64_t curLumOutTex;
					uint64_t sampler;
				} push {
					.lowResColorTex = lowResColor.index(),
					.curLumOutTex = cmd.texture(d.currentLuminance).index(),
					.sampler = 0
				};
				cmd.cmdPushConstants(push);
				const mythril::Dimensions texDims = lowResColor.dimensions;
				constexpr glm::vec2 threadsPerGroup = {16, 16};
				// uhhhhhhh
				const auto groupX = static_cast<uint32_t>(static_cast<float>(texDims.width + static_cast<uint32_t>(threadsPerGroup.x) - 1u) / threadsPerGroup.x);
				const auto groupY = static_cast<uint32_t>(static_cast<float>(texDims.height + static_cast<uint32_t>(threadsPerGroup.y) - 1u) / threadsPerGroup.y);
				cmd.cmdDispatchThreadGroup({groupX, groupY, 1});
			});
			graph.addComputePass<BloomChain>("Upsampling", [&](mythril::ComputePassBuilder& b, BloomChain& d) {
				setupBloomChain(b, d, upsampleComputePipeline, kNumColorMips - 1, std::nullopt);
			}, [](const BloomChain& d, mythril::ComputeCmd& cmd) {
				cmd.cmdBindComputePipeline(d.pipeline);
				for (int i = kNumColorMips-1; i > 0; i--) {
					const mythril::ResolvedTexture out = cmd.texture(d.mips[i-1]);
					const mythril::Dimensions outdims = out.dimensions;
					struct PushConstants {
						uint64_t inTex;
						uint64_t outTex;
						uint64_t sampler;
						glm::ivec2 outSize;
						float filterRadius;
					} push {
						.inTex = cmd.texture(d.mips[i]).index(),
						.outTex = out.index(),
						.sampler = d.sampler,
						.outSize = {outdims.width, outdims.height},
						.filterRadius = d.filterRadius,
					};
					cmd.cmdPushConstants(push);
					static constexpr uint32_t threadNum = 8;
					const mythril::Dimensions roundedDims = {
						(outdims.width + threadNum - 1) / threadNum,
						(outdims.height + threadNum - 1) / threadNum,
						1};
					cmd.cmdDispatchThreadGroup(roundedDims);
				}
			});
			struct Adaptation {
				mythril::ComputePipelineHandle pipeline;
				mythril::GraphTexture currentLuminance;
				mythril::GraphTexture prevAdapted;
				mythril::GraphTexture nextAdapted;
				float adaptionSpeed = 0.f;
			};
			graph.addComputePass<Adaptation>("adaptation", [&](mythril::ComputePassBuilder& b, Adaptation& d) {
				currentLuminance = b.dependency(currentLuminance, mythril::TextureAccess::StorageReadWrite);
				b.dependency(adaptedLuminance, mythril::TextureAccess::StorageReadWrite);
				const auto ct = std::chrono::steady_clock::now();
				const float dt = std::chrono::duration<float>(ct - st).count();
				st = ct;
				b.async();
				d = {adaptationComputePipeline.handle(), currentLuminance, adaptedLuminance[lumFlip], adaptedLuminance[!lumFlip], adaptationSpeed * dt};
			}, [](const Adaptation& d, mythril::ComputeCmd& cmd) {
				cmd.cmdBindComputePipeline(d.pipeline);
				struct PushConstant {
					uint64_t currentSceneLumTex;
					uint64_t prevAdaptedLumTex;
					uint64_t nextAdaptedLumTex;
					float adaptionSpeed;
				} push {
					.currentSceneLumTex = cmd.texture(d.currentLuminance).index(),
					.prevAdaptedLumTex = cmd.texture(d.prevAdapted).index(),
					.nextAdaptedLumTex = cmd.texture(d.nextAdapted).index(),
					.adaptionSpeed = d.adaptionSpeed
				};
				cmd.cmdPushConstants(push);
				cmd.cmdDispatchThreadGroup({1, 1});
			});
			struct CompositePush {
				uint64_t colorTex; // 32 bytes
				uint64_t avgLuminanceTex;
				uint64_t bloomTex;
				uint64_t sampler;
				float exposure; // 48 bytes
				float bloomStrength;
				int toneMapMode;
				// reinhard
				float maxWhite;
				// uchimira
				float P; // max display brightness
				float a; // contrast
				float m; // linear section start
				float l; // linear section length
				float c; // black tightness
				float b; // pedestal
				// Khronos PBR
				float startCompression; // highlight compression start
				float desaturation;
				// AgX
				glm::vec3 slope;
				glm::vec3 offset;
				glm::vec3 power;
				float saturation;
			};
			struct Composite {
				mythril::GraphicsPipelineHandle pipeline;
				mythril::GraphTexture color;
				mythril::GraphTexture avgLuminance;
				mythril::GraphTexture bloom;
				CompositePush push;
			};
			graph.addGraphicsPass<Composite>("composite", [&](mythril::GraphicsPassBuilder& builder, Composite& d) {
				finalColor = builder.attachment({
					.texDesc = finalColor,
					.clearValue = mythril::ClearValue::color(0.f, 0.f, 0.f, 1.f),
					.loadOp = mythril::LoadOp::CLEAR,
					.storeOp = mythril::StoreOp::STORE
				}).texture;
				builder.dependency(offscreenColor, mythril::TextureAccess::Sampled);
				builder.dependency(bloomMips[0], mythril::TextureAccess::Sampled);
				builder.dependency(adaptedLuminance, mythril::TextureAccess::Sampled);
				d.pipeline = fullscreenCompositePipeline.handle();
				d.color = offscreenColor;
				d.avgLuminance = adaptedLuminance[lumFlip];
				d.bloom = bloomMips[0];
				d.push = {
					.sampler = 0,
					.exposure = exposure_final,
					.bloomStrength = bloom_strength,
					.toneMapMode = static_cast<int>(tone_mapping_mode),
					.maxWhite = reinhardMaxWhite,
					.P = uchimuraP,
					.a = uchimuraA,
					.m = uchimuraM,
					.l = uchimuraL,
					.c = uchimuraC,
					.b = uchimuraB,
					.startCompression = khronosStartCompression,
					.desaturation = khronosDesaturation,
					.slope = agxSlope,
					.offset = agxOffset,
					.power = agxPower,
					.saturation = agxSaturation
				};
			}, [](const Composite& d, mythril::GraphicsCmd& cmd) {
				cmd.cmdBindGraphicsPipeline(d.pipeline);
				CompositePush push = d.push;
				push.colorTex = cmd.texture(d.color).index();
				push.avgLuminanceTex = cmd.texture(d.avgLuminance).index();
				push.bloomTex = cmd.texture(d.bloom).index();
				cmd.cmdPushConstants(push);
				cmd.cmdDraw(3);
			});
			struct ShadowPosDebug {
				const AssetCompiled* arrow = nullptr;
				mythril::GraphicsPipelineHandle pipeline;
				glm::mat4 model;
				VkDeviceAddress frame;
			};
			graph.addGraphicsPass<ShadowPosDebug>("shadow_pos_debug", [&](mythril::GraphicsPassBuilder& b, ShadowPosDebug& d) {
				finalColor = b.attachment({
					.texDesc = finalColor,
					.loadOp = mythril::LoadOp::LOAD,
					.storeOp = mythril::StoreOp::STORE
				}).texture;
				b.dependency(frameBuffer, mythril::BufferAccess::ShaderRead);
				glm::mat4 model = glm::translate(glm::mat4(1.0), scene_center);
				model = model * glm::mat4_cast(sun_quaternion);
				model = glm::rotate(model, 90.f, glm::vec3(1, 1, 1));
				model = glm::rotate(model, 180.f, glm::vec3(0, 1, 0));
				d = {
					.arrow = &arrowCompiled,
					.pipeline = redDebugPipeline.handle(),
					.model = model,
					.frame = ctx->gpuAddress(frameDataHandle)
				};
			}, [](const ShadowPosDebug& d, mythril::GraphicsCmd& cmd) {
				cmd.cmdBindGraphicsPipeline(d.pipeline);
				cmd.cmdBindDepthState({mythril::CompareOp::Less, true});
				for (const auto& mesh : d.arrow->meshes) {
					struct PushConstant {
						glm::mat4 model;
						VkDeviceAddress vba;
						VkDeviceAddress frame;
					} push {
						.model = d.model,
						.vba = mesh.vertexAddress,
						.frame = d.frame
					};
					cmd.cmdPushConstants(push);
					cmd.cmdBindIndexBuffer(mesh.indexBufHandle);
					cmd.cmdDrawIndexed(mesh.indexCount);
				}
			});
			graph.addGraphicsPass("gui", [&](mythril::GraphicsPassBuilder& b, mythril::NoPassData&) {
				finalColor = b.attachment({
					.texDesc = finalColor,
					.loadOp = mythril::LoadOp::LOAD,
					.storeOp = mythril::StoreOp::STORE
				}).texture;
				b.dependency(adaptedLuminance, mythril::TextureAccess::Sampled);
				b.dependency(bloomMips, mythril::TextureAccess::Sampled);
				b.dependency(shadowMap, mythril::TextureAccess::Sampled);
			}, [](const mythril::NoPassData&, mythril::GraphicsCmd& cmd) {
				cmd.cmdDrawImGui();
			});
			graph.addTransferPass("present", [&](mythril::TransferBuilder& b) {
				b.blit(finalColor, graph.backBuffer());
			});
		};
		static bool focused = false;
		SDL_SetWindowRelativeMouseMode(sdlWindow, focused);
		constexpr float kMouseSensitivity = 0.3f;
		constexpr float kBaseCameraSpeed = 8.f;
		constexpr float kShiftCameraSpeed = kBaseCameraSpeed * 3.f;
		glm::vec3 camera_position = {0.f, 5.f, 0.f};

		lightingData.environmentLight.color     = {0.25f, 0.30f, 0.35f};
		lightingData.environmentLight.intensity = 0.2f;

		float dayTime = 0.25f; // 0..1 (0=midnight, 0.25=sunrise, 0.5=noon, 0.75=sunset)
		float daySpeed = 0.02f; // cycles per second (0.02 = 50s day)
		bool  autoDayCycle = false;

		auto applyDayNightCycle = [&](float t) {
			const float sunAngle = -(t - 0.25f) * glm::two_pi<float>();
			sun_quaternion = glm::angleAxis(sunAngle, glm::vec3(1.f, 0.f, 0.f));

			const glm::vec3 sunDir = sun_quaternion * glm::vec3(0.f, 0.f, -1.f);
			const float sunUp = glm::max(0.f, -sunDir.y);
			const float horizonT = glm::pow(1.f - glm::abs(sunDir.y), 6.f) * sunUp;
			constexpr glm::vec3 noonColor = {1.00f, 0.97f, 0.90f};
			constexpr glm::vec3 horizonColor = {1.00f, 0.45f, 0.10f};
			constexpr glm::vec3 moonColor = {0.55f, 0.62f, 0.78f};
			const float moonUp = glm::max(0.f, sunDir.y);
			// const float nightBlend = glm::smoothstep(0.0f, 0.12f, -sunDir.y);

			if (sunUp > 0.001f) {
				lightingData.directionalLights.color     = glm::mix(noonColor, horizonColor, horizonT);
				lightingData.directionalLights.intensity = sunUp * 10.f;
				lightingData.directionalLights.direction = sunDir;
			} else {
				lightingData.directionalLights.color     = moonColor;
				lightingData.directionalLights.intensity = moonUp * 0.35f;
				lightingData.directionalLights.direction = -sunDir;
			}

			constexpr glm::vec3 nightEnv = {0.02f, 0.03f, 0.09f};
			constexpr glm::vec3 dayEnv   = {0.22f, 0.32f, 0.52f};
			lightingData.environmentLight.color     = glm::mix(nightEnv, dayEnv, sunUp);
			lightingData.environmentLight.intensity = glm::mix(0.05f, 0.28f, sunUp);
		};

		auto startTime = std::chrono::steady_clock::now();
		bool quit = false;
		while (!quit) {
			static auto lastTime = std::chrono::steady_clock::now();
			auto now = std::chrono::steady_clock::now();
			const float deltaTime = std::chrono::duration<float>(now - lastTime).count();
			const float elapsedTime = std::chrono::duration<float>(now - startTime).count();
			lastTime = now;

			const auto [width, height] = GetSDLWindowFramebufferSize(sdlWindow);
			if (ctx->isSwapchainDirty()) {
				ctx->recreateSwapchain({
					.width = width,
					.height = height
				});
			}

			static float yaw = 0, pitch = 0;

			SDL_Event e;
			while (SDL_PollEvent(&e)) {
				ImGui_ImplSDL3_ProcessEvent(&e);

				if (e.type == SDL_EVENT_QUIT) quit = true;
				if (e.type == SDL_EVENT_KEY_DOWN) {
					if (e.key.key == SDLK_Q) quit = true;
					if (e.key.key == SDLK_C) SDL_SetWindowRelativeMouseMode(sdlWindow, focused = !focused);
				}
				if (e.type == SDL_EVENT_MOUSE_MOTION) {
					if (focused) {
						float dx = e.motion.xrel;
						float dy = e.motion.yrel;
						yaw   += dx * kMouseSensitivity;
						pitch -= dy * kMouseSensitivity;
						pitch = glm::clamp(pitch, -89.0f, 89.0f);
					}
				}
			}
			glm::vec3 front;
			front.x = cos(glm::radians(yaw)) * cos(glm::radians(pitch));
			front.y = sin(glm::radians(pitch));
			front.z = sin(glm::radians(yaw)) * cos(glm::radians(pitch));
			front = glm::normalize(front);
			glm::vec3 right = glm::normalize(glm::cross(front, glm::vec3(0,1,0)));
			glm::vec3 up = {0, 1, 0};

			const bool* state = SDL_GetKeyboardState(nullptr);

			// for movement
			float camera_speed = kBaseCameraSpeed;
			if (state[SDL_SCANCODE_LSHIFT]) camera_speed = kShiftCameraSpeed;

			if (state[SDL_SCANCODE_W]) camera_position += front * camera_speed * deltaTime;
			if (state[SDL_SCANCODE_S]) camera_position -= front * camera_speed * deltaTime;
			if (state[SDL_SCANCODE_A]) camera_position -= right * camera_speed * deltaTime;
			if (state[SDL_SCANCODE_D]) camera_position += right * camera_speed * deltaTime;
			if (state[SDL_SCANCODE_SPACE]) camera_position += up * camera_speed * deltaTime;
			if (state[SDL_SCANCODE_LCTRL]) camera_position -= up * camera_speed * deltaTime;

			auto windowFramebufferSize = GetSDLWindowFramebufferSize(sdlWindow);
			int windowWidth, windowHeight;
			SDL_GetWindowSize(sdlWindow, &windowWidth, &windowHeight);
			const Camera camera = {
				.position = camera_position,
				.forwardVector = front,
				.upVector = up,
				.aspectRatio = static_cast<float>(windowWidth) / static_cast<float>(windowHeight),
				.fov = 80.f,
				.nearPlane = 0.1f,
				.farPlane = 200.f
			};
			const glm::mat4 reversedCameraProj = calculateProjectionMatrix(camera, true);
			const glm::mat4 cameraView = calculateViewMatrix(camera);

			auto UVToViewRay = [&](const glm::vec2& uv, const glm::mat4& invProj) -> glm::vec3 {
				glm::vec4 ndc;
				ndc.x = uv.x * 2.0f - 1.0f;
				ndc.y = 1.0f - uv.y * 2.0f; // flip Y for vulkan
				ndc.z = 1.0f;              // far plane
				ndc.w = 1.0f;
				const glm::vec4 view = invProj * ndc;
				const glm::vec3 viewPos = glm::vec3(view) / view.w;
				return viewPos / viewPos.z;
			};
			glm::mat4 invProj = glm::inverse(reversedCameraProj);
			glm::vec3 view00 = UVToViewRay({0,0}, invProj);
			glm::vec3 view11 = UVToViewRay({1,1}, invProj);
			glm::vec2 ray00 = { view00.x / view00.z, view00.y / view00.z };
			glm::vec2 ray11 = { view11.x / view11.z, view11.y / view11.z };
			projViewFromCamera = reversedCameraProj * cameraView;
			const GPU::CameraData cameraData = {
				.projView = projViewFromCamera,
				.invProj = invProj,
				.forward = glm::normalize(-glm::vec3(cameraView[0][2], cameraView[1][2], cameraView[2][2])),
				.up = glm::normalize(glm::vec3(cameraView[0][1], cameraView[1][1], cameraView[2][1])),
				.right = glm::normalize(glm::vec3(cameraView[0][0], cameraView[1][0], cameraView[2][0])),
				.position = camera.position,
				.uvToViewA = ray11 - ray00,
				.uvToViewB = ray00,
				.far = camera.farPlane,
				.near = camera.nearPlane
			};

			ImGui_ImplVulkan_NewFrame();
			ImGui_ImplSDL3_NewFrame();
			ImGui::NewFrame();

			ImGui::Begin("shadow_controls");
			ImGui::DragFloat("Near Plane", &near, 0.1f, 100.f);
			ImGui::DragFloat("Far Plane", &far, 0.1f, 1000.f);
			ImGui::DragFloat("Depth Bias Constant", &depthBiasConstant, 0.01f, 0.01f, 10.f, "%.1f");
			ImGui::DragFloat("Depth Bias Slope", &depthBiasSlope, 0.01f,  0.01f, 10.f, "%.1f");
			//		ImGui::DragFloat3("Sun Position", &sun_pos[0], 0.1f, 1000.f);
			ImGui::DragFloat("Ortho Size", &orthoSize, 10.f, 300.f);
			ImGui::DragFloat("Distance From Scene", &distance_from_center);
			//		ImGui::Text("Sun Position: %.1f %.1f %1.f", sun_pos.x, sun_pos.y, sun_pos.z);
			//		ImGui::DragFloat("Scene Radius (Determines Sun Position)", &sceneRadius);
			ImGui::End();

			ImGuiWindowFlags window_flags = focused ? ImGuiWindowFlags_NoMouseInputs : ImGuiWindowFlags_None;
			ImGui::Begin("Previews", nullptr, window_flags);
			ImGui::Image(rgMsaaDepthTarget, {200, 200});
			ImGui::Image(rgShadowMap, {200, 200});
			ImGui::Text("Average Luminance");
			ImGui::Image(adaptedLuminanceTextures[lumFlip], {100, 100});
			// for (int face = 0; face < 6; face++) {
			// ImGui::Image(pointLightShadowTex[0], pointLightShadowTex[0].getView(0, face), {200, 200});
			// }
			ImGui::Text("Progressively Blurred Color Targets");
			for (int i = 0; i < kNumColorMips; i++) {
				mythril::Dimensions dims = {320, 240};
				ImGui::Image(rgOffscreenColorTexs[kNumColorMips-1-i], {static_cast<float>(dims.width), static_cast<float>(dims.height)});
			}
			ImGui::End();

			ImGui::Begin("Lighting", nullptr, window_flags);
			ImGui::DragFloat("Point Light Near", &pointlight_nearplane);
			ImGui::DragFloat("Point Light Far", &pointlight_farplane);
			ImGui::DragFloat("Particle Speed", &particle_speed);
			ImGui::DragFloat("Particle Emission", &particle_emission, 0.1f);
			ImGui::DragFloat("Upsample Blending", &blend, 0.001f);
			ImGui::DragFloat("Filter Radius (texels)", &filter_radius, 0.01f, 0.f, 4.f);
			ImGui::DragFloat("Bloom Strength", &bloom_strength, 0.01f, 0.0f, 100.f);
			ImGui::DragFloat("Adaptation Speed", &adaptationSpeed, 0.1f, 0.1f, 10.f);
			ImGui::DragFloat("Final Exposure", &exposure_final, 0.01f, 0.0001f, 10.f);

			if (ImGui::BeginMenu("Tone Mapping Modes")) {
				if (ImGui::MenuItem("None")) tone_mapping_mode = ToneMappingMode::None;
				if (ImGui::MenuItem("Reinhard")) tone_mapping_mode = ToneMappingMode::Reinhard;
				if (ImGui::MenuItem("Uchimura")) tone_mapping_mode = ToneMappingMode::Uchimira;
				if (ImGui::MenuItem("KhronosPBR")) tone_mapping_mode = ToneMappingMode::KhronosPBR;
				if (ImGui::MenuItem("AgX")) tone_mapping_mode = ToneMappingMode::AgX;
				ImGui::EndMenu();
			}

			switch (tone_mapping_mode) {
				case ToneMappingMode::None: {
					ImGui::Text("None Options");
				} break;
				case ToneMappingMode::Reinhard: {
					ImGui::Text("Reinhard Options");
					ImGui::DragFloat("Max White", &reinhardMaxWhite, 0.01f, 0.0001f, 10.f);
				} break;
				case ToneMappingMode::Uchimira: {
					ImGui::Text("Uchimura Options");
					ImGui::DragFloat("Max Display Brightness", &uchimuraP, 0.01f, 0.0001f, 10.f);
					ImGui::DragFloat("Contrast", &uchimuraA, 0.01f, 0.0001f, 10.f);
					ImGui::DragFloat("Linear Section Start", &uchimuraM, 0.01f, 0.0001f, 10.f);
					ImGui::DragFloat("Linear Section Length", &uchimuraL, 0.01f, 0.0001f, 10.f);
					ImGui::DragFloat("Black Tighness", &uchimuraC, 0.01f, 0.0001f, 10.f);
					ImGui::DragFloat("Pedestal", &uchimuraB, 0.01f, 0.0001f, 10.f);
				} break;
				case ToneMappingMode::KhronosPBR: {
					ImGui::Text("KhronosPBR Options");
					ImGui::DragFloat("Highlight Compression Start", &khronosStartCompression, 0.01f, 0.04f, 0.8f);
					ImGui::DragFloat("Desaturation", &khronosDesaturation, 0.01f, 0.001f, 1.f);
				} break;
				case ToneMappingMode::AgX: {
					ImGui::Text("AgX Options");
					ImGui::SliderFloat3("Slope", &agxSlope[0], 0.f, 1.f);
					ImGui::SliderFloat3("Offset", &agxOffset[0], 0.f, 1.f);
					ImGui::SliderFloat3("Power", &agxPower[0], 0.f, 1.f);
					ImGui::DragFloat("Saturation", &agxSaturation, 0.1f, 0.1f, 10.f);
				} break;
			}

			ImGui::Separator();
			ImGui::Text("Day/Night Cycle");
			ImGui::Checkbox("Auto Cycle", &autoDayCycle);
			ImGui::SliderFloat("Day Time", &dayTime, 0.f, 1.f);
			if (!autoDayCycle && ImGui::Button("Apply Day Time")) applyDayNightCycle(dayTime);
			ImGui::DragFloat("Day Speed (cycles/s)", &daySpeed, 0.001f, 0.0005f, 2.f);
			ImGui::End();
			DrawLightingUI(lightingData);
			ImGui::Begin("Information");
			{
				ImGui::Text("Frame %d", ImGui::GetFrameCount());
				ImGui::Text("Framerate: %.0f", ImGui::GetIO().Framerate);
				static float smoothedDeltaTime = 0.0f;
				float alpha = 0.05f; // lower = smoother, range from 0.0-1.0
				smoothedDeltaTime = smoothedDeltaTime * (1.0f - alpha) + deltaTime * alpha;
				ImGui::Text("CPU EMA Frametime: %.2f ms", smoothedDeltaTime * 1000.0f);
				ImGui::Text("CPU Usage: %.1f%%", GetCPUUsage());
				ImGui::Text("Memory Usage: %.1f MB", GetMemoryUsageMB());
#if defined(__APPLE__)
				static const char* title = "GPU Allocations";
#else
				static const char* title = "GPU Memory Usage";
#endif
				ImGui::Text("%s: %.1f MB", title, static_cast<double>(ctx->getGPUMemoryUsage()) / (1024.0 * 1024.0));
				ImGui::Text("ImGui Backend: %s", ImGui::GetIO().BackendRendererName);
				ImGui::Text("Display Size: %.0fx%.0f", ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y);
				ImGui::Text("Display Framebuffer Scale: %.0fx%.0f", ImGui::GetIO().DisplayFramebufferScale.x, ImGui::GetIO().DisplayFramebufferScale.y);
				ImGui::Text("Framebuffer Size: %dx%d", windowFramebufferSize.width, windowFramebufferSize.height);
			}
			ImGui::End();
			ImGui::SetNextWindowPos({660, 60}, ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowSize({420, 420}, ImGuiCond_FirstUseEver);
			ImGui::Begin("Render Graph");
			if (ImGui::BeginTable("schedule", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit)) {
				ImGui::TableSetupColumn("Pass");
				ImGui::TableSetupColumn("Queue");
				ImGui::TableSetupColumn("Batch");
				ImGui::TableSetupColumn("Group");
				ImGui::TableSetupColumn("Culled");
				ImGui::TableHeadersRow();
				for (const mythril::ScheduledPass& pass : graph.schedule()) {
					constexpr const char* kQueueNames[] = {"main", "async compute", "async transfer"};
					ImGui::TableNextRow();
					ImGui::TableNextColumn(); ImGui::TextUnformatted(pass.name.c_str());
					ImGui::TableNextColumn(); ImGui::TextUnformatted(kQueueNames[static_cast<int>(pass.queue)]);
					ImGui::TableNextColumn(); pass.culled ? ImGui::TextUnformatted("-") : ImGui::Text("%u", pass.batch);
					ImGui::TableNextColumn(); pass.culled ? ImGui::TextUnformatted("-") : ImGui::Text("%u", pass.mergeGroup);
					ImGui::TableNextColumn(); ImGui::TextUnformatted(pass.culled ? "yes" : "");
				}
				ImGui::EndTable();
			}
			ImGui::End();
			DrawGpuTimeline(gpuTimeline);

			ImGuizmo::BeginFrame();
			glm::mat4 transformMatrix = glm::translate(glm::mat4(1.f), scene_center) *
										glm::mat4_cast(sun_quaternion);


			ImGuizmo::SetOrthographic(false);
			ImGuizmo::SetDrawlist(ImGui::GetBackgroundDrawList());
			ImGuizmo::SetRect(0, 0, static_cast<float>(windowWidth), static_cast<float>(windowHeight));
			ImGuizmo::Manipulate(
					glm::value_ptr(cameraView),
					glm::value_ptr(calculateProjectionMatrix(camera, false)),
					ImGuizmo::OPERATION::ROTATE,
					ImGuizmo::MODE::WORLD,
					glm::value_ptr(transformMatrix),
					nullptr
					);
			if (ImGuizmo::IsUsing()) {
				glm::vec3 position;
				glm::quat rotation;
				glm::vec3 scale;
				glm::vec3 skew;        // useless
				glm::vec4 perspective; // useless
				glm::decompose(transformMatrix, scale, rotation, position, skew, perspective);
				sun_quaternion = glm::normalize(rotation);
			}

			if (autoDayCycle) {
				dayTime = fmod(dayTime + deltaTime * daySpeed, 1.f);
				applyDayNightCycle(dayTime);
			} else {
				lightingData.directionalLights.direction = sun_quaternion * glm::vec3(0, 0, -1);
			}
			frameData = GPU::FrameData {
				.camera = cameraData,
				.lighting = lightingData,
				.time = elapsedTime,
				.deltaTime = deltaTime
			};

			MYTH_PROFILER_ZONE("updating buffers cpu");
			UpdateParticles(particles, deltaTime, particle_speed);
			UpdatePointLightShadowMatrices(frameData.lighting, kMODELSCALE, pointlight_nearplane, pointlight_farplane, shadowMatrices);
			MYTH_PROFILER_ZONE_END();
			{
				MYTH_PROFILER_ZONE_COLOR("Execution", MYTH_PROFILER_COLOR_RENDERPASS);
				graph.reset();
				buildGraph();
				graph.compile(*ctx);
				gpuTimeline.collect(*ctx, graph.schedule());
				graph.execute(*ctx, gpuTimeline.pool.handle(), gpuTimeline.firstQuery());
				gpuTimeline.advance();
				MYTH_PROFILER_ZONE_END();
			}
			lastFramelightingData = lightingData;
			lumFlip = !lumFlip;
		}
	}
	DestroySDLWindow(sdlWindow);
	return 0;
}
