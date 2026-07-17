#include "gui/mac_scroll_bridge.hpp"

#import <AppKit/AppKit.h>

namespace cind::gui {

void* install_mac_scroll_monitor(void* native_window, MacScrollCallback callback, void* context) {
    if (native_window == nullptr || callback == nullptr) {
        return nullptr;
    }

    NSWindow* window = (__bridge NSWindow*)native_window;
    id monitor = [NSEvent
        addLocalMonitorForEventsMatchingMask:NSEventMaskScrollWheel
                                    handler:^NSEvent*(NSEvent* event) {
                                      if (event.window != window) {
                                          return event;
                                      }
                                      float x = static_cast<float>(event.scrollingDeltaX);
                                      float y = static_cast<float>(event.scrollingDeltaY);
                                      if (event.directionInvertedFromDevice) {
                                          x = -x;
                                          y = -y;
                                      }
                                      callback({.x = x,
                                                .y = y,
                                                .precise = event.hasPreciseScrollingDeltas},
                                               context);
                                      return nil;
                                    }];
    return (__bridge_retained void*)monitor;
}

void uninstall_mac_scroll_monitor(void* monitor) {
    if (monitor == nullptr) {
        return;
    }
    id token = (__bridge_transfer id)monitor;
    [NSEvent removeMonitor:token];
}

} // namespace cind::gui
