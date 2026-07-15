#include "cli/session.hpp"
#include "cli/style_loader.hpp"
#include "commands/file_io.hpp"
#include "document/text.hpp"
#include "gui/skia_presenter.hpp"
#include "ui/editor_scene.hpp"
#include "ui/line_signs.hpp"
#include "ui/text_position.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cind::gui {

namespace {

class EditorModel {
public:
    EditorModel(std::string path, std::string initial, CppIndentStyle style,
                std::string style_origin, std::uint32_t initial_line)
        : path_(std::move(path)), session_(std::move(initial), style),
          style_origin_(std::move(style_origin)), saved_text_(session_.snapshot().content()) {
        const DocumentSnapshot snapshot = session_.snapshot();
        if (initial_line > 0) {
            const std::uint32_t line =
                std::min(initial_line - 1, snapshot.content().line_count() - 1);
            session_.set_caret(snapshot.content().line_start(line));
        }
        message_ = "SDL3 Wayland · Skia · Ctrl-S save · Ctrl-Q quit";
    }

    ui::Scene compose(int rows, int columns) {
        const DocumentSnapshot snapshot = session_.snapshot();
        const std::string_view echo =
            preedit_.empty() ? std::string_view(message_) : std::string_view(preedit_);
        return ui::compose_editor_scene({.text = snapshot.content(),
                                         .tokens = session_.analysis().tree.tokens(),
                                         .signs = signs(),
                                         .caret = session_.caret(),
                                         .selection = std::nullopt,
                                         .rows = rows,
                                         .cols = columns,
                                         .tab_width = session_.style().tab_width,
                                         .path = path_,
                                         .dirty = dirty(),
                                         .revision = snapshot.revision(),
                                         .style_origin = style_origin_,
                                         .last_key = last_key_,
                                         .echo = echo,
                                         .echo_cursor_column = std::nullopt},
                                        viewport_);
    }

    bool handle_key(SDL_Scancode scancode, SDL_Keymod modifiers, int page_rows) {
        const bool control = (modifiers & SDL_KMOD_CTRL) != 0;
        const bool alt = (modifiers & SDL_KMOD_ALT) != 0;
        const bool shift = (modifiers & SDL_KMOD_SHIFT) != 0;
        bool handled = true;
        bool edited = false;

        if (const char* name = SDL_GetScancodeName(scancode); name && *name) {
            last_key_ = name;
        }

        if (control) {
            switch (scancode) {
            case SDL_SCANCODE_S:
                save();
                return true;
            case SDL_SCANCODE_Q:
                request_quit(shift);
                return true;
            case SDL_SCANCODE_Z:
                session_.undo();
                edited = true;
                break;
            case SDL_SCANCODE_R:
                session_.redo();
                edited = true;
                break;
            case SDL_SCANCODE_A:
                move_home();
                break;
            case SDL_SCANCODE_E:
                move_end();
                break;
            case SDL_SCANCODE_N:
                move_vertical(1);
                break;
            case SDL_SCANCODE_P:
                move_vertical(-1);
                break;
            case SDL_SCANCODE_V:
                move_vertical(page_rows);
                break;
            default:
                handled = false;
                break;
            }
        } else if (alt && scancode == SDL_SCANCODE_V) {
            move_vertical(-page_rows);
        } else if (!alt) {
            switch (scancode) {
            case SDL_SCANCODE_LEFT:
                move_horizontal(false);
                break;
            case SDL_SCANCODE_RIGHT:
                move_horizontal(true);
                break;
            case SDL_SCANCODE_UP:
                move_vertical(-1);
                break;
            case SDL_SCANCODE_DOWN:
                move_vertical(1);
                break;
            case SDL_SCANCODE_HOME:
                move_home();
                break;
            case SDL_SCANCODE_END:
                move_end();
                break;
            case SDL_SCANCODE_PAGEUP:
                move_vertical(-page_rows);
                break;
            case SDL_SCANCODE_PAGEDOWN:
                move_vertical(page_rows);
                break;
            case SDL_SCANCODE_BACKSPACE:
                erase_code_point(false);
                edited = true;
                break;
            case SDL_SCANCODE_DELETE:
                erase_code_point(true);
                edited = true;
                break;
            case SDL_SCANCODE_RETURN:
            case SDL_SCANCODE_KP_ENTER:
                session_.enter();
                edited = true;
                break;
            case SDL_SCANCODE_TAB:
                session_.indent();
                edited = true;
                break;
            default:
                handled = false;
                break;
            }
        } else {
            handled = false;
        }

        if (handled && edited) {
            after_edit();
        }
        return handled;
    }

    void insert_text(std::string_view text) {
        if (text.empty()) {
            return;
        }
        if (text.size() == 1 && static_cast<unsigned char>(text.front()) >= 0x20U) {
            session_.type_text(text);
        } else {
            session_.insert_text(text);
        }
        last_key_ = "text";
        after_edit();
    }

    void set_preedit(std::string_view text) {
        preedit_ = text.empty() ? std::string() : std::format("IME · {}", text);
    }

    void click(int cell_row, int cell_column) {
        const DocumentSnapshot snapshot = session_.snapshot();
        const Text& text = snapshot.content();
        const int text_column = ui::text_area_column(text.line_count());
        const int visible_rows = std::max(1, last_rows_ - 2);
        if (cell_row < 0 || cell_row >= visible_rows) {
            return;
        }
        const std::uint32_t line = std::min(
            viewport_.top_line + static_cast<std::uint32_t>(cell_row), text.line_count() - 1);
        if (cell_column < text_column) {
            session_.set_caret(text.line_start(line));
            return;
        }
        const int display_column = viewport_.left_column + std::max(0, cell_column - text_column);
        session_.set_caret(
            ui::offset_at_display_column(text, line, display_column, session_.style().tab_width));
    }

    void set_frame_rows(int rows) { last_rows_ = rows; }
    void scroll_lines(int delta) { move_vertical(delta); }
    bool should_quit() const { return quit_; }

    void request_quit(bool force = false) {
        if (!dirty() || force || quit_armed_) {
            quit_ = true;
            return;
        }
        quit_armed_ = true;
        message_ = "unsaved changes · Ctrl-S saves · Ctrl-Q again or Ctrl-Shift-Q discards";
    }

private:
    bool dirty() const { return diff_edit(saved_text_, session_.snapshot().content()).has_value(); }

    const ui::LineSigns& signs() {
        const DocumentSnapshot snapshot = session_.snapshot();
        if (sign_revision_ != snapshot.revision() || sign_generation_ != save_generation_) {
            signs_ = ui::line_signs(saved_text_, snapshot.content());
            sign_revision_ = snapshot.revision();
            sign_generation_ = save_generation_;
        }
        return signs_;
    }

    void after_edit() {
        quit_armed_ = false;
        message_.clear();
        preedit_.clear();
    }

    void save() {
        const DocumentSnapshot snapshot = session_.snapshot();
        if (std::error_code error = save_file_atomically(path_, snapshot.content())) {
            message_ = std::format("save failed: {}", error.message());
            return;
        }
        saved_text_ = snapshot.content();
        ++save_generation_;
        quit_armed_ = false;
        message_ = std::format("saved {}", path_);
    }

    void move_horizontal(bool forward) {
        const DocumentSnapshot snapshot = session_.snapshot();
        session_.set_caret(forward ? ui::next_code_point(snapshot.content(), session_.caret())
                                   : ui::previous_code_point(snapshot.content(), session_.caret()));
    }

    void move_vertical(int delta) {
        const DocumentSnapshot snapshot = session_.snapshot();
        const Text& text = snapshot.content();
        const LinePosition position = text.position(session_.caret());
        const int current_column =
            ui::display_column(text, session_.caret(), session_.style().tab_width);
        const int last_line = static_cast<int>(text.line_count()) - 1;
        const int target = std::clamp(static_cast<int>(position.line) + delta, 0, last_line);
        session_.set_caret(ui::offset_at_display_column(
            text, static_cast<std::uint32_t>(target), current_column, session_.style().tab_width));
    }

    void move_home() {
        const DocumentSnapshot snapshot = session_.snapshot();
        const Text& text = snapshot.content();
        session_.set_caret(text.line_start(text.position(session_.caret()).line));
    }

    void move_end() {
        const DocumentSnapshot snapshot = session_.snapshot();
        const Text& text = snapshot.content();
        session_.set_caret(text.line_content_end(text.position(session_.caret()).line));
    }

    void erase_code_point(bool forward) {
        const DocumentSnapshot snapshot = session_.snapshot();
        const Text& text = snapshot.content();
        const TextOffset caret = session_.caret();
        if (forward) {
            session_.erase(TextRange{caret, ui::next_code_point(text, caret)});
        } else {
            session_.erase(TextRange{ui::previous_code_point(text, caret), caret});
        }
    }

    std::string path_;
    EditSession session_;
    std::string style_origin_;
    Text saved_text_;
    ui::EditorViewport viewport_;
    ui::LineSigns signs_;
    RevisionId sign_revision_ = static_cast<RevisionId>(-1);
    std::uint32_t save_generation_ = 0;
    std::uint32_t sign_generation_ = static_cast<std::uint32_t>(-1);
    int last_rows_ = 24;
    std::string message_;
    std::string preedit_;
    std::string last_key_;
    bool quit_armed_ = false;
    bool quit_ = false;
};

class SdlRuntime {
public:
    SdlRuntime() {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "wayland");
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            throw std::runtime_error(
                std::format("SDL video initialization failed: {}", SDL_GetError()));
        }
        initialized_ = true;
        const char* driver = SDL_GetCurrentVideoDriver();
        if (!driver || std::string_view(driver) != "wayland") {
            SDL_Quit();
            initialized_ = false;
            throw std::runtime_error(
                std::format("SDL selected '{}' instead of Wayland", driver ? driver : "none"));
        }
    }

    ~SdlRuntime() {
        if (initialized_) {
            SDL_Quit();
        }
    }

    SdlRuntime(const SdlRuntime&) = delete;
    SdlRuntime& operator=(const SdlRuntime&) = delete;

private:
    bool initialized_ = false;
};

struct WindowDeleter {
    void operator()(SDL_Window* window) const { SDL_DestroyWindow(window); }
};

struct RendererDeleter {
    void operator()(SDL_Renderer* renderer) const { SDL_DestroyRenderer(renderer); }
};

struct TextureDeleter {
    void operator()(SDL_Texture* texture) const { SDL_DestroyTexture(texture); }
};

class SdlWindow {
public:
    SdlWindow(EditorModel& editor, SkiaPresenter& presenter)
        : editor_(editor), presenter_(presenter) {
        constexpr SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
        window_.reset(SDL_CreateWindow("cind · Skia · Wayland", 1100, 760, flags));
        if (!window_) {
            throw std::runtime_error(std::format("SDL window creation failed: {}", SDL_GetError()));
        }
        SDL_SetWindowMinimumSize(window_.get(), presenter_.cell_width() * 40,
                                 presenter_.cell_height() * 6);
        renderer_.reset(SDL_CreateRenderer(window_.get(), nullptr));
        if (!renderer_) {
            throw std::runtime_error(
                std::format("SDL renderer creation failed: {}", SDL_GetError()));
        }
        SDL_SetRenderVSync(renderer_.get(), 1);
        if (!SDL_StartTextInput(window_.get())) {
            throw std::runtime_error(std::format("SDL text input failed: {}", SDL_GetError()));
        }
    }

    void run() {
        paint();
        while (!editor_.should_quit()) {
            SDL_Event event{};
            if (!SDL_WaitEvent(&event)) {
                throw std::runtime_error(std::format("SDL event wait failed: {}", SDL_GetError()));
            }
            bool repaint = handle_event(event);
            while (SDL_PollEvent(&event)) {
                repaint = handle_event(event) || repaint;
            }
            if (repaint && !editor_.should_quit()) {
                paint();
            }
        }
    }

private:
    bool handle_event(const SDL_Event& event) {
        switch (event.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            editor_.request_quit();
            return !editor_.should_quit();
        case SDL_EVENT_WINDOW_EXPOSED:
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        case SDL_EVENT_WINDOW_RESTORED:
            return true;
        case SDL_EVENT_KEY_DOWN: {
            const int page_rows = std::max(1, rows_ - 2);
            return editor_.handle_key(event.key.scancode, event.key.mod, page_rows);
        }
        case SDL_EVENT_TEXT_INPUT:
            editor_.insert_text(event.text.text ? event.text.text : "");
            return true;
        case SDL_EVENT_TEXT_EDITING:
            editor_.set_preedit(event.edit.text ? event.edit.text : "");
            return true;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (event.button.button == SDL_BUTTON_LEFT) {
                editor_.click(mouse_cell_row(event.button.y), mouse_cell_column(event.button.x));
                return true;
            }
            return false;
        case SDL_EVENT_MOUSE_WHEEL: {
            float amount = event.wheel.y;
            if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
                amount = -amount;
            }
            if (amount > 0.0F) {
                editor_.scroll_lines(-std::max(1, static_cast<int>(std::ceil(amount))));
            } else if (amount < 0.0F) {
                editor_.scroll_lines(std::max(1, static_cast<int>(std::ceil(-amount))));
            }
            return amount != 0.0F;
        }
        default:
            return false;
        }
    }

    void ensure_texture(int width, int height) {
        if (texture_ && texture_width_ == width && texture_height_ == height) {
            return;
        }
        texture_.reset(SDL_CreateTexture(renderer_.get(), SDL_PIXELFORMAT_ARGB8888,
                                         SDL_TEXTUREACCESS_STREAMING, width, height));
        if (!texture_) {
            throw std::runtime_error(
                std::format("SDL texture creation failed: {}", SDL_GetError()));
        }
        texture_width_ = width;
        texture_height_ = height;
    }

    float display_scale() const {
        const float scale = SDL_GetWindowDisplayScale(window_.get());
        return scale > 0.0F ? scale : 1.0F;
    }

    void paint() {
        int pixel_width = 0;
        int pixel_height = 0;
        if (!SDL_GetRenderOutputSize(renderer_.get(), &pixel_width, &pixel_height) ||
            pixel_width <= 0 || pixel_height <= 0) {
            return;
        }

        const float scale = display_scale();
        rows_ = std::max(3, static_cast<int>(std::floor(
                                static_cast<float>(pixel_height) /
                                (static_cast<float>(presenter_.cell_height()) * scale))));
        columns_ = std::max(20, static_cast<int>(std::floor(
                                    static_cast<float>(pixel_width) /
                                    (static_cast<float>(presenter_.cell_width()) * scale))));
        editor_.set_frame_rows(rows_);
        ui::Scene scene = editor_.compose(rows_, columns_);

        const std::size_t pixel_count =
            static_cast<std::size_t>(pixel_width) * static_cast<std::size_t>(pixel_height);
        pixels_.resize(pixel_count);
        const std::size_t row_bytes = static_cast<std::size_t>(pixel_width) * sizeof(std::uint32_t);
        presenter_.render(scene, pixel_width, pixel_height, pixels_.data(), row_bytes, scale);

        ensure_texture(pixel_width, pixel_height);
        if (!SDL_UpdateTexture(texture_.get(), nullptr, pixels_.data(),
                               pixel_width * static_cast<int>(sizeof(std::uint32_t))) ||
            !SDL_RenderClear(renderer_.get()) ||
            !SDL_RenderTexture(renderer_.get(), texture_.get(), nullptr, nullptr)) {
            throw std::runtime_error(std::format("SDL presentation failed: {}", SDL_GetError()));
        }
        update_text_input_area(scene, pixel_width, pixel_height, scale);
        SDL_RenderPresent(renderer_.get());
    }

    void update_text_input_area(const ui::Scene& scene, int pixel_width, int pixel_height,
                                float scale) {
        int window_width = 0;
        int window_height = 0;
        if (!SDL_GetWindowSize(window_.get(), &window_width, &window_height) || window_width <= 0 ||
            window_height <= 0) {
            return;
        }
        const float x_factor =
            static_cast<float>(window_width) * scale / static_cast<float>(pixel_width);
        const float y_factor =
            static_cast<float>(window_height) * scale / static_cast<float>(pixel_height);
        SDL_Rect area{
            .x = static_cast<int>(std::floor(static_cast<float>(scene.cursor_col - 1) *
                                             static_cast<float>(presenter_.cell_width()) *
                                             x_factor)),
            .y = static_cast<int>(std::floor(static_cast<float>(scene.cursor_row - 1) *
                                             static_cast<float>(presenter_.cell_height()) *
                                             y_factor)),
            .w = std::max(1, static_cast<int>(std::ceil(
                                 static_cast<float>(presenter_.cell_width()) * x_factor))),
            .h = std::max(1, static_cast<int>(std::ceil(
                                 static_cast<float>(presenter_.cell_height()) * y_factor))),
        };
        SDL_SetTextInputArea(window_.get(), &area, 0);
    }

    int mouse_cell_column(float window_x) const {
        int window_width = 0;
        int window_height = 0;
        SDL_GetWindowSize(window_.get(), &window_width, &window_height);
        if (window_width <= 0) {
            return 0;
        }
        return std::clamp(static_cast<int>(std::floor(window_x / static_cast<float>(window_width) *
                                                      static_cast<float>(columns_))),
                          0, columns_ - 1);
    }

    int mouse_cell_row(float window_y) const {
        int window_width = 0;
        int window_height = 0;
        SDL_GetWindowSize(window_.get(), &window_width, &window_height);
        if (window_height <= 0) {
            return 0;
        }
        return std::clamp(static_cast<int>(std::floor(window_y / static_cast<float>(window_height) *
                                                      static_cast<float>(rows_))),
                          0, rows_ - 1);
    }

    EditorModel& editor_;
    SkiaPresenter& presenter_;
    std::unique_ptr<SDL_Window, WindowDeleter> window_;
    std::unique_ptr<SDL_Renderer, RendererDeleter> renderer_;
    std::unique_ptr<SDL_Texture, TextureDeleter> texture_;
    std::vector<std::uint32_t> pixels_;
    int texture_width_ = 0;
    int texture_height_ = 0;
    int rows_ = 24;
    int columns_ = 80;
};

int run_editor(const std::string& path, std::uint32_t initial_line) {
    std::string initial;
    if (std::filesystem::exists(path)) {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            throw std::runtime_error(std::format("cannot open {}", path));
        }
        std::stringstream contents;
        contents << input.rdbuf();
        initial = contents.str();
    }

    CppIndentStyle style;
    std::string style_origin = "llvm (fallback)";
    if (auto loaded = load_clang_format_style(std::filesystem::absolute(path).parent_path())) {
        style = loaded->style;
        style_origin = loaded->config_path.filename().string();
    }

    EditorModel editor(path, std::move(initial), style, std::move(style_origin), initial_line);
    SkiaPresenter presenter;
    SdlRuntime runtime;
    SdlWindow window(editor, presenter);
    window.run();
    return 0;
}

} // namespace

} // namespace cind::gui

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::fprintf(stderr, "usage: cind-gui [+LINE] <file>\n");
        return 2;
    }
    std::uint32_t initial_line = 0;
    int file_argument = 1;
    if (argc == 3 && argv[1][0] == '+') {
        initial_line = static_cast<std::uint32_t>(std::strtoul(argv[1] + 1, nullptr, 10));
        file_argument = 2;
    }
    try {
        return cind::gui::run_editor(argv[file_argument], initial_line);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "cind-gui: %s\n", error.what());
        return 1;
    }
}
