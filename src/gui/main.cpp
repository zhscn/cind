#include "cli/style_loader.hpp"
#include "gui/editor_model.hpp"
#include "gui/inspect_server.hpp"
#include "gui/inspection.hpp"
#include "gui/skia_presenter.hpp"

#include <SDL3/SDL.h>

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
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
    SdlWindow(EditorModel& editor, SkiaPresenter& presenter, InspectionHub* inspection)
        : editor_(editor), presenter_(presenter), inspection_(inspection) {
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
            bool repaint = dispatch_event(event).repaint;
            while (SDL_PollEvent(&event)) {
                repaint = dispatch_event(event).repaint || repaint;
            }
            if (repaint && !editor_.should_quit()) {
                paint();
            }
        }
    }

private:
    struct EventResult {
        bool handled = false;
        bool repaint = false;
    };

    EventResult dispatch_event(const SDL_Event& event) {
        const RevisionId revision_before = editor_.revision();
        const EventResult result = handle_event(event);
        if (inspection_) {
            const std::uint64_t sequence =
                inspection_->record_event({.type = event_type(event),
                                           .detail = event_detail(event),
                                           .handled = result.handled,
                                           .repaint = result.repaint,
                                           .revision_before = revision_before,
                                           .revision_after = editor_.revision()});
            if (result.repaint) {
                last_repaint_event_sequence_ = sequence;
            }
        }
        return result;
    }

    EventResult handle_event(const SDL_Event& event) {
        switch (event.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            editor_.request_quit();
            return {true, !editor_.should_quit()};
        case SDL_EVENT_WINDOW_EXPOSED:
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        case SDL_EVENT_WINDOW_RESTORED:
            return {true, true};
        case SDL_EVENT_KEY_DOWN: {
            const int page_rows = std::max(1, rows_ - 2);
            const bool handled = editor_.handle_key(event.key.scancode, event.key.mod, page_rows);
            return {handled, handled};
        }
        case SDL_EVENT_TEXT_INPUT:
            editor_.insert_text(event.text.text ? event.text.text : "");
            return {true, true};
        case SDL_EVENT_TEXT_EDITING:
            editor_.set_preedit(event.edit.text ? event.edit.text : "");
            return {true, true};
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (event.button.button == SDL_BUTTON_LEFT) {
                editor_.click(mouse_cell_row(event.button.y), mouse_cell_column(event.button.x));
                return {true, true};
            }
            return {};
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
            const bool handled = amount != 0.0F;
            return {handled, handled};
        }
        default:
            return {};
        }
    }

    static std::string event_type(const SDL_Event& event) {
        switch (event.type) {
        case SDL_EVENT_QUIT:
            return "quit";
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            return "window-close";
        case SDL_EVENT_WINDOW_EXPOSED:
            return "window-exposed";
        case SDL_EVENT_WINDOW_RESIZED:
            return "window-resized";
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            return "window-pixel-size-changed";
        case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
            return "window-display-changed";
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
            return "window-display-scale-changed";
        case SDL_EVENT_WINDOW_RESTORED:
            return "window-restored";
        case SDL_EVENT_KEY_DOWN:
            return "key-down";
        case SDL_EVENT_TEXT_INPUT:
            return "text-input";
        case SDL_EVENT_TEXT_EDITING:
            return "text-editing";
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            return "mouse-button-down";
        case SDL_EVENT_MOUSE_WHEEL:
            return "mouse-wheel";
        default:
            return std::format("sdl-event-{}", event.type);
        }
    }

    std::string event_detail(const SDL_Event& event) const {
        switch (event.type) {
        case SDL_EVENT_KEY_DOWN:
            return std::format(
                "scancode={} name={} modifiers={}", static_cast<int>(event.key.scancode),
                SDL_GetScancodeName(event.key.scancode), static_cast<unsigned int>(event.key.mod));
        case SDL_EVENT_TEXT_INPUT:
            return std::format("text={}", event.text.text ? event.text.text : "");
        case SDL_EVENT_TEXT_EDITING:
            return std::format("preedit={}", event.edit.text ? event.edit.text : "");
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            return std::format("button={} window=({}, {}) cell=({}, {})",
                               static_cast<int>(event.button.button), event.button.x,
                               event.button.y, mouse_cell_row(event.button.y),
                               mouse_cell_column(event.button.x));
        case SDL_EVENT_MOUSE_WHEEL:
            return std::format("delta=({}, {}) direction={}", event.wheel.x, event.wheel.y,
                               static_cast<int>(event.wheel.direction));
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            return std::format("size={}x{}", event.window.data1, event.window.data2);
        default:
            return {};
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
        publish_inspection(std::move(scene), pixel_width, pixel_height, scale);
    }

    void publish_inspection(ui::Scene scene, int pixel_width, int pixel_height, float scale) {
        if (!inspection_) {
            return;
        }
        int window_width = 0;
        int window_height = 0;
        SDL_GetWindowSize(window_.get(), &window_width, &window_height);
        const SkiaTheme& theme = presenter_.theme();
        RenderStateSnapshot render{
            .video_driver = SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "",
            .window_width = window_width,
            .window_height = window_height,
            .output_width = pixel_width,
            .output_height = pixel_height,
            .display_scale = scale,
            .cell_width = presenter_.cell_width(),
            .cell_height = presenter_.cell_height(),
            .rows = rows_,
            .columns = columns_,
            .texture_format = "ARGB8888",
            .font_family = presenter_.font_family(),
            .font_size = presenter_.font_size(),
            .theme = {.background = theme.background,
                      .gutter_background = theme.gutter_background,
                      .status_background = theme.status_background,
                      .echo_background = theme.echo_background,
                      .selection_background = theme.selection_background,
                      .cursor = theme.cursor,
                      .sign_added = theme.sign_added,
                      .sign_modified = theme.sign_modified,
                      .sign_deleted = theme.sign_deleted},
            .pixel_hash = hash_pixels(),
        };
        inspection_->publish(editor_.inspect(), std::move(scene), std::move(render),
                             last_repaint_event_sequence_);
    }

    std::uint64_t hash_pixels() const {
        std::uint64_t hash = 1469598103934665603ULL;
        for (const std::uint32_t pixel : pixels_) {
            hash ^= pixel;
            hash *= 1099511628211ULL;
        }
        return hash;
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
    InspectionHub* inspection_ = nullptr;
    std::unique_ptr<SDL_Window, WindowDeleter> window_;
    std::unique_ptr<SDL_Renderer, RendererDeleter> renderer_;
    std::unique_ptr<SDL_Texture, TextureDeleter> texture_;
    std::vector<std::uint32_t> pixels_;
    int texture_width_ = 0;
    int texture_height_ = 0;
    int rows_ = 24;
    int columns_ = 80;
    std::uint64_t last_repaint_event_sequence_ = 0;
};

int run_editor(const std::string& path, std::uint32_t initial_line,
               const std::optional<std::filesystem::path>& inspector_socket) {
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
    std::unique_ptr<InspectionHub> inspection;
    std::unique_ptr<InspectorServer> inspector;
    if (inspector_socket) {
        inspection = std::make_unique<InspectionHub>();
        inspector = std::make_unique<InspectorServer>(*inspection, *inspector_socket);
        std::fprintf(stderr, "cind-gui inspector: %s\n", inspector->socket_path().c_str());
    }
    SdlWindow window(editor, presenter, inspection.get());
    window.run();
    return 0;
}

} // namespace

} // namespace cind::gui

int main(int argc, char** argv) {
    std::uint32_t initial_line = 0;
    bool inspect = false;
    std::optional<std::filesystem::path> inspector_socket;
    std::optional<std::string> file;

    try {
        for (int index = 1; index < argc; ++index) {
            const std::string_view argument = argv[index];
            if (argument == "--inspect") {
                inspect = true;
            } else if (argument == "--help" || argument == "-h") {
                std::fprintf(stderr, "usage: cind-gui [--inspect] [--inspect-socket PATH] [+LINE] "
                                     "<file>\n");
                return 0;
            } else if (argument == "--inspect-socket") {
                if (++index >= argc) {
                    throw std::runtime_error("--inspect-socket requires a path");
                }
                inspect = true;
                inspector_socket = std::filesystem::absolute(argv[index]);
            } else if (argument.starts_with('+')) {
                if (argument.size() == 1 || initial_line != 0) {
                    throw std::runtime_error("invalid +LINE argument");
                }
                char* end = nullptr;
                errno = 0;
                const unsigned long parsed = std::strtoul(argument.data() + 1, &end, 10);
                if (errno != 0 || !end || *end != '\0' || parsed == 0 ||
                    parsed > std::numeric_limits<std::uint32_t>::max()) {
                    throw std::runtime_error("invalid +LINE argument");
                }
                initial_line = static_cast<std::uint32_t>(parsed);
            } else if (!file) {
                file = std::string(argument);
            } else {
                throw std::runtime_error("more than one file was provided");
            }
        }
        if (!file) {
            std::fprintf(stderr,
                         "usage: cind-gui [--inspect] [--inspect-socket PATH] [+LINE] <file>\n");
            return 2;
        }
        if (inspect && !inspector_socket) {
            inspector_socket =
                cind::gui::default_inspector_socket_path(static_cast<int>(::getpid()));
        }
        return cind::gui::run_editor(*file, initial_line, inspector_socket);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "cind-gui: %s\n", error.what());
        return 1;
    }
}
