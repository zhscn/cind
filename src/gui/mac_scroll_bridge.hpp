#pragma once

namespace cind::gui {

struct MacScrollDelta {
    float x = 0.0F;
    float y = 0.0F;
    bool precise = false;
};

using MacScrollCallback = void (*)(MacScrollDelta delta, void* context);

void* install_mac_scroll_monitor(void* native_window, MacScrollCallback callback, void* context);
void uninstall_mac_scroll_monitor(void* monitor);

} // namespace cind::gui
