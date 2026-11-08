// Interface between the native renderer's modules (NATIVE_RENDERER.md):
// src/native_renderer.cpp owns the SDL_GPU device and the frame, the
// resource modules decode guest resources and upload them, and
// src/native_targets.cpp keeps the render targets.
#pragma once

#include <SDL3/SDL.h>

#include <rex/cvar.h>

REXCVAR_DECLARE(bool, gears1_native_renderer);

// Main thread: the native renderer's window, or null.
SDL_Window* NativeRendererWindow();

// Main thread: creates and uploads the textures decoded since the last call,
// in a copy pass on command_buffer (before any render pass).
void NativeTexturesUpload(SDL_GPUDevice* device, SDL_GPUCommandBuffer* command_buffer);
// Main thread, inside an ImGui frame: the texture browser window.
void NativeTexturesDrawBrowser();

// Guest thread, after each guest frame: publishes the frame's draws.
void NativeBuffersEndFrame();
// Main thread: creates and uploads the vertex and index buffers captured since
// the last call, in a copy pass on command_buffer (before any render pass).
void NativeBuffersUpload(SDL_GPUDevice* device, SDL_GPUCommandBuffer* command_buffer);
// Main thread, inside an ImGui frame: the draw list and preview windows.
void NativeBuffersDrawBrowser();
// Main thread, after the ImGui frame and before the window's render pass:
// renders the selected draw's preview.
void NativeBuffersRenderPreview(SDL_GPUDevice* device, SDL_GPUCommandBuffer* command_buffer);

// Guest thread, after each guest frame (D3D Swap, whose front buffer texture is
// front_buffer): publishes the frame's render target operations.
void NativeTargetsEndFrame(uint32_t front_buffer, uint8_t* base);
// Guest thread: whether a Resolve has written this texture (its address as in
// the fetch constant, format and base level size), so it is not decoded from
// guest memory.
bool NativeTargetsIsResolved(uint32_t address, uint32_t format, uint32_t width, uint32_t height);
// Main thread: replays the render target operations of the guest frames since
// the last call (render passes on command_buffer).
void NativeTargetsExecute(SDL_GPUDevice* device, SDL_GPUCommandBuffer* command_buffer);
// Main thread: draws the last replayed frame's front buffer into the
// swapchain; false if there is none yet.
bool NativeTargetsPresent(SDL_GPUDevice* device, SDL_GPUCommandBuffer* command_buffer,
                          SDL_GPUTexture* swapchain, SDL_GPUTextureFormat format, uint32_t width,
                          uint32_t height);
// Main thread, gears1_native_dump_dir: records a download of the front buffer
// on command_buffer when one is due; then, after the command buffer has
// completed, NativeTargetsFinishDump writes it.
bool NativeTargetsQueueDump(SDL_GPUDevice* device, SDL_GPUCommandBuffer* command_buffer);
void NativeTargetsFinishDump(SDL_GPUDevice* device);
// Main thread, inside an ImGui frame: the render target browser.
void NativeTargetsDrawBrowser();
