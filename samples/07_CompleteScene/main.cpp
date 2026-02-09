//
// Created by Hayden Rivas on 12/2/25.
//

#include "mythril/CTXBuilder.h"
#include "mythril/Objects.h"

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

#include "Helpers.h"
#include "HostStructs.h"
#include "Primitives.h"

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
	uint32_t vertexCount;
	mythril::BufferHandle indexBufHandle;
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
	std::vector<mythril::TextureHandle> textureHandles;
	std::vector<MaterialData> materials;

	uint32_t getNumOfOpaqueMeshes() const {
		uint32_t n = 0;
		for (const MeshCompiled& mesh : meshes) {
			const MaterialData& material = materials[mesh.materialIndex];
			if (material.isTransparent) continue;
			n++;
		}
		return n;
	}
	uint32_t getNumOfTransparentMeshes() const {
		uint32_t n = 0;
		for (const MeshCompiled& mesh : meshes) {
			const MaterialData& material = materials[mesh.materialIndex];
			if (!material.isTransparent) continue;
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
	ASSERT_MSG(data_result.error() == fastgltf::Error::None, "Data load from path {} failed!", filepath.string().c_str());
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
	ASSERT_MSG(asset_result.error() == fastgltf::Error::None, "Error when validating {}", filepath.string().c_str());
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
					GPU::GeneralVertex vertex = {};
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
					ASSERT_MSG(false, "Unsupported image named: '{}' from gltf: '{}'", image.name, filepath.string().c_str());
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
								ASSERT_MSG(image_data, "Failed to load data from image named: '{}' from gltf: '{}'", image.name, filepath.string().c_str());
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
					ASSERT_MSG(image_data, "Failed to load data from image named: '{}' from gltf: '{}'", image.name, filepath.string().c_str());
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
					ASSERT_MSG(image_data, "Failed to load data from image named: '{}' from gltf: '{}'", image.name, filepath.string().c_str());
					texture.width = w;
					texture.height = h;
					texture.channels = nChannels;
					texture.pixels.reset(image_data);
				},
				[&](const fastgltf::sources::URI& uri_source) {
					ASSERT(uri_source.fileByteOffset == 0);
					ASSERT(uri_source.uri.isLocalPath());

					const std::filesystem::path image_path = uri_source.uri.path();
					unsigned char* image_data = stbi_load(image_path.string().c_str(), &w, &h, &nChannels, 4);
					ASSERT_MSG(image_data, "Failed to load data from image named: '{}' from gltf: '{}'", image.name, filepath.string().c_str());
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
		.numMipLevels = mythril::vkutil::CalcNumMipLevels(w, h),
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

			mythril::BufferHandle mesh_vertex_buffer = ctx.createBuffer({
				.size = sizeof(GPU::GeneralVertex) * primitive_data.vertexData.size(),
				.usage = mythril::BufferUsageBits::BufferUsageBits_Storage,
				.storage = mythril::StorageType::Device,
				.initialData = primitive_data.vertexData.data(),
				.debugName = vertex_name_buf
			}).release();
			mesh_compiled.vertexBufHandle = mesh_vertex_buffer;
			mesh_compiled.vertexCount = primitive_data.vertexData.size();

			mythril::BufferHandle mesh_index_buffer = ctx.createBuffer({
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
				.numMipLevels = mythril::vkutil::CalcNumMipLevels(texture_data.width, texture_data.height),
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
void UpdateParticles(std::vector<ParticleData>& particles, float deltaTime, float speed) {
	const float dtSpeed = deltaTime * speed;
	for (auto& p : particles) {
		p.position += p.velocity * dtSpeed;
		// Optional: bounce off bounds
		for (int i = 0; i < 3; ++i) {
			if (p.position[i] < -50.0f || p.position[i] > 100.0f)
				p.velocity[i] = -p.velocity[i]; // reflect velocity
		}
	}
}

static constexpr uint8_t kNumPointLights = 4;
static void UpdatePointLightShadowMatrices(
	const GPU::LightingData& lightingData,
	float modelScale,
	float nearPlane,
	float farPlane,
	glm::mat4 outShadowMatrices[kNumPointLights][6]) {
	const glm::mat4 scaledModel = glm::scale(glm::mat4(1.0f), glm::vec3(modelScale));
	for (int i = 0; i < kNumPointLights; ++i) {
		const glm::vec3 lightPos = lightingData.pointLights[i].position;
		glm::mat4 shadowProj = glm::perspective(glm::radians(90.0f), 1.0f, nearPlane, farPlane);
		// we need to swap the +y and -y in order to account for the swith on vulkan
		const glm::mat4 shadowViews[6] = {
			glm::lookAt(lightPos, lightPos + glm::vec3( 1,  0,  0), glm::vec3(0, -1,  0)), // +X
			glm::lookAt(lightPos, lightPos + glm::vec3(-1,  0,  0), glm::vec3(0, -1,  0)), // -X
			glm::lookAt(lightPos, lightPos + glm::vec3( 0, -1,  0), glm::vec3(0,  0, -1)), // -Y
			glm::lookAt(lightPos, lightPos + glm::vec3(0, 1, 0), glm::vec3(0, 0, 1)), // +Y
			glm::lookAt(lightPos, lightPos + glm::vec3( 0,  0,  1), glm::vec3(0, -1,  0)), // +Z
			glm::lookAt(lightPos, lightPos + glm::vec3( 0,  0, -1), glm::vec3(0, -1,  0)), // -Z
		};
		for (int face = 0; face < 6; ++face) {
			outShadowMatrices[i][face] = shadowProj * shadowViews[face] * scaledModel;
		}
	}
}

int main() {
	static constexpr VkFormat kOffscreenFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
	const std::filesystem::path kDataDir = std::filesystem::path(MYTH_SAMPLE_NAME).concat("_data/");
	const static std::vector<std::string> slang_searchpaths = {
		"../../include/",
		"../include/",
		(kDataDir / "shaders/").string()
	};
	static std::vector<const char*> vulkan_extensions = {};
	auto ctx = mythril::CTXBuilder{}
	.set_vulkan_cfg({
		.app_name = "Cool App Name",
		.engine_name = "Cool Engine Name",
		.app_version = {0, 0, 1},
		.engine_version = {0, 0, 1},
		.enableValidation = true,
		.deviceExtensions = vulkan_extensions,
	})
	.set_window_spec({
		.title = "Cool Window Name",
		.mode = mythril::WindowMode::Windowed,
		.width = 1280,
		.height = 720,
		.resizeable = true
	})
	.set_slang_cfg({
		.searchpaths = slang_searchpaths
	})
	.with_default_swapchain()
	.with_ImGui({
		.format = kOffscreenFormat,
	})
	.with_TracyGPU()
	.build();


	const VkExtent2D framebufferExtent2D = ctx->getWindow().getFramebufferSize();
	const mythril::Dimensions windowDimensions = { framebufferExtent2D.width, framebufferExtent2D.height, 1 };
	mythril::Texture offscreenColorTarget = ctx->createTexture({
		.dimension = windowDimensions,
		.format = kOffscreenFormat,
		.samples = mythril::SampleCount::X1,
		.usage = mythril::TextureUsageBits::TextureUsageBits_Attachment | mythril::TextureUsageBits_Sampled | mythril::TextureUsageBits::TextureUsageBits_Storage,
		.storage = mythril::StorageType::Device,
		.debugName = "Offscreen Color Texture"
	});
	mythril::Texture msaaColorTarget = ctx->createTexture({
		.dimension = windowDimensions,
		.format = kOffscreenFormat,
		.samples = mythril::SampleCount::X4,
		.usage = mythril::TextureUsageBits::TextureUsageBits_Attachment,
		.storage = mythril::StorageType::Memoryless,
		.debugName = "MSAA Color Texture"
	});

	static constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
	mythril::Texture msaaDepthTarget = ctx->createTexture({
		.dimension = windowDimensions,
		.format = kDepthFormat,
		.samples = mythril::SampleCount::X4,
		.usage = mythril::TextureUsageBits::TextureUsageBits_Attachment,
		.storage = mythril::StorageType::Memoryless,
		.debugName = "MSAA Depth Texture"
	});
	mythril::Texture depthTarget = ctx->createTexture({
		.dimension = windowDimensions,
		.format = kDepthFormat,
		.samples = mythril::SampleCount::X1,
		.usage = mythril::TextureUsageBits::TextureUsageBits_Attachment | mythril::TextureUsageBits_Sampled,
		.storage = mythril::StorageType::Device,
		.debugName = "Offscreen Depth Texture"
	});

	constexpr uint32_t shadow_map_size = 4096;
	static constexpr VkFormat kShadowFormat = VK_FORMAT_D16_UNORM;
	mythril::Texture shadowMap = ctx->createTexture({
		.dimension = {shadow_map_size, shadow_map_size},
		.format = kShadowFormat,
		.usage = mythril::TextureUsageBits::TextureUsageBits_Attachment | mythril::TextureUsageBits::TextureUsageBits_Sampled,
		.debugName = "Shadow Map Texture"
	});
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
		.filePath = kDataDir / "shaders/DirectionalShadow.slang",
		.debugName = "Shadow Shader"
	});

	mythril::GraphicsPipeline shadowPipeline = ctx->createGraphicsPipeline({
		.vertexShader = {shadowShader},
		.fragmentShader = {shadowShader},
		.cull = mythril::CullMode::OFF,
		.debugName = "Shadow Graphics Pipeline"
	});

	mythril::Shader redDebugShader = ctx->createShader({
		.filePath = kDataDir / "shaders/RedDebug.slang",
		.debugName = "Red Shader"
	});
	mythril::GraphicsPipeline redDebugPipeline = ctx->createGraphicsPipeline({
		.vertexShader = {redDebugShader},
		.fragmentShader = {redDebugShader},
		.debugName = "Red Graphics Pipeline"
	});

	mythril::Buffer frameDataHandle = ctx->createBuffer({
		.size = sizeof(GPU::FrameData),
		.usage = mythril::BufferUsageBits::BufferUsageBits_Uniform,
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
		.filePath = kDataDir / "shaders/FullscreenComposite.slang",
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

	mythril::Texture finalColorTarget = ctx->createTexture({
		.dimension = windowDimensions,
		.format = kOffscreenFormat,
		.usage = mythril::TextureUsageBits::TextureUsageBits_Attachment,
		.storage = mythril::StorageType::Device,
		.debugName = "Final Color Target (Takes in OffscreenColor)"
	});


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
	mythril::RenderGraph graph;

	GPU::LightingData lightingData = {};
	lightingData.directionalLights.color     = {1.0f, 1.0f, 0.97f};
	lightingData.directionalLights.intensity = 8.0f;

	float near = 1.f, far = 450.f;
	float depthBiasConstant = 1.25f, depthBiasSlope = 1.75f;

	glm::mat4 lightSpaceMatrix{};
	float orthoSize = 100.f;

	float distance_from_center = 330.f;

	// not to be chanegd by us, its calculated
	constexpr auto scene_center = glm::vec3(0, 30, 0);

	glm::vec3 eulerAngles = glm::radians(glm::vec3(-40.f, -60.f, 0.f));
	auto sun_quaternion = glm::quat(eulerAngles);

	// depth prepass for shadows from directional light
	graph.addGraphicsPass("shadow_map")
	.attachment({
		.texDesc = {shadowMap},
		.clearValue = {1.f, 0},
		.loadOp = mythril::LoadOp::CLEAR,
		.storeOp = mythril::StoreOp::STORE
	})
	.setExecuteCallback([&](mythril::CommandBuffer& cmd) {
		cmd.cmdBeginRendering();
		cmd.cmdBindGraphicsPipeline(shadowPipeline);
		cmd.cmdBindDepthState({mythril::CompareOp::LessEqual, true});
		cmd.cmdSetDepthBiasEnable(true);
		cmd.cmdSetDepthBias(depthBiasConstant, depthBiasSlope, 0.f);

		glm::vec3 lightDir = sun_quaternion * glm::vec3(0, 0, -1);
		lightDir = glm::normalize(lightDir);
		const glm::vec3 lightPos = scene_center - lightDir * distance_from_center;

		if (far < near) {
			far = near;
		}
		assert(far >= near);

		const glm::mat4 lightView = glm::lookAt(
				lightPos,
				scene_center,
				glm::vec3(0, 1, 0)
		);
		const glm::mat4 lightProj = glm::ortho(
				-orthoSize, orthoSize,
				-orthoSize, orthoSize,
				near, far
		);

		lightSpaceMatrix = lightProj * lightView;

		const auto model = glm::scale(glm::mat4(1.0), glm::vec3(kMODELSCALE));
		struct PushConstant {
			glm::mat4 mvp;
			VkDeviceAddress vba;
		} push{
			.mvp = lightSpaceMatrix * model,
			.vba = opaqueSponzaVertexBuf.gpuAddress()
		};
		cmd.cmdPushConstants(push);
		cmd.cmdBindIndexBuffer(opaqueSponzaIndexBuf);
		cmd.cmdDrawIndexedIndirect(opaqueSponzaIndirectBuf, 0, opaqueDrawCount);
		cmd.cmdEndRendering();
	});

	static constexpr uint32_t pointLightShadowResolution = 512;
	mythril::Texture pointLightShadowTex[kNumPointLights];
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
	}
	mythril::Shader pointshadowShader = ctx->createShader({
		.filePath = kDataDir / "shaders/PointShadow.slang",
		.debugName = "Point Shadow Map Shader"
	});
	mythril::GraphicsPipeline pointShadowGraphicsPipeline = ctx->createGraphicsPipeline({
		.vertexShader = pointshadowShader,
		.fragmentShader = pointshadowShader,
		.cull = mythril::CullMode::BACK,
		.debugName = "Point Shadow Map Graphics Pipeline"
	});
	// we need position data
	lightingData.pointLights[0] = { {1.0f, 0.8f, 0.7f},  10.0f, { 10.0f,  3.0f,  2.0f}, 20.f };
	lightingData.pointLights[1] = { {0.7f, 0.8f, 1.0f},  9.0f, {-30.0f,  2.5f, -1.0f}, 50.f };
	lightingData.pointLights[2] = { {0.7f, 1.0f, 0.7f},  12.0f, { 40.0f,  4.0f, -3.0f}, 14.f };
	lightingData.pointLights[3] = { {1.0f, 0.0f, 0.1f},  11.0f, { -40.0f,  1.0f,  4.0f}, 25.f };

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
	for (int i = 0; i < kNumPointLights; i++) {
		graph.addGraphicsPass(fmt::format("pointlight_shadow_{}", i).c_str())
		.attachment({
			.texDesc = pointLightShadowTex[i],
			.clearValue = {1.f, 1 },
			.loadOp = mythril::LoadOp::CLEAR,
			.storeOp = mythril::StoreOp::STORE,
		})
		.setExecuteCallback([&, i](mythril::CommandBuffer& cmd) {
			// easy optimization
			if (lightingData.pointLights[i].position == lastFramelightingData.pointLights[i].position) return;

			cmd.cmdBeginRendering(6, 0b111111);
			cmd.cmdBindGraphicsPipeline(pointShadowGraphicsPipeline);
			cmd.cmdBindDepthState({mythril::CompareOp::LessEqual, true});
			// i cant tell enough of a difference with it on or off
			struct PushConstant {
				VkDeviceAddress shadowMVPs;
				VkDeviceAddress vba;
				glm::mat4 model;
				uint32_t nLight;
				glm::vec3 lightPos;
			} push {
				.shadowMVPs = pointLightShadowMatrixBuf.gpuAddress(),
				.vba = opaqueSponzaVertexBuf.gpuAddress(),
				.model = scaledModel,
				.nLight = static_cast<uint32_t>(i),
				.lightPos = lightingData.pointLights[i].position,
			};
			cmd.cmdPushConstants(push);
			cmd.cmdBindIndexBuffer(opaqueSponzaIndexBuf);
			cmd.cmdDrawIndexedIndirect(opaqueSponzaIndirectBuf, 0, opaqueDrawCount);
			cmd.cmdEndRendering();
		});
	}

	mythril::Sampler shadowSampler = ctx->createSampler({
		.wrapU = mythril::SamplerWrap::ClampBorder,
		.wrapV = mythril::SamplerWrap::ClampBorder,
		.depthCompareEnabled = true,
		.depthCompareOp = mythril::CompareOp::LessEqual,
		.debugName = "Shadow Sampler Comparison"
	});

	mythril::Shader standardShader = ctx->createShader({
		.filePath = kDataDir / "shaders/Standard.slang",
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
	graph.addGraphicsPass("geometry_opaque")
	.attachment({
		.texDesc = msaaColorTarget,
		.clearValue = {0.349f, 0.635f, 0.82f, 1.f},
		.loadOp = mythril::LoadOp::CLEAR,
		.storeOp = mythril::StoreOp::STORE,
	})
	.attachment({
		.texDesc = msaaDepthTarget,
		// clear to 0, we do reverse depth buffering
		.clearValue = {0.f, 0},
		.loadOp = mythril::LoadOp::CLEAR,
		.storeOp = mythril::StoreOp::STORE,
	})
	.dependency(shadowMap, mythril::Layout::READ)
	.dependency(&pointLightShadowTex[0], kNumPointLights, mythril::Layout::READ)
	.setExecuteCallback([&](mythril::CommandBuffer& cmd) {
		cmd.cmdBeginRendering();
		cmd.cmdBindGraphicsPipeline(opaquePipeline);
		cmd.cmdBindDepthState({mythril::CompareOp::Greater, true});
		for (const MeshCompiled& mesh : sponzaCompiled.meshes) {
			const MaterialData& material = sponzaCompiled.materials[mesh.materialIndex];
			if (material.isTransparent) continue;

			mythril::TextureHandle baseColorTex = sponzaCompiled.textureHandles[material.baseColorTextureIndex];
			mythril::TextureHandle normalTex = sponzaCompiled.textureHandles[material.normalTextureIndex];
			mythril::TextureHandle roughnessMetallicTex = sponzaCompiled.textureHandles[material.roughnessMetallicTextureIndex];

			const GPU::GeometryPushConstants push {
				.model = scaledModel,
				.vba = ctx->gpuAddress(mesh.vertexBufHandle),
				.tintColor = {1, 1, 1, 1},
				.baseColorTexture = baseColorTex.index(),
				.normalTexture = normalTex.index(),
				.roughnessMetallicTexture = roughnessMetallicTex.index(),
				.samplerState = repeatSampler.index(),
				.shadowTexture = shadowMap.index(),
				.shadowSampler = shadowSampler.index(),
				.lightSpaceMatrix = lightSpaceMatrix,
				.pointLightShadowTextures = {
					pointLightShadowTex[0].index(),
					pointLightShadowTex[1].index(),
					pointLightShadowTex[2].index(),
					pointLightShadowTex[3].index()
				}
			};
			cmd.cmdPushConstants(push);
			cmd.cmdBindIndexBuffer(mesh.indexBufHandle);
			cmd.cmdDrawIndexed(mesh.indexCount);
		}
		cmd.cmdEndRendering();
	});

	mythril::GraphicsPipeline transparentPipeline = ctx->createGraphicsPipeline({
		.vertexShader = {standardShader},
		.fragmentShader = {standardShader},
		.blend = mythril::BlendingMode::ALPHA_BLEND,
		.cull = mythril::CullMode::OFF,
		.multisample = mythril::SampleCount::X4,
		.debugName = "Transparent Graphics Pipeline"
	});

	graph.addGraphicsPass("geometry_transparent")
	.attachment({
		.texDesc = msaaColorTarget,
		.loadOp = mythril::LoadOp::LOAD,
		.storeOp = mythril::StoreOp::STORE,
	})
	.attachment({
		.texDesc = msaaDepthTarget,
		.loadOp = mythril::LoadOp::LOAD,
		.storeOp = mythril::StoreOp::STORE,
	})
	.dependency(shadowMap, mythril::Layout::READ)
	.setExecuteCallback([&](mythril::CommandBuffer& cmd) {
		cmd.cmdBeginRendering();
		cmd.cmdBindGraphicsPipeline(transparentPipeline);
		cmd.cmdBindDepthState({mythril::CompareOp::Greater, true});
		auto model = glm::mat4(1.0);
		model = glm::scale(model, glm::vec3(kMODELSCALE));
		// what we call a mesh is really a primitive for fastgltf
		for (const MeshCompiled& mesh : sponzaCompiled.meshes) {
			const MaterialData& material = sponzaCompiled.materials[mesh.materialIndex];
			if (!material.isTransparent) continue;

			mythril::TextureHandle baseColorTex = sponzaCompiled.textureHandles[material.baseColorTextureIndex];
			mythril::TextureHandle normalTex = sponzaCompiled.textureHandles[material.normalTextureIndex];
			mythril::TextureHandle roughnessMetallicTex = sponzaCompiled.textureHandles[material.roughnessMetallicTextureIndex];

			const GPU::GeometryPushConstants push {
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
		cmd.cmdEndRendering();
	});

	mythril::Shader particleShader = ctx->createShader({
		.filePath = kDataDir / "shaders/Particles.slang",
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
	graph.addGraphicsPass("Ambient Particles")
	.attachment({
		.texDesc = msaaColorTarget,
		.loadOp = mythril::LoadOp::LOAD,
		.storeOp = mythril::StoreOp::NO_CARE,
		.resolveTexDesc = offscreenColorTarget
	})
	.attachment({
		.texDesc = msaaDepthTarget,
		.loadOp = mythril::LoadOp::LOAD,
		.storeOp = mythril::StoreOp::NO_CARE,
		.resolveTexDesc = depthTarget
	})
	.setExecuteCallback([&](mythril::CommandBuffer& cmd) {
		cmd.cmdBeginRendering();
		cmd.cmdBindGraphicsPipeline(particle_pipeline);
		cmd.cmdBindDepthState({mythril::CompareOp::Greater, true});
		struct PushConstant {
			VkDeviceAddress vba;
			VkDeviceAddress bufferAddr;
			float brightness;
			float speed;
		} push {
			.vba = particle_vertex_buffer.gpuAddress(),
			.bufferAddr = particles_buffer.gpuAddress(),
			.brightness = particle_emission,
			.speed = particle_speed
		};
		cmd.cmdPushConstants(push);
		cmd.cmdBindIndexBuffer(particle_index_buffer);
		cmd.cmdDrawIndexed(Primitives::kHexIndices.size(), particles.size());
		cmd.cmdEndRendering();
	});

	constexpr unsigned int kNumColorMips = 8;
	mythril::Texture offscreenColorTexs[kNumColorMips];
	mythril::Dimensions colorbloomdimensions = offscreenColorTarget->getDimensions();
	for (int i = 0; i < kNumColorMips; i++) {
		char namebuf[64];
		snprintf(namebuf, sizeof(namebuf), "Color Bloom Tex %i", i);
		offscreenColorTexs[i] = ctx->createTexture({
			.dimension = colorbloomdimensions,
			.format = kOffscreenFormat,
			.usage = mythril::TextureUsageBits_Sampled | mythril::TextureUsageBits_Storage,
			.debugName = namebuf
		});
		colorbloomdimensions = colorbloomdimensions.divide2D(2);
	}
	mythril::Shader downsampleComputeShader = ctx->createShader({
		.filePath = kDataDir / "shaders/Downsample.slang",
		.debugName = "Downsample Compute Shader"
	});
	mythril::ComputePipeline downsampleComputePipeline = ctx->createComputePipeline({
		.shader = downsampleComputeShader.handle(),
		.debugName = "Downsample Compute Pipeline"
	});
	graph.addIntermediate("copy offscreen to toplevel mipmap color")
	.copy(offscreenColorTarget, offscreenColorTexs[0])
	.finish();

	graph.addComputePass("Downsampling")
	.dependency(&offscreenColorTexs[0], kNumColorMips, mythril::Layout::GENERAL)
	.setExecuteCallback([&](mythril::CommandBuffer& cmd) {
		cmd.cmdBindComputePipeline(downsampleComputePipeline);
		for (int i = 0; i < kNumColorMips-1; i++) {
			struct PushConstants {
				uint64_t inTex;
				uint64_t outTex;
				uint64_t sampler;
				uint32_t mipLevel;
			} push {
				.inTex = offscreenColorTexs[i].index(),
				.outTex = offscreenColorTexs[i+1].index(),
				.sampler = clampSampler.index(),
				.mipLevel = static_cast<uint32_t>(i)
			};
			cmd.cmdPushConstants(push);
			static constexpr uint32_t threadNum = 8;
			const mythril::Dimensions texDims = offscreenColorTexs[i+1]->getDimensions();
			const mythril::Dimensions roundedDims = {
				(texDims.width + threadNum - 1) / threadNum,
				(texDims.height + threadNum - 1) / threadNum,
				1};
			cmd.cmdDispatchThreadGroup(roundedDims);
		}
	});

	mythril::Shader luminanceConversionShader = ctx->createShader({
		.filePath = kDataDir /  "shaders/ConversionLuminance.slang",
		.debugName = "Luminance Conversion Compute Shader"
	});
	mythril::ComputePipeline conversionLuminancePipeline = ctx->createComputePipeline({
		.shader = luminanceConversionShader.handle(),
		.debugName = "Luminance Conversion Pipeline"
	});
	mythril::Texture currentLuminanceTex = ctx->createTexture({
		.dimension = {1, 1, 1},
		.format = VK_FORMAT_R8_UNORM,
		.usage = mythril::TextureUsageBits::TextureUsageBits_Storage,
		.components = red_swizzle,
		.debugName = "Luminance Current Texture"
	});
	graph.addComputePass("convert into current lum")
	.dependency(offscreenColorTexs[7], mythril::Layout::READ)
	.dependency(currentLuminanceTex, mythril::Layout::GENERAL)
	.setExecuteCallback([&](mythril::CommandBuffer& cmd) {
		cmd.cmdBindComputePipeline(conversionLuminancePipeline);
		struct PushConstant {
			uint64_t lowResColorTex;
			uint64_t curLumOutTex;
			uint64_t sampler;
		} push {
			.lowResColorTex = offscreenColorTexs[7].index(),
			.curLumOutTex = currentLuminanceTex.index(),
			.sampler = 0
		};
		cmd.cmdPushConstants(push);
		mythril::Dimensions texDims = offscreenColorTexs[7]->getDimensions();
		glm::vec2 threadsPerGroup = {16, 16};
		uint32_t groupX = (texDims.width + threadsPerGroup.x - 1) / threadsPerGroup.x;
		uint32_t groupY = (texDims.height + threadsPerGroup.y - 1) / threadsPerGroup.y;
		cmd.cmdDispatchThreadGroup({groupX, groupY, 1});
	});

	mythril::Shader upsampleComputeShader = ctx->createShader({
		.filePath = kDataDir / "shaders/Upsample.slang",
		.debugName = "Upsample Compute Shader"
	});
	mythril::ComputePipeline upsampleComputePipeline = ctx->createComputePipeline({
		.shader = upsampleComputeShader.handle(),
		.debugName = "Upsample Compute Pipeline"
	});
	float filter_radius = 0.002f;
	float blend = 1.f;
	graph.addComputePass("Upsampling")
	.dependency(&offscreenColorTexs[0], kNumColorMips)
	.setExecuteCallback([&](mythril::CommandBuffer& cmd) {
		cmd.cmdBindComputePipeline(upsampleComputePipeline);
		for (int i = kNumColorMips-1; i > 0; i--) {
			mythril::Dimensions outdims = offscreenColorTexs[i-1]->getDimensions();
			struct PushConstants {
				uint64_t inTex;
				uint64_t outTex;
				uint64_t sampler;
				glm::ivec2 outSize;
				float filterRadius;
			} push {
				.inTex = offscreenColorTexs[i].index(),
				.outTex = offscreenColorTexs[i-1].index(),
				.sampler = clampSampler.index(),
				.outSize = {outdims.width, outdims.height},
				.filterRadius = filter_radius,
			};
			cmd.cmdPushConstants(push);
			static constexpr uint32_t threadNum = 8;
			const mythril::Dimensions texDims = offscreenColorTexs[i-1]->getDimensions();
			const mythril::Dimensions roundedDims = {
				(texDims.width + threadNum - 1) / threadNum,
				(texDims.height + threadNum - 1) / threadNum,
				1};
			cmd.cmdDispatchThreadGroup(roundedDims);
		}
	});

	mythril::Shader adaptationShader = ctx->createShader({
		.filePath = kDataDir / "shaders/Adaptation.slang",
		.debugName = "Adaptation Compute Shader"
	});
	mythril::ComputePipeline adaptationComputePipeline = ctx->createComputePipeline({
		.shader = adaptationShader.handle(),
		.debugName = "Adaptation Compute Pipeline"
	});

	float adaptationSpeed = 1.5f;
	auto st = std::chrono::steady_clock::now();
	graph.addComputePass("adaptation")
	.dependency(currentLuminanceTex, mythril::Layout::GENERAL)
	.dependency(adaptedLuminanceTextures[0], mythril::Layout::GENERAL)
	.dependency(adaptedLuminanceTextures[1], mythril::Layout::GENERAL)
	.setExecuteCallback([&](mythril::CommandBuffer& cmd) {
		cmd.cmdBindComputePipeline(adaptationComputePipeline);
	    const auto ct = std::chrono::steady_clock::now();
		const float dt = std::chrono::duration<float>(ct - st).count();
		st = ct;

		struct PushConstant {
			uint64_t currentSceneLumTex;
			uint64_t prevAdaptedLumTex;
			uint64_t nextAdaptedLumTex;
			float adaptionSpeed;
		} push {
			.currentSceneLumTex = currentLuminanceTex.index(),
			.prevAdaptedLumTex = adaptedLuminanceTextures[0].index(),
			.nextAdaptedLumTex = adaptedLuminanceTextures[1].index(),
			.adaptionSpeed = adaptationSpeed * dt
		};
		cmd.cmdPushConstants(push);
		cmd.cmdDispatchThreadGroup({1, 1});
	});


	// should be pretty low we crrently dont handle this well
	float bloom_strength = 0.135;
	float exposure_final = 1.f;
	enum class ToneMappingMode : int {
		None = 0,
		Reinhard = 1,
		Uchimira = 2,
		KhronosPBR = 3,
		AgX = 4
	} tone_mapping_mode = ToneMappingMode::KhronosPBR;
	float maxWhite = 1.5f;
	float P = 1.f;
	float a = 1.05f;
	float m = 0.1f;
	float l = 0.8f;
	float c = 3.0f;
	float b = 0.0f;
	float startCompression = 0.15f;
	float desaturation = 0.15f;
	glm::vec3 slope = {1.f, 1.f, 1.f};
	glm::vec3 offset = {0.f, 0.f, 0.f};
	glm::vec3 power = {1.f, 1.f, 1.f};
	float saturation = 1.f;
	graph.addGraphicsPass("composite")
	.attachment({
		.texDesc = finalColorTarget,
		.clearValue = {0, 0, 0, 1},
		.loadOp = mythril::LoadOp::CLEAR,
		.storeOp = mythril::StoreOp::STORE
	})
	.dependency(offscreenColorTarget, mythril::Layout::READ)
	.dependency(offscreenColorTexs[0], mythril::Layout::READ)
	.dependency(adaptedLuminanceTextures[1], mythril::Layout::READ)
	.setExecuteCallback([&](mythril::CommandBuffer& cmd) {
		cmd.cmdBeginRendering();
		cmd.cmdBindGraphicsPipeline(fullscreenCompositePipeline);
		struct PushConstant { // 32 + 48 = 80 bytes total
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
		} push {
			.colorTex = offscreenColorTarget.index(),
			.avgLuminanceTex = adaptedLuminanceTextures[0].index(),
			.bloomTex = offscreenColorTexs[0].index(),
			.sampler = 0,
			.exposure = exposure_final,
			.bloomStrength = bloom_strength,
			.toneMapMode = static_cast<int>(tone_mapping_mode),

			.maxWhite = maxWhite,
			.P = P,
			.a = a,
			.m = m,
			.l = l,
			.c = c,
			.b = b,
			.startCompression = startCompression,
			.desaturation = desaturation,
			.slope = slope,
			.offset = offset,
			.power = power,
			.saturation = saturation
		};
		cmd.cmdPushConstants(push);
		cmd.cmdDraw(3);
		cmd.cmdEndRendering();
	});

	graph.addGraphicsPass("shadow_pos_debug")
	.attachment({
		.texDesc = finalColorTarget,
		.loadOp = mythril::LoadOp::LOAD,
		.storeOp = mythril::StoreOp::STORE
	})
	.setExecuteCallback([&](mythril::CommandBuffer& cmd) {
		cmd.cmdBeginRendering();
		cmd.cmdBindGraphicsPipeline(redDebugPipeline);
		cmd.cmdBindDepthState({mythril::CompareOp::Less, true});

		glm::mat4 model = glm::translate(glm::mat4(1.0), scene_center);
		model = model * glm::mat4_cast(sun_quaternion);
		model = glm::rotate(model, 90.f, glm::vec3(1, 1, 1));
		model = glm::rotate(model, 180.f, glm::vec3(0, 1, 0));

		for (const auto& mesh : arrowCompiled.meshes) {
			struct PushConstant {
				glm::mat4 model;
				VkDeviceAddress vba;
			} push {
				.model = model,
				.vba = ctx->gpuAddress(mesh.vertexBufHandle)
			};
			cmd.cmdPushConstants(push);
			cmd.cmdBindIndexBuffer(mesh.indexBufHandle);
			cmd.cmdDrawIndexed(mesh.indexCount);
		}
		cmd.cmdEndRendering();
	});

	graph.addGraphicsPass("gui")
	.attachment({
		.texDesc = finalColorTarget,
		.loadOp = mythril::LoadOp::LOAD,
		.storeOp = mythril::StoreOp::STORE
	})
	.dependency(shadowMap, mythril::Layout::READ)
	.dependency(adaptedLuminanceTextures[0], mythril::Layout::READ)
	.dependency(&offscreenColorTexs[0], kNumColorMips, mythril::Layout::READ)
	.setExecuteCallback([&](mythril::CommandBuffer& cmd) {
		cmd.cmdBeginRendering();
		cmd.cmdDrawImGui();
		cmd.cmdEndRendering();
	});

	graph.addIntermediate("present")
	.blit(finalColorTarget, ctx->getBackBufferTexture())
	.finish();
	graph.compile(*ctx);


	// todo: right now if a shader is used across multiple pipelines, its descriptor sets have to be updated for each one
	{
		mythril::DescriptorSetWriter writer = ctx->openDescriptorUpdate(opaquePipeline);
		writer.updateBinding(frameDataHandle, "frame");
		ctx->submitDescriptorUpdate(writer);
	}
	{
		mythril::DescriptorSetWriter writer = ctx->openDescriptorUpdate(transparentPipeline);
		writer.updateBinding(frameDataHandle, "frame");
		ctx->submitDescriptorUpdate(writer);
	}
	{
		mythril::DescriptorSetWriter writer = ctx->openDescriptorUpdate(redDebugPipeline);
		writer.updateBinding(frameDataHandle, "frame");
		ctx->submitDescriptorUpdate(writer);
	}
	{
		mythril::DescriptorSetWriter writer = ctx->openDescriptorUpdate(particle_pipeline);
		writer.updateBinding(frameDataHandle, "frame");
		ctx->submitDescriptorUpdate(writer);
	}


	static bool focused = false;
	ctx->getWindow().setMouseMode(focused);
	constexpr float kMouseSensitivity = 0.3f;
	constexpr float kBaseCameraSpeed = 8.f;
	constexpr float kShiftCameraSpeed = kBaseCameraSpeed * 3.f;
	glm::vec3 camera_position = {0.f, 5.f, 0.f};


	lightingData.environmentLight.color     = {0.25f, 0.30f, 0.35f};
	lightingData.environmentLight.intensity = 0.3f;

	auto startTime = std::chrono::steady_clock::now();
	bool quit = false;
	while (!quit) {
		static auto lastTime = std::chrono::steady_clock::now();
		auto now = std::chrono::steady_clock::now();
		const float deltaTime = std::chrono::duration<float>(now - lastTime).count();
		const float elapsedTime = std::chrono::duration<float>(now - startTime).count();
		lastTime = now;

		if (ctx->isSwapchainDirty()) {
			ctx->recreateSwapchainStandard();

			const mythril::Window &window = ctx->getWindow();
			const auto [width, height] = window.getFramebufferSize();
			mythril::Dimensions new_2D_dimensions = {width, height, 1};
			msaaColorTarget.resize(new_2D_dimensions);
			offscreenColorTarget.resize(new_2D_dimensions);
			msaaDepthTarget.resize(new_2D_dimensions);
			depthTarget.resize(new_2D_dimensions);
			finalColorTarget.resize(new_2D_dimensions);
			
			for (auto& offscreenTexs : offscreenColorTexs) {
				offscreenTexs.resize(new_2D_dimensions);
				new_2D_dimensions = new_2D_dimensions.divide2D(2);
			}
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

		const VkExtent2D windowSize = ctx->getWindow().getWindowSize();
		const Camera camera = {
			.position = camera_position,
			.forwardVector = front,
			.upVector = up,
			.aspectRatio = static_cast<float>(windowSize.width) / static_cast<float>(windowSize.height),
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
			glm::vec4 view = invProj * ndc;
			glm::vec3 viewPos = glm::vec3(view) / view.w;
			return viewPos / viewPos.z;
		};
		glm::mat4 invProj = glm::inverse(reversedCameraProj);
		glm::vec3 view00 = UVToViewRay({0,0}, invProj);
		glm::vec3 view11 = UVToViewRay({1,1}, invProj);
		glm::vec2 ray00 = { view00.x / view00.z, view00.y / view00.z };
		glm::vec2 ray11 = { view11.x / view11.z, view11.y / view11.z };
		const GPU::CameraData cameraData = {
			.projView = reversedCameraProj * cameraView,
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
		ImGui::Image(shadowMap, {200, 200});
		ImGui::Text("Average Luminance");
		ImGui::Image(adaptedLuminanceTextures[0], {100, 100});
		ImGui::Text("Progressively Blurred Color Targets");
		for (int face = 0; face < 6; face++) {
			// ImGui::Image(pointLightShadowTex[0], pointLightShadowTex[0].getView(0, face), {200, 200});
		}
		for (int i = 0; i < kNumColorMips; i++) {
			mythril::Dimensions dims = {320, 240};
			ImGui::Image(offscreenColorTexs[kNumColorMips-1-i], {static_cast<float>(dims.width), static_cast<float>(dims.height)});
		}
		ImGui::End();

		ImGui::Begin("Lighting", nullptr, window_flags);
		ImGui::DragFloat("Point Light Near", &pointlight_nearplane);
		ImGui::DragFloat("Point Light Far", &pointlight_farplane);
		ImGui::DragFloat("Particle Speed", &particle_speed);
		ImGui::DragFloat("Particle Emission", &particle_emission, 0.1f);
		ImGui::DragFloat("Upsample Blending", &blend, 0.001f);
		ImGui::DragFloat("Filter Radius", &filter_radius, 0.001f, -1.f, 1.f);
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
				ImGui::DragFloat("Max White", &maxWhite, 0.01f, 0.0001f, 10.f);
			} break;
			case ToneMappingMode::Uchimira: {
				ImGui::Text("Uchimura Options");
				ImGui::DragFloat("Max Display Brightness", &P, 0.01f, 0.0001f, 10.f);
				ImGui::DragFloat("Contrast", &a, 0.01f, 0.0001f, 10.f);
				ImGui::DragFloat("Linear Section Start", &m, 0.01f, 0.0001f, 10.f);
				ImGui::DragFloat("Linear Section Length", &l, 0.01f, 0.0001f, 10.f);
				ImGui::DragFloat("Black Tighness", &c, 0.01f, 0.0001f, 10.f);
				ImGui::DragFloat("Pedestal", &b, 0.01f, 0.0001f, 10.f);
			} break;
			case ToneMappingMode::KhronosPBR: {
				ImGui::Text("KhronosPBR Options");
				ImGui::DragFloat("Highlight Compression Start", &startCompression, 0.01f, 0.04f, 0.8f);
				ImGui::DragFloat("Desaturation", &desaturation, 0.01f, 0.001f, 1.f);
			} break;
			case ToneMappingMode::AgX: {
				ImGui::Text("AgX Options");
				ImGui::SliderFloat3("Slope", &slope[0], 0.f, 1.f);
				ImGui::SliderFloat3("Offset", &offset[0], 0.f, 1.f);
				ImGui::SliderFloat3("Power", &power[0], 0.f, 1.f);
				ImGui::DragFloat("Saturation", &saturation, 0.1f, 0.1f, 10.f);
			} break;
		}

		ImGui::End();
		DrawLightingUI(lightingData);
		ImGui::Begin("Information");
		{
			ImGui::Text("Frame %d", ImGui::GetFrameCount());
			ImGui::Text("Framerate: %.0f", ImGui::GetIO().Framerate);
			ImGui::Text("ImGui Backend: %s", ImGui::GetIO().BackendRendererName);
			ImGui::Text("Display Size: %.0fx%.0f", ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y);
			ImGui::Text("Display Framebuffer Scale: %.0fx%.0f", ImGui::GetIO().DisplayFramebufferScale.x, ImGui::GetIO().DisplayFramebufferScale.y);
			ImGui::Text("Framebuffer Size: %dx%d", ctx->getWindow().getFramebufferSize().width, ctx->getWindow().getFramebufferSize().height);
		}
		ImGui::End();

		ImGuizmo::BeginFrame();
		glm::mat4 transformMatrix = glm::translate(glm::mat4(1.f), scene_center) *
									glm::mat4_cast(sun_quaternion);


		ImGuizmo::SetOrthographic(false);
		ImGuizmo::SetDrawlist(ImGui::GetBackgroundDrawList());
		ImGuizmo::SetRect(0, 0, windowSize.width, windowSize.height);
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

		lightingData.directionalLights.direction = sun_quaternion * glm::vec3(0, 0, -1);
		const GPU::FrameData frameData {
			.camera = cameraData,
			.lighting = lightingData,
			.time = elapsedTime,
			.deltaTime = deltaTime
		};

		UpdateParticles(particles, deltaTime, particle_speed);
		UpdatePointLightShadowMatrices(frameData.lighting, kMODELSCALE, pointlight_nearplane, pointlight_farplane, shadowMatrices);

		mythril::CommandBuffer& cmd = ctx->openCommand(mythril::CommandBuffer::Type::Graphics);
		cmd.cmdUpdateBuffer(frameDataHandle, frameData);
		// because the data we want to store in the particle buffer is so large, we need to upload it like this
		ctx->upload(particles_buffer.handle(), particles.data(), sizeof(ParticleData) * particles.size(), 0);
		ctx->upload(pointLightShadowMatrixBuf.handle(), &shadowMatrices[0][0], sizeof(glm::mat4) * kNumPointLights * 6, 0);
		graph.execute(cmd);
		ctx->submitCommand(cmd);
		lastFramelightingData = lightingData;
		std::swap(adaptedLuminanceTextures[0], adaptedLuminanceTextures[1]);
	}
	return 0;
}
