#pragma once

#include "document/text_types.hpp"
#include "editor/buffer.hpp"
#include "editor/ids.hpp"
#include "editor/settings.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace cind {

struct ViewportState {
    // The first intersecting document line and the fraction of that line
    // clipped above the viewport. Together they form a continuous vertical
    // scroll position in line-height units.
    std::uint32_t top_line = 0;
    float top_line_offset = 0.0F;
    int left_column = 0;
    std::optional<int> preferred_column;
};

struct SelectionEndpoints {
    TextOffset anchor;
    TextOffset head;
};

class View {
public:
    ViewId id() const { return id_; }
    BufferId buffer_id() const { return buffer_id_; }
    ViewportState& viewport() { return viewport_; }
    const ViewportState& viewport() const { return viewport_; }
    SettingsLayer& settings() { return settings_; }
    const SettingsLayer& settings() const { return settings_; }
    std::vector<KeymapId>& keymaps() { return keymaps_; }
    const std::vector<KeymapId>& keymaps() const { return keymaps_; }
    std::uint32_t attached_window_count() const { return attached_windows_; }

private:
    friend class ViewRegistry;
    friend class WindowRegistry;

    View(ViewId id, BufferId buffer_id, AnchorId caret, const SettingRegistry& settings)
        : id_(id), buffer_id_(buffer_id), caret_(caret), settings_(settings, SettingScope::View) {}

    ViewId id_;
    BufferId buffer_id_;
    AnchorId caret_ = 0;
    std::optional<AnchorId> mark_;
    ViewportState viewport_;
    SettingsLayer settings_;
    std::vector<KeymapId> keymaps_;
    std::uint32_t attached_windows_ = 0;
};

class ViewRegistry {
public:
    ViewRegistry(BufferRegistry& buffers, const SettingRegistry& settings)
        : buffers_(&buffers), settings_(&settings) {}
    ~ViewRegistry();

    ViewRegistry(const ViewRegistry&) = delete;
    ViewRegistry& operator=(const ViewRegistry&) = delete;

    ViewId create(BufferId buffer, TextOffset caret = {});
    bool erase(ViewId id);

    View& get(ViewId id);
    const View& get(ViewId id) const;
    View* try_get(ViewId id);
    const View* try_get(ViewId id) const;

    TextOffset caret(ViewId id) const;
    void set_caret(ViewId id, TextOffset caret);
    std::optional<TextOffset> mark(ViewId id) const;
    std::optional<TextRange> selection(ViewId id) const;
    void set_selection(ViewId id, SelectionEndpoints selection);
    void clear_selection(ViewId id);

private:
    struct Slot {
        std::uint32_t generation = 1;
        std::unique_ptr<View> value;
    };

    void remove_anchors(View& view);
    AnchorId make_anchor(Buffer& buffer, TextOffset offset, AnchorAffinity affinity);

    BufferRegistry* buffers_;
    const SettingRegistry* settings_;
    std::vector<Slot> slots_;
    std::vector<std::uint32_t> free_slots_;
};

} // namespace cind
