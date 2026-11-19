#pragma once
// ============================================================================
// OPENGL ES TEXTURE LOADER FROM IN-MEMORY PNG / JPEG BYTES
// ============================================================================
// Project: crack_master
//
// SYSTEM ROLE:
// Decodes raw in-memory image bytes (e.g. from anime.hpp or trick.hpp) using stb_image
// and generates a standard OpenGL ES 2D texture (GL_TEXTURE_2D) for ImGui::Image rendering.
// ============================================================================

#include <cstdint>
#include <cstddef>
#include <GLES2/gl2.h> 

namespace ui::image {
    bool LoadTextureFromMemory(const uint8_t* data, size_t data_size, GLuint* out_texture, int* out_width, int* out_height);
}

