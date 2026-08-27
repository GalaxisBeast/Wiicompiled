#pragma once

#include <webgpu/webgpu_cpp.h>

namespace aurora::openxr {
bool initialize() noexcept;
void shutdown() noexcept;
bool active() noexcept;
// Submits the completed desktop image on a playspace-anchored cinema quad.
// This is the first functional compositor path; stereo GX replay is layered on
// this interface rather than changing OpenXR session ownership.
void present(wgpu::BindGroup source) noexcept;
} // namespace aurora::openxr
