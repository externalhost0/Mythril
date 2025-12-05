//
// Created by Hayden Rivas on 12/2/25.
//

#include "mythril/CTXBuilder.h"

#include "GPUStructs.h"
#include "SDL3/SDL_events.h"

#include <vector>
#include <filesystem>

#include <glm/glm.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/ext/matrix_clip_space.hpp>

#include <fastgltf/core.hpp>
#include <fastgltf/types.hpp>
#include <fastgltf/glm_element_traits.hpp>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

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
	int baseColorTextureIndex = -1;
	int normalTextureIndex = -1;
	int roughnessTextureIndex = -1;
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

	auto asset_result = parser.loadGltf(data, filepath.parent_path(), options);
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
		}
		if (material.normalTexture) {
			size_t texture_index = material.normalTexture->textureIndex;
			material_data.normalTextureIndex = gltf.textures[texture_index].imageIndex.value();
		}
		if (material.pbrData.metallicRoughnessTexture) {
			size_t texture_index = material.pbrData.metallicRoughnessTexture->textureIndex;
			material_data.roughnessTextureIndex = gltf.textures[texture_index].imageIndex.value();
		}
		auto& base_color = material.pbrData.baseColorFactor;
		material_data.baseColorFactor = {base_color.x(), base_color.y(), base_color.z(), base_color.w()};

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
			// load material index
			int material_index = primitive.materialIndex.value();
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
	return AssetData{meshes, std::move(textures), materials};
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
	for (TextureData& texture_data : data.textureData) {
		char texture_name_buf[64];
		snprintf(texture_name_buf, sizeof(texture_name_buf), "Sponza Image %d", i);

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
	.with_ImGui()
	.build();

	

	VkExtent2D extent2D = ctx->getWindow().getFramebufferSize();
	mythril::InternalTextureHandle colorTarget = ctx->createTexture({
		.dimension = extent2D,
		.samples = mythril::SampleCount::X1,
		.usage = mythril::TextureUsageBits::TextureUsageBits_Attachment | mythril::TextureUsageBits_Sampled,
		.storage = mythril::StorageType::Device,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.debugName = "Color Texture"
	});
	mythril::InternalTextureHandle depthTarget = ctx->createTexture({
		.dimension = extent2D,
		.samples = mythril::SampleCount::X1,
		.usage = mythril::TextureUsageBits::TextureUsageBits_Attachment,
		.format = VK_FORMAT_D32_SFLOAT_S8_UINT,
		.debugName = "Depth Texture"
	});

	mythril::InternalShaderHandle standardShader = ctx->createShader({
		.filePath = "Standard.slang",
		.debugName = "Standard Shader"
	});
	mythril::InternalGraphicsPipelineHandle standardPipeline = ctx->createGraphicsPipeline({
		.vertexShader = {standardShader},
		.fragmentShader = {standardShader},
		.blend = mythril::BlendingMode::OFF,
		.cull = mythril::CullMode::BACK,
		.debugName = "Standard Graphics Pipeline"
	});


	// load gltf asset
	AssetData sponzaData = loadGLTFAsset("KhronosGroup_glTF-Sample-Assets_Models-Sponza/glTF/Sponza.gltf");
	AssetCompiled sponzaCompiled = compileGLTFAsset(*ctx, sponzaData);

	mythril::RenderGraph graph;
	graph.addPass("geometry", mythril::PassSource::Type::Graphics)
	.write({
		.texture = colorTarget,
		.clearValue = {0.2f, 0.2f, 0.2f, 1.f},
		.loadOp = mythril::LoadOperation::CLEAR,
		.storeOp = mythril::StoreOperation::STORE
	})
	.write({
		.texture = depthTarget,
		.clearValue = {1.f, 0},
		.loadOp = mythril::LoadOperation::CLEAR
	})
	.setExecuteCallback([&](mythril::CommandBuffer& cmd) {
		cmd.cmdBindRenderPipeline(standardPipeline);
		cmd.cmdBindDepthState({mythril::CompareOperation::CompareOp_Less, true});
		auto model = glm::mat4(1.0);

		// what we call a mesh is really a primitive for fastgltf
		for (const MeshCompiled& mesh : sponzaCompiled.meshes) {
			MaterialData material = sponzaCompiled.materials[mesh.materialIndex];
			mythril::InternalTextureHandle baseColorTex = sponzaCompiled.textureHandles[material.baseColorTextureIndex];

			GPU::GeometryPushConstants push {
					.model = model,
					.vba = ctx->gpuAddress(mesh.vertexBufHandle),
					.tintColor = {1, 1, 1, 1},
					.baseColorTexture = baseColorTex.index(),
					.samplerState = 0
			};
			cmd.cmdPushConstants(push);
			cmd.cmdBindIndexBuffer(mesh.indexBufHandle);
			cmd.cmdDrawIndexed(mesh.indexCount);
		}
	});
	graph.compile(*ctx);

	mythril::InternalBufferHandle frameDataHandle = ctx->createBuffer({
		.size = sizeof(GPU::FrameData),
		.usage = mythril::BufferUsageBits::BufferUsageBits_Uniform,
		.storage = mythril::StorageType::Device,
		.debugName = "frameData Buffer"
	});

	mythril::DescriptorSetWriter writer = ctx->openUpdate(standardPipeline);
	writer.updateBinding(frameDataHandle, "frame");
	ctx->submitUpdate(writer);

	auto startTime = std::chrono::high_resolution_clock::now();

	static bool focused = true;
	ctx->getWindow().setMouseMode(focused);
	constexpr float kMouseSensitivity = 0.3f;
	constexpr float kBaseCameraSpeed = 100.f;
	constexpr float kShiftCameraSpeed = kBaseCameraSpeed * 3.f;
	glm::vec3 position = {0.f, 0.f, 5.f};

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
			if (e.type == SDL_EVENT_QUIT) quit = true;
			if (e.type == SDL_EVENT_KEY_DOWN) {
				if (e.key.key == SDLK_Q) quit = true;
				if (e.key.key == SDLK_C) ctx->getWindow().setMouseMode(focused = !focused);
			}
			if (e.type == SDL_EVENT_MOUSE_MOTION) {
				auto dx = (float)e.motion.xrel;
				auto dy = (float)e.motion.yrel;
				yaw   += dx * kMouseSensitivity;
				pitch -= dy * kMouseSensitivity;
				pitch = glm::clamp(pitch, -89.0f, 89.0f);
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

		if (state[SDL_SCANCODE_W]) position += front * camera_speed * deltaTime;
		if (state[SDL_SCANCODE_S]) position -= front * camera_speed * deltaTime;
		if (state[SDL_SCANCODE_A]) position -= right * camera_speed * deltaTime;
		if (state[SDL_SCANCODE_D]) position += right * camera_speed * deltaTime;
		if (state[SDL_SCANCODE_SPACE]) position += up * camera_speed * deltaTime;
		if (state[SDL_SCANCODE_LCTRL]) position -= up * camera_speed * deltaTime;

		const VkExtent2D windowSize = ctx->getWindow().getWindowSize();
		const Camera camera = {
				.position = position,
				.forwardVector = front,
				.upVector = up,
				.aspectRatio = (float) windowSize.width / (float) windowSize.height,
				.fov = 80.f,
				.nearPlane = 0.1f,
				.farPlane = 2500.f
		};
		const GPU::CameraData cameraData = {
				.proj = calculateProjectionMatrix(camera),
				.view = calculateViewMatrix(camera),
				.position = camera.position,
				.far = camera.farPlane,
				.near = camera.nearPlane
		};
		const GPU::FrameData frameData {
			.camera = cameraData
		};

		mythril::CommandBuffer& cmd = ctx->openCommand(mythril::CommandBuffer::Type::Graphics);
		cmd.cmdUpdateBuffer(frameDataHandle, frameData);
		graph.execute(cmd);
		ctx->submitCommand(cmd);
	}

	return 0;
}

