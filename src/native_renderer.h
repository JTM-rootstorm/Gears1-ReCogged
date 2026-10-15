// Interface between the native renderer's modules (NATIVE_RENDERER.md):
// src/native_renderer.cpp owns the SDL_GPU device and the frame, the
// resource modules decode guest resources and upload them.
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
