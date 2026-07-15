#include "ui/scene_damage.hpp"

#include "ui/char_width.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace cind::ui {

namespace {

template <typename T> void append_integer(std::string& output, T value) {
    using Unsigned = std::make_unsigned_t<T>;
    Unsigned bits = static_cast<Unsigned>(value);
    for (std::size_t byte = 0; byte < sizeof(bits); ++byte) {
        output.push_back(static_cast<char>(bits & 0xFFU));
        bits >>= 8U;
    }
}

bool contains(const Rect& rect, int row, int col) {
    return row >= rect.row && row < rect.row + rect.rows && col >= rect.col &&
           col < rect.col + rect.cols;
}

bool same_rect(const std::optional<Rect>& left, const std::optional<Rect>& right) {
    if (left.has_value() != right.has_value()) {
        return false;
    }
    return !left || (left->row == right->row && left->col == right->col &&
                     left->rows == right->rows && left->cols == right->cols);
}

std::optional<Rect> overlay_rect(const Scene& scene) {
    for (const Region& region : scene.regions) {
        if (region.vertical_anchor == VerticalAnchor::Overlay) {
            return region.rect;
        }
    }
    return std::nullopt;
}

std::size_t cell_index(int row, int col, int cols) {
    return static_cast<std::size_t>(row) * static_cast<std::size_t>(cols) +
           static_cast<std::size_t>(col);
}

void append_fragment(std::string& signature, std::string_view text, StyleClass style, bool selected,
                     PrimKind kind, int glyph_cell, int glyph_width) {
    signature.push_back(static_cast<char>(style));
    signature.push_back(static_cast<char>(kind));
    signature.push_back(selected ? '\1' : '\0');
    append_integer(signature, glyph_cell);
    append_integer(signature, glyph_width);
    append_integer(signature, static_cast<std::uint32_t>(text.size()));
    signature.append(text);
}

std::vector<std::string> visual_cells(const Scene& scene) {
    const std::size_t size = static_cast<std::size_t>(std::max(0, scene.rows)) *
                             static_cast<std::size_t>(std::max(0, scene.cols));
    std::vector<std::string> cells(size, std::string{static_cast<char>(SurfaceClass::Editor),
                                                     static_cast<char>(VerticalAnchor::Grid)});
    if (scene.rows <= 0 || scene.cols <= 0) {
        return cells;
    }

    for (const Region& region : scene.regions) {
        const int first_row = std::clamp(region.rect.row, 0, scene.rows);
        const int last_row = std::clamp(region.rect.row + region.rect.rows, 0, scene.rows);
        const int first_col = std::clamp(region.rect.col, 0, scene.cols);
        const int last_col = std::clamp(region.rect.col + region.rect.cols, 0, scene.cols);
        for (int row = first_row; row < last_row; ++row) {
            for (int col = first_col; col < last_col; ++col) {
                cells[cell_index(row, col, scene.cols)] = {
                    static_cast<char>(region.surface), static_cast<char>(region.vertical_anchor)};
                if (scene.active_text_row == row && (region.role == RegionRole::TextArea ||
                                                     region.role == RegionRole::LineNumbers ||
                                                     region.role == RegionRole::ChangeSigns)) {
                    cells[cell_index(row, col, scene.cols)].push_back('\x7f');
                }
            }
        }

        for (const Prim& prim : region.prims) {
            const int row = region.rect.row + prim.row;
            int col = region.rect.col + prim.col;
            if (row < 0 || row >= scene.rows) {
                continue;
            }
            if (prim.kind != PrimKind::Text) {
                if (col >= 0 && col < scene.cols && contains(region.rect, row, col)) {
                    append_fragment(cells[cell_index(row, col, scene.cols)], {}, prim.style,
                                    prim.selected, prim.kind, 0, 1);
                }
                continue;
            }

            std::string_view text = prim.text;
            while (!text.empty()) {
                const GraphemeDecode grapheme = decode_grapheme(text);
                const std::string_view bytes =
                    text.substr(0, static_cast<std::size_t>(grapheme.bytes));
                const int width = std::max(1, grapheme.width);
                const int glyph_col =
                    grapheme.width == 0 ? std::max(region.rect.col, col - 1) : col;
                for (int part = 0; part < width; ++part) {
                    const int painted_col = glyph_col + part;
                    if (painted_col >= 0 && painted_col < scene.cols &&
                        contains(region.rect, row, painted_col)) {
                        append_fragment(cells[cell_index(row, painted_col, scene.cols)], bytes,
                                        prim.style, prim.selected, prim.kind, part, width);
                    }
                }
                col += grapheme.width;
                text.remove_prefix(static_cast<std::size_t>(grapheme.bytes));
            }
        }
        if (!region.popup) {
            continue;
        }
        if (region.rect.row >= 0 && region.rect.row < scene.rows && region.rect.col >= 0 &&
            region.rect.col < scene.cols) {
            const std::optional<std::string> popup_input = region.popup.and_then(
                [](const Region::PopupContent& popup) { return popup.input; });
            std::string& signature =
                cells[cell_index(region.rect.row, region.rect.col, scene.cols)];
            signature.push_back('\x1e');
            append_integer(signature, static_cast<std::uint32_t>(popup_input.has_value()));
            if (popup_input) {
                append_integer(signature, static_cast<std::uint32_t>(popup_input.value().size()));
                signature.append(popup_input.value());
            }
        }
    }
    return cells;
}

std::optional<CellPoint> visible_cursor(const Scene& scene) {
    if (!scene.cursor_visible) {
        return std::nullopt;
    }
    const CellPoint cursor{.row = scene.cursor_row - 1, .column = scene.cursor_col - 1};
    if (cursor.row < 0 || cursor.row >= scene.rows || cursor.column < 0 ||
        cursor.column >= scene.cols) {
        return std::nullopt;
    }
    return cursor;
}

bool same_point(const std::optional<CellPoint>& left, const std::optional<CellPoint>& right) {
    if (left.has_value() != right.has_value()) {
        return false;
    }
    return !left || (left->row == right->row && left->column == right->column);
}

void append_cursor_cell(std::vector<CellPoint>& cells, const std::optional<CellPoint>& cursor) {
    if (!cursor) {
        return;
    }
    if (std::ranges::none_of(cells, [&](const CellPoint& cell) {
            return cell.row == cursor->row && cell.column == cursor->column;
        })) {
        cells.push_back(*cursor);
    }
}

std::vector<Rect> coalesce_cells(const std::vector<bool>& dirty, const Scene& scene) {
    std::vector<Rect> rectangles;
    for (int row = 0; row < scene.rows; ++row) {
        int col = 0;
        while (col < scene.cols) {
            while (col < scene.cols && !dirty[cell_index(row, col, scene.cols)]) {
                ++col;
            }
            const int first = col;
            while (col < scene.cols && dirty[cell_index(row, col, scene.cols)]) {
                ++col;
            }
            if (first == col) {
                continue;
            }

            auto existing = std::ranges::find_if(rectangles, [&](const Rect& rect) {
                return rect.row + rect.rows == row && rect.col == first && rect.cols == col - first;
            });
            if (existing != rectangles.end()) {
                existing->rows += 1;
            } else {
                rectangles.push_back({.row = row, .col = first, .rows = 1, .cols = col - first});
            }
        }
    }
    return rectangles;
}

} // namespace

SceneDamage SceneDamageTracker::update(const Scene& scene, bool force_full_repaint) {
    std::vector<std::string> next_cells = visual_cells(scene);
    const std::optional<CellPoint> next_cursor = visible_cursor(scene);
    const std::optional<Rect> next_overlay_rect = overlay_rect(scene);
    const std::size_t total_cells = next_cells.size();
    const bool geometry_changed = rows_ != scene.rows || cols_ != scene.cols ||
                                  std::abs(grid_offset_rows_ - scene.grid_offset_rows) > 0.0001F ||
                                  !same_rect(overlay_rect_, next_overlay_rect);

    SceneDamage damage;
    if (force_full_repaint || !initialized_ || geometry_changed) {
        damage.full_repaint = true;
        damage.damaged_cells = total_cells;
    } else {
        std::vector<bool> dirty(total_cells, false);
        for (std::size_t index = 0; index < total_cells; ++index) {
            if (cells_[index] != next_cells[index]) {
                dirty[index] = true;
                ++damage.damaged_cells;
            }
        }
        if (total_cells > 0 && damage.damaged_cells * 2 >= total_cells) {
            damage.full_repaint = true;
        } else {
            damage.cell_rects = coalesce_cells(dirty, scene);
            if (!same_point(cursor_, next_cursor)) {
                append_cursor_cell(damage.cursor_cells, cursor_);
                append_cursor_cell(damage.cursor_cells, next_cursor);
            }
        }
    }

    rows_ = scene.rows;
    cols_ = scene.cols;
    grid_offset_rows_ = scene.grid_offset_rows;
    initialized_ = true;
    cells_ = std::move(next_cells);
    cursor_ = next_cursor;
    overlay_rect_ = next_overlay_rect;
    return damage;
}

void SceneDamageTracker::reset() {
    rows_ = 0;
    cols_ = 0;
    grid_offset_rows_ = 0.0F;
    initialized_ = false;
    cells_.clear();
    cursor_.reset();
    overlay_rect_.reset();
}

} // namespace cind::ui
