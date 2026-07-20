#include "script/guile_async_bridge.hpp"

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <format>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace cind {

namespace {

struct CallbackSet {
    SCM completed = SCM_UNDEFINED;
    SCM failed = SCM_BOOL_F;
    SCM cancelled = SCM_BOOL_F;
};

bool scheme_false(SCM value) {
    return scm_to_bool(scm_eq_p(value, SCM_BOOL_F)) != 0;
}

bool scheme_true(SCM value) {
    return !scheme_false(value);
}

std::string scheme_string(SCM value) {
    char* converted = scm_to_utf8_string(value);
    if (converted == nullptr) {
        throw std::runtime_error("Guile failed to convert a string");
    }
    std::string result(converted);
    std::free(converted);
    return result;
}

std::string scheme_string_with_nuls(SCM value) {
    std::size_t length = 0;
    char* converted = scm_to_utf8_stringn(value, &length);
    if (converted == nullptr) {
        throw std::runtime_error("Guile failed to convert a string");
    }
    std::string result(converted, length);
    std::free(converted);
    return result;
}

std::string scheme_name(SCM value, const char* caller, int position) {
    if (!scheme_true(scm_symbol_p(value))) {
        scm_wrong_type_arg_msg(caller, position, value, "symbol");
    }
    return scheme_string(scm_symbol_to_string(value));
}

template <typename Tag> EntityId<Tag> entity_id(SCM value, const char* caller, int position) {
    if (!scm_is_vector(value) || scm_c_vector_length(value) != 2 ||
        scm_is_unsigned_integer(scm_c_vector_ref(value, 0), 0,
                                std::numeric_limits<std::uint32_t>::max()) == 0 ||
        scm_is_unsigned_integer(scm_c_vector_ref(value, 1), 1,
                                std::numeric_limits<std::uint32_t>::max()) == 0) {
        scm_wrong_type_arg_msg(caller, position, value, "entity ID vector");
    }
    return {.slot = scm_to_uint32(scm_c_vector_ref(value, 0)),
            .generation = scm_to_uint32(scm_c_vector_ref(value, 1))};
}

bool symbol_is(SCM value, const char* expected) {
    return scheme_true(scm_symbol_p(value)) &&
           scheme_true(scm_eq_p(value, scm_from_utf8_symbol(expected)));
}

SCM name_symbol(std::string_view name) {
    return scm_from_utf8_symbol(std::string(name).c_str());
}

std::vector<std::string> string_sequence(SCM value, const char* caller) {
    std::vector<std::string> result;
    if (scm_is_vector(value)) {
        const std::size_t size = scm_c_vector_length(value);
        result.reserve(size);
        for (std::size_t index = 0; index < size; ++index) {
            const SCM item = scm_c_vector_ref(value, index);
            if (!scm_is_string(item)) {
                scm_wrong_type_arg_msg(caller, 2, item, "string");
            }
            result.push_back(scheme_string(item));
        }
        return result;
    }
    const long size = scm_ilength(value);
    if (size < 0) {
        scm_wrong_type_arg_msg(caller, 2, value, "proper list or vector of strings");
    }
    result.reserve(static_cast<std::size_t>(size));
    for (SCM rest = value; !scheme_true(scm_null_p(rest)); rest = scm_cdr(rest)) {
        const SCM item = scm_car(rest);
        if (!scm_is_string(item)) {
            scm_wrong_type_arg_msg(caller, 2, item, "string");
        }
        result.push_back(scheme_string(item));
    }
    return result;
}

std::vector<ProjectDiscoveryProvider> project_providers(SCM value, const char* caller,
                                                        int position) {
    if (!scm_is_vector(value)) {
        scm_wrong_type_arg_msg(caller, position, value, "project provider vector");
    }
    const std::size_t count = scm_c_vector_length(value);
    std::vector<ProjectDiscoveryProvider> result;
    result.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const SCM provider = scm_c_vector_ref(value, index);
        if (!scm_is_vector(provider) || scm_c_vector_length(provider) != 2 ||
            !scm_is_string(scm_c_vector_ref(provider, 0))) {
            scm_wrong_type_arg_msg(caller, position, provider, "#(provider-name marker-vector)");
        }
        result.push_back({.name = scheme_string(scm_c_vector_ref(provider, 0)),
                          .markers = string_sequence(scm_c_vector_ref(provider, 1), caller)});
    }
    return result;
}

SCM cpp_indent_style_value(const CppIndentStyle& style) {
    SCM converted = scm_c_make_vector(18, SCM_UNSPECIFIED);
    scm_c_vector_set_x(converted, 0, name_symbol("cpp-indent-style"));
    scm_c_vector_set_x(converted, 1, scm_from_int(style.indent_width));
    scm_c_vector_set_x(converted, 2, scm_from_int(style.continuation_indent));
    scm_c_vector_set_x(converted, 3, scm_from_int(style.tab_width));
    scm_c_vector_set_x(converted, 4, scm_from_bool(style.use_tabs));
    scm_c_vector_set_x(converted, 5, scm_from_bool(style.align_open_bracket));
    scm_c_vector_set_x(converted, 6, scm_from_bool(style.brace_init_continuation));
    scm_c_vector_set_x(converted, 7, scm_from_bool(style.indent_wrapped_function_names));
    scm_c_vector_set_x(converted, 8, scm_from_bool(style.align_operands));
    scm_c_vector_set_x(converted, 9, scm_from_bool(style.break_before_ternary));
    const char* namespace_indent =
        style.namespace_indentation == CppIndentStyle::NamespaceIndentation::None    ? "none"
        : style.namespace_indentation == CppIndentStyle::NamespaceIndentation::Inner ? "inner"
                                                                                     : "all";
    scm_c_vector_set_x(converted, 10, name_symbol(namespace_indent));
    scm_c_vector_set_x(converted, 11, scm_from_bool(style.indent_type_body));
    scm_c_vector_set_x(converted, 12, scm_from_bool(style.indent_case_label));
    scm_c_vector_set_x(converted, 13, scm_from_bool(style.indent_case_body));
    scm_c_vector_set_x(converted, 14, scm_from_int(style.access_specifier_offset));
    const char* pp_indent =
        style.pp_directive_indent == CppIndentStyle::PPDirectiveIndent::None        ? "none"
        : style.pp_directive_indent == CppIndentStyle::PPDirectiveIndent::AfterHash ? "after-hash"
                                                                                    : "before-hash";
    scm_c_vector_set_x(converted, 15, name_symbol(pp_indent));
    scm_c_vector_set_x(converted, 16, scm_from_int(style.pp_indent_width));
    const char* constructor_style =
        style.constructor_initializers == CppIndentStyle::ConstructorInitializerStyle::NormalIndent
            ? "normal-indent"
        : style.constructor_initializers ==
                CppIndentStyle::ConstructorInitializerStyle::ContinuationIndent
            ? "continuation-indent"
        : style.constructor_initializers ==
                CppIndentStyle::ConstructorInitializerStyle::AlignFirstInitializer
            ? "align-first-initializer"
        : style.constructor_initializers ==
                CppIndentStyle::ConstructorInitializerStyle::AlignAfterColon
            ? "align-after-colon"
            : "align-with-colon";
    scm_c_vector_set_x(converted, 17, name_symbol(constructor_style));
    return converted;
}

} // namespace

ScriptAsyncRequest script_async_request_from_scheme(SCM value, const char* caller, int position) {
    if (!scm_is_vector(value) || scm_c_vector_length(value) == 0) {
        scm_wrong_type_arg_msg(caller, position, value, "async request vector");
    }
    const std::size_t size = scm_c_vector_length(value);
    const SCM tag = scm_c_vector_ref(value, 0);
    if (symbol_is(tag, "file-read")) {
        if (size != 2 || !scm_is_string(scm_c_vector_ref(value, 1))) {
            scm_wrong_type_arg_msg(caller, position, value, "#(file-read path) request");
        }
        return ScriptFileReadRequest{.path = scheme_string(scm_c_vector_ref(value, 1))};
    }
    if (symbol_is(tag, "file-write")) {
        if (size != 3 || !scm_is_string(scm_c_vector_ref(value, 1)) ||
            !scm_is_string(scm_c_vector_ref(value, 2))) {
            scm_wrong_type_arg_msg(caller, position, value, "#(file-write path contents) request");
        }
        return ScriptFileWriteRequest{.path = scheme_string(scm_c_vector_ref(value, 1)),
                                      .contents =
                                          scheme_string_with_nuls(scm_c_vector_ref(value, 2))};
    }
    if (symbol_is(tag, "directory-list")) {
        if (size != 3 || !scm_is_string(scm_c_vector_ref(value, 1)) ||
            scm_is_unsigned_integer(scm_c_vector_ref(value, 2), 0,
                                    std::numeric_limits<std::size_t>::max()) == 0) {
            scm_wrong_type_arg_msg(caller, position, value,
                                   "#(directory-list path non-negative-maximum-entries) request");
        }
        return ScriptDirectoryListRequest{.path = scheme_string(scm_c_vector_ref(value, 1)),
                                          .maximum_entries =
                                              scm_to_size_t(scm_c_vector_ref(value, 2))};
    }
    if (symbol_is(tag, "directory-list-many")) {
        if (size != 3 || !scm_is_vector(scm_c_vector_ref(value, 1)) ||
            scm_is_unsigned_integer(scm_c_vector_ref(value, 2), 0,
                                    std::numeric_limits<std::size_t>::max()) == 0) {
            scm_wrong_type_arg_msg(
                caller, position, value,
                "#(directory-list-many path-vector non-negative-maximum-entries) request");
        }
        const SCM paths_value = scm_c_vector_ref(value, 1);
        std::vector<std::string> paths;
        paths.reserve(scm_c_vector_length(paths_value));
        for (std::size_t index = 0; index < scm_c_vector_length(paths_value); ++index) {
            const SCM path = scm_c_vector_ref(paths_value, index);
            if (!scm_is_string(path)) {
                scm_wrong_type_arg_msg(caller, position, value,
                                       "directory-list-many string path vector");
            }
            paths.push_back(scheme_string(path));
        }
        return ScriptDirectoryListManyRequest{.paths = std::move(paths),
                                              .maximum_entries =
                                                  scm_to_size_t(scm_c_vector_ref(value, 2))};
    }
    if (symbol_is(tag, "clang-format-style")) {
        if (size != 4 || !scm_is_string(scm_c_vector_ref(value, 1)) ||
            !scm_is_string(scm_c_vector_ref(value, 2)) ||
            !scm_is_string(scm_c_vector_ref(value, 3))) {
            scm_wrong_type_arg_msg(
                caller, position, value,
                "#(clang-format-style path fallback-preset fallback-origin) request");
        }
        return ScriptClangFormatStyleRequest{
            .path = scheme_string(scm_c_vector_ref(value, 1)),
            .fallback_preset = scheme_string(scm_c_vector_ref(value, 2)),
            .fallback_origin = scheme_string(scm_c_vector_ref(value, 3))};
    }
    if (symbol_is(tag, "project-discovery")) {
        if (size != 3 || !scm_is_string(scm_c_vector_ref(value, 1))) {
            scm_wrong_type_arg_msg(caller, position, value,
                                   "#(project-discovery path provider-vector) request");
        }
        return ScriptProjectDiscoveryRequest{
            .path = scheme_string(scm_c_vector_ref(value, 1)),
            .providers = project_providers(scm_c_vector_ref(value, 2), caller, position)};
    }
    if (symbol_is(tag, "rg-result-parse")) {
        if (size != 3 || !scm_is_string(scm_c_vector_ref(value, 1)) ||
            !scm_is_string(scm_c_vector_ref(value, 2))) {
            scm_wrong_type_arg_msg(caller, position, value,
                                   "#(rg-result-parse project-root output) request");
        }
        return ScriptRgResultParseRequest{.project_root = scheme_string(scm_c_vector_ref(value, 1)),
                                          .output =
                                              scheme_string_with_nuls(scm_c_vector_ref(value, 2))};
    }
    if (symbol_is(tag, "process")) {
        if (size != 4 || !scm_is_string(scm_c_vector_ref(value, 1)) ||
            !scm_is_string(scm_c_vector_ref(value, 3))) {
            scm_wrong_type_arg_msg(
                caller, position, value,
                "#(process executable string-sequence working-directory) request");
        }
        return ScriptProcessRequest{.file = scheme_string(scm_c_vector_ref(value, 1)),
                                    .arguments =
                                        string_sequence(scm_c_vector_ref(value, 2), caller),
                                    .working_directory = scheme_string(scm_c_vector_ref(value, 3))};
    }
    if (symbol_is(tag, "lsp-navigation")) {
        if (size != 6 || !scheme_true(scm_symbol_p(scm_c_vector_ref(value, 4))) ||
            !scm_is_string(scm_c_vector_ref(value, 5))) {
            scm_wrong_type_arg_msg(caller, position, value,
                                   "#(lsp-navigation window buffer view kind provider) request");
        }
        return ScriptLspNavigationRequest{
            .target = {.window = entity_id<WindowTag>(scm_c_vector_ref(value, 1), caller, position),
                       .buffer = entity_id<BufferTag>(scm_c_vector_ref(value, 2), caller, position),
                       .view = entity_id<ViewTag>(scm_c_vector_ref(value, 3), caller, position)},
            .kind = scheme_name(scm_c_vector_ref(value, 4), caller, position),
            .provider = scheme_string(scm_c_vector_ref(value, 5))};
    }
    scm_misc_error(caller, "unknown async request kind", SCM_EOL);
    return ScriptFileReadRequest{};
}

namespace {

void protect_callbacks(const CallbackSet& callbacks) {
    (void)scm_gc_protect_object(callbacks.completed);
    if (!scheme_false(callbacks.failed)) {
        (void)scm_gc_protect_object(callbacks.failed);
    }
    if (!scheme_false(callbacks.cancelled)) {
        (void)scm_gc_protect_object(callbacks.cancelled);
    }
}

void unprotect_callbacks(const CallbackSet& callbacks) {
    (void)scm_gc_unprotect_object(callbacks.completed);
    if (!scheme_false(callbacks.failed)) {
        (void)scm_gc_unprotect_object(callbacks.failed);
    }
    if (!scheme_false(callbacks.cancelled)) {
        (void)scm_gc_unprotect_object(callbacks.cancelled);
    }
}

const char* task_kind_name(ScriptAsyncTaskKind kind) {
    switch (kind) {
    case ScriptAsyncTaskKind::FileRead:
        return "file-read";
    case ScriptAsyncTaskKind::FileWrite:
        return "file-write";
    case ScriptAsyncTaskKind::DirectoryList:
        return "directory-list";
    case ScriptAsyncTaskKind::ClangFormatStyle:
        return "clang-format-style";
    case ScriptAsyncTaskKind::ProjectDiscovery:
        return "project-discovery";
    case ScriptAsyncTaskKind::RgResultParse:
        return "rg-result-parse";
    case ScriptAsyncTaskKind::Process:
        return "process";
    case ScriptAsyncTaskKind::LspNavigation:
        return "lsp-navigation";
    }
    return "unknown";
}

} // namespace

SCM script_async_result_to_scheme(ScriptAsyncResult result) {
    return std::visit(
        [](const auto& value) {
            using Result = std::remove_cvref_t<decltype(value)>;
            if constexpr (std::is_same_v<Result, ScriptFileReadResult>) {
                SCM converted = scm_c_make_vector(4, SCM_UNSPECIFIED);
                scm_c_vector_set_x(converted, 0, name_symbol("file-read"));
                scm_c_vector_set_x(converted, 1, scm_from_utf8_string(value.path.c_str()));
                scm_c_vector_set_x(converted, 2, scm_from_bool(value.exists));
                scm_c_vector_set_x(
                    converted, 3,
                    scm_from_utf8_stringn(value.contents.data(), value.contents.size()));
                return converted;
            } else if constexpr (std::is_same_v<Result, ScriptFileWriteResult>) {
                SCM converted = scm_c_make_vector(2, SCM_UNSPECIFIED);
                scm_c_vector_set_x(converted, 0, name_symbol("file-write"));
                scm_c_vector_set_x(converted, 1, scm_from_utf8_string(value.path.c_str()));
                return converted;
            } else if constexpr (std::is_same_v<Result, ScriptDirectoryListResult>) {
                SCM entries = scm_c_make_vector(value.entries.size(), SCM_UNSPECIFIED);
                for (std::size_t index = 0; index < value.entries.size(); ++index) {
                    const ScriptDirectoryEntry& entry = value.entries[index];
                    SCM converted_entry = scm_c_make_vector(3, SCM_UNSPECIFIED);
                    scm_c_vector_set_x(converted_entry, 0,
                                       scm_from_utf8_string(entry.path.c_str()));
                    scm_c_vector_set_x(converted_entry, 1,
                                       scm_from_utf8_string(entry.name.c_str()));
                    scm_c_vector_set_x(converted_entry, 2, scm_from_bool(entry.directory));
                    scm_c_vector_set_x(entries, index, converted_entry);
                }
                SCM converted = scm_c_make_vector(3, SCM_UNSPECIFIED);
                scm_c_vector_set_x(converted, 0, name_symbol("directory-list"));
                scm_c_vector_set_x(converted, 1, scm_from_utf8_string(value.path.c_str()));
                scm_c_vector_set_x(converted, 2, entries);
                return converted;
            } else if constexpr (std::is_same_v<Result, ScriptDirectoryListManyResult>) {
                SCM listings = scm_c_make_vector(value.listings.size(), SCM_UNSPECIFIED);
                for (std::size_t listing_index = 0; listing_index < value.listings.size();
                     ++listing_index) {
                    const ScriptDirectoryListResult& listing = value.listings[listing_index];
                    SCM entries = scm_c_make_vector(listing.entries.size(), SCM_UNSPECIFIED);
                    for (std::size_t entry_index = 0; entry_index < listing.entries.size();
                         ++entry_index) {
                        const ScriptDirectoryEntry& entry = listing.entries[entry_index];
                        SCM converted_entry = scm_c_make_vector(3, SCM_UNSPECIFIED);
                        scm_c_vector_set_x(converted_entry, 0,
                                           scm_from_utf8_string(entry.path.c_str()));
                        scm_c_vector_set_x(converted_entry, 1,
                                           scm_from_utf8_string(entry.name.c_str()));
                        scm_c_vector_set_x(converted_entry, 2, scm_from_bool(entry.directory));
                        scm_c_vector_set_x(entries, entry_index, converted_entry);
                    }
                    SCM converted_listing = scm_c_make_vector(3, SCM_UNSPECIFIED);
                    scm_c_vector_set_x(converted_listing, 0, name_symbol("directory-list"));
                    scm_c_vector_set_x(converted_listing, 1,
                                       scm_from_utf8_string(listing.path.c_str()));
                    scm_c_vector_set_x(converted_listing, 2, entries);
                    scm_c_vector_set_x(listings, listing_index, converted_listing);
                }
                SCM converted = scm_c_make_vector(2, SCM_UNSPECIFIED);
                scm_c_vector_set_x(converted, 0, name_symbol("directory-list-many"));
                scm_c_vector_set_x(converted, 1, listings);
                return converted;
            } else if constexpr (std::is_same_v<Result, ScriptClangFormatStyleResult>) {
                SCM converted = scm_c_make_vector(5, SCM_UNSPECIFIED);
                scm_c_vector_set_x(converted, 0, name_symbol("clang-format-style"));
                scm_c_vector_set_x(converted, 1, scm_from_utf8_string(value.path.c_str()));
                scm_c_vector_set_x(converted, 2, scm_from_bool(value.found));
                scm_c_vector_set_x(converted, 3, cpp_indent_style_value(value.style));
                scm_c_vector_set_x(converted, 4, scm_from_utf8_string(value.origin.c_str()));
                return converted;
            } else if constexpr (std::is_same_v<Result, ScriptProjectDiscoveryResult>) {
                SCM converted = scm_c_make_vector(5, SCM_BOOL_F);
                scm_c_vector_set_x(converted, 0, name_symbol("project-discovery"));
                scm_c_vector_set_x(converted, 1, scm_from_utf8_string(value.path.c_str()));
                if (value.discovery) {
                    scm_c_vector_set_x(converted, 2,
                                       scm_from_utf8_string(value.discovery->root.c_str()));
                    scm_c_vector_set_x(converted, 3,
                                       scm_from_utf8_string(value.discovery->provider.c_str()));
                    scm_c_vector_set_x(converted, 4,
                                       scm_from_utf8_string(value.discovery->marker.c_str()));
                }
                return converted;
            } else if constexpr (std::is_same_v<Result, ScriptRgResultParseResult>) {
                SCM locations = scm_c_make_vector(value.locations.size(), SCM_UNSPECIFIED);
                for (std::size_t index = 0; index < value.locations.size(); ++index) {
                    const BufferLocation& location = value.locations[index];
                    SCM converted_location = scm_c_make_vector(7, SCM_UNSPECIFIED);
                    scm_c_vector_set_x(converted_location, 0,
                                       scm_from_uint32(location.source_range.start.value));
                    scm_c_vector_set_x(converted_location, 1,
                                       scm_from_uint32(location.source_range.end.value));
                    scm_c_vector_set_x(converted_location, 2,
                                       scm_from_utf8_string(location.resource.c_str()));
                    scm_c_vector_set_x(converted_location, 3,
                                       scm_from_uint32(location.target.line));
                    scm_c_vector_set_x(converted_location, 4,
                                       scm_from_uint32(location.target.column));
                    scm_c_vector_set_x(
                        converted_location, 5,
                        scm_from_utf8_stringn(location.excerpt.data(), location.excerpt.size()));
                    scm_c_vector_set_x(converted_location, 6, name_symbol("bytes"));
                    scm_c_vector_set_x(locations, index, converted_location);
                }
                SCM converted = scm_c_make_vector(3, SCM_UNSPECIFIED);
                scm_c_vector_set_x(converted, 0, name_symbol("rg-result-parse"));
                scm_c_vector_set_x(converted, 1,
                                   scm_from_utf8_stringn(value.text.data(), value.text.size()));
                scm_c_vector_set_x(converted, 2, locations);
                return converted;
            } else if constexpr (std::is_same_v<Result, ScriptProcessResult>) {
                SCM converted = scm_c_make_vector(5, SCM_UNSPECIFIED);
                scm_c_vector_set_x(converted, 0, name_symbol("process"));
                scm_c_vector_set_x(converted, 1, scm_from_int64(value.exit_status));
                scm_c_vector_set_x(converted, 2, scm_from_int(value.term_signal));
                scm_c_vector_set_x(converted, 3,
                                   scm_from_utf8_stringn(value.standard_output.data(),
                                                         value.standard_output.size()));
                scm_c_vector_set_x(converted, 4,
                                   scm_from_utf8_stringn(value.standard_error.data(),
                                                         value.standard_error.size()));
                return converted;
            } else {
                SCM locations = scm_c_make_vector(value.locations.size(), SCM_UNSPECIFIED);
                for (std::size_t index = 0; index < value.locations.size(); ++index) {
                    const ScriptLspLocation& location = value.locations[index];
                    SCM converted_location = scm_c_make_vector(6, SCM_UNSPECIFIED);
                    scm_c_vector_set_x(converted_location, 0,
                                       scm_from_utf8_string(location.resource.c_str()));
                    scm_c_vector_set_x(converted_location, 1, scm_from_uint32(location.start_line));
                    scm_c_vector_set_x(converted_location, 2,
                                       scm_from_uint32(location.start_column));
                    scm_c_vector_set_x(converted_location, 3, scm_from_uint32(location.end_line));
                    scm_c_vector_set_x(converted_location, 4, scm_from_uint32(location.end_column));
                    scm_c_vector_set_x(converted_location, 5, name_symbol("utf-16"));
                    scm_c_vector_set_x(locations, index, converted_location);
                }
                SCM converted = scm_c_make_vector(2, SCM_UNSPECIFIED);
                scm_c_vector_set_x(converted, 0, name_symbol("lsp-navigation"));
                scm_c_vector_set_x(converted, 1, locations);
                return converted;
            }
        },
        result);
}

namespace {

struct ProcedureCall {
    SCM procedure = SCM_UNDEFINED;
    std::vector<SCM> arguments;
    SCM result = SCM_UNDEFINED;
    std::exception_ptr cpp_failure;
    std::string error;
};

SCM call_body(void* data) {
    auto& call = *static_cast<ProcedureCall*>(data);
    try {
        call.result = scm_call_n(call.procedure, call.arguments.data(), call.arguments.size());
    } catch (...) {
        call.cpp_failure = std::current_exception();
    }
    return call.result;
}

SCM call_handler(void* data, SCM tag, SCM arguments) {
    auto& call = *static_cast<ProcedureCall*>(data);
    try {
        const SCM message = scm_simple_format(SCM_BOOL_F, scm_from_utf8_string("~S: ~S"),
                                              scm_list_2(tag, arguments));
        call.error = scheme_string(message);
    } catch (...) {
        call.cpp_failure = std::current_exception();
    }
    return SCM_UNSPECIFIED;
}

std::expected<SCM, std::string> invoke(SCM procedure, std::vector<SCM> arguments) {
    ProcedureCall call;
    call.procedure = procedure;
    call.arguments = std::move(arguments);
    (void)scm_c_catch(SCM_BOOL_T, call_body, &call, call_handler, &call, nullptr, nullptr);
    if (call.cpp_failure) {
        try {
            std::rethrow_exception(call.cpp_failure);
        } catch (const std::exception& exception) {
            return std::unexpected(std::format("C++ Guile bridge failure: {}", exception.what()));
        } catch (...) {
            return std::unexpected("unknown C++ Guile bridge failure");
        }
    }
    if (!call.error.empty()) {
        return std::unexpected(std::move(call.error));
    }
    return call.result;
}

GuileAsyncBridgeResolver bridge_resolver = nullptr;
std::once_flag bridge_bindings_once;

SCM start_binding(SCM host, SCM request, SCM completed, SCM failed, SCM cancelled) {
    return bridge_resolver(host, "%start-async-task!").start(request, completed, failed, cancelled);
}

SCM cancel_binding(SCM host, SCM task) {
    return bridge_resolver(host, "%cancel-async-task!").cancel(task);
}

SCM summaries_binding(SCM host) {
    return bridge_resolver(host, "%async-task-summaries").summaries();
}

} // namespace

struct GuileAsyncBridgeState {
    std::thread::id owner;
    bool active = true;
    GuileAsyncBridge::Start start;
    GuileAsyncBridge::Cancel cancel;
    GuileAsyncBridge::Inspect inspect;
    GuileAsyncBridge::ReportError report_error;
    std::unordered_map<std::uint64_t, CallbackSet> callbacks;
    std::unordered_map<std::uint64_t, ScriptAsyncCallbacks> native_callbacks;
};

namespace {

void report(const std::shared_ptr<GuileAsyncBridgeState>& state, std::string message) {
    if (state->report_error) {
        state->report_error(std::move(message));
    }
}

std::optional<CallbackSet> take_callbacks(const std::shared_ptr<GuileAsyncBridgeState>& state,
                                          std::uint64_t task) {
    auto node = state->callbacks.extract(task);
    if (node.empty()) {
        return std::nullopt;
    }
    return node.mapped();
}

void invoke_callback(const std::shared_ptr<GuileAsyncBridgeState>& state, std::uint64_t task,
                     SCM procedure, std::vector<SCM> arguments, std::string_view kind) {
    if (std::this_thread::get_id() != state->owner || !state->active) {
        report(state, "Guile async callback used outside its runtime");
        return;
    }
    if (scheme_false(procedure)) {
        return;
    }
    arguments.insert(arguments.begin(), scm_from_uint64(task));
    const std::expected<SCM, std::string> invoked = invoke(procedure, std::move(arguments));
    if (!invoked) {
        report(state, std::format("Guile async {} callback failed: {}", kind, invoked.error()));
    }
}

void complete_task(const std::weak_ptr<GuileAsyncBridgeState>& weak_state, std::uint64_t task,
                   ScriptAsyncResult result) {
    const std::shared_ptr<GuileAsyncBridgeState> state = weak_state.lock();
    if (!state || !state->active) {
        return;
    }
    std::optional<CallbackSet> callbacks = take_callbacks(state, task);
    if (!callbacks) {
        return;
    }
    try {
        invoke_callback(state, task, callbacks->completed,
                        {script_async_result_to_scheme(std::move(result))}, "completion");
    } catch (const std::exception& exception) {
        report(state,
               std::format("failed to deliver Guile async completion: {}", exception.what()));
    } catch (...) {
        report(state, "failed to deliver Guile async completion");
    }
    unprotect_callbacks(*callbacks);
}

void cancel_task(const std::weak_ptr<GuileAsyncBridgeState>& weak_state, std::uint64_t task) {
    const std::shared_ptr<GuileAsyncBridgeState> state = weak_state.lock();
    if (!state || !state->active) {
        return;
    }
    std::optional<CallbackSet> callbacks = take_callbacks(state, task);
    if (!callbacks) {
        return;
    }
    invoke_callback(state, task, callbacks->cancelled, {}, "cancellation");
    unprotect_callbacks(*callbacks);
}

void fail_task(const std::weak_ptr<GuileAsyncBridgeState>& weak_state, std::uint64_t task,
               std::string message) {
    const std::shared_ptr<GuileAsyncBridgeState> state = weak_state.lock();
    if (!state || !state->active) {
        return;
    }
    std::optional<CallbackSet> callbacks = take_callbacks(state, task);
    if (!callbacks) {
        return;
    }
    if (scheme_false(callbacks->failed)) {
        report(state, std::format("Guile async task failed: {}", message));
    } else {
        invoke_callback(state, task, callbacks->failed, {scm_from_utf8_string(message.c_str())},
                        "failure");
    }
    unprotect_callbacks(*callbacks);
}

std::optional<ScriptAsyncCallbacks>
take_native_callbacks(const std::shared_ptr<GuileAsyncBridgeState>& state, std::uint64_t task) {
    auto node = state->native_callbacks.extract(task);
    if (node.empty()) {
        return std::nullopt;
    }
    return std::move(node.mapped());
}

void deliver_native_completion(const std::weak_ptr<GuileAsyncBridgeState>& weak_state,
                               std::uint64_t task, ScriptAsyncResult result) {
    const std::shared_ptr<GuileAsyncBridgeState> state = weak_state.lock();
    if (!state || !state->active || std::this_thread::get_id() != state->owner) {
        return;
    }
    std::optional<ScriptAsyncCallbacks> callbacks = take_native_callbacks(state, task);
    if (callbacks && callbacks->completed) {
        try {
            callbacks->completed(task, std::move(result));
        } catch (const std::exception& exception) {
            report(state,
                   std::format("native Guile async completion failed: {}", exception.what()));
        } catch (...) {
            report(state, "native Guile async completion failed");
        }
    }
}

void deliver_native_cancellation(const std::weak_ptr<GuileAsyncBridgeState>& weak_state,
                                 std::uint64_t task) {
    const std::shared_ptr<GuileAsyncBridgeState> state = weak_state.lock();
    if (!state || !state->active || std::this_thread::get_id() != state->owner) {
        return;
    }
    std::optional<ScriptAsyncCallbacks> callbacks = take_native_callbacks(state, task);
    if (callbacks && callbacks->cancelled) {
        try {
            callbacks->cancelled(task);
        } catch (const std::exception& exception) {
            report(state,
                   std::format("native Guile async cancellation failed: {}", exception.what()));
        } catch (...) {
            report(state, "native Guile async cancellation failed");
        }
    }
}

void deliver_native_failure(const std::weak_ptr<GuileAsyncBridgeState>& weak_state,
                            std::uint64_t task, std::string message) {
    const std::shared_ptr<GuileAsyncBridgeState> state = weak_state.lock();
    if (!state || !state->active || std::this_thread::get_id() != state->owner) {
        return;
    }
    std::optional<ScriptAsyncCallbacks> callbacks = take_native_callbacks(state, task);
    if (callbacks && callbacks->failed) {
        try {
            callbacks->failed(task, std::move(message));
        } catch (const std::exception& exception) {
            report(state,
                   std::format("native Guile async failure callback failed: {}", exception.what()));
        } catch (...) {
            report(state, "native Guile async failure callback failed");
        }
    }
}

} // namespace

GuileAsyncBridge::GuileAsyncBridge(Start start, Cancel cancel, Inspect inspect,
                                   ReportError report_error)
    : state_(std::make_shared<GuileAsyncBridgeState>(
          GuileAsyncBridgeState{.owner = std::this_thread::get_id(),
                                .active = true,
                                .start = std::move(start),
                                .cancel = std::move(cancel),
                                .inspect = std::move(inspect),
                                .report_error = std::move(report_error),
                                .callbacks = {},
                                .native_callbacks = {}})) {}

GuileAsyncBridge::~GuileAsyncBridge() {
    release(false);
}

// The Guile ABI fixes four adjacent SCM arguments; validation preserves their
// semantic positions.
SCM GuileAsyncBridge::start(SCM request, SCM completed, SCM failed, SCM cancelled) {
    constexpr const char* caller = "%start-async-task!";
    if (!state_->active) {
        scm_misc_error(caller, "async task capability is shut down", SCM_EOL);
    }
    if (!state_->start) {
        scm_misc_error(caller, "async task capability is unavailable", SCM_EOL);
    }
    if (!scheme_true(scm_procedure_p(completed))) {
        scm_wrong_type_arg_msg(caller, 3, completed, "procedure");
    }
    if (!scheme_false(failed) && !scheme_true(scm_procedure_p(failed))) {
        scm_wrong_type_arg_msg(caller, 4, failed, "procedure or #f");
    }
    if (!scheme_false(cancelled) && !scheme_true(scm_procedure_p(cancelled))) {
        scm_wrong_type_arg_msg(caller, 5, cancelled, "procedure or #f");
    }
    ScriptAsyncRequest native_request = script_async_request_from_scheme(request, caller, 2);
    const CallbackSet callbacks{.completed = completed, .failed = failed, .cancelled = cancelled};
    protect_callbacks(callbacks);
    try {
        const std::weak_ptr<GuileAsyncBridgeState> state = state_;
        std::expected<std::uint64_t, std::string> started =
            state_->start(std::move(native_request),
                          {.completed =
                               [state](std::uint64_t task, ScriptAsyncResult result) {
                                   complete_task(state, task, std::move(result));
                               },
                           .cancelled = [state](std::uint64_t task) { cancel_task(state, task); },
                           .failed =
                               [state](std::uint64_t task, std::string message) {
                                   fail_task(state, task, std::move(message));
                               }});
        if (!started) {
            unprotect_callbacks(callbacks);
            scm_misc_error(caller, "host operation failed: ~A",
                           scm_list_1(scm_from_utf8_string(started.error().c_str())));
        }
        const auto [iterator, inserted] = state_->callbacks.emplace(*started, callbacks);
        (void)iterator;
        if (!inserted) {
            if (state_->cancel) {
                (void)state_->cancel(*started);
            }
            unprotect_callbacks(callbacks);
            scm_misc_error(caller, "async task ID is already active", SCM_EOL);
        }
        return scm_from_uint64(*started);
    } catch (const std::exception& exception) {
        unprotect_callbacks(callbacks);
        scm_misc_error(caller, "host operation failed: ~A",
                       scm_list_1(scm_from_utf8_string(exception.what())));
    } catch (...) {
        unprotect_callbacks(callbacks);
        scm_misc_error(caller, "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM GuileAsyncBridge::cancel(SCM task) {
    constexpr const char* caller = "%cancel-async-task!";
    if (scm_is_unsigned_integer(task, 1, std::numeric_limits<std::uint64_t>::max()) == 0) {
        scm_wrong_type_arg_msg(caller, 2, task, "positive async task ID");
    }
    if (!state_->cancel) {
        scm_misc_error(caller, "async cancellation capability is unavailable", SCM_EOL);
    }
    try {
        return scm_from_bool(state_->cancel(scm_to_uint64(task)));
    } catch (const std::exception& exception) {
        scm_misc_error(caller, "host operation failed: ~A",
                       scm_list_1(scm_from_utf8_string(exception.what())));
    } catch (...) {
        scm_misc_error(caller, "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM GuileAsyncBridge::summaries() const {
    constexpr const char* caller = "%async-task-summaries";
    if (!state_->inspect) {
        scm_misc_error(caller, "async task inspection capability is unavailable", SCM_EOL);
    }
    try {
        const std::vector<ScriptAsyncTaskSummary> tasks = state_->inspect();
        SCM result = scm_c_make_vector(tasks.size(), SCM_UNSPECIFIED);
        for (std::size_t index = 0; index < tasks.size(); ++index) {
            SCM summary = scm_c_make_vector(2, SCM_UNSPECIFIED);
            scm_c_vector_set_x(summary, 0, scm_from_uint64(tasks[index].id));
            scm_c_vector_set_x(summary, 1, name_symbol(task_kind_name(tasks[index].kind)));
            scm_c_vector_set_x(result, index, summary);
        }
        return result;
    } catch (const std::exception& exception) {
        scm_misc_error(caller, "host operation failed: ~A",
                       scm_list_1(scm_from_utf8_string(exception.what())));
    } catch (...) {
        scm_misc_error(caller, "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

std::expected<std::uint64_t, std::string>
GuileAsyncBridge::start_native_task(ScriptAsyncRequest request, ScriptAsyncCallbacks callbacks) {
    if (std::this_thread::get_id() != state_->owner || !state_->active) {
        return std::unexpected("Guile async task capability is shut down");
    }
    if (!state_->start) {
        return std::unexpected("Guile async task capability is unavailable");
    }
    if (!callbacks.completed) {
        return std::unexpected("native Guile async task requires a completion callback");
    }
    const std::weak_ptr<GuileAsyncBridgeState> state = state_;
    std::expected<std::uint64_t, std::string> started = state_->start(
        std::move(request),
        {.completed =
             [state](std::uint64_t task, ScriptAsyncResult result) {
                 deliver_native_completion(state, task, std::move(result));
             },
         .cancelled = [state](std::uint64_t task) { deliver_native_cancellation(state, task); },
         .failed =
             [state](std::uint64_t task, std::string message) {
                 deliver_native_failure(state, task, std::move(message));
             }});
    if (!started) {
        return std::unexpected(std::move(started.error()));
    }
    const auto [iterator, inserted] =
        state_->native_callbacks.emplace(*started, std::move(callbacks));
    (void)iterator;
    if (!inserted) {
        if (state_->cancel) {
            (void)state_->cancel(*started);
        }
        return std::unexpected("async task ID is already active");
    }
    return *started;
}

bool GuileAsyncBridge::cancel_native_task(std::uint64_t task) {
    if (std::this_thread::get_id() != state_->owner || !state_->active || !state_->cancel ||
        !state_->native_callbacks.contains(task)) {
        return false;
    }
    return state_->cancel(task);
}

std::size_t GuileAsyncBridge::outstanding() const {
    return state_->callbacks.size() + state_->native_callbacks.size();
}

std::vector<std::uint64_t> GuileAsyncBridge::checkpoint() const {
    std::vector<std::uint64_t> result;
    result.reserve(state_->callbacks.size() + state_->native_callbacks.size());
    for (const auto& [task, callbacks] : state_->callbacks) {
        (void)callbacks;
        result.push_back(task);
    }
    for (const auto& [task, callbacks] : state_->native_callbacks) {
        (void)callbacks;
        result.push_back(task);
    }
    return result;
}

void GuileAsyncBridge::rollback_to(std::span<const std::uint64_t> checkpoint) noexcept {
    for (auto iterator = state_->callbacks.begin(); iterator != state_->callbacks.end();) {
        if (std::ranges::find(checkpoint, iterator->first) != checkpoint.end()) {
            ++iterator;
            continue;
        }
        if (state_->cancel) {
            try {
                (void)state_->cancel(iterator->first);
            } catch (...) {
                state_->active = false;
            }
        }
        unprotect_callbacks(iterator->second);
        iterator = state_->callbacks.erase(iterator);
    }
    for (auto iterator = state_->native_callbacks.begin();
         iterator != state_->native_callbacks.end();) {
        if (std::ranges::find(checkpoint, iterator->first) != checkpoint.end()) {
            ++iterator;
            continue;
        }
        if (state_->cancel) {
            try {
                (void)state_->cancel(iterator->first);
            } catch (...) {
                state_->active = false;
            }
        }
        iterator = state_->native_callbacks.erase(iterator);
    }
}

void GuileAsyncBridge::shutdown() noexcept {
    release(true);
}

void GuileAsyncBridge::release(bool cancel_tasks) noexcept {
    state_->active = false;
    for (const auto& [task, callbacks] : state_->callbacks) {
        if (cancel_tasks && state_->cancel) {
            try {
                (void)state_->cancel(task);
            } catch (...) {
                // The native adapter independently makes late delivery inert.
                state_->cancel = {};
            }
        }
        unprotect_callbacks(callbacks);
    }
    for (const auto& [task, callbacks] : state_->native_callbacks) {
        (void)callbacks;
        if (cancel_tasks && state_->cancel) {
            try {
                (void)state_->cancel(task);
            } catch (...) {
                state_->cancel = {};
            }
        }
    }
    state_->callbacks.clear();
    state_->native_callbacks.clear();
}

void initialize_guile_async_host_bindings(GuileAsyncBridgeResolver resolver) {
    std::call_once(bridge_bindings_once, [resolver] {
        bridge_resolver = resolver;
        (void)scm_c_define_gsubr("%start-async-task!", 5, 0, 0,
                                 reinterpret_cast<scm_t_subr>(start_binding));
        (void)scm_c_define_gsubr("%cancel-async-task!", 2, 0, 0,
                                 reinterpret_cast<scm_t_subr>(cancel_binding));
        (void)scm_c_define_gsubr("%async-task-summaries", 1, 0, 0,
                                 reinterpret_cast<scm_t_subr>(summaries_binding));
        scm_c_export("%start-async-task!", "%cancel-async-task!", "%async-task-summaries", nullptr);
    });
}

} // namespace cind
