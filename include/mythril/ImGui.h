//
// Created by Hayden Rivas on 11/21/26.
//

#pragma once

#ifdef MYTH_ENABLED_IMGUI
#include <imgui.h>

#include "Objects.h"
#include "RenderGraphDescriptions.h"
namespace ImGui {
	void Image(const mythril::Texture& texture, const ImVec2& image_size = {0, 0}, const ImVec2& uv0 = {0, 0}, const ImVec2& uv1 = {1, 1});
	void Image(const mythril::Texture& texture, const mythril::Sampler& sampler, const ImVec2& image_size = {0, 0}, const ImVec2& uv0 = {0, 0}, const ImVec2& uv1 = {1, 1});
	void Image(mythril::TextureHandle texHandle, const ImVec2& image_size = {0, 0}, const ImVec2& uv0 = {0, 0}, const ImVec2& uv1 = {1, 1});
	void Image(mythril::TextureHandle texHandle, mythril::SamplerHandle samplerHandle, const ImVec2& image_size, const ImVec2& uv0 = {0, 0}, const ImVec2& uv1 = {1, 1});
	void Image(const mythril::Texture& texture, mythril::TextureViewKey viewKey, const ImVec2& image_size = {0, 0}, const ImVec2& uv0 = {0, 0}, const ImVec2& uv1 = {1, 1});
	void Image(mythril::GraphTexture texture, const ImVec2& image_size, const ImVec2& uv0 = {0, 0}, const ImVec2& uv1 = {1, 1});
} // namespace ImGui
#endif
