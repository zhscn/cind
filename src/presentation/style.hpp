#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace cind {

// Text roles are frontend-independent presentation inputs. Scheme resolves
// each role to concrete attributes; presenters only encode those attributes
// for their rendering backend.
enum class PresentationTextRole : std::uint8_t {
    Text,
    Keyword,
    String,
    Number,
    Comment,
    Preprocessor,
    Gutter,
    SignAdded,
    SignModified,
    SignDeleted,
    DiagnosticError,
    DiagnosticWarning,
    DiagnosticInformation,
    DiagnosticHint,
    StatusBar,
    StatusKey,
    Message,
    Popup,
    PositionHint,
    PopupPrompt,
    PopupCount,
    PopupLabel,
    PopupInput,
    PopupItem,
    PopupDetail,
    PopupSelected,
    EchoKey,
    ModelineChip,
    ModelineInactive,
    ModelineInactiveChip,
    Count,
};

enum class PresentationWeight : std::uint8_t { Regular, Strong };

struct PresentationTextStyle {
    std::uint32_t foreground = 0;
    std::optional<std::uint32_t> background;
    PresentationWeight weight = PresentationWeight::Regular;

    friend bool operator==(const PresentationTextStyle&, const PresentationTextStyle&) = default;
};

struct PresentationStyleSheet {
    static constexpr std::size_t text_role_count =
        static_cast<std::size_t>(PresentationTextRole::Count);
    static constexpr std::size_t modeline_tone_count = 6;

    std::array<PresentationTextStyle, text_role_count> text{};
    std::array<std::uint32_t, modeline_tone_count> modeline{};
    std::uint8_t inactive_alpha = 0;
    std::uint8_t secondary_alpha = 0;

    PresentationTextStyle& style(PresentationTextRole role) {
        return text[static_cast<std::size_t>(role)];
    }
    const PresentationTextStyle& style(PresentationTextRole role) const {
        return text[static_cast<std::size_t>(role)];
    }

    friend bool operator==(const PresentationStyleSheet&, const PresentationStyleSheet&) = default;
};

inline constexpr std::array<std::string_view, PresentationStyleSheet::text_role_count>
    presentation_text_role_names = {
        "text",
        "keyword",
        "string",
        "number",
        "comment",
        "preprocessor",
        "gutter",
        "sign-added",
        "sign-modified",
        "sign-deleted",
        "diagnostic-error",
        "diagnostic-warning",
        "diagnostic-information",
        "diagnostic-hint",
        "status-bar",
        "status-key",
        "message",
        "popup",
        "position-hint",
        "popup-prompt",
        "popup-count",
        "popup-label",
        "popup-input",
        "popup-item",
        "popup-detail",
        "popup-selected",
        "echo-key",
        "modeline-chip",
        "modeline-inactive",
        "modeline-inactive-chip",
};

constexpr std::string_view presentation_text_role_name(PresentationTextRole role) {
    return presentation_text_role_names[static_cast<std::size_t>(role)];
}

} // namespace cind
