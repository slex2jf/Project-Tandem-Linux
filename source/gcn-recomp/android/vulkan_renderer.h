#pragma once
#include <android/native_window.h>
#include "gx/render.h"

namespace gx::vk {
bool init(ANativeWindow* window, uint32_t width, uint32_t height);
bool draw(Batch& batch);
void resize(ANativeWindow* window, uint32_t width, uint32_t height);
void shutdown();
}
