#include "editor/scheme_mode.hpp"

#include "document/document.hpp"
#include "editor/language_mechanism.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace cind {

namespace {

enum class DatumTokenKind : std::uint8_t {
    Atom,
    Open,
    Close,
    Prefix,
};

struct DatumToken {
    DatumTokenKind kind = DatumTokenKind::Atom;
    std::size_t start = 0;
    std::size_t end = 0;
    std::optional<std::size_t> pair;
};

bool is_space(char byte) {
    return std::isspace(static_cast<unsigned char>(byte)) != 0;
}

bool is_open(char byte) {
    return byte == '(' || byte == '[' || byte == '{';
}

bool is_close(char byte) {
    return byte == ')' || byte == ']' || byte == '}';
}

bool matches(char open, char close) {
    return (open == '(' && close == ')') || (open == '[' && close == ']') ||
           (open == '{' && close == '}');
}

bool is_atom_delimiter(char byte) {
    return is_space(byte) || is_open(byte) || is_close(byte) || byte == ';' || byte == '"' ||
           byte == '|' || byte == '\'' || byte == '`' || byte == ',';
}

std::size_t skip_nested_comment(std::string_view text, std::size_t at) {
    std::size_t depth = 1;
    at += 2;
    while (at < text.size() && depth != 0) {
        if (at + 1 < text.size() && text[at] == '#' && text[at + 1] == '|') {
            ++depth;
            at += 2;
        } else if (at + 1 < text.size() && text[at] == '|' && text[at + 1] == '#') {
            --depth;
            at += 2;
        } else {
            ++at;
        }
    }
    return at;
}

std::size_t quoted_end(std::string_view text, std::size_t at, char quote) {
    ++at;
    bool escaped = false;
    while (at < text.size()) {
        const char byte = text[at++];
        if (escaped) {
            escaped = false;
        } else if (byte == '\\') {
            escaped = true;
        } else if (byte == quote) {
            break;
        }
    }
    return at;
}

std::size_t character_end(std::string_view text, std::size_t at) {
    at += 2;
    if (at == text.size()) {
        return at;
    }
    ++at;
    while (at < text.size() && !is_atom_delimiter(text[at])) {
        ++at;
    }
    return at;
}

std::vector<DatumToken> tokenize(std::string_view text) {
    std::vector<DatumToken> tokens;
    std::vector<std::size_t> opens;
    std::size_t at = 0;
    while (at < text.size()) {
        if (is_space(text[at])) {
            ++at;
            continue;
        }
        if (text[at] == ';') {
            while (at < text.size() && text[at] != '\n') {
                ++at;
            }
            continue;
        }
        if (at + 1 < text.size() && text[at] == '#' && text[at + 1] == '|') {
            at = skip_nested_comment(text, at);
            continue;
        }
        if (text[at] == '"' || text[at] == '|') {
            const std::size_t start = at;
            at = quoted_end(text, at, text[at]);
            tokens.push_back(
                {.kind = DatumTokenKind::Atom, .start = start, .end = at, .pair = std::nullopt});
            continue;
        }
        if (at + 1 < text.size() && text[at] == '#' && text[at + 1] == '\\') {
            const std::size_t start = at;
            at = character_end(text, at);
            tokens.push_back(
                {.kind = DatumTokenKind::Atom, .start = start, .end = at, .pair = std::nullopt});
            continue;
        }
        if (is_open(text[at])) {
            const std::size_t index = tokens.size();
            tokens.push_back(
                {.kind = DatumTokenKind::Open, .start = at, .end = at + 1, .pair = std::nullopt});
            opens.push_back(index);
            ++at;
            continue;
        }
        if (is_close(text[at])) {
            const std::size_t index = tokens.size();
            tokens.push_back(
                {.kind = DatumTokenKind::Close, .start = at, .end = at + 1, .pair = std::nullopt});
            if (!opens.empty() && matches(text[tokens[opens.back()].start], text[at])) {
                const std::size_t open = opens.back();
                opens.pop_back();
                tokens[open].pair = index;
                tokens[index].pair = open;
            }
            ++at;
            continue;
        }
        if (text[at] == '\'' || text[at] == '`' || text[at] == ',') {
            const std::size_t start = at++;
            if (text[start] == ',' && at < text.size() && text[at] == '@') {
                ++at;
            }
            tokens.push_back(
                {.kind = DatumTokenKind::Prefix, .start = start, .end = at, .pair = std::nullopt});
            continue;
        }
        if (at + 1 < text.size() && text[at] == '#' &&
            (text[at + 1] == ';' || text[at + 1] == '\'' || text[at + 1] == '`' ||
             text[at + 1] == ',')) {
            const std::size_t start = at;
            at += 2;
            if (text[start + 1] == ',' && at < text.size() && text[at] == '@') {
                ++at;
            }
            tokens.push_back(
                {.kind = DatumTokenKind::Prefix, .start = start, .end = at, .pair = std::nullopt});
            continue;
        }
        if (text[at] == '#') {
            std::size_t marker_end = at + 1;
            while (marker_end < text.size() &&
                   std::isalnum(static_cast<unsigned char>(text[marker_end])) != 0) {
                ++marker_end;
            }
            if (marker_end < text.size() && is_open(text[marker_end])) {
                tokens.push_back({.kind = DatumTokenKind::Prefix,
                                  .start = at,
                                  .end = marker_end,
                                  .pair = std::nullopt});
                at = marker_end;
                continue;
            }
        }

        const std::size_t start = at;
        while (at < text.size() && !is_atom_delimiter(text[at])) {
            if (at + 1 < text.size() && text[at] == '#' && text[at + 1] == '|') {
                break;
            }
            ++at;
        }
        if (at == start) {
            ++at;
        }
        tokens.push_back(
            {.kind = DatumTokenKind::Atom, .start = start, .end = at, .pair = std::nullopt});
    }
    return tokens;
}

std::optional<std::size_t> forward_token(const std::vector<DatumToken>& tokens, std::size_t index) {
    while (index < tokens.size() && tokens[index].kind == DatumTokenKind::Prefix) {
        ++index;
    }
    if (index == tokens.size() || tokens[index].kind == DatumTokenKind::Close) {
        return std::nullopt;
    }
    if (tokens[index].kind == DatumTokenKind::Open) {
        return tokens[index].pair;
    }
    return index;
}

std::optional<TextOffset> forward_datum(const std::vector<DatumToken>& tokens, std::size_t from) {
    const auto found =
        std::ranges::find_if(tokens, [from](const DatumToken& token) { return token.end > from; });
    if (found == tokens.end()) {
        return std::nullopt;
    }
    const std::size_t index = static_cast<std::size_t>(found - tokens.begin());
    const std::optional<std::size_t> end = forward_token(tokens, index);
    return end ? std::optional<TextOffset>{TextOffset{static_cast<std::uint32_t>(tokens[*end].end)}}
               : std::nullopt;
}

std::optional<TextOffset> backward_datum(const std::vector<DatumToken>& tokens, std::size_t from) {
    std::optional<std::size_t> found;
    for (std::size_t index = tokens.size(); index > 0; --index) {
        if (tokens[index - 1].start < from) {
            found = index - 1;
            break;
        }
    }
    if (!found) {
        return std::nullopt;
    }
    std::size_t index = *found;
    if (tokens[index].kind == DatumTokenKind::Close) {
        const std::optional<std::size_t> pair = tokens[index].pair;
        if (!pair) {
            return std::nullopt;
        }
        index = *pair;
    } else if (tokens[index].kind == DatumTokenKind::Open ||
               tokens[index].kind == DatumTokenKind::Prefix) {
        return std::nullopt;
    }
    while (index > 0 && tokens[index - 1].kind == DatumTokenKind::Prefix) {
        --index;
    }
    return TextOffset{static_cast<std::uint32_t>(tokens[index].start)};
}

std::optional<TextOffset> enclosing_list(const std::vector<DatumToken>& tokens, std::size_t from) {
    const DatumToken* selected = nullptr;
    for (const DatumToken& token : tokens) {
        if (token.kind != DatumTokenKind::Open || token.start >= from) {
            continue;
        }
        const bool encloses = !token.pair || from <= tokens[*token.pair].start;
        if (encloses && (selected == nullptr || selected->start < token.start)) {
            selected = &token;
        }
    }
    return selected
               ? std::optional<TextOffset>{TextOffset{static_cast<std::uint32_t>(selected->start)}}
               : std::nullopt;
}

class SchemeMechanismSession final : public LanguageMechanismSession {
public:
    std::optional<TextOffset> move_structurally(const DocumentSnapshot& snapshot, TextOffset from,
                                                StructuralMotion motion) override {
        const std::vector<DatumToken> tokens = tokenize(snapshot.content().to_string());
        switch (motion) {
        case StructuralMotion::ForwardExpression:
            return forward_datum(tokens, from.value);
        case StructuralMotion::BackwardExpression:
            return backward_datum(tokens, from.value);
        case StructuralMotion::UpList:
            return enclosing_list(tokens, from.value);
        }
        return std::nullopt;
    }
};

std::shared_ptr<const LanguageMechanism> scheme_mechanism() {
    static const std::shared_ptr<const LanguageMechanism> mechanism =
        std::make_shared<const LanguageMechanism>(
            language_facet_bit(LanguageFacet::StructuralMotion),
            [] { return std::make_unique<SchemeMechanismSession>(); });
    return mechanism;
}

} // namespace

SchemeMechanismsRegistration ensure_scheme_mechanisms(EditorRuntime& runtime) {
    const std::shared_ptr<const LanguageMechanism> mechanism = scheme_mechanism();
    constexpr std::string_view name = "cind.scheme.structural-motion";
    if (const std::optional<LanguageProviderId> existing =
            runtime.languages().find_provider(name)) {
        const LanguageRegistry::ProviderDefinition& definition =
            runtime.languages().provider(*existing);
        if (definition.facet != LanguageFacet::StructuralMotion ||
            definition.mechanism != mechanism) {
            throw std::logic_error("Scheme provider has an incompatible mechanism");
        }
        return {.structural_motion = *existing};
    }
    return {.structural_motion = runtime.languages().define_provider(
                std::string(name), LanguageFacet::StructuralMotion, mechanism)};
}

} // namespace cind
