#pragma once

#include "document/text.hpp"

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace cind {

struct LspPosition {
    std::uint32_t line = 0;
    std::uint32_t character = 0;
};

struct LspRange {
    LspPosition start;
    LspPosition end;
};

struct LspTextEdit {
    LspRange range;
    std::string new_text;
};

struct LspCompletionEdit {
    LspRange insert_range;
    LspRange replace_range;
    std::string new_text;
};

struct LspCompletionItem {
    std::string label;
    std::string insert_text;
    std::string filter_text;
    std::string sort_text;
    std::string kind;
    std::string detail;
    std::optional<LspCompletionEdit> edit;
    bool is_snippet = false;
    bool resolved = true;
    std::string documentation;
    std::vector<LspTextEdit> additional_edits;
    std::string raw;
};

struct LspCompletionResponse {
    std::vector<LspCompletionItem> items;
    bool is_incomplete = false;
};

enum class LspCompletionTriggerKind : std::uint8_t {
    Invoked = 1,
    TriggerCharacter = 2,
    TriggerForIncompleteCompletions = 3,
};

struct LspCompletionRequest {
    std::string uri;
    std::string language_id;
    RevisionId revision = 0;
    Text text;
    TextOffset caret;
    LspCompletionTriggerKind trigger = LspCompletionTriggerKind::Invoked;
    std::string trigger_character;
};

std::string path_to_file_uri(std::string_view path);
std::expected<std::string, std::string> file_uri_to_path(std::string_view uri);
LspPosition lsp_position(const Text& text, TextOffset offset);
std::optional<TextRange> text_range_from_lsp(const Text& text, LspRange range);

} // namespace cind
