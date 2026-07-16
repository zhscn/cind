#pragma once

#include "ui/style.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace cind::ui {

// Backend-independent frame model. Layout partitions the screen into regions;
// each region carries a stable identity, a surface class, a vertical anchoring
// policy, and exactly one content representation. Document regions expose a
// display list in local coordinates; chrome regions expose semantic content
// that each presenter lays out for its native coordinate system.

// Cell-unit rectangle in scene coordinates (0-based row/col). A GUI
// presenter maps cells to pixels with its font metrics.
struct Rect {
    int row = 0;
    int col = 0;
    int rows = 0;
    int cols = 0;
};

struct CellPoint {
    int row = 0;
    int column = 0;
};

enum class PrimKind : std::uint8_t {
    Text,
    ChangeBar,
    ChangeDeletion,
};

// One painted primitive at a region-local position. Text is clipped, tabs are
// expanded, and grapheme widths are measured before the scene is published.
struct Prim {
    Prim() = default;
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    Prim(int row, int col, std::string text, StyleClass style, bool selected,
         PrimKind kind = PrimKind::Text, std::string id = {})
        : row(row), col(col), text(std::move(text)), style(style), selected(selected), kind(kind),
          id(std::move(id)) {}

    int row = 0; // region-local
    int col = 0;
    std::string text;
    StyleClass style = StyleClass::Text;
    bool selected = false;
    PrimKind kind = PrimKind::Text;
    std::string id;
};

enum class SurfaceClass : std::uint8_t {
    Editor,
    Gutter,
    Status,
    Echo,
};

enum class RegionRole : std::uint8_t {
    TextArea,
    LineNumbers,
    ChangeSigns,
    StatusBar,
    EchoArea, // message / prompt line
    Popup,
};

enum class VerticalAnchor : std::uint8_t {
    Grid,
    Bottom,
    Overlay,
};

struct Region {
    struct DocumentMapping {
        std::uint32_t first_line = 0;
        std::optional<int> first_display_column;
    };
    struct PrimitiveContent {
        std::vector<Prim> items;
        std::optional<DocumentMapping> document;
    };
    struct PopupItem {
        std::string label;
        std::string detail;
    };
    struct PopupContent {
        std::string title;
        std::optional<std::string> input;
        std::optional<std::size_t> input_cursor;
        std::size_t first_item = 0;
        std::size_t total_items = 0;
        std::optional<std::size_t> selected_item;
        std::vector<PopupItem> items;
    };
    struct StatusContent {
        std::string path;
        bool dirty = false;
        std::uint32_t line = 0;
        std::uint32_t column = 0;
        std::uint32_t line_count = 0;
        std::uint64_t revision = 0;
        std::string style_origin;
        std::string key;
    };
    struct EchoContent {
        std::string text;
        // UTF-8 byte offset in `text`. A present value makes the echo area an
        // active input surface; presenters derive their native caret geometry
        // from this offset.
        std::optional<std::size_t> cursor_byte;
    };
    using Content = std::variant<PrimitiveContent, PopupContent, StatusContent, EchoContent>;

    Region() = default;
    Region(RegionRole role, Rect rect, std::vector<Prim> prims,
           SurfaceClass surface = SurfaceClass::Editor,
           VerticalAnchor vertical_anchor = VerticalAnchor::Grid, std::string id = {},
           std::uint64_t revision = 0)
        : role(role), rect(rect), surface(surface), vertical_anchor(vertical_anchor),
          id(std::move(id)), revision(revision),
          content(PrimitiveContent{.items = std::move(prims), .document = std::nullopt}) {}

    RegionRole role = RegionRole::TextArea;
    Rect rect;
    SurfaceClass surface = SurfaceClass::Editor;
    VerticalAnchor vertical_anchor = VerticalAnchor::Grid;
    std::string id;
    std::uint64_t revision = 0;
    Content content = PrimitiveContent{};

    std::vector<Prim>& primitives() {
        PrimitiveContent* primitives = std::get_if<PrimitiveContent>(&content);
        if (primitives == nullptr) {
            throw std::logic_error("semantic region has no primitive content");
        }
        return primitives->items;
    }
    const std::vector<Prim>& primitives() const {
        static const std::vector<Prim> empty;
        const PrimitiveContent* primitives = std::get_if<PrimitiveContent>(&content);
        return primitives != nullptr ? primitives->items : empty;
    }

    PopupContent* popup() { return std::get_if<PopupContent>(&content); }
    const PopupContent* popup() const { return std::get_if<PopupContent>(&content); }
    StatusContent* status() { return std::get_if<StatusContent>(&content); }
    const StatusContent* status() const { return std::get_if<StatusContent>(&content); }
    EchoContent* echo() { return std::get_if<EchoContent>(&content); }
    const EchoContent* echo() const { return std::get_if<EchoContent>(&content); }

    void set_popup(PopupContent popup) { content = std::move(popup); }
    void set_status(StatusContent status) { content = std::move(status); }
    void set_echo(EchoContent echo) { content = std::move(echo); }
    void set_document_mapping(DocumentMapping mapping) {
        std::get<PrimitiveContent>(content).document = mapping;
    }
    const DocumentMapping* document_mapping() const {
        const PrimitiveContent* primitives = std::get_if<PrimitiveContent>(&content);
        return primitives != nullptr && primitives->document ? &*primitives->document : nullptr;
    }

    std::size_t item_count() const {
        if (const PopupContent* popup_content = popup()) {
            return popup_content->items.size() + 1;
        }
        if (status() != nullptr || echo() != nullptr) {
            return 1;
        }
        return primitives().size();
    }
};

struct Scene {
    // Full frame geometry in cells. Grid regions may start at a fractional
    // row above the viewport; bottom-anchored regions remain fixed.
    int rows = 0;
    int cols = 0;
    float grid_offset_rows = 0.0F;

    // Zero-based scene row occupied by the document caret. This remains
    // available while an overlay owns the visible input cursor.
    std::optional<int> active_text_row;

    std::vector<Region> regions;

    // Terminal cursor, 1-based scene coordinates; compose computes it
    // (text caret or prompt input point).
    int cursor_row = 1;
    int cursor_col = 1;
    bool cursor_visible = true;

    const Region* find(RegionRole role) const {
        for (const Region& r : regions) {
            if (r.role == role) {
                return &r;
            }
        }
        return nullptr;
    }
};

} // namespace cind::ui
