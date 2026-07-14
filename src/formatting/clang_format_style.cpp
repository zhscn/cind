#include "formatting/clang_format_style.hpp"

#include <array>
#include <charconv>
#include <format>
#include <optional>

namespace cind {

namespace {

using NI = CppIndentStyle::NamespaceIndentation;

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') {
            x = static_cast<char>(x - 'A' + 'a');
        }
        if (y >= 'A' && y <= 'Z') {
            y = static_cast<char>(y - 'A' + 'a');
        }
        if (x != y) {
            return false;
        }
    }
    return true;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

// clang-format's indentation-relevant preset values, cross-validated against
// `clang-format --dump-config` (see clang_format_style_tests.cpp).
// access_modifier_offset and ctor_init_width keep clang-format semantics here;
// the conversion to CppIndentStyle happens at finalize time because it depends
// on the final IndentWidth.
struct Preset {
    std::string_view name;
    int indent_width;
    int continuation;
    int tab_width;
    int access_modifier_offset;
    NI namespace_indentation;
    bool indent_case_labels;
    bool braced_list_continuation;
    bool align_open_bracket;
    bool align_operands;
    int ctor_init_width;
    bool ctor_break_before_comma; // BreakConstructorInitializers: BeforeComma
};

constexpr std::array<Preset, 7> kPresets = {{
    {"LLVM", 2, 4, 8, -2, NI::None, false, true, true, true, 4, false},
    {"Google", 2, 4, 8, -1, NI::None, true, true, true, true, 4, false},
    {"Chromium", 2, 4, 8, -1, NI::None, true, true, true, true, 4, false},
    {"Mozilla", 2, 2, 8, -2, NI::None, true, false, true, true, 2, true},
    {"WebKit", 4, 4, 8, -4, NI::Inner, false, false, false, false, 4, true},
    {"GNU", 2, 4, 8, -2, NI::None, false, false, true, true, 4, false},
    {"Microsoft", 4, 4, 4, -2, NI::None, false, true, true, true, 4, false},
}};

// Parser state: AccessModifierOffset and ConstructorInitializerIndentWidth
// stay in clang-format's own terms until every width override has been seen.
struct PendingStyle {
    CppIndentStyle style;
    int access_modifier_offset;
    int ctor_init_width;

    explicit PendingStyle(const CppIndentStyle& base)
        : style(base),
          access_modifier_offset(base.access_specifier_offset - base.indent_width),
          ctor_init_width(base.continuation_indent) {}
};

const Preset* find_preset(std::string_view name) {
    for (const Preset& preset : kPresets) {
        if (iequals(preset.name, name)) {
            return &preset;
        }
    }
    return nullptr;
}

void apply_preset(const Preset& preset, PendingStyle& pending) {
    CppIndentStyle s; // presets reset unmapped fields to kernel defaults
    s.indent_width = preset.indent_width;
    s.continuation_indent = preset.continuation;
    s.tab_width = preset.tab_width;
    s.use_tabs = false;
    s.align_open_bracket = preset.align_open_bracket;
    s.align_operands = preset.align_operands;
    s.break_before_ternary = true; // uniform across all built-in presets
    s.brace_init_continuation = preset.braced_list_continuation;
    s.indent_wrapped_function_names = false;
    s.namespace_indentation = preset.namespace_indentation;
    s.indent_case_label = preset.indent_case_labels;
    s.constructor_initializers =
        preset.ctor_break_before_comma
            ? CppIndentStyle::ConstructorInitializerStyle::AlignWithColon
            : CppIndentStyle::ConstructorInitializerStyle::AlignFirstInitializer;
    pending.style = s;
    pending.access_modifier_offset = preset.access_modifier_offset;
    pending.ctor_init_width = preset.ctor_init_width;
}

struct KeyValue {
    std::string_view key;
    std::string_view value;
};

// One `---`/`...`-delimited YAML document, reduced to its top-level scalars.
using YamlDocument = std::vector<KeyValue>;

std::vector<YamlDocument> split_documents(std::string_view yaml) {
    std::vector<YamlDocument> documents(1);
    std::size_t pos = 0;
    while (pos <= yaml.size()) {
        std::size_t eol = yaml.find('\n', pos);
        std::string_view line =
            yaml.substr(pos, eol == std::string_view::npos ? std::string_view::npos : eol - pos);
        pos = eol == std::string_view::npos ? yaml.size() + 1 : eol + 1;

        std::string_view stripped = trim(line);
        if (stripped == "---" || stripped == "...") {
            if (!documents.back().empty()) {
                documents.emplace_back();
            }
            continue;
        }
        if (stripped.empty() || stripped.front() == '#') {
            continue;
        }
        if (line.front() == ' ' || line.front() == '\t') {
            continue; // nested block content — none of our keys live there
        }
        std::size_t colon = stripped.find(':');
        if (colon == std::string_view::npos) {
            continue;
        }
        std::string_view key = trim(stripped.substr(0, colon));
        std::string_view value = stripped.substr(colon + 1);
        // Cut trailing comments; our keys' values never contain '#'.
        if (std::size_t hash = value.find('#'); hash != std::string_view::npos) {
            value = value.substr(0, hash);
        }
        value = trim(value);
        if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') &&
            value.back() == value.front()) {
            value = value.substr(1, value.size() - 2);
        }
        if (key.empty() || value.empty()) {
            continue; // `Key:` opening a nested block, or malformed
        }
        documents.back().push_back({key, value});
    }
    return documents;
}

const std::string_view* doc_value(const YamlDocument& doc, std::string_view key) {
    for (const KeyValue& kv : doc) {
        if (kv.key == key) {
            return &kv.value;
        }
    }
    return nullptr;
}

std::optional<int> parse_int(std::string_view value) {
    int out = 0;
    auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), out);
    if (ec != std::errc() || ptr != value.data() + value.size()) {
        return std::nullopt;
    }
    return out;
}

std::optional<bool> parse_bool(std::string_view value) {
    if (value == "true") {
        return true;
    }
    if (value == "false") {
        return false;
    }
    return std::nullopt;
}

bool value_in(std::string_view value, std::initializer_list<std::string_view> set) {
    for (std::string_view candidate : set) {
        if (value == candidate) {
            return true;
        }
    }
    return false;
}

void apply_key(const KeyValue& kv, PendingStyle& pending, ClangFormatStyle& result) {
    auto warn_value = [&] {
        result.warnings.push_back(
            std::format("invalid value for {}: '{}'", kv.key, kv.value));
    };
    auto set_int = [&](int& out) {
        if (auto v = parse_int(kv.value)) {
            out = *v;
        } else {
            warn_value();
        }
    };
    auto set_bool = [&](bool& out) {
        if (auto v = parse_bool(kv.value)) {
            out = *v;
        } else {
            warn_value();
        }
    };

    CppIndentStyle& s = pending.style;
    if (kv.key == "IndentWidth") {
        set_int(s.indent_width);
    } else if (kv.key == "ContinuationIndentWidth") {
        set_int(s.continuation_indent);
    } else if (kv.key == "TabWidth") {
        set_int(s.tab_width);
    } else if (kv.key == "UseTab") {
        if (value_in(kv.value, {"Never", "false"})) {
            s.use_tabs = false;
        } else if (value_in(kv.value, {"ForIndentation", "ForContinuationAndIndentation",
                                       "AlignWithSpaces", "Always", "true"})) {
            s.use_tabs = true;
        } else {
            warn_value();
        }
    } else if (kv.key == "AccessModifierOffset") {
        set_int(pending.access_modifier_offset);
    } else if (kv.key == "NamespaceIndentation") {
        if (kv.value == "None") {
            s.namespace_indentation = NI::None;
        } else if (kv.value == "Inner") {
            s.namespace_indentation = NI::Inner;
        } else if (kv.value == "All") {
            s.namespace_indentation = NI::All;
        } else {
            warn_value();
        }
    } else if (kv.key == "IndentCaseLabels") {
        set_bool(s.indent_case_label);
    } else if (kv.key == "Cpp11BracedListStyle") {
        // Enum since clang-format 22; true/false accepted for compatibility.
        if (value_in(kv.value, {"Block", "false"})) {
            s.brace_init_continuation = false;
        } else if (value_in(kv.value, {"AlignFirstComment", "FunctionCall", "true"})) {
            s.brace_init_continuation = true;
        } else {
            warn_value();
        }
    } else if (kv.key == "AlignAfterOpenBracket") {
        // AlwaysBreak/BlockIndent break after the bracket and use continuation
        // indent, so for reindentation purposes they behave like DontAlign.
        if (value_in(kv.value, {"Align", "true"})) {
            s.align_open_bracket = true;
        } else if (value_in(kv.value, {"DontAlign", "false", "AlwaysBreak", "BlockIndent"})) {
            s.align_open_bracket = false;
        } else {
            warn_value();
        }
    } else if (kv.key == "IndentWrappedFunctionNames") {
        set_bool(s.indent_wrapped_function_names);
    } else if (kv.key == "AlignOperands") {
        // AlignAfterOperator additionally un-indents the operator itself;
        // approximated as plain alignment.
        if (value_in(kv.value, {"Align", "AlignAfterOperator", "true"})) {
            s.align_operands = true;
        } else if (value_in(kv.value, {"DontAlign", "false"})) {
            s.align_operands = false;
        } else {
            warn_value();
        }
    } else if (kv.key == "BreakBeforeTernaryOperators") {
        set_bool(s.break_before_ternary);
    } else if (kv.key == "ConstructorInitializerIndentWidth") {
        set_int(pending.ctor_init_width);
    } else if (kv.key == "BreakConstructorInitializers") {
        using CtorStyle = CppIndentStyle::ConstructorInitializerStyle;
        if (kv.value == "BeforeComma") {
            s.constructor_initializers = CtorStyle::AlignWithColon;
        } else if (value_in(kv.value, {"BeforeColon", "AfterColon"})) {
            // AfterColon only moves where the ':' itself is written; wrapped
            // items still align with the first initializer.
            s.constructor_initializers = CtorStyle::AlignFirstInitializer;
        } else {
            warn_value();
        }
    } else if (kv.key == "IndentPPDirectives") {
        if (kv.value != "None") {
            result.warnings.push_back(
                std::format("unsupported: IndentPPDirectives: {}", kv.value));
        }
    } else if (kv.key == "IndentAccessModifiers") {
        if (kv.value == "true") {
            result.warnings.push_back("unsupported: IndentAccessModifiers: true");
        }
    } else if (kv.key == "LambdaBodyIndentation") {
        if (kv.value == "OuterScope") {
            result.warnings.push_back("unsupported: LambdaBodyIndentation: OuterScope");
        }
    } else if (kv.key == "IndentExternBlock") {
        if (kv.value == "Indent") {
            result.warnings.push_back(
                "unsupported: IndentExternBlock: Indent (extern blocks follow "
                "NamespaceIndentation)");
        }
    } else if (kv.key == "IndentCaseBlocks") {
        if (kv.value == "true") {
            result.warnings.push_back("unsupported: IndentCaseBlocks: true");
        }
    } else if (kv.key == "DisableFormat") {
        if (kv.value == "true") {
            result.disable_format = true;
        }
    }
    // Every other key is not about indentation; ignore silently.
}

} // namespace

bool apply_clang_format_preset(std::string_view name, CppIndentStyle& style) {
    const Preset* preset = find_preset(name);
    if (!preset) {
        return false;
    }
    PendingStyle pending(style);
    apply_preset(*preset, pending);
    pending.style.access_specifier_offset =
        pending.style.indent_width + pending.access_modifier_offset;
    style = pending.style;
    return true;
}

ClangFormatStyle parse_clang_format_yaml(std::string_view yaml, const CppIndentStyle& base) {
    ClangFormatStyle result;
    PendingStyle pending(base);

    for (const YamlDocument& doc : split_documents(yaml)) {
        if (const std::string_view* language = doc_value(doc, "Language");
            language && *language != "Cpp") {
            continue;
        }
        // BasedOnStyle applies before every other key of its document,
        // regardless of position — same as clang-format's reader.
        if (const std::string_view* based_on = doc_value(doc, "BasedOnStyle")) {
            if (iequals(*based_on, "InheritParentConfig")) {
                result.inherit_parent = true;
            } else if (const Preset* preset = find_preset(*based_on)) {
                apply_preset(*preset, pending);
            } else {
                result.warnings.push_back(
                    std::format("unknown BasedOnStyle: '{}'", *based_on));
            }
        }
        for (const KeyValue& kv : doc) {
            if (kv.key == "BasedOnStyle" || kv.key == "Language") {
                continue;
            }
            apply_key(kv, pending, result);
        }
    }

    pending.style.access_specifier_offset =
        pending.style.indent_width + pending.access_modifier_offset;
    if (pending.ctor_init_width != pending.style.continuation_indent) {
        result.warnings.push_back(std::format(
            "unsupported: ConstructorInitializerIndentWidth {} differs from "
            "ContinuationIndentWidth {}; keeping first-initializer alignment",
            pending.ctor_init_width, pending.style.continuation_indent));
    }
    result.style = pending.style;
    return result;
}

} // namespace cind
