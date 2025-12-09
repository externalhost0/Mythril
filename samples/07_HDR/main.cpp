//
// Created by Hayden Rivas on 12/2/25.
//

#include "mythril/CTXBuilder.h"

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


#include <fastgltf/core.hpp>
#include <fastgltf/types.hpp>
#include <fastgltf/glm_element_traits.hpp>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include "HostStructs.h"

struct Camera {
	glm::vec3 position;
	glm::vec3 forwardVector;
	glm::vec3 upVector;
	float aspectRatio;
	float fov;
	float nearPlane;
	float farPlane;
};
static glm::mat4 calculateViewMatrix(Camera camera) {
	return glm::lookAt(camera.position, camera.position + camera.forwardVector, camera.upVector);
}
static glm::mat4 calculateProjectionMatrix(Camera camera) {
	return glm::perspective(glm::radians(camera.fov), camera.aspectRatio, camera.nearPlane, camera.farPlane);
}

struct MeshCompiled {
	mythril::InternalBufferHandle vertexBufHandle;
	uint32_t vertexCount;
	mythril::InternalBufferHandle indexBufHandle;
	uint32_t indexCount;
	int materialIndex;
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
	std::vector<mythril::InternalTextureHandle> textureHandles;
	std::vector<MaterialData> materials;
};

struct TextureData {
	std::unique_ptr<uint8_t[], decltype(&stbi_image_free)> pixels{nullptr, stbi_image_free};
	uint32_t width;
	uint32_t height;
	uint32_t channels;
};
struct PrimitiveData {
	std::vector<GPU::Vertex> vertexData;
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
};

static AssetData loadGLTFAsset(const std::filesystem::path& filepath) {
	// 1. load asset
	fastgltf::Parser parser;
	auto data_result = fastgltf::GltfDataBuffer::FromPath(filepath);
	ASSERT_MSG(data_result.error() == fastgltf::Error::None, "Data load from path {} failed!", filepath.c_str());
	fastgltf::GltfDataBuffer data = std::move(data_result.get());

	constexpr fastgltf::Options options =
			fastgltf::Options::DontRequireValidAssetMember |
			fastgltf::Options::LoadExternalBuffers         |
			fastgltf::Options::LoadExternalImages          |
			fastgltf::Options::AllowDouble;

	fastgltf::Expected<fastgltf::Asset> asset_result = fastgltf::Expected<fastgltf::Asset>(fastgltf::Error::None);
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
	ASSERT_MSG(asset_result.error() == fastgltf::Error::None, "Error when validating {}", filepath.c_str());
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
			std::vector<GPU::Vertex> vertices;
			std::vector<uint32_t> indices;

			// initial vertex used by both
			size_t initial_vertex = vertices.size();
			// load indicies
			{
				const fastgltf::Accessor& indexAccessor = gltf.accessors[primitive.indicesAccessor.value()];
				indices.reserve(indices.size() + indexAccessor.count);
				fastgltf::iterateAccessor<uint32_t>(gltf, indexAccessor, [&](uint32_t idx) {
					indices.push_back(idx + initial_vertex);
				});
			}
			// load vertices
			{
				const fastgltf::Accessor& posAccessor = gltf.accessors[primitive.findAttribute("POSITION")->accessorIndex];
				vertices.resize(vertices.size() + posAccessor.count);

				// initialize vertex values
				// load position
				fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf, posAccessor, [&](glm::vec3 v, size_t index) {
					GPU::Vertex vertex = {};
					vertex.position = v;
					vertex.uv_x = 0;
					vertex.normal = glm::vec3{ 1, 0, 0 };
					vertex.uv_y = 0;
					vertex.tangent = glm::vec4{ 1.f };
					vertices[initial_vertex + index] = vertex;
				});

				// load normals
				auto normals = primitive.findAttribute("NORMAL");
				if (normals != primitive.attributes.end()) {
					fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf, gltf.accessors[(*normals).accessorIndex], [&](glm::vec3 v, size_t index) {
						vertices[initial_vertex + index].normal = v;
					});
				}

				// load UVs
				auto uv = primitive.findAttribute("TEXCOORD_0");
				if (uv != primitive.attributes.end()) {
					fastgltf::iterateAccessorWithIndex<glm::vec2>(gltf, gltf.accessors[(*uv).accessorIndex], [&](glm::vec2 v, size_t index) {
						vertices[initial_vertex + index].uv_x = v.x;
						vertices[initial_vertex + index].uv_y = v.y;
					});
				}

				// load tangents
				auto tangents = primitive.findAttribute("TANGENT");
				if (tangents != primitive.attributes.end()) {
					fastgltf::iterateAccessorWithIndex<glm::vec4>(gltf, gltf.accessors[(*tangents).accessorIndex], [&](glm::vec4 v, size_t index) {
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
//					ASSERT_MSG(false, "Unsupported image named: '{}' from gltf: '{}'", image.name, filepath.c_str());
				},
				[&](const fastgltf::sources::BufferView& view) {
					const fastgltf::BufferView& bufferView = gltf.bufferViews[view.bufferViewIndex];
					const fastgltf::Buffer& buffer = gltf.buffers[bufferView.bufferIndex];
					std::visit(fastgltf::visitor {
							[](auto& arg) {},
							[&](fastgltf::sources::Vector& vector) {
								unsigned char* data = stbi_load_from_memory(
										reinterpret_cast<const stbi_uc*>(vector.bytes.data() + bufferView.byteOffset),
										static_cast<int>(bufferView.byteLength),
										&w, &h, &nChannels, 4);
								ASSERT_MSG(data, "Failed to load data from image named: '{}' from gltf: '{}'", image.name, filepath.c_str());
								texture.width = w;
								texture.height = h;
								texture.channels = nChannels;
								texture.pixels.reset(data);
							}}, buffer.data);
				},
				// this is the codepath basically most of sponza takes
				[&](const fastgltf::sources::Array& array) {
					unsigned char* data = stbi_load_from_memory(
							reinterpret_cast<const stbi_uc*>(array.bytes.data()),
							static_cast<int>(array.bytes.size()),
							&w, &h, &nChannels, 4);
					ASSERT_MSG(data, "Failed to load data from image named: '{}' from gltf: '{}'", image.name, filepath.c_str());
					texture.width = w;
					texture.height = h;
					texture.channels = nChannels;
					texture.pixels.reset(data);
				},
				[&](const fastgltf::sources::Vector& vector) {
					unsigned char* data = stbi_load_from_memory(
							reinterpret_cast<const stbi_uc*>(vector.bytes.data()),
							static_cast<int>(vector.bytes.size()),
							&w, &h, &nChannels, 4);
					ASSERT_MSG(data, "Failed to load data from image named: '{}' from gltf: '{}'", image.name, filepath.c_str());
					texture.width = w;
					texture.height = h;
					texture.channels = nChannels;
					texture.pixels.reset(data);
				},
				[&](const fastgltf::sources::URI& uri_source) {
					ASSERT(uri_source.fileByteOffset == 0);
					ASSERT(uri_source.uri.isLocalPath());

					std::filesystem::path image_path = uri_source.uri.path();
					unsigned char* data = stbi_load(image_path.c_str(), &w, &h, &nChannels, 4);
					ASSERT_MSG(data, "Failed to load data from image named: '{}' from gltf: '{}'", image.name, filepath.c_str());
					texture.width = w;
					texture.height = h;
					texture.channels = nChannels;
					texture.pixels.reset(data);
				}
		}, image.data);
		textures.push_back(std::move(texture));
	}
	return AssetData{meshes, std::move(textures), std::move(materials)};
}

static AssetCompiled compileGLTFAsset(mythril::CTX& ctx, AssetData& data) {
	AssetCompiled asset;
	// resize all vectors that asset owns
	asset.meshes.reserve(data.meshData.size());
	asset.textureHandles.reserve(data.textureData.size());
	// and just transfer the data regarding materials
	asset.materials = std::move(data.materialData);

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

			mythril::InternalBufferHandle mesh_vertex_buffer = ctx.createBuffer({
				.size = sizeof(GPU::Vertex) * primitive_data.vertexData.size(),
				.usage = mythril::BufferUsageBits::BufferUsageBits_Storage,
				.storage = mythril::StorageType::Device,
				.initialData = primitive_data.vertexData.data(),
				.debugName = vertex_name_buf
			});
			mesh_compiled.vertexBufHandle = mesh_vertex_buffer;
			mesh_compiled.vertexCount = primitive_data.vertexData.size();

			mythril::InternalBufferHandle mesh_index_buffer = ctx.createBuffer({
				.size = sizeof(uint32_t) * primitive_data.indexData.size(),
				.usage = mythril::BufferUsageBits::BufferUsageBits_Index,
				.storage = mythril::StorageType::Device,
				.initialData = primitive_data.indexData.data(),
				.debugName = index_name_buf
			});
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

		mythril::InternalTextureHandle texture_handle;
		if (texture_data.pixels) {
			texture_handle = ctx.createTexture({
				.dimension = { texture_data.width, texture_data.height },
				.usage = mythril::TextureUsageBits::TextureUsageBits_Sampled,
				.format = VK_FORMAT_R8G8B8A8_UNORM,
				.initialData = texture_data.pixels.get(),
				.debugName = texture_name_buf
			});
			// call stbi_image_free on our data now that its uploaded safely
			texture_data.pixels.reset(nullptr);
		} else {
			texture_handle = ctx.getNullTexture();
		}
		asset.textureHandles.push_back(texture_handle);
	}
	return asset;
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



const std::vector<GPU::Vertex> cubeVertices = {
		// front face
		{{-1.f, -1.f, -1.f}}, // A 0
		{{1.f,  -1.f, -1.f}}, // B 1
		{{1.f,  1.f,  -1.f}}, // C 2
		{{-1.f, 1.f,  -1.f}}, // D 3

		// back face
		{{-1.f, -1.f, 1.f}}, // E 4
		{{1.f,  -1.f, 1.f}}, // F 5
		{{1.f,  1.f,  1.f}}, // G 6
		{{-1.f, 1.f,  1.f}},  // H 7

		// left face
		{{-1.f, 1.f,  -1.f}}, // D 8
		{{-1.f, -1.f, -1.f}}, // A 9
		{{-1.f, -1.f, 1.f}}, // E 10
		{{-1.f, 1.f,  1.f}}, // H 11

		// right face
		{{1.f,  -1.f, -1.f}}, // B 12
		{{1.f,  1.f,  -1.f}}, // C 13
		{{1.f,  1.f,  1.f}}, // G 14
		{{1.f,  -1.f, 1.f}}, // F 15

		// bottom face
		{{-1.f, -1.f, -1.f}}, // A 16
		{{1.f,  -1.f, -1.f}}, // B 17
		{{1.f,  -1.f, 1.f}}, // F 18
		{{-1.f, -1.f, 1.f}}, // E 19

		// top face
		{{1.f,  1.f,  -1.f}}, // C 20
		{{-1.f, 1.f,  -1.f}}, // D 21
		{{-1.f, 1.f,  1.f}}, // H 22
		{{1.f,  1.f,  1.f}}, // G 23
};
const std::vector<uint32_t> cubeIndices = {
		// front and back
		0, 3, 2,
		2, 1, 0,
		4, 5, 6,
		6, 7, 4,
		// left and right
		11, 8, 9,
		9, 10, 11,
		12, 13, 14,
		14, 15, 12,
		// bottom and top
		16, 17, 18,
		18, 19, 16,
		20, 21, 22,
		22, 23, 20
};

int main() {
	auto ctx = mythril::CTXBuilder{}
	.set_info_spec({
		.app_name = "Cool App Name",
		.engine_name = "Cool Engine Name"
	})
	.set_window_spec({
		.title = "Cool Window Name",
		.mode = mythril::WindowMode::Windowed,
		.width = 1280,
		.height = 720,
		.resizeable = true
	})
	.set_shader_search_paths({
		"../../include/"
	})
	.with_ImGui({
		.format = VK_FORMAT_R16G16B16A16_SFLOAT
	})
	.build();

	VkExtent2D extent2D = ctx->getWindow().getFramebufferSize();
	mythril::InternalTextureHandle colorTarget = ctx->createTexture({
		.dimension = extent2D,
		.samples = mythril::SampleCount::X1,
		.usage = mythril::TextureUsageBits::TextureUsageBits_Attachment | mythril::TextureUsageBits_Sampled,
		.storage = mythril::StorageType::Device,
		.format = VK_FORMAT_R16G16B16A16_SFLOAT,
		.debugName = "Color Texture"
	});
	mythril::InternalTextureHandle msaaColorTarget = ctx->createTexture({
		.dimension = extent2D,
		.samples = mythril::SampleCount::X4,
		.usage = mythril::TextureUsageBits::TextureUsageBits_Attachment,
		.storage = mythril::StorageType::Memoryless,
		.format = VK_FORMAT_R16G16B16A16_SFLOAT,
		.debugName = "MSAA Color Texture"
	});
	mythril::InternalTextureHandle depthTarget = ctx->createTexture({
		.dimension = extent2D,
		.samples = mythril::SampleCount::X1,
		.usage = mythril::TextureUsageBits::TextureUsageBits_Attachment,
		.format = VK_FORMAT_D32_SFLOAT_S8_UINT,
		.debugName = "Depth Texture"
	});


	constexpr uint32_t shadow_map_size = 4096;
	mythril::InternalTextureHandle shadowMap = ctx->createTexture({
		.dimension = {shadow_map_size, shadow_map_size},
		.samples = mythril::SampleCount::X1,
		.usage = mythril::TextureUsageBits::TextureUsageBits_Attachment | mythril::TextureUsageBits::TextureUsageBits_Sampled,
		.format = VK_FORMAT_D16_UNORM,
		.debugName = "Shadow Map Texture"
	});
	mythril::InternalSamplerHandle shadowSampler = ctx->createSampler({
		.magFilter = mythril::SamplerFilter::Nearest,
		.minFilter = mythril::SamplerFilter::Nearest,
		.mipMap = mythril::SamplerMipMap::Linear,
		.wrapU = mythril::SamplerWrap::Clamp,
		.wrapV = mythril::SamplerWrap::Clamp,
		.compareEnabled = true,
		.compareOp = mythril::CompareOp::LessEqual,
		.debugName = "Shadow Sampler"
	});

	mythril::InternalShaderHandle standardShader = ctx->createShader({
		.filePath = "Standard.slang",
		.debugName = "Standard Shader"
	});

	mythril::InternalGraphicsPipelineHandle opaquePipeline = ctx->createGraphicsPipeline({
		.vertexShader = {standardShader},
		.fragmentShader = {standardShader},
		.blend = mythril::BlendingMode::OFF,
		.cull = mythril::CullMode::BACK,
		.debugName = "Opaque Graphics Pipeline"
	});
	mythril::InternalGraphicsPipelineHandle transparentPipeline = ctx->createGraphicsPipeline({
		.vertexShader = {standardShader},
		.fragmentShader = {standardShader},
		.blend = mythril::BlendingMode::ALPHA_BLEND,
		.cull = mythril::CullMode::OFF,
		.debugName = "Transparent Graphics Pipeline"
	});

	mythril::InternalSamplerHandle repeatSampler = ctx->createSampler({
		.magFilter = mythril::SamplerFilter::Linear,
		.minFilter = mythril::SamplerFilter::Linear,
		.mipMap = mythril::SamplerMipMap::Disabled,
		.wrapU = mythril::SamplerWrap::Repeat,
		.wrapV = mythril::SamplerWrap::Repeat,
		.wrapW = mythril::SamplerWrap::Repeat,
		.debugName = "Repeating Sampler" ,
	});

	mythril::InternalShaderHandle shadowShader = ctx->createShader({
		.filePath = "Shadow.slang",
		.debugName = "Shadow Shader"
	});

	mythril::InternalGraphicsPipelineHandle shadowPipeline = ctx->createGraphicsPipeline({
		.vertexShader = {shadowShader},
		.fragmentShader = {shadowShader},
		.cull = mythril::CullMode::FRONT,
		.debugName = "Shadow Graphics Pipeline"
	});

	mythril::InternalShaderHandle fullscreenShader = ctx->createShader({
		.filePath = "Fullscreen.slang",
		.debugName = "Fullscreen Shader"
	});
	mythril::InternalGraphicsPipelineHandle fullscreenPipeline = ctx->createGraphicsPipeline({
		.vertexShader = {fullscreenShader},
		.fragmentShader = {fullscreenShader},
		.debugName = "Fullscreen Graphics Pipeline"
	});

	mythril::InternalShaderHandle redDebugShader = ctx->createShader({
		.filePath = "RedDebug.slang",
		.debugName = "Red Shader"
	});
	mythril::InternalGraphicsPipelineHandle redDebugPipeline = ctx->createGraphicsPipeline({
		.vertexShader = {redDebugShader},
		.fragmentShader = {redDebugShader},
		.debugName = "Red Graphics Pipeline"
	});


	mythril::InternalBufferHandle frameDataHandle = ctx->createBuffer({
		.size = sizeof(GPU::FrameData),
		.usage = mythril::BufferUsageBits::BufferUsageBits_Uniform,
		.storage = mythril::StorageType::Device,
		.debugName = "frameData Buffer"
	});


//	mythril::InternalShaderHandle brightPassShader = ctx->createShader({
//		.filePath = "",
//		.debugName = "Bright Pass Shader"
//	});
//	mythril::InternalComputePipelineHandle compteBloomPipelineX = ctx->createComputePipeline({
//		.shader = {brightPassShader},
//		.debugName = "Compute Bloom X"
//	});




	// load gltf asset
	AssetData sponzaData = loadGLTFAsset("KhronosGroup_glTF-Sample-Assets_Models-Sponza/glTF/Sponza.gltf");
	AssetCompiled sponzaCompiled = compileGLTFAsset(*ctx, sponzaData);

	// load my arrow visualizer
	AssetData arrowData = loadGLTFAsset("arrow.glb");
	AssetCompiled arrowCompiled = compileGLTFAsset(*ctx, arrowData);

	constexpr float kMODELSCALE = 0.05f;
	mythril::RenderGraph graph;

	GPU::LightingData lightingData = {};
	lightingData.directionalLights.color     = {1.0f, 1.0f, 0.97f};
	lightingData.directionalLights.intensity = 8.0f;

	float near = 1.f;
	float far = 450.f;
	glm::mat4 lightSpaceMatrix{};
	float orthoSize = 100.f;

	float distance_from_center = 330.f;

	// not to be chanegd by us, its calculated
	glm::vec3 sun_pos;
	glm::vec3 scene_center = glm::vec3(0, 30, 0);

	glm::vec3 eulerAngles = glm::radians(glm::vec3(-40.f, -60.f, 0.f));
	auto sun_quaternion = glm::quat(eulerAngles);

	float minBias = 0.005, maxBias = 0.05;


	// depth prepass for shadows from directional light
	graph.addPass("shadow_map", mythril::PassSource::Type::Graphics)
	.write({
		.texture = shadowMap,
		.clearValue = { 1.f, 0 },
		.loadOp = mythril::LoadOperation::CLEAR,
		.storeOp = mythril::StoreOperation::STORE
	})
	.setExecuteCallback([&](mythril::CommandBuffer& cmd) {
		cmd.cmdBindRenderPipeline(shadowPipeline);
		cmd.cmdBindDepthState({mythril::CompareOperation::CompareOp_Less, true});

		glm::vec3 lightDir = sun_quaternion * glm::vec3(0, 0, -1);
		lightDir = glm::normalize(lightDir);
		glm::vec3 lightPos = scene_center - lightDir * distance_from_center;

		if (far < near) {
			far = near;
		}
		assert(far >= near);

		glm::mat4 lightView = glm::lookAt(
				lightPos,
				scene_center,
				glm::vec3(0, 1, 0)
		);
		glm::mat4 lightProj = glm::ortho(
				-orthoSize, orthoSize,
				-orthoSize, orthoSize,
				near, far
		);

		lightSpaceMatrix = lightProj * lightView;
		
		auto model = glm::mat4(1.0);
		model = glm::scale(model, glm::vec3(kMODELSCALE));
		for (const MeshCompiled& mesh : sponzaCompiled.meshes) {
			struct PushConstant {
				glm::mat4 mvp;
				VkDeviceAddress vba;
			} push {
				.mvp = lightSpaceMatrix * model,
				.vba = ctx->gpuAddress(mesh.vertexBufHandle)
			};
			cmd.cmdPushConstants(push);
			cmd.cmdBindIndexBuffer(mesh.indexBufHandle);
			cmd.cmdDrawIndexed(mesh.indexCount);
		}
	});

	graph.addPass("geometry_opaque", mythril::PassSource::Type::Graphics)
	.write({
		.texture = colorTarget,
		.clearValue = {0.349f, 0.635f, 0.82f, 1.f},
		.loadOp = mythril::LoadOperation::CLEAR,
		.storeOp = mythril::StoreOperation::STORE
	})
	.write({
		.texture = depthTarget,
		.clearValue = {1.f, 0},
		.loadOp = mythril::LoadOperation::CLEAR,
		.storeOp = mythril::StoreOperation::STORE
	})
	.read({ .texture = shadowMap })
	.setExecuteCallback([&](mythril::CommandBuffer& cmd) {
		cmd.cmdBindRenderPipeline(opaquePipeline);
		cmd.cmdBindDepthState({mythril::CompareOperation::CompareOp_Less, true});
		auto model = glm::mat4(1.0);
		model = glm::scale(model, glm::vec3(kMODELSCALE));
		for (const MeshCompiled& mesh : sponzaCompiled.meshes) {
			const MaterialData& material = sponzaCompiled.materials[mesh.materialIndex];
			if (material.isTransparent) continue;

			mythril::InternalTextureHandle baseColorTex = sponzaCompiled.textureHandles[material.baseColorTextureIndex];
			mythril::InternalTextureHandle normalTex = sponzaCompiled.textureHandles[material.normalTextureIndex];
			mythril::InternalTextureHandle roughnessMetallicTex = sponzaCompiled.textureHandles[material.roughnessMetallicTextureIndex];

			GPU::GeometryPushConstants push {
				.model = model,
				.vba = ctx->gpuAddress(mesh.vertexBufHandle),
				.tintColor = {1, 1, 1, 1},
				.baseColorTexture = baseColorTex.index(),
				.normalTexture = normalTex.index(),
				.roughnessMetallicTexture = roughnessMetallicTex.index(),
				.samplerState = repeatSampler.index(),
				.shadowTexture = shadowMap.index(),
				.shadowSampler = shadowSampler.index(),
				.lightSpaceMatrix = lightSpaceMatrix,
				.minBias = minBias,
				.maxBias = maxBias
			};
			cmd.cmdPushConstants(push);
			cmd.cmdBindIndexBuffer(mesh.indexBufHandle);
			cmd.cmdDrawIndexed(mesh.indexCount);
		}
	});
	graph.addPass("geometry_transparent", mythril::PassSource::Type::Graphics)
	.write({
		.texture = colorTarget,
		.loadOp = mythril::LoadOperation::LOAD,
		.storeOp = mythril::StoreOperation::STORE
	})
	.write({
		.texture = depthTarget,
		.loadOp = mythril::LoadOperation::LOAD,
		.storeOp = mythril::StoreOperation::STORE
	})
	.read({ .texture = shadowMap })
	.setExecuteCallback([&](mythril::CommandBuffer& cmd) {
		cmd.cmdBindRenderPipeline(transparentPipeline);
		cmd.cmdBindDepthState({mythril::CompareOperation::CompareOp_Less, true});
		auto model = glm::mat4(1.0);
		model = glm::scale(model, glm::vec3(kMODELSCALE));
		// what we call a mesh is really a primitive for fastgltf
		for (const MeshCompiled& mesh : sponzaCompiled.meshes) {
			const MaterialData& material = sponzaCompiled.materials[mesh.materialIndex];
			if (!material.isTransparent) continue;

			mythril::InternalTextureHandle baseColorTex = sponzaCompiled.textureHandles[material.baseColorTextureIndex];
			mythril::InternalTextureHandle normalTex = sponzaCompiled.textureHandles[material.normalTextureIndex];
			mythril::InternalTextureHandle roughnessMetallicTex = sponzaCompiled.textureHandles[material.roughnessMetallicTextureIndex];

			GPU::GeometryPushConstants push {
					.model = model,
					.vba = ctx->gpuAddress(mesh.vertexBufHandle),
					.tintColor = {1, 1, 1, 1},
					.baseColorTexture = baseColorTex.index(),
					.normalTexture = normalTex.index(),
					.roughnessMetallicTexture = roughnessMetallicTex.index(),
					.samplerState = repeatSampler.index(),
					.shadowTexture = shadowMap.index(),
					.shadowSampler = shadowSampler.index(),
					.lightSpaceMatrix = lightSpaceMatrix,
			};
			cmd.cmdPushConstants(push);
			cmd.cmdBindIndexBuffer(mesh.indexBufHandle);
			cmd.cmdDrawIndexed(mesh.indexCount);
		}
	});
	graph.addPass("shadow_pos_debug", mythril::PassSource::Type::Graphics)
	.write({
		.texture = colorTarget,
		.loadOp = mythril::LoadOperation::LOAD,
		.storeOp = mythril::StoreOperation::STORE
	})
	.setExecuteCallback([&](mythril::CommandBuffer& cmd) {
		cmd.cmdBindRenderPipeline(redDebugPipeline);
		cmd.cmdBindDepthState({mythril::CompareOperation::CompareOp_Less, true});

		glm::mat4 model = glm::translate(glm::mat4(1.0), scene_center);
		model = model * glm::mat4_cast(sun_quaternion);
		model = glm::rotate(model, 180.f, glm::vec3(0, 0, 1));

		for (auto& mesh : arrowCompiled.meshes) {
			struct PushConstant {
				glm::mat4 model;
				VkDeviceAddress vba;
			} push {
				.model = model,
				.vba = ctx->gpuAddress(mesh.vertexBufHandle),
				};
			cmd.cmdPushConstants(push);
			cmd.cmdBindIndexBuffer(mesh.indexBufHandle);
			cmd.cmdDrawIndexed(mesh.indexCount);
		}
	});
//	graph.addPass("test", mythril::PassSource::Type::Graphics)
//	.write({
//		.texture = colorTarget,
//		.clearValue = { 0, 0, 0, 1},
//		.loadOp = mythril::LoadOperation::CLEAR,
//		.storeOp = mythril::StoreOperation::STORE
//	})
//	.read({
//		.texture = shadowMap
//	})
//	.setExecuteCallback([&](mythril::CommandBuffer& cmd) {
//		cmd.cmdBindRenderPipeline(fullscreenPipeline);
//		struct P {
//			uint64_t texture;
//			uint64_t sampler;
//		} push {
//			.texture = shadowMap.index(),
//			.sampler = 0
//		};
//		cmd.cmdPushConstants(push);
//		cmd.cmdDraw(3);
//	});
	graph.addPass("gui", mythril::PassSource::Type::Graphics)
	.write({
		.texture = colorTarget,
		.loadOp = mythril::LoadOperation::LOAD,
		.storeOp = mythril::StoreOperation::STORE
	})
	.setExecuteCallback([](mythril::CommandBuffer& cmd) {
		cmd.cmdDrawImGui();
	});
	graph.compile(*ctx);


	// todo: right now if a shader is used across multiple pipelines, its descriptor sets have to be updated for each one
	{
		mythril::DescriptorSetWriter writer = ctx->openUpdate(opaquePipeline);
		writer.updateBinding(frameDataHandle, "frame");
		ctx->submitUpdate(writer);
	}
	{
		mythril::DescriptorSetWriter writer = ctx->openUpdate(transparentPipeline);
		writer.updateBinding(frameDataHandle, "frame");
		ctx->submitUpdate(writer);
	}
	{
		mythril::DescriptorSetWriter writer = ctx->openUpdate(redDebugPipeline);
		writer.updateBinding(frameDataHandle, "frame");
		ctx->submitUpdate(writer);
	}


	auto startTime = std::chrono::high_resolution_clock::now();

	static bool focused = false;
	ctx->getWindow().setMouseMode(focused);
	constexpr float kMouseSensitivity = 0.3f;
	constexpr float kBaseCameraSpeed = 8.f;
	constexpr float kShiftCameraSpeed = kBaseCameraSpeed * 3.f;
	glm::vec3 camera_position = {0.f, 5.f, 0.f};


	lightingData.pointLights[0] = { {1.0f, 0.8f, 0.7f},  10.0f, { 10.0f,  3.0f,  2.0f}, 20.f };
	lightingData.pointLights[1] = { {0.7f, 0.8f, 1.0f},  9.0f, {-30.0f,  2.5f, -1.0f}, 50.f };
	lightingData.pointLights[2] = { {0.7f, 1.0f, 0.7f},  5.0f, { 60.0f,  4.0f, -3.0f}, 5.f };
	lightingData.pointLights[3] = { {1.0f, 0.0f, 0.1f},  11.0f, { -40.0f,  1.0f,  4.0f}, 25.f };

	lightingData.environmentLight.color     = {0.25f, 0.30f, 0.35f};
	lightingData.environmentLight.intensity = 0.3f;


	bool quit = false;
	while (!quit) {
		if (ctx->isSwapchainDirty()) {
			ctx->cleanSwapchain();

			const mythril::Window &window = ctx->getWindow();
			VkExtent2D new_extent_2d = window.getFramebufferSize();
			ctx->resizeTexture(colorTarget, new_extent_2d);
			ctx->resizeTexture(depthTarget, new_extent_2d);
			graph.compile(*ctx);
		}

		static float yaw = 0, pitch = 0;

		SDL_Event e;
		while (SDL_PollEvent(&e)) {
			ImGui_ImplSDL3_ProcessEvent(&e);

			if (e.type == SDL_EVENT_QUIT) quit = true;
			if (e.type == SDL_EVENT_KEY_DOWN) {
				if (e.key.key == SDLK_Q) quit = true;
				if (e.key.key == SDLK_C) ctx->getWindow().setMouseMode(focused = !focused);
			}
			if (e.type == SDL_EVENT_MOUSE_MOTION) {
				if (focused) {
					auto dx = (float)e.motion.xrel;
					auto dy = (float)e.motion.yrel;
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

		auto currentTime = std::chrono::high_resolution_clock::now();
		float deltaTime = std::chrono::duration<float>(currentTime - startTime).count();
		startTime = std::chrono::high_resolution_clock::now();

		const bool* state = SDL_GetKeyboardState(nullptr);

		float camera_speed = kBaseCameraSpeed;
		if (state[SDL_SCANCODE_LSHIFT]) camera_speed = kShiftCameraSpeed;

		if (state[SDL_SCANCODE_W]) camera_position += front * camera_speed * deltaTime;
		if (state[SDL_SCANCODE_S]) camera_position -= front * camera_speed * deltaTime;
		if (state[SDL_SCANCODE_A]) camera_position -= right * camera_speed * deltaTime;
		if (state[SDL_SCANCODE_D]) camera_position += right * camera_speed * deltaTime;
		if (state[SDL_SCANCODE_SPACE]) camera_position += up * camera_speed * deltaTime;
		if (state[SDL_SCANCODE_LCTRL]) camera_position -= up * camera_speed * deltaTime;

		const VkExtent2D windowSize = ctx->getWindow().getWindowSize();
		const Camera camera = {
				.position = camera_position,
				.forwardVector = front,
				.upVector = up,
				.aspectRatio = (float) windowSize.width / (float) windowSize.height,
				.fov = 80.f,
				.nearPlane = 0.1f,
				.farPlane = 200.f
		};
		const GPU::CameraData cameraData = {
				.proj = calculateProjectionMatrix(camera),
				.view = calculateViewMatrix(camera),
				.position = camera.position,
				.far = camera.farPlane,
				.near = camera.nearPlane
		};

		ImGui_ImplVulkan_NewFrame();
		ImGui_ImplSDL3_NewFrame();
		ImGui::NewFrame();


		ImGui::Begin("shadow_controls");
		ImGui::DragFloat("Near Plane", &near, 0.1f, 100.f);
		ImGui::DragFloat("Far Plane", &far, 0.1f, 1000.f);
		ImGui::DragFloat("Min Bias", &minBias, 0.00001, 0.00001, 0.01f, "%.6f");
		ImGui::DragFloat("Max Bias", &maxBias, 0.0001, 0.0001, 0.01f, "%.6f");
//		ImGui::DragFloat3("Sun Position", &sun_pos[0], 0.1f, 1000.f);
		ImGui::DragFloat("Ortho Size", &orthoSize, 10.f, 300.f);
		ImGui::DragFloat("Distance From Scene", &distance_from_center);
//		ImGui::Text("Sun Position: %.1f %.1f %1.f", sun_pos.x, sun_pos.y, sun_pos.z);
//		ImGui::DragFloat("Scene Radius (Determines Sun Position)", &sceneRadius);
//		ImGui::DragFloat("Scene Margin", &margin);
		ImGui::End();

		ImGuiWindowFlags window_flags = focused ? ImGuiWindowFlags_NoMouseInputs : ImGuiWindowFlags_None;
		ImGui::Begin("Lighting", nullptr, window_flags);
		ImGui::Image(shadowMap, {200, 200});
		ImGui::End();
		DrawLightingUI(lightingData);

		ImGuizmo::BeginFrame();

		glm::mat4 transformMatrix = glm::translate(glm::mat4(1.f), scene_center) *
									glm::mat4_cast(sun_quaternion);


		ImGuizmo::SetOrthographic(false);
		ImGuizmo::SetDrawlist(ImGui::GetBackgroundDrawList());
		ImGuizmo::SetRect(0, 0, windowSize.width, windowSize.height);
		ImGuizmo::Manipulate(
				glm::value_ptr(cameraData.view),
				glm::value_ptr(cameraData.proj),
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
		

		lightingData.directionalLights.direction = sun_quaternion * glm::vec3(0, 0, -1);
		const GPU::FrameData frameData {
			.camera = cameraData,
			.lighting = lightingData
		};

		mythril::CommandBuffer& cmd = ctx->openCommand(mythril::CommandBuffer::Type::Graphics);
		cmd.cmdUpdateBuffer(frameDataHandle, frameData);
		graph.execute(cmd);
		ctx->submitCommand(cmd);
	}

	return 0;
}

