#include "script/guile_runtime.hpp"

#include "editor/command_loop.hpp"
#include "editor/cpp_mode.hpp"
#include "editor/resource_policy.hpp"
#include "editor/runtime.hpp"
#include "editor/scheme_mode.hpp"
#include "script/guile_async_bridge.hpp"

#include <libguile.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>

namespace cind {

namespace {

constexpr std::array<std::string_view, 20> bundled_guile_modules = {
    "application", "command",    "input",       "lsp",  "async",      "lifecycle", "pointer",
    "extension",   "emacs",      "toy-modal",   "meow", "vim",        "helix",     "structural",
    "paredit",     "minibuffer", "development", "ares", "introspect", "core",
};

const char* command_loop_status_name(CommandLoopStatus status) {
    switch (status) {
    case CommandLoopStatus::NotHandled:
        return "not-handled";
    case CommandLoopStatus::Prefix:
        return "prefix";
    case CommandLoopStatus::PrefixArgument:
        return "prefix-argument";
    case CommandLoopStatus::Executed:
        return "executed";
    case CommandLoopStatus::AwaitingInput:
        return "awaiting-input";
    case CommandLoopStatus::Disabled:
        return "disabled";
    case CommandLoopStatus::Cancelled:
        return "cancelled";
    case CommandLoopStatus::Error:
        return "error";
    }
    throw std::logic_error("unknown command loop status");
}

struct ScriptCommand {
    SCM execute = SCM_UNDEFINED;
    SCM enabled = SCM_BOOL_F;
};

struct ScriptProvider {
    SCM complete = SCM_UNDEFINED;
};

struct ScriptCompletionProvider {
    std::string name;
    SCM complete = SCM_UNDEFINED;
    SCM resolve = SCM_BOOL_F;
};

struct ScriptInputState {
    SCM handler = SCM_BOOL_F;
};

struct ScriptPositionHintProvider {
    SCM procedure = SCM_UNDEFINED;
};

struct ScriptInputStateLifecycle {
    SCM on_enter = SCM_BOOL_F;
    SCM on_exit = SCM_BOOL_F;
};

struct ScriptInputStateObserver {
    SCM procedure = SCM_UNDEFINED;
    InputStateRegistry::ListenerId listener = 0;
};

struct ScriptModePolicyObserver {
    SCM procedure = SCM_UNDEFINED;
    ModeRegistry::ListenerId listener = 0;
};

struct LoadedExtension {
    std::string path;
    SCM module = SCM_UNDEFINED;
};

struct GuileState {
    std::thread::id owner;
    bool active = true;
    std::vector<ScriptCommand> commands;
    std::vector<ScriptProvider> providers;
    std::vector<ScriptCompletionProvider> completion_providers;
    std::unordered_map<std::string, std::size_t> completion_providers_by_name;
    std::vector<ScriptInputState> input_states;
    std::vector<ScriptPositionHintProvider> position_hint_providers;
    std::vector<ScriptInputStateLifecycle> input_state_lifecycles;
    std::vector<ScriptInputStateObserver> input_state_observers;
    std::vector<ScriptModePolicyObserver> mode_policy_observers;
    std::vector<LoadedExtension> extensions;
    SCM evaluation_module = SCM_UNDEFINED;
    bool evaluation_module_initialized = false;
    std::uint64_t command_revision = 0;
    std::uint64_t provider_revision = 0;
    std::uint64_t input_state_revision = 0;
    std::size_t input_state_definitions = 0;
    std::size_t input_strategy_definitions = 0;
    std::uint64_t mode_revision = 0;
    std::uint64_t resource_policy_revision = 0;
    std::uint64_t binding_revision = 0;
    std::size_t mode_definitions = 0;
    std::optional<std::string> last_error;
    std::string definition_source = "scheme";
};

struct HostLease {
    EditorRuntime* runtime = nullptr;
    std::shared_ptr<GuileState> state;
    GuileHostServices services;
    GuileAsyncBridge* async_bridge = nullptr;
    SCM capability = SCM_UNDEFINED;
    std::size_t commands_installed = 0;
    std::size_t providers_installed = 0;
    std::size_t bindings_installed = 0;
    std::size_t input_states_installed = 0;
    std::size_t input_strategies_installed = 0;
    std::size_t modes_installed = 0;
    std::size_t resource_policies_installed = 0;
};

std::expected<GuileEvaluationResult, std::string> evaluate_source(HostLease& host,
                                                                  GuileEvaluationRequest request);
SCM setting_value(const SettingValue& value);
bool setting_value_p(SCM value);
SettingValue setting_from_scheme(SCM value);
SCM string_vector_value(const std::vector<std::string>& values);

SCM host_type = SCM_UNDEFINED;
std::once_flag guile_once;

std::string scheme_string(SCM value) {
    char* converted = scm_to_utf8_string(value);
    if (converted == nullptr) {
        throw std::runtime_error("Guile failed to convert a string");
    }
    std::string result(converted);
    std::free(converted);
    return result;
}

std::string scheme_name(SCM value, const char* caller, int position) {
    if (scm_is_string(value)) {
        return scheme_string(value);
    }
    if (scm_to_bool(scm_symbol_p(value)) != 0) {
        return scheme_string(scm_symbol_to_string(value));
    }
    scm_wrong_type_arg_msg(caller, position, value, "symbol or string");
    return {};
}

bool scheme_true(SCM value);

std::vector<std::string> string_sequence_from_scheme(SCM value, const char* caller, int position) {
    std::vector<std::string> result;
    if (scm_is_vector(value)) {
        const std::size_t size = scm_c_vector_length(value);
        result.reserve(size);
        for (std::size_t index = 0; index < size; ++index) {
            const SCM item = scm_c_vector_ref(value, index);
            if (!scm_is_string(item)) {
                scm_wrong_type_arg_msg(caller, position, item, "string");
            }
            result.push_back(scheme_string(item));
        }
        return result;
    }
    const long size = scm_ilength(value);
    if (size < 0) {
        scm_wrong_type_arg_msg(caller, position, value, "proper list or vector of strings");
    }
    result.reserve(static_cast<std::size_t>(size));
    for (SCM rest = value; !scheme_true(scm_null_p(rest)); rest = scm_cdr(rest)) {
        const SCM item = scm_car(rest);
        if (!scm_is_string(item)) {
            scm_wrong_type_arg_msg(caller, position, item, "string");
        }
        result.push_back(scheme_string(item));
    }
    return result;
}

SCM name_symbol(std::string_view name) {
    return scm_from_utf8_symbol(std::string(name).c_str());
}

SCM position_encoding_value(PositionEncoding encoding) {
    return name_symbol(encoding == PositionEncoding::Utf16 ? "utf-16" : "bytes");
}

PositionEncoding position_encoding_from_scheme(SCM value, const char* caller, int position) {
    const std::string name = scheme_name(value, caller, position);
    if (name == "bytes") {
        return PositionEncoding::Bytes;
    }
    if (name == "utf-16") {
        return PositionEncoding::Utf16;
    }
    scm_wrong_type_arg_msg(caller, position, value, "'bytes or 'utf-16");
    return PositionEncoding::Bytes;
}

bool scheme_false(SCM value) {
    return scm_to_bool(scm_eq_p(value, SCM_BOOL_F)) != 0;
}

bool scheme_true(SCM value) {
    return !scheme_false(value);
}

bool scheme_boolean(SCM value) {
    return (scm_is_bool)(value) != 0;
}

bool symbol_is(SCM value, const char* expected);

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

CppIndentStyle cpp_indent_style_from_scheme(SCM value, const char* caller, int position) {
    if (!scm_is_vector(value) || scm_c_vector_length(value) != 18 ||
        !symbol_is(scm_c_vector_ref(value, 0), "cpp-indent-style")) {
        scm_wrong_type_arg_msg(caller, position, value, "cpp-indent-style vector");
    }
    const auto integer = [&](std::size_t index) {
        const SCM field = scm_c_vector_ref(value, index);
        if (scm_is_integer(field) == 0) {
            scm_wrong_type_arg_msg(caller, position, field, "integer style field");
        }
        return scm_to_int(field);
    };
    const auto boolean = [&](std::size_t index) {
        const SCM field = scm_c_vector_ref(value, index);
        if (!scheme_boolean(field)) {
            scm_wrong_type_arg_msg(caller, position, field, "boolean style field");
        }
        return scheme_true(field);
    };
    CppIndentStyle style;
    style.indent_width = integer(1);
    style.continuation_indent = integer(2);
    style.tab_width = integer(3);
    style.use_tabs = boolean(4);
    style.align_open_bracket = boolean(5);
    style.brace_init_continuation = boolean(6);
    style.indent_wrapped_function_names = boolean(7);
    style.align_operands = boolean(8);
    style.break_before_ternary = boolean(9);
    const SCM namespace_indent = scm_c_vector_ref(value, 10);
    if (symbol_is(namespace_indent, "none")) {
        style.namespace_indentation = CppIndentStyle::NamespaceIndentation::None;
    } else if (symbol_is(namespace_indent, "inner")) {
        style.namespace_indentation = CppIndentStyle::NamespaceIndentation::Inner;
    } else if (symbol_is(namespace_indent, "all")) {
        style.namespace_indentation = CppIndentStyle::NamespaceIndentation::All;
    } else {
        scm_wrong_type_arg_msg(caller, position, namespace_indent, "namespace indentation symbol");
    }
    style.indent_type_body = boolean(11);
    style.indent_case_label = boolean(12);
    style.indent_case_body = boolean(13);
    style.access_specifier_offset = integer(14);
    const SCM pp_indent = scm_c_vector_ref(value, 15);
    if (symbol_is(pp_indent, "none")) {
        style.pp_directive_indent = CppIndentStyle::PPDirectiveIndent::None;
    } else if (symbol_is(pp_indent, "after-hash")) {
        style.pp_directive_indent = CppIndentStyle::PPDirectiveIndent::AfterHash;
    } else if (symbol_is(pp_indent, "before-hash")) {
        style.pp_directive_indent = CppIndentStyle::PPDirectiveIndent::BeforeHash;
    } else {
        scm_wrong_type_arg_msg(caller, position, pp_indent, "preprocessor indentation symbol");
    }
    style.pp_indent_width = integer(16);
    const SCM constructor_style = scm_c_vector_ref(value, 17);
    if (symbol_is(constructor_style, "normal-indent")) {
        style.constructor_initializers = CppIndentStyle::ConstructorInitializerStyle::NormalIndent;
    } else if (symbol_is(constructor_style, "continuation-indent")) {
        style.constructor_initializers =
            CppIndentStyle::ConstructorInitializerStyle::ContinuationIndent;
    } else if (symbol_is(constructor_style, "align-first-initializer")) {
        style.constructor_initializers =
            CppIndentStyle::ConstructorInitializerStyle::AlignFirstInitializer;
    } else if (symbol_is(constructor_style, "align-after-colon")) {
        style.constructor_initializers =
            CppIndentStyle::ConstructorInitializerStyle::AlignAfterColon;
    } else if (symbol_is(constructor_style, "align-with-colon")) {
        style.constructor_initializers =
            CppIndentStyle::ConstructorInitializerStyle::AlignWithColon;
    } else {
        scm_wrong_type_arg_msg(caller, position, constructor_style,
                               "constructor initializer style symbol");
    }
    return style;
}

SCM entity_id(std::uint32_t slot, std::uint32_t generation) {
    SCM result = scm_c_make_vector(2, SCM_UNSPECIFIED);
    scm_c_vector_set_x(result, 0, scm_from_uint32(slot));
    scm_c_vector_set_x(result, 1, scm_from_uint32(generation));
    return result;
}

template <typename Tag>
EntityId<Tag> entity_id_from_scheme(SCM value, const char* caller, int position) {
    if (!scm_is_vector(value) || scm_c_vector_length(value) != 2) {
        scm_wrong_type_arg_msg(caller, position, value, "two-element entity ID vector");
    }
    const SCM slot = scm_c_vector_ref(value, 0);
    const SCM generation = scm_c_vector_ref(value, 1);
    if (scm_is_unsigned_integer(slot, 0, std::numeric_limits<std::uint32_t>::max()) == 0 ||
        scm_is_unsigned_integer(generation, 0, std::numeric_limits<std::uint32_t>::max()) == 0) {
        scm_wrong_type_arg_msg(caller, position, value, "two-element entity ID vector");
    }
    const EntityId<Tag> result{.slot = scm_to_uint32(slot),
                               .generation = scm_to_uint32(generation)};
    if (!result.valid()) {
        scm_misc_error(caller, "entity ID is invalid", SCM_EOL);
    }
    return result;
}

void raise_host_error(const char* caller, const std::string& message) {
    scm_misc_error(caller, "host operation failed: ~A",
                   scm_list_1(scm_from_utf8_string(message.c_str())));
}

void finalize_host(SCM object) {
    delete static_cast<HostLease*>(scm_foreign_object_ref(object, 0));
}

HostLease& require_host(SCM object, const char* caller) {
    scm_assert_foreign_object_type(host_type, object);
    auto* host = static_cast<HostLease*>(scm_foreign_object_ref(object, 0));
    if (host == nullptr || host->runtime == nullptr) {
        scm_misc_error(caller, "editor host capability has expired", SCM_EOL);
    }
    if (!host->state || !host->state->active || std::this_thread::get_id() != host->state->owner) {
        scm_misc_error(caller, "editor host capability requires the editor thread", SCM_EOL);
    }
    return *host;
}

GuileAsyncBridge& require_async_bridge(SCM object, const char* caller) {
    HostLease& host = require_host(object, caller);
    if (host.async_bridge == nullptr) {
        scm_misc_error(caller, "async task capability has expired", SCM_EOL);
    }
    return *host.async_bridge;
}

KeymapId require_keymap(HostLease& host, SCM value, const char* caller, int position) {
    const std::string name = scheme_name(value, caller, position);
    const std::optional<KeymapId> keymap = host.runtime->keymaps().find(name);
    if (!keymap) {
        scm_misc_error(caller, "unknown keymap: ~S", scm_list_1(value));
    }
    return *keymap;
}

CommandId require_command(HostLease& host, SCM value, const char* caller, int position) {
    const std::string name = scheme_name(value, caller, position);
    const std::optional<CommandId> command = host.runtime->commands().find(name);
    if (!command) {
        scm_misc_error(caller, "unknown command: ~S", scm_list_1(value));
    }
    return *command;
}

SCM context_entry(SCM context, const char* key, const char* caller) {
    const SCM entry = scm_assq(scm_from_utf8_symbol(key), context);
    if (scheme_false(entry)) {
        scm_misc_error(caller, "command context is missing ~A",
                       scm_list_1(scm_from_utf8_string(key)));
    }
    return scm_cdr(entry);
}

CommandContext command_context_from_scheme(HostLease& host, SCM context, const char* caller) {
    const WindowId window =
        entity_id_from_scheme<WindowTag>(context_entry(context, "window", caller), caller, 2);
    const BufferId buffer =
        entity_id_from_scheme<BufferTag>(context_entry(context, "buffer", caller), caller, 2);
    const ViewId view =
        entity_id_from_scheme<ViewTag>(context_entry(context, "view", caller), caller, 2);
    return CommandContext(*host.runtime, window, buffer, view);
}

CommandResult invoke_script_command(const std::shared_ptr<GuileState>& state,
                                    std::size_t command_index, CommandContext& context,
                                    const CommandInvocation& invocation);
bool script_command_enabled(const std::shared_ptr<GuileState>& state, std::size_t command_index,
                            const CommandContext& context);
InteractionProviderResult invoke_script_provider(const std::shared_ptr<GuileState>& state,
                                                 std::size_t provider_index,
                                                 CommandContext& context, std::string_view query,
                                                 GuileAsyncBridge* async_bridge);
InputStateHandlerResult invoke_script_input_handler(const std::shared_ptr<GuileState>& state,
                                                    std::size_t state_index, EditorRuntime& runtime,
                                                    CommandContext& context, KeyStroke key);
PositionHintProviderResult invoke_script_position_hints(const std::shared_ptr<GuileState>& state,
                                                        std::size_t provider_index,
                                                        CommandContext& context);
void invoke_script_input_state_observer(const std::shared_ptr<GuileState>& state,
                                        std::size_t observer_index, EditorRuntime& runtime,
                                        const InputStateChange& change);
void invoke_script_input_state_lifecycle(const std::shared_ptr<GuileState>& state,
                                         std::size_t lifecycle_index, EditorRuntime& runtime,
                                         const InputStateChange& change, bool entering);
void invoke_script_mode_policy_observer(const std::shared_ptr<GuileState>& state,
                                        std::size_t observer_index, EditorRuntime& runtime,
                                        const BufferModePolicyChange& change);

// The Guile ABI fixes four adjacent SCM arguments; Scheme-level names and
// validation preserve their semantic order.
SCM define_command(SCM host_object, SCM name_value, SCM execute_value, SCM enabled_value) {
    if (!scm_is_string(name_value)) {
        scm_wrong_type_arg_msg("define-command!", 2, name_value, "string");
    }
    if (!scheme_true(scm_procedure_p(execute_value))) {
        scm_wrong_type_arg_msg("define-command!", 3, execute_value, "procedure");
    }
    if (!scheme_false(enabled_value) && !scheme_true(scm_procedure_p(enabled_value))) {
        scm_wrong_type_arg_msg("define-command!", 4, enabled_value, "procedure or #f");
    }
    HostLease& host = require_host(host_object, "define-command!");
    const std::shared_ptr<GuileState> state = host.state;
    if (!state || !state->active) {
        scm_misc_error("define-command!", "Guile runtime has expired", SCM_EOL);
    }

    try {
        const std::string name = scheme_string(name_value);
        const std::size_t command_index = state->commands.size();
        (void)scm_gc_protect_object(execute_value);
        if (!scheme_false(enabled_value)) {
            (void)scm_gc_protect_object(enabled_value);
        }
        bool appended = false;
        try {
            state->commands.push_back({.execute = execute_value, .enabled = enabled_value});
            appended = true;
            const std::weak_ptr<GuileState> weak = state;
            CommandRegistry::Enabled enabled;
            if (!scheme_false(enabled_value)) {
                enabled = [weak, command_index](const CommandContext& context) {
                    const std::shared_ptr<GuileState> locked = weak.lock();
                    return locked && script_command_enabled(locked, command_index, context);
                };
            }
            CommandRegistry::Execute execute =
                [weak, command_index](CommandContext& context,
                                      const CommandInvocation& invocation) -> CommandResult {
                const std::shared_ptr<GuileState> locked = weak.lock();
                if (!locked || !locked->active) {
                    return std::unexpected(CommandError{"Guile command runtime has expired"});
                }
                return invoke_script_command(locked, command_index, context, invocation);
            };
            const std::optional<CommandId> existing = host.runtime->commands().find(name);
            const CommandId command =
                existing ? *existing : host.runtime->commands().define(name, execute, enabled);
            if (existing) {
                host.runtime->commands().configure(command, std::move(execute), std::move(enabled));
            }
            std::string documentation;
            const SCM documentation_value = scm_procedure_documentation(execute_value);
            if (scm_is_string(documentation_value)) {
                documentation = scheme_string(documentation_value);
            }
            host.runtime->commands().describe(command, std::move(documentation),
                                              state->definition_source);
            ++host.commands_installed;
            return scm_from_uint32(command.value);
        } catch (...) {
            if (appended) {
                state->commands.pop_back();
            }
            (void)scm_gc_unprotect_object(execute_value);
            if (!scheme_false(enabled_value)) {
                (void)scm_gc_unprotect_object(enabled_value);
            }
            throw;
        }
    } catch (const std::exception& exception) {
        scm_misc_error("define-command!", exception.what(), SCM_EOL);
    } catch (...) {
        scm_misc_error("define-command!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM set_command_documentation(SCM host_object, SCM name_value, SCM documentation_value) {
    if (!scm_is_string(name_value)) {
        scm_wrong_type_arg_msg("set-command-documentation!", 2, name_value, "string");
    }
    if (!scm_is_string(documentation_value)) {
        scm_wrong_type_arg_msg("set-command-documentation!", 3, documentation_value, "string");
    }
    try {
        HostLease& host = require_host(host_object, "set-command-documentation!");
        const std::string name = scheme_string(name_value);
        const std::optional<CommandId> command = host.runtime->commands().find(name);
        if (!command) {
            scm_misc_error("set-command-documentation!", "unknown command", SCM_EOL);
        }
        const CommandRegistry::Definition& definition =
            host.runtime->commands().definition(*command);
        host.runtime->commands().describe(*command, scheme_string(documentation_value),
                                          definition.source);
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        scm_misc_error("set-command-documentation!", exception.what(), SCM_EOL);
    } catch (...) {
        scm_misc_error("set-command-documentation!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

// The Guile ABI fixes three adjacent SCM arguments; their Scheme procedure
// name and validation preserve the semantic order.
SCM define_interaction_provider(SCM host_object, SCM name_value, SCM complete_value) {
    if (!scm_is_string(name_value)) {
        scm_wrong_type_arg_msg("define-interaction-provider!", 2, name_value, "string");
    }
    if (!scheme_true(scm_procedure_p(complete_value))) {
        scm_wrong_type_arg_msg("define-interaction-provider!", 3, complete_value, "procedure");
    }
    HostLease& host = require_host(host_object, "define-interaction-provider!");
    const std::shared_ptr<GuileState> state = host.state;
    if (!state || !state->active) {
        scm_misc_error("define-interaction-provider!", "Guile runtime has expired", SCM_EOL);
    }

    try {
        const std::string name = scheme_string(name_value);
        const std::size_t provider_index = state->providers.size();
        (void)scm_gc_protect_object(complete_value);
        bool appended = false;
        try {
            state->providers.push_back({.complete = complete_value});
            appended = true;
            const std::weak_ptr<GuileState> weak = state;
            GuileAsyncBridge* async_bridge = host.async_bridge;
            InteractionProviderRegistry::Complete complete =
                [weak, provider_index, async_bridge](
                    CommandContext& context, std::string_view query) -> InteractionProviderResult {
                const std::shared_ptr<GuileState> locked = weak.lock();
                if (!locked || !locked->active) {
                    throw std::runtime_error("Guile provider runtime has expired");
                }
                return invoke_script_provider(locked, provider_index, context, query, async_bridge);
            };
            if (host.runtime->interaction_providers().contains(name)) {
                host.runtime->interaction_providers().configure(name, std::move(complete));
            } else {
                host.runtime->interaction_providers().define(name, std::move(complete));
            }
            ++host.providers_installed;
            return scm_from_size_t(provider_index);
        } catch (...) {
            if (appended) {
                state->providers.pop_back();
            }
            (void)scm_gc_unprotect_object(complete_value);
            throw;
        }
    } catch (const std::exception& exception) {
        scm_misc_error("define-interaction-provider!", exception.what(), SCM_EOL);
    } catch (...) {
        scm_misc_error("define-interaction-provider!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM define_completion_provider(SCM host_object, SCM name_value, SCM complete_value,
                               SCM resolve_value) {
    if (scheme_true(scm_eq_p(resolve_value, SCM_UNDEFINED))) {
        resolve_value = SCM_BOOL_F;
    }
    if (!scm_is_string(name_value)) {
        scm_wrong_type_arg_msg("define-completion-provider!", 2, name_value, "string");
    }
    if (!scheme_true(scm_procedure_p(complete_value))) {
        scm_wrong_type_arg_msg("define-completion-provider!", 3, complete_value, "procedure");
    }
    if (!scheme_false(resolve_value) && !scheme_true(scm_procedure_p(resolve_value))) {
        scm_wrong_type_arg_msg("define-completion-provider!", 4, resolve_value, "procedure or #f");
    }
    HostLease& host = require_host(host_object, "define-completion-provider!");
    const std::shared_ptr<GuileState> state = host.state;
    if (!state || !state->active) {
        scm_misc_error("define-completion-provider!", "Guile runtime has expired", SCM_EOL);
    }

    try {
        const std::string name = scheme_string(name_value);
        const std::size_t provider_index = state->completion_providers.size();
        (void)scm_gc_protect_object(complete_value);
        if (!scheme_false(resolve_value)) {
            (void)scm_gc_protect_object(resolve_value);
        }
        bool appended = false;
        try {
            state->completion_providers.push_back(
                {.name = name, .complete = complete_value, .resolve = resolve_value});
            appended = true;
            state->completion_providers_by_name.insert_or_assign(name, provider_index);
            ++host.providers_installed;
            return scm_from_size_t(provider_index);
        } catch (...) {
            if (appended) {
                state->completion_providers.pop_back();
            }
            (void)scm_gc_unprotect_object(complete_value);
            if (!scheme_false(resolve_value)) {
                (void)scm_gc_unprotect_object(resolve_value);
            }
            throw;
        }
    } catch (const std::exception& exception) {
        scm_misc_error("define-completion-provider!", exception.what(), SCM_EOL);
    } catch (...) {
        scm_misc_error("define-completion-provider!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

// The Guile ABI fixes four adjacent SCM arguments; their Scheme procedure
// names and validation preserve the semantic order.
SCM bind_key_if_command(SCM host_object, SCM keymap_value, SCM keys_value, SCM command_value) {
    if (!scm_is_string(keys_value)) {
        scm_wrong_type_arg_msg("bind-key-if-command!", 3, keys_value, "string");
    }
    try {
        HostLease& host = require_host(host_object, "bind-key-if-command!");
        const KeymapId keymap = require_keymap(host, keymap_value, "bind-key-if-command!", 2);
        const std::string keys = scheme_string(keys_value);
        const std::string command_name = scheme_name(command_value, "bind-key-if-command!", 4);
        const std::optional<CommandId> command = host.runtime->commands().find(command_name);
        if (!command) {
            return SCM_BOOL_F;
        }
        host.runtime->keymaps().bind(keymap, keys, *command);
        ++host.bindings_installed;
        return SCM_BOOL_T;
    } catch (const std::exception& exception) {
        scm_misc_error("bind-key-if-command!", exception.what(), SCM_EOL);
    } catch (...) {
        scm_misc_error("bind-key-if-command!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM define_keymap(SCM host_object, SCM name_value, SCM parent_value) {
    try {
        HostLease& host = require_host(host_object, "define-keymap!");
        const std::string name = scheme_name(name_value, "define-keymap!", 2);
        std::optional<KeymapId> parent;
        if (!scheme_false(parent_value)) {
            parent = require_keymap(host, parent_value, "define-keymap!", 3);
        }
        const std::optional<KeymapId> existing = host.runtime->keymaps().find(name);
        const KeymapId keymap = existing ? *existing : host.runtime->keymaps().define(name);
        host.runtime->keymaps().set_parent(keymap, parent);
        return scm_from_uint32(keymap.value);
    } catch (const std::exception& exception) {
        scm_misc_error("define-keymap!", exception.what(), SCM_EOL);
    } catch (...) {
        scm_misc_error("define-keymap!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM bind_key(SCM host_object, SCM keymap_value, SCM keys_value, SCM binding_value) {
    if (!scm_is_string(keys_value)) {
        scm_wrong_type_arg_msg("bind-key!", 3, keys_value, "string");
    }
    try {
        HostLease& host = require_host(host_object, "bind-key!");
        const KeymapId keymap = require_keymap(host, keymap_value, "bind-key!", 2);
        const std::string keys = scheme_string(keys_value);
        const long binding_size = scm_ilength(binding_value);
        if (binding_size == 2 || binding_size == 3) {
            const SCM tag = scm_car(binding_value);
            if (!symbol_is(tag, "prefix")) {
                scm_misc_error("bind-key!", "binding list must begin with prefix", SCM_EOL);
            }
            const SCM prefix_value = scm_cadr(binding_value);
            const KeymapId prefix = require_keymap(host, prefix_value, "bind-key!", 4);
            std::string label;
            if (binding_size == 3) {
                const SCM label_value = scm_caddr(binding_value);
                if (!scm_is_string(label_value)) {
                    scm_wrong_type_arg_msg("bind-key!", 4, binding_value,
                                           "(prefix keymap [label])");
                }
                label = scheme_string(label_value);
            }
            host.runtime->keymaps().bind_prefix(keymap, keys, prefix, std::move(label));
        } else {
            const CommandId command = require_command(host, binding_value, "bind-key!", 4);
            host.runtime->keymaps().bind(keymap, keys, command);
        }
        ++host.bindings_installed;
        return SCM_BOOL_T;
    } catch (const std::exception& exception) {
        scm_misc_error("bind-key!", exception.what(), SCM_EOL);
    } catch (...) {
        scm_misc_error("bind-key!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM bind_remap(SCM host_object, SCM keymap_value, SCM command_value, SCM replacement_value) {
    try {
        HostLease& host = require_host(host_object, "bind-remap!");
        const KeymapId keymap = require_keymap(host, keymap_value, "bind-remap!", 2);
        const CommandId command = require_command(host, command_value, "bind-remap!", 3);
        const CommandId replacement = require_command(host, replacement_value, "bind-remap!", 4);
        host.runtime->keymaps().bind_remap(keymap, command, replacement);
        ++host.bindings_installed;
        return SCM_BOOL_T;
    } catch (const std::exception& exception) {
        scm_misc_error("bind-remap!", exception.what(), SCM_EOL);
    } catch (...) {
        scm_misc_error("bind-remap!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

// The Guile ABI fixes two adjacent SCM arguments; validation preserves their semantic order.
SCM keymap_bindings(SCM host_object, SCM keymap_value) {
    try {
        HostLease& host = require_host(host_object, "keymap-bindings");
        const KeymapId keymap = require_keymap(host, keymap_value, "keymap-bindings", 2);
        const std::vector<KeymapEntry> entries = host.runtime->keymaps().entries(keymap);
        const std::vector<KeymapRemap> remaps = host.runtime->keymaps().remaps(keymap);
        SCM result = scm_c_make_vector(entries.size() + remaps.size(), SCM_UNSPECIFIED);
        std::size_t index = 0;
        for (const KeymapEntry& entry : entries) {
            SCM value = scm_c_make_vector(4, SCM_BOOL_F);
            scm_c_vector_set_x(value, 0,
                               scm_from_utf8_symbol(
                                   entry.kind == KeymapEntryKind::Command ? "command" : "prefix"));
            scm_c_vector_set_x(value, 1,
                               scm_from_utf8_string(format_key_sequence(entry.sequence).c_str()));
            if (entry.command) {
                scm_c_vector_set_x(
                    value, 2,
                    name_symbol(host.runtime->commands().definition(*entry.command).name));
            } else if (entry.prefix_keymap) {
                scm_c_vector_set_x(
                    value, 2,
                    name_symbol(host.runtime->keymaps().definition(*entry.prefix_keymap).name));
            }
            if (!entry.label.empty()) {
                scm_c_vector_set_x(value, 3, scm_from_utf8_string(entry.label.c_str()));
            }
            scm_c_vector_set_x(result, index++, value);
        }
        for (const KeymapRemap& remap : remaps) {
            SCM value = scm_c_make_vector(4, SCM_BOOL_F);
            scm_c_vector_set_x(value, 0, scm_from_utf8_symbol("remap"));
            scm_c_vector_set_x(
                value, 1, name_symbol(host.runtime->commands().definition(remap.command).name));
            scm_c_vector_set_x(
                value, 2, name_symbol(host.runtime->commands().definition(remap.replacement).name));
            scm_c_vector_set_x(result, index++, value);
        }
        return result;
    } catch (const std::exception& exception) {
        scm_misc_error("keymap-bindings", exception.what(), SCM_EOL);
    } catch (...) {
        scm_misc_error("keymap-bindings", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

std::vector<KeymapId> keymap_layers_from_scheme(HostLease& host, SCM layers_value,
                                                const char* caller) {
    std::vector<KeymapId> layers;
    if (scm_is_vector(layers_value)) {
        const std::size_t size = scm_c_vector_length(layers_value);
        layers.reserve(size);
        for (std::size_t index = 0; index < size; ++index) {
            layers.push_back(
                require_keymap(host, scm_c_vector_ref(layers_value, index), caller, 2));
        }
        return layers;
    }
    const long size = scm_ilength(layers_value);
    if (size < 0) {
        scm_wrong_type_arg_msg(caller, 2, layers_value, "proper list or vector of keymap names");
    }
    layers.reserve(static_cast<std::size_t>(size));
    for (SCM rest = layers_value; !scheme_true(scm_null_p(rest)); rest = scm_cdr(rest)) {
        layers.push_back(require_keymap(host, scm_car(rest), caller, 2));
    }
    return layers;
}

std::vector<InputHint> input_hints_from_scheme(SCM hints_value, const char* caller, int position) {
    if (!scm_is_vector(hints_value)) {
        scm_wrong_type_arg_msg(caller, position, hints_value,
                               "vector of #(key detail prefix?) hints");
    }
    std::vector<InputHint> hints;
    const std::size_t count = scm_c_vector_length(hints_value);
    hints.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const SCM hint = scm_c_vector_ref(hints_value, index);
        if (!scm_is_vector(hint) || scm_c_vector_length(hint) != 3 ||
            !scm_is_string(scm_c_vector_ref(hint, 0)) ||
            !scm_is_string(scm_c_vector_ref(hint, 1)) ||
            !scheme_boolean(scm_c_vector_ref(hint, 2))) {
            scm_wrong_type_arg_msg(caller, position, hint, "#(key detail prefix?) hint");
        }
        hints.push_back({.key = scheme_string(scm_c_vector_ref(hint, 0)),
                         .detail = scheme_string(scm_c_vector_ref(hint, 1)),
                         .prefix = scheme_true(scm_c_vector_ref(hint, 2))});
    }
    return hints;
}

std::vector<PositionHint> position_hints_from_scheme(SCM hints_value, const char* caller) {
    if (!scm_is_vector(hints_value)) {
        throw std::invalid_argument(
            std::format("{} must return a vector of #(byte-offset label) hints", caller));
    }
    std::vector<PositionHint> hints;
    const std::size_t count = scm_c_vector_length(hints_value);
    hints.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const SCM hint = scm_c_vector_ref(hints_value, index);
        if (!scm_is_vector(hint) || scm_c_vector_length(hint) != 2 ||
            scm_is_unsigned_integer(scm_c_vector_ref(hint, 0), 0,
                                    std::numeric_limits<std::uint32_t>::max()) == 0 ||
            !scm_is_string(scm_c_vector_ref(hint, 1))) {
            throw std::invalid_argument(
                std::format("{} hint must be #(byte-offset label)", caller));
        }
        hints.push_back({.position = TextOffset{scm_to_uint32(scm_c_vector_ref(hint, 0))},
                         .label = scheme_string(scm_c_vector_ref(hint, 1))});
    }
    return hints;
}

// The Guile ABI fixes three adjacent SCM arguments; validation preserves their semantic order.
SCM resolve_key_sequence(SCM host_object, SCM layers_value, SCM keys_value) {
    if (!scm_is_string(keys_value)) {
        scm_wrong_type_arg_msg("resolve-key-sequence", 3, keys_value, "string");
    }
    try {
        HostLease& host = require_host(host_object, "resolve-key-sequence");
        const std::vector<KeymapId> layers =
            keymap_layers_from_scheme(host, layers_value, "resolve-key-sequence");
        const std::expected<KeySequence, KeyParseError> sequence =
            parse_key_sequence(scheme_string(keys_value));
        if (!sequence) {
            scm_misc_error("resolve-key-sequence", sequence.error().message.c_str(), SCM_EOL);
        }
        const KeymapMatch match = host.runtime->keymaps().resolve(layers, *sequence);
        if (match.kind == KeymapMatchKind::None) {
            SCM result = scm_c_make_vector(1, SCM_UNSPECIFIED);
            scm_c_vector_set_x(result, 0, scm_from_utf8_symbol("none"));
            return result;
        }
        SCM result =
            scm_c_make_vector(match.kind == KeymapMatchKind::Command ? 3 : 2, SCM_UNSPECIFIED);
        scm_c_vector_set_x(
            result, 0,
            scm_from_utf8_symbol(match.kind == KeymapMatchKind::Command ? "command" : "prefix"));
        std::size_t source_index = 1;
        if (match.kind == KeymapMatchKind::Command) {
            scm_c_vector_set_x(
                result, 1, name_symbol(host.runtime->commands().definition(match.command).name));
            source_index = 2;
        }
        if (!match.source) {
            scm_misc_error("resolve-key-sequence", "resolved keymap source is missing", SCM_EOL);
        }
        scm_c_vector_set_x(result, source_index,
                           name_symbol(host.runtime->keymaps().definition(*match.source).name));
        return result;
    } catch (const std::exception& exception) {
        scm_misc_error("resolve-key-sequence", exception.what(), SCM_EOL);
    } catch (...) {
        scm_misc_error("resolve-key-sequence", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM keymap_names_value(const EditorRuntime& runtime, std::span<const KeymapId> keymaps) {
    SCM result = scm_c_make_vector(keymaps.size(), SCM_UNSPECIFIED);
    for (std::size_t index = 0; index < keymaps.size(); ++index) {
        scm_c_vector_set_x(result, index,
                           name_symbol(runtime.keymaps().definition(keymaps[index]).name));
    }
    return result;
}

SCM named_keymap_source_value(const EditorRuntime& runtime, const char* tag, std::string_view name,
                              std::span<const KeymapId> keymaps) {
    SCM result = scm_c_make_vector(3, SCM_UNSPECIFIED);
    scm_c_vector_set_x(result, 0, scm_from_utf8_symbol(tag));
    scm_c_vector_set_x(result, 1, name_symbol(name));
    scm_c_vector_set_x(result, 2, keymap_names_value(runtime, keymaps));
    return result;
}

SCM buffer_kind_symbol(BufferKind kind) {
    switch (kind) {
    case BufferKind::File:
        return scm_from_utf8_symbol("file");
    case BufferKind::Scratch:
        return scm_from_utf8_symbol("scratch");
    case BufferKind::Generated:
        return scm_from_utf8_symbol("generated");
    case BufferKind::Process:
        return scm_from_utf8_symbol("process");
    case BufferKind::Minibuffer:
        return scm_from_utf8_symbol("minibuffer");
    }
    throw std::logic_error("unknown buffer kind");
}

BufferKind buffer_kind_from_scheme(SCM value, const char* caller, int position) {
    if (symbol_is(value, "scratch")) {
        return BufferKind::Scratch;
    }
    if (symbol_is(value, "file")) {
        return BufferKind::File;
    }
    if (symbol_is(value, "generated")) {
        return BufferKind::Generated;
    }
    if (symbol_is(value, "process")) {
        return BufferKind::Process;
    }
    scm_wrong_type_arg_msg(caller, position, value, "'scratch, 'file, 'generated, or 'process");
}

SCM keymap_context_snapshot(SCM host_object, SCM context_value) {
    try {
        HostLease& host = require_host(host_object, "keymap-context-snapshot");
        const CommandContext context =
            command_context_from_scheme(host, context_value, "keymap-context-snapshot");
        const EditorRuntime& runtime = *host.runtime;
        const Window& window = runtime.windows().get(context.window_id());
        const View& view = runtime.views().get(context.view_id());
        const Buffer& buffer = runtime.buffers().get(context.buffer_id());

        const std::vector<InputStateId>& state_stack = view.input_states().stack();
        SCM states = scm_c_make_vector(state_stack.size(), SCM_UNSPECIFIED);
        for (std::size_t index = 0; index < state_stack.size(); ++index) {
            const InputStateRegistry::Definition& state =
                runtime.input_states().definition(state_stack[index]);
            scm_c_vector_set_x(
                states, index,
                named_keymap_source_value(runtime, "input-state", state.name, state.keymaps));
        }

        const std::vector<ModeId>& minor_modes = buffer.modes().minors();
        SCM minors = scm_c_make_vector(minor_modes.size(), SCM_UNSPECIFIED);
        for (std::size_t index = 0; index < minor_modes.size(); ++index) {
            const ModeRegistry::Definition& mode = runtime.modes().definition(minor_modes[index]);
            const std::vector<KeymapId> keymaps =
                runtime.modes().effective_keymaps(minor_modes[index]);
            scm_c_vector_set_x(
                minors, index,
                named_keymap_source_value(runtime, "minor-mode", mode.name, keymaps));
        }

        SCM major = SCM_BOOL_F;
        if (const std::optional<ModeId> major_mode = buffer.modes().major()) {
            const ModeRegistry::Definition& mode = runtime.modes().definition(*major_mode);
            const std::vector<KeymapId> keymaps = runtime.modes().effective_keymaps(*major_mode);
            major = named_keymap_source_value(runtime, "major-mode", mode.name, keymaps);
        }

        SCM result = scm_c_make_vector(10, SCM_UNSPECIFIED);
        scm_c_vector_set_x(result, 0, scm_from_utf8_symbol("keymap-context"));
        scm_c_vector_set_x(result, 1, buffer_kind_symbol(buffer.kind()));
        scm_c_vector_set_x(result, 2, states);
        scm_c_vector_set_x(result, 3, keymap_names_value(runtime, window.keymaps()));
        scm_c_vector_set_x(result, 4, keymap_names_value(runtime, view.keymaps()));
        scm_c_vector_set_x(result, 5, keymap_names_value(runtime, buffer.keymaps()));
        scm_c_vector_set_x(result, 6, minors);
        scm_c_vector_set_x(result, 7, major);
        scm_c_vector_set_x(result, 8, scm_from_bool(window.created_by_policy()));
        scm_c_vector_set_x(
            result, 9,
            scm_from_bool(host.services.completion_active && host.services.completion_active()));
        return result;
    } catch (const std::exception& exception) {
        scm_misc_error("keymap-context-snapshot", exception.what(), SCM_EOL);
    } catch (...) {
        scm_misc_error("keymap-context-snapshot", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

// The Guile ABI fixes three adjacent SCM arguments; validation preserves their semantic order.
SCM key_sequence_completions(SCM host_object, SCM layers_value, SCM keys_value) {
    if (!scm_is_string(keys_value)) {
        scm_wrong_type_arg_msg("key-sequence-completions", 3, keys_value, "string");
    }
    try {
        HostLease& host = require_host(host_object, "key-sequence-completions");
        const std::vector<KeymapId> layers =
            keymap_layers_from_scheme(host, layers_value, "key-sequence-completions");
        KeySequence prefix;
        const std::string keys = scheme_string(keys_value);
        if (!keys.empty()) {
            const std::expected<KeySequence, KeyParseError> parsed = parse_key_sequence(keys);
            if (!parsed) {
                scm_misc_error("key-sequence-completions", parsed.error().message.c_str(), SCM_EOL);
            }
            prefix = *parsed;
        }
        const std::vector<KeymapCompletion> completions =
            host.runtime->keymaps().completions(layers, prefix);
        SCM result = scm_c_make_vector(completions.size(), SCM_UNSPECIFIED);
        for (std::size_t index = 0; index < completions.size(); ++index) {
            const KeymapCompletion& completion = completions[index];
            SCM value = scm_c_make_vector(3, SCM_UNSPECIFIED);
            scm_c_vector_set_x(value, 0,
                               scm_from_utf8_string(format_key_stroke(completion.key).c_str()));
            const std::string detail =
                completion.command ? host.runtime->commands().definition(*completion.command).name
                                   : completion.label;
            scm_c_vector_set_x(value, 1, scm_from_utf8_string(detail.c_str()));
            scm_c_vector_set_x(value, 2, scm_from_bool(completion.prefix));
            scm_c_vector_set_x(result, index, value);
        }
        return result;
    } catch (const std::exception& exception) {
        scm_misc_error("key-sequence-completions", exception.what(), SCM_EOL);
    } catch (...) {
        scm_misc_error("key-sequence-completions", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

// The Guile ABI fixes four adjacent SCM arguments; validation preserves their semantic order.
SCM set_input_feedback(SCM host_object, SCM view_value, SCM sequence_value, SCM hints_value) {
    if (!scm_is_string(sequence_value)) {
        scm_wrong_type_arg_msg("set-input-feedback!", 3, sequence_value, "string");
    }
    try {
        HostLease& host = require_host(host_object, "set-input-feedback!");
        const ViewId view = entity_id_from_scheme<ViewTag>(view_value, "set-input-feedback!", 2);
        host.runtime->views().set_input_feedback(
            view, {.sequence = scheme_string(sequence_value),
                   .hints = input_hints_from_scheme(hints_value, "set-input-feedback!", 4)});
    } catch (const std::exception& exception) {
        scm_misc_error("set-input-feedback!", exception.what(), SCM_EOL);
    } catch (...) {
        scm_misc_error("set-input-feedback!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

SCM clear_input_feedback(SCM host_object, SCM view_value) {
    try {
        HostLease& host = require_host(host_object, "clear-input-feedback!");
        const ViewId view = entity_id_from_scheme<ViewTag>(view_value, "clear-input-feedback!", 2);
        host.runtime->views().clear_input_feedback(view);
    } catch (const std::exception& exception) {
        scm_misc_error("clear-input-feedback!", exception.what(), SCM_EOL);
    } catch (...) {
        scm_misc_error("clear-input-feedback!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

// The low-level Guile ABI fixes eight adjacent SCM arguments. The `(cind
// input)` wrapper supplies the public keyword interface and state-local
// optional capabilities.
SCM define_input_state(SCM host_object, SCM name_value, SCM keymaps_value, SCM text_input_value,
                       SCM text_command_value, SCM cursor_value, SCM indicator_value,
                       SCM handler_value) {
    if (!scm_is_string(indicator_value)) {
        scm_wrong_type_arg_msg("%define-input-state!", 7, indicator_value, "string");
    }
    if (!scheme_false(handler_value) && !scheme_true(scm_procedure_p(handler_value))) {
        scm_wrong_type_arg_msg("%define-input-state!", 8, handler_value, "procedure or #f");
    }
    try {
        HostLease& host = require_host(host_object, "%define-input-state!");
        const std::string name = scheme_name(name_value, "%define-input-state!", 2);
        const std::vector<KeymapId> keymaps =
            keymap_layers_from_scheme(host, keymaps_value, "%define-input-state!");
        TextInputPolicy text_input;
        if (symbol_is(text_input_value, "accept")) {
            text_input = TextInputPolicy::Accept;
        } else if (symbol_is(text_input_value, "ignore")) {
            text_input = TextInputPolicy::Ignore;
        } else {
            scm_wrong_type_arg_msg("%define-input-state!", 4, text_input_value,
                                   "'accept or 'ignore");
        }
        std::optional<std::string> text_command;
        if (scm_is_string(text_command_value)) {
            text_command = scheme_string(text_command_value);
            if (text_command->empty()) {
                scm_misc_error("%define-input-state!", "text command must not be empty", SCM_EOL);
            }
        } else if (!scheme_false(text_command_value)) {
            scm_wrong_type_arg_msg("%define-input-state!", 5, text_command_value,
                                   "command name string or #f");
        }
        CursorShape cursor;
        if (symbol_is(cursor_value, "beam")) {
            cursor = CursorShape::Beam;
        } else if (symbol_is(cursor_value, "block")) {
            cursor = CursorShape::Block;
        } else if (symbol_is(cursor_value, "underline")) {
            cursor = CursorShape::Underline;
        } else {
            scm_wrong_type_arg_msg("%define-input-state!", 6, cursor_value,
                                   "'beam, 'block, or 'underline");
        }

        const std::shared_ptr<GuileState> state = host.state;
        if (!state || !state->active) {
            scm_misc_error("%define-input-state!", "Guile runtime has expired", SCM_EOL);
        }
        const std::size_t state_index = state->input_states.size();
        if (!scheme_false(handler_value)) {
            (void)scm_gc_protect_object(handler_value);
        }
        try {
            state->input_states.push_back({.handler = handler_value});
            InputStateHandler handler;
            if (!scheme_false(handler_value)) {
                const std::weak_ptr<GuileState> weak = state;
                EditorRuntime* runtime = host.runtime;
                handler = [weak, state_index, runtime](CommandContext& context,
                                                       KeyStroke key) -> InputStateHandlerResult {
                    const std::shared_ptr<GuileState> locked = weak.lock();
                    if (!locked || !locked->active) {
                        return std::unexpected("Guile input state runtime has expired");
                    }
                    return invoke_script_input_handler(locked, state_index, *runtime, context, key);
                };
            }
            InputStateRegistry::Definition definition{.name = name,
                                                      .keymaps = keymaps,
                                                      .text_input = text_input,
                                                      .text_command = std::move(text_command),
                                                      .cursor = cursor,
                                                      .indicator = scheme_string(indicator_value),
                                                      .handler = std::move(handler),
                                                      .position_hints = {},
                                                      .on_enter = {},
                                                      .on_exit = {}};
            const std::optional<InputStateId> existing = host.runtime->input_states().find(name);
            InputStateId id;
            if (existing) {
                id = *existing;
                host.runtime->input_states().configure(id, std::move(definition));
            } else {
                id = host.runtime->input_states().define(std::move(definition));
                ++state->input_state_definitions;
            }
            ++host.input_states_installed;
            return scm_from_uint32(id.value);
        } catch (...) {
            state->input_states.pop_back();
            if (!scheme_false(handler_value)) {
                (void)scm_gc_unprotect_object(handler_value);
            }
            throw;
        }
    } catch (const std::exception& exception) {
        scm_misc_error("%define-input-state!", exception.what(), SCM_EOL);
    } catch (...) {
        scm_misc_error("%define-input-state!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM set_input_state_lifecycle(SCM host_object, SCM name_value, SCM on_enter_value,
                              SCM on_exit_value) {
    if (!scheme_false(on_enter_value) && !scheme_true(scm_procedure_p(on_enter_value))) {
        scm_wrong_type_arg_msg("set-input-state-lifecycle!", 3, on_enter_value, "procedure or #f");
    }
    if (!scheme_false(on_exit_value) && !scheme_true(scm_procedure_p(on_exit_value))) {
        scm_wrong_type_arg_msg("set-input-state-lifecycle!", 4, on_exit_value, "procedure or #f");
    }
    try {
        HostLease& host = require_host(host_object, "set-input-state-lifecycle!");
        const std::string name = scheme_name(name_value, "set-input-state-lifecycle!", 2);
        const std::optional<InputStateId> state_id = host.runtime->input_states().find(name);
        if (!state_id) {
            scm_misc_error("set-input-state-lifecycle!", "unknown input state: ~S",
                           scm_list_1(name_value));
        }
        InputStateRegistry::Definition& definition =
            host.runtime->input_states().definition_for_configuration(*state_id);
        if (scheme_false(on_enter_value) && scheme_false(on_exit_value)) {
            definition.on_enter = {};
            definition.on_exit = {};
            return SCM_UNSPECIFIED;
        }

        const std::shared_ptr<GuileState> state = host.state;
        if (!state || !state->active) {
            scm_misc_error("set-input-state-lifecycle!", "Guile runtime has expired", SCM_EOL);
        }
        const std::size_t lifecycle_index = state->input_state_lifecycles.size();
        if (!scheme_false(on_enter_value)) {
            (void)scm_gc_protect_object(on_enter_value);
        }
        if (!scheme_false(on_exit_value)) {
            (void)scm_gc_protect_object(on_exit_value);
        }
        bool appended = false;
        try {
            const std::weak_ptr<GuileState> weak = state;
            EditorRuntime* runtime = host.runtime;
            InputStateLifecycleHandler on_enter =
                scheme_false(on_enter_value)
                    ? InputStateLifecycleHandler{}
                    : InputStateLifecycleHandler{
                          [weak, lifecycle_index, runtime](const InputStateChange& change) {
                              if (const std::shared_ptr<GuileState> locked = weak.lock();
                                  locked && locked->active) {
                                  invoke_script_input_state_lifecycle(locked, lifecycle_index,
                                                                      *runtime, change, true);
                              }
                          }};
            InputStateLifecycleHandler on_exit =
                scheme_false(on_exit_value)
                    ? InputStateLifecycleHandler{}
                    : InputStateLifecycleHandler{
                          [weak, lifecycle_index, runtime](const InputStateChange& change) {
                              if (const std::shared_ptr<GuileState> locked = weak.lock();
                                  locked && locked->active) {
                                  invoke_script_input_state_lifecycle(locked, lifecycle_index,
                                                                      *runtime, change, false);
                              }
                          }};
            state->input_state_lifecycles.push_back(
                {.on_enter = on_enter_value, .on_exit = on_exit_value});
            appended = true;
            definition.on_enter = std::move(on_enter);
            definition.on_exit = std::move(on_exit);
        } catch (...) {
            if (appended) {
                state->input_state_lifecycles.pop_back();
            }
            if (!scheme_false(on_enter_value)) {
                (void)scm_gc_unprotect_object(on_enter_value);
            }
            if (!scheme_false(on_exit_value)) {
                (void)scm_gc_unprotect_object(on_exit_value);
            }
            throw;
        }
    } catch (const std::exception& exception) {
        scm_misc_error("set-input-state-lifecycle!", exception.what(), SCM_EOL);
    } catch (...) {
        scm_misc_error("set-input-state-lifecycle!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

// The provider is an optional capability of a named state rather than part of
// the fixed-arity state-definition ABI.
SCM set_input_state_position_hints(SCM host_object, SCM name_value, SCM provider_value) {
    if (!scheme_false(provider_value) && !scheme_true(scm_procedure_p(provider_value))) {
        scm_wrong_type_arg_msg("set-input-state-position-hints!", 3, provider_value,
                               "procedure or #f");
    }
    try {
        HostLease& host = require_host(host_object, "set-input-state-position-hints!");
        const std::string name = scheme_name(name_value, "set-input-state-position-hints!", 2);
        const std::optional<InputStateId> state_id = host.runtime->input_states().find(name);
        if (!state_id) {
            scm_misc_error("set-input-state-position-hints!", "unknown input state: ~S",
                           scm_list_1(name_value));
        }
        InputStateRegistry::Definition& definition =
            host.runtime->input_states().definition_for_configuration(*state_id);
        if (scheme_false(provider_value)) {
            definition.position_hints = {};
            return SCM_UNSPECIFIED;
        }

        const std::shared_ptr<GuileState> state = host.state;
        if (!state || !state->active) {
            scm_misc_error("set-input-state-position-hints!", "Guile runtime has expired", SCM_EOL);
        }
        const std::size_t provider_index = state->position_hint_providers.size();
        (void)scm_gc_protect_object(provider_value);
        bool appended = false;
        try {
            state->position_hint_providers.push_back({.procedure = provider_value});
            appended = true;
            const std::weak_ptr<GuileState> weak = state;
            definition.position_hints =
                [weak, provider_index](CommandContext& context) -> PositionHintProviderResult {
                const std::shared_ptr<GuileState> locked = weak.lock();
                if (!locked || !locked->active) {
                    return std::unexpected("Guile input state runtime has expired");
                }
                return invoke_script_position_hints(locked, provider_index, context);
            };
        } catch (...) {
            if (appended) {
                state->position_hint_providers.pop_back();
            }
            (void)scm_gc_unprotect_object(provider_value);
            throw;
        }
    } catch (const std::exception& exception) {
        scm_misc_error("set-input-state-position-hints!", exception.what(), SCM_EOL);
    } catch (...) {
        scm_misc_error("set-input-state-position-hints!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

InputStateId require_input_state(HostLease& host, SCM value, const char* caller, int position) {
    const std::string name = scheme_name(value, caller, position);
    const std::optional<InputStateId> state = host.runtime->input_states().find(name);
    if (!state) {
        scm_misc_error(caller, "unknown input state: ~S", scm_list_1(value));
    }
    return *state;
}

InputStrategyId require_input_strategy(HostLease& host, SCM value, const char* caller,
                                       int position) {
    const std::string name = scheme_name(value, caller, position);
    const std::optional<InputStrategyId> strategy = host.runtime->input_strategies().find(name);
    if (!strategy) {
        scm_misc_error(caller, "unknown input strategy: ~S", scm_list_1(value));
    }
    return *strategy;
}

SelectionEditPolicy selection_edit_policy_from_scheme(SCM value, const char* caller, int position) {
    if (symbol_is(value, "collapse")) {
        return SelectionEditPolicy::Collapse;
    }
    if (symbol_is(value, "preserve")) {
        return SelectionEditPolicy::Preserve;
    }
    scm_wrong_type_arg_msg(caller, position, value, "'collapse or 'preserve");
    return SelectionEditPolicy::Collapse;
}

// The Guile ABI fixes five adjacent SCM arguments; validation preserves their semantic order.
SCM define_input_strategy(SCM host_object, SCM name_value, SCM editing_value, SCM interface_value,
                          SCM selection_policy_value) {
    try {
        HostLease& host = require_host(host_object, "define-input-strategy!");
        const std::string name = scheme_name(name_value, "define-input-strategy!", 2);
        InputStrategyRegistry::Definition definition{
            .name = name,
            .editing = require_input_state(host, editing_value, "define-input-strategy!", 3),
            .interface = require_input_state(host, interface_value, "define-input-strategy!", 4),
            .selection_after_edit = selection_edit_policy_from_scheme(selection_policy_value,
                                                                      "define-input-strategy!", 5)};
        const std::optional<InputStrategyId> existing = host.runtime->input_strategies().find(name);
        InputStrategyId id;
        if (existing) {
            id = *existing;
            host.runtime->input_strategies().configure(id, std::move(definition));
        } else {
            id = host.runtime->input_strategies().define(std::move(definition));
            ++host.state->input_strategy_definitions;
        }
        host.runtime->views().refresh_mode_input_states();
        ++host.input_strategies_installed;
        return scm_from_uint32(id.value);
    } catch (const std::exception& exception) {
        scm_misc_error("define-input-strategy!", exception.what(), SCM_EOL);
    } catch (...) {
        scm_misc_error("define-input-strategy!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM set_default_input_strategy(SCM host_object, SCM strategy_value) {
    try {
        HostLease& host = require_host(host_object, "set-default-input-strategy!");
        const InputStrategyId strategy =
            require_input_strategy(host, strategy_value, "set-default-input-strategy!", 2);
        host.runtime->set_default_input_strategy(strategy);
    } catch (const std::exception& exception) {
        scm_misc_error("set-default-input-strategy!", exception.what(), SCM_EOL);
    } catch (...) {
        scm_misc_error("set-default-input-strategy!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

SCM set_view_input_strategy(SCM host_object, SCM view_value, SCM strategy_value) {
    try {
        HostLease& host = require_host(host_object, "set-view-input-strategy!");
        const ViewId view =
            entity_id_from_scheme<ViewTag>(view_value, "set-view-input-strategy!", 2);
        std::optional<InputStrategyId> strategy;
        if (!scheme_false(strategy_value)) {
            strategy = require_input_strategy(host, strategy_value, "set-view-input-strategy!", 3);
        }
        host.runtime->views().set_input_strategy(view, strategy);
    } catch (const std::exception& exception) {
        scm_misc_error("set-view-input-strategy!", exception.what(), SCM_EOL);
    } catch (...) {
        scm_misc_error("set-view-input-strategy!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

SCM view_input_strategy(SCM host_object, SCM view_value) {
    try {
        HostLease& host = require_host(host_object, "view-input-strategy");
        const ViewId view = entity_id_from_scheme<ViewTag>(view_value, "view-input-strategy", 2);
        const View& selected = host.runtime->views().get(view);
        const std::optional<InputStrategyId> strategy =
            selected.input_strategy() ? selected.input_strategy()
                                      : host.runtime->input_strategies().default_strategy();
        return strategy ? name_symbol(host.runtime->input_strategies().definition(*strategy).name)
                        : SCM_BOOL_F;
    } catch (const std::exception& exception) {
        scm_misc_error("view-input-strategy", exception.what(), SCM_EOL);
    } catch (...) {
        scm_misc_error("view-input-strategy", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

ModeId require_mode(HostLease& host, SCM value, const char* caller, int position) {
    const std::string name = scheme_name(value, caller, position);
    const std::optional<ModeId> mode = host.runtime->modes().find(name);
    if (!mode) {
        scm_misc_error(caller, "unknown mode: ~S", scm_list_1(value));
    }
    return *mode;
}

LanguageProfileId require_language_profile(HostLease& host, SCM value, const char* caller,
                                           int position) {
    const std::string name = scheme_name(value, caller, position);
    const std::optional<LanguageProfileId> profile = host.runtime->languages().find_profile(name);
    if (!profile) {
        scm_misc_error(caller, "unknown language profile: ~S", scm_list_1(value));
    }
    return *profile;
}

InteractionClass interaction_class_from_scheme(SCM value, const char* caller, int position) {
    if (symbol_is(value, "editing")) {
        return InteractionClass::Editing;
    }
    if (symbol_is(value, "interface")) {
        return InteractionClass::Interface;
    }
    scm_wrong_type_arg_msg(caller, position, value, "'editing or 'interface");
    return InteractionClass::Editing;
}

LanguageFacet language_facet_from_scheme(SCM value, const char* caller, int position) {
    if (symbol_is(value, "lexing")) {
        return LanguageFacet::Lexing;
    }
    if (symbol_is(value, "syntax")) {
        return LanguageFacet::Syntax;
    }
    if (symbol_is(value, "indentation")) {
        return LanguageFacet::Indentation;
    }
    if (symbol_is(value, "structural-motion")) {
        return LanguageFacet::StructuralMotion;
    }
    if (symbol_is(value, "structural-editing")) {
        return LanguageFacet::StructuralEditing;
    }
    if (symbol_is(value, "highlighting")) {
        return LanguageFacet::Highlighting;
    }
    if (symbol_is(value, "completion")) {
        return LanguageFacet::Completion;
    }
    if (symbol_is(value, "formatting")) {
        return LanguageFacet::Formatting;
    }
    scm_wrong_type_arg_msg(caller, position, value, "language facet symbol");
    return LanguageFacet::Lexing;
}

SCM interaction_class_symbol(InteractionClass interaction_class) {
    return scm_from_utf8_symbol(interaction_class == InteractionClass::Editing ? "editing"
                                                                               : "interface");
}

std::vector<ModeThingBinding> mode_things_from_scheme(SCM value, const char* caller, int position) {
    const long length = scm_ilength(value);
    if (length < 0) {
        scm_wrong_type_arg_msg(caller, position, value, "proper alist of thing bindings");
    }
    std::vector<ModeThingBinding> things;
    things.reserve(static_cast<std::size_t>(length));
    for (SCM rest = value; !scheme_true(scm_null_p(rest)); rest = scm_cdr(rest)) {
        const SCM entry = scm_car(rest);
        if (!scheme_true(scm_pair_p(entry))) {
            scm_wrong_type_arg_msg(caller, position, value, "proper alist of thing bindings");
        }
        things.push_back({.name = scheme_name(scm_car(entry), caller, position),
                          .definition = scheme_name(scm_cdr(entry), caller, position)});
    }
    return things;
}

SCM mode_things_value(const std::vector<ModeThingBinding>& things) {
    SCM result = SCM_EOL;
    for (auto thing = things.rbegin(); thing != things.rend(); ++thing) {
        result =
            scm_cons(scm_cons(name_symbol(thing->name), name_symbol(thing->definition)), result);
    }
    return result;
}

SCM mode_policy_value(const EffectiveModePolicy& policy, const EditorRuntime& runtime) {
    SCM result = scm_c_make_vector(5, SCM_BOOL_F);
    scm_c_vector_set_x(result, 0, interaction_class_symbol(policy.interaction_class));
    if (policy.initial_state) {
        scm_c_vector_set_x(
            result, 1, name_symbol(runtime.input_states().definition(*policy.initial_state).name));
    }
    scm_c_vector_set_x(result, 2, mode_things_value(policy.things));
    scm_c_vector_set_x(result, 3, string_vector_value(policy.completion_providers));
    scm_c_vector_set_x(result, 4, scm_from_bool(policy.completion_auto));
    return result;
}

ThingPattern thing_pattern_from_scheme(SCM value, const char* caller, int position,
                                       std::size_t depth = 0) {
    if (depth >= 32) {
        scm_misc_error(caller, "thing pattern nesting exceeds 32 levels", SCM_EOL);
    }
    const long length = scm_ilength(value);
    if (length < 1) {
        scm_wrong_type_arg_msg(caller, position, value, "non-empty proper thing pattern list");
    }
    const SCM tag = scm_car(value);
    if (symbol_is(tag, "pair")) {
        if (length != 3 || !scm_is_string(scm_cadr(value)) || !scm_is_string(scm_caddr(value))) {
            scm_wrong_type_arg_msg(caller, position, value, "(pair opening-string closing-string)");
        }
        return {.kind = ThingPatternKind::Pair,
                .arguments = {scheme_string(scm_cadr(value)), scheme_string(scm_caddr(value))},
                .alternatives = {}};
    }
    if (symbol_is(tag, "cst-node") || symbol_is(tag, "char-class")) {
        if (length != 2) {
            scm_wrong_type_arg_msg(caller, position, value, "(cst-node name) or (char-class name)");
        }
        return {.kind = symbol_is(tag, "cst-node") ? ThingPatternKind::CstNode
                                                   : ThingPatternKind::CharacterClass,
                .arguments = {scheme_name(scm_cadr(value), caller, position)},
                .alternatives = {}};
    }
    if (symbol_is(tag, "multi")) {
        if (length < 2) {
            scm_wrong_type_arg_msg(caller, position, value, "(multi pattern pattern ...)");
        }
        ThingPattern result{.kind = ThingPatternKind::Multi, .arguments = {}, .alternatives = {}};
        result.alternatives.reserve(static_cast<std::size_t>(length - 1));
        for (SCM rest = scm_cdr(value); !scheme_true(scm_null_p(rest)); rest = scm_cdr(rest)) {
            result.alternatives.push_back(
                thing_pattern_from_scheme(scm_car(rest), caller, position, depth + 1));
        }
        return result;
    }
    scm_wrong_type_arg_msg(caller, position, value,
                           "pair, cst-node, char-class, or multi thing pattern");
    return {};
}

MotionMechanism motion_mechanism_from_scheme(SCM value, const char* caller, int position) {
    if (symbol_is(value, "forward-character"))
        return MotionMechanism::ForwardCharacter;
    if (symbol_is(value, "backward-character"))
        return MotionMechanism::BackwardCharacter;
    if (symbol_is(value, "forward-word"))
        return MotionMechanism::ForwardWord;
    if (symbol_is(value, "forward-word-end"))
        return MotionMechanism::ForwardWordEnd;
    if (symbol_is(value, "backward-word"))
        return MotionMechanism::BackwardWord;
    if (symbol_is(value, "forward-symbol"))
        return MotionMechanism::ForwardSymbol;
    if (symbol_is(value, "backward-symbol"))
        return MotionMechanism::BackwardSymbol;
    if (symbol_is(value, "forward-expression"))
        return MotionMechanism::ForwardExpression;
    if (symbol_is(value, "backward-expression"))
        return MotionMechanism::BackwardExpression;
    if (symbol_is(value, "up-list"))
        return MotionMechanism::UpList;
    scm_wrong_type_arg_msg(caller, position, value, "registered motion mechanism symbol");
    return MotionMechanism::ForwardCharacter;
}

SCM define_thing(SCM host_object, SCM name_value, SCM pattern_value) {
    try {
        HostLease& host = require_host(host_object, "define-thing!");
        const std::string name = scheme_name(name_value, "define-thing!", 2);
        ThingPattern pattern = thing_pattern_from_scheme(pattern_value, "define-thing!", 3);
        const std::optional<ThingId> existing = host.runtime->things().find(name);
        if (existing) {
            host.runtime->things().configure(*existing, std::move(pattern));
            return scm_from_uint32(existing->value);
        }
        const ThingId thing = host.runtime->things().define(name, std::move(pattern));
        return scm_from_uint32(thing.value);
    } catch (const std::exception& exception) {
        scm_misc_error("define-thing!", exception.what(), SCM_EOL);
    } catch (...) {
        scm_misc_error("define-thing!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM define_motion(SCM host_object, SCM name_value, SCM mechanism_value) {
    try {
        HostLease& host = require_host(host_object, "define-motion!");
        const std::string name = scheme_name(name_value, "define-motion!", 2);
        const MotionMechanism mechanism =
            motion_mechanism_from_scheme(mechanism_value, "define-motion!", 3);
        const std::optional<MotionId> existing = host.runtime->motions().find(name);
        const MotionId motion =
            existing ? *existing : host.runtime->motions().define(name, mechanism);
        if (existing) {
            host.runtime->motions().configure(motion, mechanism);
        }
        return scm_from_uint32(motion.value);
    } catch (const std::exception& exception) {
        scm_misc_error("define-motion!", exception.what(), SCM_EOL);
    } catch (...) {
        scm_misc_error("define-motion!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

// The public Scheme wrapper names the provider/default association lists.
SCM define_language_profile(SCM host_object, SCM name_value, SCM providers_value,
                            SCM defaults_value) {
    try {
        HostLease& host = require_host(host_object, "define-language-profile!");
        const std::string name = scheme_name(name_value, "define-language-profile!", 2);
        if (scm_ilength(providers_value) < 0) {
            scm_wrong_type_arg_msg("define-language-profile!", 3, providers_value,
                                   "proper association list");
        }
        if (scm_ilength(defaults_value) < 0) {
            scm_wrong_type_arg_msg("define-language-profile!", 4, defaults_value,
                                   "proper association list");
        }

        std::vector<std::pair<LanguageFacet, LanguageProviderId>> providers;
        for (SCM remaining = providers_value; !scheme_true(scm_null_p(remaining));
             remaining = scm_cdr(remaining)) {
            const SCM entry = scm_car(remaining);
            if (!scm_is_pair(entry)) {
                scm_wrong_type_arg_msg("define-language-profile!", 3, entry,
                                       "(facet . provider) entry");
            }
            const LanguageFacet facet =
                language_facet_from_scheme(scm_car(entry), "define-language-profile!", 3);
            const std::string provider_name =
                scheme_name(scm_cdr(entry), "define-language-profile!", 3);
            const std::optional<LanguageProviderId> provider =
                host.runtime->languages().find_provider(provider_name);
            if (!provider) {
                scm_misc_error("define-language-profile!", "unknown language provider: ~S",
                               scm_list_1(scm_cdr(entry)));
            }
            providers.emplace_back(facet, *provider);
        }

        std::vector<std::pair<SettingId, SettingValue>> defaults;
        SettingsLayer validated_defaults(host.runtime->setting_definitions(),
                                         SettingScope::Language);
        for (SCM remaining = defaults_value; !scheme_true(scm_null_p(remaining));
             remaining = scm_cdr(remaining)) {
            const SCM entry = scm_car(remaining);
            if (!scm_is_pair(entry) || !setting_value_p(scm_cdr(entry))) {
                scm_wrong_type_arg_msg("define-language-profile!", 4, entry,
                                       "(setting . value) entry");
            }
            const std::string setting_name =
                scheme_name(scm_car(entry), "define-language-profile!", 4);
            const std::optional<SettingId> setting =
                host.runtime->setting_definitions().find(setting_name);
            if (!setting) {
                scm_misc_error("define-language-profile!", "unknown setting: ~S",
                               scm_list_1(scm_car(entry)));
            }
            SettingValue value = setting_from_scheme(scm_cdr(entry));
            validated_defaults.set(*setting, value);
            defaults.emplace_back(*setting, std::move(value));
        }

        const std::optional<LanguageProfileId> existing =
            host.runtime->languages().find_profile(name);
        const LanguageProfileId profile =
            existing ? *existing : host.runtime->languages().define_profile(name);
        host.runtime->languages().clear_profile(profile);
        for (const auto& [facet, provider] : providers) {
            host.runtime->languages().bind(profile, facet, provider);
        }
        SettingsLayer& profile_defaults =
            host.runtime->languages().profile_for_configuration(profile).defaults;
        for (auto& [setting, value] : defaults) {
            profile_defaults.set(setting, std::move(value));
        }
        return scm_from_uint32(profile.value);
    } catch (const std::exception& exception) {
        scm_misc_error("define-language-profile!", exception.what(), SCM_EOL);
    } catch (...) {
        scm_misc_error("define-language-profile!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

// The Guile ABI fixes ten adjacent SCM arguments; the public Scheme wrappers
// provide keyword arguments and preserve this normalized host boundary.
SCM define_mode(SCM host_object, SCM name_value, SCM kind_value, SCM parent_value,
                SCM language_value, SCM keymap_value, SCM interaction_class_value,
                SCM initial_state_value, SCM things_value, SCM completion_providers_value) {
    try {
        HostLease& host = require_host(host_object, "%define-mode!");
        const std::string name = scheme_name(name_value, "%define-mode!", 2);
        const ModeKind kind =
            symbol_is(kind_value, "major") ? ModeKind::Major
            : symbol_is(kind_value, "minor")
                ? ModeKind::Minor
                : throw std::invalid_argument("mode kind must be 'major or 'minor");
        std::optional<ModeId> parent;
        if (!scheme_false(parent_value)) {
            parent = require_mode(host, parent_value, "%define-mode!", 4);
        }
        std::optional<LanguageProfileId> language;
        if (!scheme_false(language_value)) {
            language = require_language_profile(host, language_value, "%define-mode!", 5);
        }
        std::optional<KeymapId> keymap;
        if (!scheme_false(keymap_value)) {
            keymap = require_keymap(host, keymap_value, "%define-mode!", 6);
        }
        std::optional<InteractionClass> interaction_class;
        if (!scheme_false(interaction_class_value)) {
            interaction_class =
                interaction_class_from_scheme(interaction_class_value, "%define-mode!", 7);
        }
        std::optional<InputStateId> initial_state;
        if (!scheme_false(initial_state_value)) {
            initial_state = require_input_state(host, initial_state_value, "%define-mode!", 8);
        }
        std::vector<ModeThingBinding> things =
            mode_things_from_scheme(things_value, "%define-mode!", 9);
        std::optional<std::vector<std::string>> completion_providers;
        if (!scheme_false(completion_providers_value)) {
            completion_providers =
                string_sequence_from_scheme(completion_providers_value, "%define-mode!", 10);
        }
        const std::optional<ModeId> existing = host.runtime->modes().find(name);
        const ModeId mode =
            existing ? *existing : host.runtime->modes().define(name, kind, language);
        if (host.runtime->modes().definition(mode).kind != kind) {
            throw std::invalid_argument("mode definition cannot change its kind");
        }
        if (host.runtime->modes().definition(mode).language != language) {
            throw std::invalid_argument("mode definition cannot change its language profile");
        }
        host.runtime->modes().set_parent(mode, parent);
        host.runtime->modes().set_interaction_class(mode, interaction_class);
        host.runtime->modes().set_initial_state(mode, initial_state);
        host.runtime->modes().set_things(mode, std::move(things));
        host.runtime->modes().set_completion_providers(mode, std::move(completion_providers));
        host.runtime->modes().clear_keymaps(mode);
        if (keymap) {
            host.runtime->modes().add_keymap(mode, *keymap);
        }
        if (!existing) {
            ++host.state->mode_definitions;
        }
        ++host.modes_installed;
        return scm_from_uint32(mode.value);
    } catch (const std::exception& exception) {
        scm_misc_error("%define-mode!", exception.what(), SCM_EOL);
    } catch (...) {
        scm_misc_error("%define-mode!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM set_mode_completion_auto(SCM host_object, SCM mode_value, SCM enabled_value) {
    if (!scheme_boolean(enabled_value) && !symbol_is(enabled_value, "inherit")) {
        scm_wrong_type_arg_msg("set-mode-completion-auto!", 3, enabled_value,
                               "boolean or 'inherit");
    }
    try {
        HostLease& host = require_host(host_object, "set-mode-completion-auto!");
        const ModeId mode = require_mode(host, mode_value, "set-mode-completion-auto!", 2);
        host.runtime->modes().set_completion_auto(
            mode, symbol_is(enabled_value, "inherit")
                      ? std::nullopt
                      : std::optional<bool>{scheme_true(enabled_value)});
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        scm_misc_error("set-mode-completion-auto!", exception.what(), SCM_EOL);
    } catch (...) {
        scm_misc_error("set-mode-completion-auto!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

// The Guile ABI fixes five adjacent SCM arguments; the public Scheme policy
// supplies declarative matcher lists.
SCM define_file_mode_rule(SCM host_object, SCM name_value, SCM mode_value, SCM suffixes_value,
                          SCM filenames_value) {
    try {
        HostLease& host = require_host(host_object, "define-file-mode-rule!");
        const std::string name = scheme_name(name_value, "define-file-mode-rule!", 2);
        const ModeId mode = require_mode(host, mode_value, "define-file-mode-rule!", 3);
        std::vector<std::string> suffixes =
            string_sequence_from_scheme(suffixes_value, "define-file-mode-rule!", 4);
        std::vector<std::string> filenames =
            string_sequence_from_scheme(filenames_value, "define-file-mode-rule!", 5);
        host.runtime->resource_policies().define_file_mode(name, mode, std::move(suffixes),
                                                           std::move(filenames));
        ++host.resource_policies_installed;
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        scm_misc_error("define-file-mode-rule!", exception.what(), SCM_EOL);
    } catch (...) {
        scm_misc_error("define-file-mode-rule!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

// Guile requires callbacks to expose one SCM parameter per Scheme argument.
SCM define_project_provider(SCM host_object, SCM name_value, SCM markers_value) {
    try {
        HostLease& host = require_host(host_object, "define-project-provider!");
        const std::string name = scheme_name(name_value, "define-project-provider!", 2);
        std::vector<std::string> markers =
            string_sequence_from_scheme(markers_value, "define-project-provider!", 3);
        host.runtime->resource_policies().define_project_provider(name, std::move(markers));
        ++host.resource_policies_installed;
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        scm_misc_error("define-project-provider!", exception.what(), SCM_EOL);
    } catch (...) {
        scm_misc_error("define-project-provider!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

SCM mode_properties(SCM host_object, SCM mode_value) {
    try {
        HostLease& host = require_host(host_object, "mode-properties");
        const ModeId mode = require_mode(host, mode_value, "mode-properties", 2);
        const ModeRegistry::Definition& definition = host.runtime->modes().definition(mode);
        SCM result = scm_c_make_vector(10, SCM_BOOL_F);
        scm_c_vector_set_x(result, 0, name_symbol(definition.name));
        scm_c_vector_set_x(
            result, 1,
            scm_from_utf8_symbol(definition.kind == ModeKind::Major ? "major" : "minor"));
        if (definition.parent) {
            scm_c_vector_set_x(
                result, 2, name_symbol(host.runtime->modes().definition(*definition.parent).name));
        }
        if (definition.interaction_class) {
            scm_c_vector_set_x(result, 3, interaction_class_symbol(*definition.interaction_class));
        }
        if (definition.initial_state) {
            scm_c_vector_set_x(
                result, 4,
                name_symbol(
                    host.runtime->input_states().definition(*definition.initial_state).name));
        }
        scm_c_vector_set_x(result, 5, mode_things_value(definition.things));
        const std::vector<KeymapId> keymaps = host.runtime->modes().effective_keymaps(mode);
        SCM keymap_names = scm_c_make_vector(keymaps.size(), SCM_UNSPECIFIED);
        for (std::size_t index = 0; index < keymaps.size(); ++index) {
            scm_c_vector_set_x(
                keymap_names, index,
                name_symbol(host.runtime->keymaps().definition(keymaps[index]).name));
        }
        scm_c_vector_set_x(result, 6, keymap_names);
        if (definition.language) {
            scm_c_vector_set_x(
                result, 7,
                name_symbol(host.runtime->languages().profile(*definition.language).name));
        }
        if (definition.completion_providers) {
            scm_c_vector_set_x(result, 8, string_vector_value(*definition.completion_providers));
        }
        if (definition.completion_auto) {
            scm_c_vector_set_x(
                result, 9,
                scm_from_utf8_symbol(*definition.completion_auto ? "enabled" : "disabled"));
        }
        return result;
    } catch (const std::exception& exception) {
        scm_misc_error("mode-properties", exception.what(), SCM_EOL);
    } catch (...) {
        scm_misc_error("mode-properties", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM buffer_language_facet_p(SCM host_object, SCM buffer_value, SCM facet_value) {
    try {
        HostLease& host = require_host(host_object, "buffer-language-facet?");
        const BufferId buffer =
            entity_id_from_scheme<BufferTag>(buffer_value, "buffer-language-facet?", 2);
        const LanguageFacet facet =
            language_facet_from_scheme(facet_value, "buffer-language-facet?", 3);
        return scm_from_bool(host.runtime->language_provider(buffer, facet).has_value());
    } catch (const std::exception& exception) {
        raise_host_error("buffer-language-facet?", exception.what());
    } catch (...) {
        scm_misc_error("buffer-language-facet?", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM set_buffer_major_mode(SCM host_object, SCM buffer_value, SCM mode_value) {
    try {
        HostLease& host = require_host(host_object, "set-buffer-major-mode!");
        const BufferId buffer =
            entity_id_from_scheme<BufferTag>(buffer_value, "set-buffer-major-mode!", 2);
        std::optional<ModeId> mode;
        if (!scheme_false(mode_value)) {
            mode = require_mode(host, mode_value, "set-buffer-major-mode!", 3);
        }
        host.runtime->buffers().get(buffer).modes().set_major(host.runtime->modes(), mode);
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        scm_misc_error("set-buffer-major-mode!", exception.what(), SCM_EOL);
    } catch (...) {
        scm_misc_error("set-buffer-major-mode!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

SCM set_buffer_minor_mode(SCM host_object, SCM buffer_value, SCM mode_value, SCM enabled_value) {
    if (!scheme_boolean(enabled_value)) {
        scm_wrong_type_arg_msg("set-buffer-minor-mode!", 4, enabled_value, "boolean");
    }
    try {
        HostLease& host = require_host(host_object, "set-buffer-minor-mode!");
        const BufferId buffer =
            entity_id_from_scheme<BufferTag>(buffer_value, "set-buffer-minor-mode!", 2);
        const ModeId mode = require_mode(host, mode_value, "set-buffer-minor-mode!", 3);
        BufferModes& modes = host.runtime->buffers().get(buffer).modes();
        const bool changed = scheme_true(enabled_value)
                                 ? modes.enable_minor(host.runtime->modes(), mode)
                                 : modes.disable_minor(mode);
        return scm_from_bool(changed);
    } catch (const std::exception& exception) {
        scm_misc_error("set-buffer-minor-mode!", exception.what(), SCM_EOL);
    } catch (...) {
        scm_misc_error("set-buffer-minor-mode!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM buffer_mode_policy(SCM host_object, SCM buffer_value) {
    try {
        HostLease& host = require_host(host_object, "buffer-mode-policy");
        const BufferId buffer =
            entity_id_from_scheme<BufferTag>(buffer_value, "buffer-mode-policy", 2);
        return mode_policy_value(
            host.runtime->modes().effective_policy(host.runtime->buffers().get(buffer).modes()),
            *host.runtime);
    } catch (const std::exception& exception) {
        scm_misc_error("buffer-mode-policy", exception.what(), SCM_EOL);
    } catch (...) {
        scm_misc_error("buffer-mode-policy", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

// Returns #(major-mode-or-#f minor-modes effective-policy).
SCM buffer_mode_summary(SCM host_object, SCM buffer_value) {
    try {
        HostLease& host = require_host(host_object, "buffer-mode-summary");
        const BufferId buffer =
            entity_id_from_scheme<BufferTag>(buffer_value, "buffer-mode-summary", 2);
        const BufferModes& modes = host.runtime->buffers().get(buffer).modes();
        SCM result = scm_c_make_vector(3, SCM_BOOL_F);
        if (modes.major()) {
            scm_c_vector_set_x(result, 0,
                               name_symbol(host.runtime->modes().definition(*modes.major()).name));
        }
        SCM minors = scm_c_make_vector(modes.minors().size(), SCM_UNSPECIFIED);
        for (std::size_t index = 0; index < modes.minors().size(); ++index) {
            scm_c_vector_set_x(
                minors, index,
                name_symbol(host.runtime->modes().definition(modes.minors()[index]).name));
        }
        scm_c_vector_set_x(result, 1, minors);
        scm_c_vector_set_x(
            result, 2,
            mode_policy_value(host.runtime->modes().effective_policy(modes), *host.runtime));
        return result;
    } catch (const std::exception& exception) {
        scm_misc_error("buffer-mode-summary", exception.what(), SCM_EOL);
    } catch (...) {
        scm_misc_error("buffer-mode-summary", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM observe_mode_policy_changes(SCM host_object, SCM procedure_value) {
    if (!scheme_true(scm_procedure_p(procedure_value))) {
        scm_wrong_type_arg_msg("observe-mode-policy-changes!", 2, procedure_value, "procedure");
    }
    try {
        HostLease& host = require_host(host_object, "observe-mode-policy-changes!");
        const std::shared_ptr<GuileState> state = host.state;
        if (!state || !state->active) {
            scm_misc_error("observe-mode-policy-changes!", "Guile runtime has expired", SCM_EOL);
        }
        const std::size_t observer_index = state->mode_policy_observers.size();
        (void)scm_gc_protect_object(procedure_value);
        ModeRegistry::ListenerId listener = 0;
        try {
            const std::weak_ptr<GuileState> weak = state;
            EditorRuntime* runtime = host.runtime;
            listener = host.runtime->modes().subscribe([weak, observer_index, runtime](
                                                           const BufferModePolicyChange& change) {
                const std::shared_ptr<GuileState> locked = weak.lock();
                if (locked && locked->active) {
                    invoke_script_mode_policy_observer(locked, observer_index, *runtime, change);
                }
            });
            state->mode_policy_observers.push_back(
                {.procedure = procedure_value, .listener = listener});
            return scm_from_size_t(observer_index);
        } catch (...) {
            if (listener != 0) {
                (void)host.runtime->modes().unsubscribe(listener);
            }
            (void)scm_gc_unprotect_object(procedure_value);
            throw;
        }
    } catch (const std::exception& exception) {
        scm_misc_error("observe-mode-policy-changes!", exception.what(), SCM_EOL);
    } catch (...) {
        scm_misc_error("observe-mode-policy-changes!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM set_base_input_state(SCM host_object, SCM view_value, SCM state_value) {
    try {
        HostLease& host = require_host(host_object, "set-base-input-state!");
        const ViewId view = entity_id_from_scheme<ViewTag>(view_value, "set-base-input-state!", 2);
        const InputStateId state =
            require_input_state(host, state_value, "set-base-input-state!", 3);
        host.runtime->views().set_base_input_state(view, state);
    } catch (const std::exception& exception) {
        scm_misc_error("set-base-input-state!", exception.what(), SCM_EOL);
    } catch (...) {
        scm_misc_error("set-base-input-state!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

SCM push_input_state(SCM host_object, SCM view_value, SCM state_value) {
    try {
        HostLease& host = require_host(host_object, "push-input-state!");
        const ViewId view = entity_id_from_scheme<ViewTag>(view_value, "push-input-state!", 2);
        const InputStateId state = require_input_state(host, state_value, "push-input-state!", 3);
        host.runtime->views().push_input_state(view, state);
    } catch (const std::exception& exception) {
        scm_misc_error("push-input-state!", exception.what(), SCM_EOL);
    } catch (...) {
        scm_misc_error("push-input-state!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

SCM pop_input_state(SCM host_object, SCM view_value) {
    try {
        HostLease& host = require_host(host_object, "pop-input-state!");
        const ViewId view = entity_id_from_scheme<ViewTag>(view_value, "pop-input-state!", 2);
        const std::optional<InputStateId> removed = host.runtime->views().pop_input_state(view);
        return removed ? name_symbol(host.runtime->input_states().definition(*removed).name)
                       : SCM_BOOL_F;
    } catch (const std::exception& exception) {
        scm_misc_error("pop-input-state!", exception.what(), SCM_EOL);
    } catch (...) {
        scm_misc_error("pop-input-state!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM reset_input_states(SCM host_object, SCM view_value) {
    try {
        HostLease& host = require_host(host_object, "reset-input-states!");
        const ViewId view = entity_id_from_scheme<ViewTag>(view_value, "reset-input-states!", 2);
        host.runtime->views().reset_input_states(view);
    } catch (const std::exception& exception) {
        scm_misc_error("reset-input-states!", exception.what(), SCM_EOL);
    } catch (...) {
        scm_misc_error("reset-input-states!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

SCM view_input_states(SCM host_object, SCM view_value) {
    try {
        HostLease& host = require_host(host_object, "view-input-states");
        const ViewId view = entity_id_from_scheme<ViewTag>(view_value, "view-input-states", 2);
        const std::vector<InputStateId>& stack =
            host.runtime->views().get(view).input_states().stack();
        SCM result = scm_c_make_vector(stack.size(), SCM_UNSPECIFIED);
        for (std::size_t index = 0; index < stack.size(); ++index) {
            scm_c_vector_set_x(
                result, index,
                name_symbol(host.runtime->input_states().definition(stack[index]).name));
        }
        return result;
    } catch (const std::exception& exception) {
        scm_misc_error("view-input-states", exception.what(), SCM_EOL);
    } catch (...) {
        scm_misc_error("view-input-states", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM observe_input_state_changes(SCM host_object, SCM procedure_value) {
    if (!scheme_true(scm_procedure_p(procedure_value))) {
        scm_wrong_type_arg_msg("observe-input-state-changes!", 2, procedure_value, "procedure");
    }
    try {
        HostLease& host = require_host(host_object, "observe-input-state-changes!");
        const std::shared_ptr<GuileState> state = host.state;
        if (!state || !state->active) {
            scm_misc_error("observe-input-state-changes!", "Guile runtime has expired", SCM_EOL);
        }
        const std::size_t observer_index = state->input_state_observers.size();
        (void)scm_gc_protect_object(procedure_value);
        InputStateRegistry::ListenerId listener = 0;
        try {
            const std::weak_ptr<GuileState> weak = state;
            EditorRuntime* runtime = host.runtime;
            listener = host.runtime->input_states().subscribe([weak, observer_index, runtime](
                                                                  const InputStateChange& change) {
                const std::shared_ptr<GuileState> locked = weak.lock();
                if (locked && locked->active) {
                    invoke_script_input_state_observer(locked, observer_index, *runtime, change);
                }
            });
            state->input_state_observers.push_back(
                {.procedure = procedure_value, .listener = listener});
            return scm_from_size_t(observer_index);
        } catch (...) {
            if (listener != 0) {
                (void)host.runtime->input_states().unsubscribe(listener);
            }
            (void)scm_gc_unprotect_object(procedure_value);
            throw;
        }
    } catch (const std::exception& exception) {
        scm_misc_error("observe-input-state-changes!", exception.what(), SCM_EOL);
    } catch (...) {
        scm_misc_error("observe-input-state-changes!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

// The Guile ABI fixes two adjacent SCM arguments; their Scheme procedure name
// and validation preserve the semantic order.
SCM enabled_command_names(SCM host_object, SCM context_value) {
    try {
        HostLease& host = require_host(host_object, "enabled-command-names");
        CommandContext context =
            command_context_from_scheme(host, context_value, "enabled-command-names");
        std::vector<std::string> names;
        for (const CommandId command : host.runtime->commands().all()) {
            if (host.runtime->commands().enabled(command, context)) {
                names.push_back(host.runtime->commands().definition(command).name);
            }
        }
        SCM result = scm_c_make_vector(names.size(), SCM_UNSPECIFIED);
        for (std::size_t index = 0; index < names.size(); ++index) {
            scm_c_vector_set_x(result, index, scm_from_utf8_string(names[index].c_str()));
        }
        return result;
    } catch (const std::exception& exception) {
        raise_host_error("enabled-command-names", exception.what());
    } catch (...) {
        scm_misc_error("enabled-command-names", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

// Returns #(name documentation-or-#f source enabled? bindings).
SCM command_properties(SCM host_object, SCM context_value, SCM name_value) {
    if (!scm_is_string(name_value)) {
        scm_wrong_type_arg_msg("command-properties", 3, name_value, "string");
    }
    try {
        HostLease& host = require_host(host_object, "command-properties");
        CommandContext context =
            command_context_from_scheme(host, context_value, "command-properties");
        const std::string name = scheme_string(name_value);
        const std::optional<CommandId> command = host.runtime->commands().find(name);
        if (!command) {
            return SCM_BOOL_F;
        }
        const CommandRegistry::Definition& definition =
            host.runtime->commands().definition(*command);
        std::vector<std::string> bindings;
        if (host.services.active_key_bindings) {
            for (const GuileKeyBindingSummary& binding : host.services.active_key_bindings()) {
                if (binding.command == name) {
                    bindings.push_back(binding.keys);
                }
            }
        }
        SCM binding_values = scm_c_make_vector(bindings.size(), SCM_UNSPECIFIED);
        for (std::size_t index = 0; index < bindings.size(); ++index) {
            scm_c_vector_set_x(binding_values, index,
                               scm_from_utf8_string(bindings[index].c_str()));
        }
        SCM result = scm_c_make_vector(5, SCM_BOOL_F);
        scm_c_vector_set_x(result, 0, scm_from_utf8_string(definition.name.c_str()));
        if (!definition.documentation.empty()) {
            scm_c_vector_set_x(result, 1, scm_from_utf8_string(definition.documentation.c_str()));
        }
        scm_c_vector_set_x(result, 2, scm_from_utf8_string(definition.source.c_str()));
        scm_c_vector_set_x(result, 3,
                           scm_from_bool(host.runtime->commands().enabled(*command, context)));
        scm_c_vector_set_x(result, 4, binding_values);
        return result;
    } catch (const std::exception& exception) {
        scm_misc_error("command-properties", exception.what(), SCM_EOL);
    } catch (...) {
        scm_misc_error("command-properties", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM buffer_summary_value(const Buffer& buffer, std::optional<bool> visitor = std::nullopt) {
    SCM summary = scm_c_make_vector(visitor ? 4 : 3, SCM_UNSPECIFIED);
    scm_c_vector_set_x(summary, 0, scm_from_utf8_string(buffer.name().c_str()));
    scm_c_vector_set_x(summary, 1,
                       buffer.resource_uri() ? scm_from_utf8_string(buffer.resource_uri()->c_str())
                                             : SCM_BOOL_F);
    scm_c_vector_set_x(summary, 2, scm_from_bool(buffer.modified()));
    if (visitor) {
        scm_c_vector_set_x(summary, 3, scm_from_bool(*visitor));
    }
    return summary;
}

SCM buffer_summaries_value(HostLease& host, const std::vector<BufferId>& buffers,
                           const std::vector<ProjectId>* scope = nullptr) {
    SCM result = scm_c_make_vector(buffers.size(), SCM_UNSPECIFIED);
    for (std::size_t index = 0; index < buffers.size(); ++index) {
        const Buffer& buffer = host.runtime->buffers().get(buffers[index]);
        std::optional<bool> visitor;
        if (scope != nullptr) {
            visitor = !buffer.project_id() ||
                      std::ranges::find(*scope, *buffer.project_id()) == scope->end();
        }
        scm_c_vector_set_x(result, index, buffer_summary_value(buffer, visitor));
    }
    return result;
}

std::vector<GuileWorkbenchSummary> workbench_snapshot(HostLease& host, const char* caller) {
    if (!host.services.workbenches) {
        scm_misc_error(caller, "workbench snapshot capability is unavailable", SCM_EOL);
    }
    return host.services.workbenches();
}

const GuileWorkbenchSummary& require_workbench(const std::vector<GuileWorkbenchSummary>& values,
                                               WorkbenchId workbench, const char* caller) {
    const auto found = std::ranges::find(values, workbench, &GuileWorkbenchSummary::workbench);
    if (found == values.end()) {
        scm_misc_error(caller, "unknown workbench", SCM_EOL);
    }
    return *found;
}

SCM open_buffer_summaries(SCM host_object) {
    HostLease& host = require_host(host_object, "open-buffer-summaries");
    if (!host.services.open_buffers) {
        scm_misc_error("open-buffer-summaries", "open-buffer snapshot capability is unavailable",
                       SCM_EOL);
    }
    try {
        return buffer_summaries_value(host, host.services.open_buffers());
    } catch (const std::exception& exception) {
        raise_host_error("open-buffer-summaries", exception.what());
    } catch (...) {
        scm_misc_error("open-buffer-summaries", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM workbench_list(SCM host_object) {
    try {
        HostLease& host = require_host(host_object, "workbench-list");
        const std::vector<GuileWorkbenchSummary> values =
            workbench_snapshot(host, "workbench-list");
        SCM result = scm_c_make_vector(values.size(), SCM_UNSPECIFIED);
        for (std::size_t index = 0; index < values.size(); ++index) {
            SCM summary = scm_c_make_vector(3, SCM_UNSPECIFIED);
            scm_c_vector_set_x(
                summary, 0,
                entity_id(values[index].workbench.slot, values[index].workbench.generation));
            scm_c_vector_set_x(summary, 1, scm_from_utf8_string(values[index].name.c_str()));
            scm_c_vector_set_x(summary, 2, scm_from_bool(values[index].active));
            scm_c_vector_set_x(result, index, summary);
        }
        return result;
    } catch (const std::exception& exception) {
        raise_host_error("workbench-list", exception.what());
    } catch (...) {
        scm_misc_error("workbench-list", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM current_workbench(SCM host_object) {
    HostLease& host = require_host(host_object, "current-workbench");
    if (!host.services.active_workbench) {
        scm_misc_error("current-workbench", "active workbench capability is unavailable", SCM_EOL);
    }
    try {
        const WorkbenchId workbench = host.services.active_workbench();
        return entity_id(workbench.slot, workbench.generation);
    } catch (const std::exception& exception) {
        raise_host_error("current-workbench", exception.what());
    } catch (...) {
        scm_misc_error("current-workbench", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM workbench_scope(SCM host_object, SCM workbench_value) {
    try {
        HostLease& host = require_host(host_object, "workbench-scope");
        const WorkbenchId workbench =
            entity_id_from_scheme<WorkbenchTag>(workbench_value, "workbench-scope", 2);
        const std::vector<GuileWorkbenchSummary> values =
            workbench_snapshot(host, "workbench-scope");
        const std::vector<ProjectId>& scope =
            require_workbench(values, workbench, "workbench-scope").scope;
        SCM result = scm_c_make_vector(scope.size(), SCM_UNSPECIFIED);
        for (std::size_t index = 0; index < scope.size(); ++index) {
            scm_c_vector_set_x(result, index,
                               entity_id(scope[index].slot, scope[index].generation));
        }
        return result;
    } catch (const std::exception& exception) {
        raise_host_error("workbench-scope", exception.what());
    } catch (...) {
        scm_misc_error("workbench-scope", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM workbench_mru(SCM host_object, SCM workbench_value) {
    try {
        HostLease& host = require_host(host_object, "workbench-mru");
        const WorkbenchId workbench =
            entity_id_from_scheme<WorkbenchTag>(workbench_value, "workbench-mru", 2);
        const std::vector<GuileWorkbenchSummary> values = workbench_snapshot(host, "workbench-mru");
        const std::vector<BufferId>& mru =
            require_workbench(values, workbench, "workbench-mru").mru;
        SCM result = scm_c_make_vector(mru.size(), SCM_UNSPECIFIED);
        for (std::size_t index = 0; index < mru.size(); ++index) {
            scm_c_vector_set_x(result, index, entity_id(mru[index].slot, mru[index].generation));
        }
        return result;
    } catch (const std::exception& exception) {
        raise_host_error("workbench-mru", exception.what());
    } catch (...) {
        scm_misc_error("workbench-mru", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM workbench_buffer_summaries(SCM host_object, SCM workbench_value, SCM widen_value) {
    if (!scheme_boolean(widen_value)) {
        scm_wrong_type_arg_msg("workbench-buffer-summaries", 3, widen_value, "boolean");
    }
    try {
        HostLease& host = require_host(host_object, "workbench-buffer-summaries");
        if (!host.services.workbench_buffers) {
            scm_misc_error("workbench-buffer-summaries",
                           "workbench buffer capability is unavailable", SCM_EOL);
        }
        const WorkbenchId workbench =
            entity_id_from_scheme<WorkbenchTag>(workbench_value, "workbench-buffer-summaries", 2);
        const std::vector<GuileWorkbenchSummary> values =
            workbench_snapshot(host, "workbench-buffer-summaries");
        const std::vector<ProjectId>& scope =
            require_workbench(values, workbench, "workbench-buffer-summaries").scope;
        return buffer_summaries_value(
            host, host.services.workbench_buffers(workbench, scheme_true(widen_value)), &scope);
    } catch (const std::exception& exception) {
        raise_host_error("workbench-buffer-summaries", exception.what());
    } catch (...) {
        scm_misc_error("workbench-buffer-summaries", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM workbench_buffer_ids(SCM host_object, SCM workbench_value, SCM widen_value) {
    if (!scheme_boolean(widen_value)) {
        scm_wrong_type_arg_msg("workbench-buffer-ids", 3, widen_value, "boolean");
    }
    HostLease& host = require_host(host_object, "workbench-buffer-ids");
    if (!host.services.workbench_buffers) {
        scm_misc_error("workbench-buffer-ids", "workbench buffer capability is unavailable",
                       SCM_EOL);
    }
    try {
        const WorkbenchId workbench =
            entity_id_from_scheme<WorkbenchTag>(workbench_value, "workbench-buffer-ids", 2);
        const std::vector<BufferId> buffers =
            host.services.workbench_buffers(workbench, scheme_true(widen_value));
        SCM result = scm_c_make_vector(buffers.size(), SCM_UNSPECIFIED);
        for (std::size_t index = 0; index < buffers.size(); ++index) {
            scm_c_vector_set_x(result, index,
                               entity_id(buffers[index].slot, buffers[index].generation));
        }
        return result;
    } catch (const std::exception& exception) {
        raise_host_error("workbench-buffer-ids", exception.what());
    } catch (...) {
        scm_misc_error("workbench-buffer-ids", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM project_list(SCM host_object) {
    try {
        HostLease& host = require_host(host_object, "project-list");
        const std::vector<ProjectId> projects = host.runtime->projects().all();
        SCM result = scm_c_make_vector(projects.size(), SCM_UNSPECIFIED);
        for (std::size_t index = 0; index < projects.size(); ++index) {
            const Project& project = host.runtime->projects().get(projects[index]);
            SCM summary = scm_c_make_vector(3, SCM_BOOL_F);
            scm_c_vector_set_x(summary, 0,
                               entity_id(projects[index].slot, projects[index].generation));
            scm_c_vector_set_x(summary, 1, scm_from_utf8_string(project.name().c_str()));
            if (!project.roots().empty()) {
                scm_c_vector_set_x(summary, 2,
                                   scm_from_utf8_string(project.roots().front().c_str()));
            }
            scm_c_vector_set_x(result, index, summary);
        }
        return result;
    } catch (const std::exception& exception) {
        raise_host_error("project-list", exception.what());
    } catch (...) {
        scm_misc_error("project-list", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM create_workbench(SCM host_object, SCM name_value, SCM project_value) {
    if (!scm_is_string(name_value)) {
        scm_wrong_type_arg_msg("new-workbench!", 2, name_value, "string");
    }
    HostLease& host = require_host(host_object, "new-workbench!");
    if (!host.services.create_workbench) {
        scm_misc_error("new-workbench!", "workbench creation capability is unavailable", SCM_EOL);
    }
    try {
        std::optional<ProjectId> project;
        if (!scheme_false(project_value)) {
            project = entity_id_from_scheme<ProjectTag>(project_value, "new-workbench!", 3);
        }
        const std::expected<WorkbenchId, std::string> created =
            host.services.create_workbench(scheme_string(name_value), project);
        if (!created) {
            raise_host_error("new-workbench!", created.error());
        }
        return entity_id(created->slot, created->generation);
    } catch (const std::exception& exception) {
        raise_host_error("new-workbench!", exception.what());
    } catch (...) {
        scm_misc_error("new-workbench!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM switch_workbench(SCM host_object, SCM workbench_value) {
    HostLease& host = require_host(host_object, "switch-workbench!");
    if (!host.services.switch_workbench) {
        scm_misc_error("switch-workbench!", "workbench switch capability is unavailable", SCM_EOL);
    }
    try {
        const WorkbenchId workbench =
            entity_id_from_scheme<WorkbenchTag>(workbench_value, "switch-workbench!", 2);
        const std::expected<void, std::string> switched = host.services.switch_workbench(workbench);
        if (!switched) {
            raise_host_error("switch-workbench!", switched.error());
        }
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("switch-workbench!", exception.what());
    } catch (...) {
        scm_misc_error("switch-workbench!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

SCM close_workbench(SCM host_object, SCM workbench_value) {
    HostLease& host = require_host(host_object, "close-workbench!");
    if (!host.services.close_workbench) {
        scm_misc_error("close-workbench!", "workbench close capability is unavailable", SCM_EOL);
    }
    try {
        const WorkbenchId workbench =
            entity_id_from_scheme<WorkbenchTag>(workbench_value, "close-workbench!", 2);
        const std::expected<void, std::string> closed = host.services.close_workbench(workbench);
        if (!closed) {
            raise_host_error("close-workbench!", closed.error());
        }
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("close-workbench!", exception.what());
    } catch (...) {
        scm_misc_error("close-workbench!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

SCM adopt_project(SCM host_object, SCM workbench_value, SCM project_value) {
    HostLease& host = require_host(host_object, "adopt-project!");
    if (!host.services.adopt_project) {
        scm_misc_error("adopt-project!", "workbench adoption capability is unavailable", SCM_EOL);
    }
    try {
        const WorkbenchId workbench =
            entity_id_from_scheme<WorkbenchTag>(workbench_value, "adopt-project!", 2);
        const ProjectId project =
            entity_id_from_scheme<ProjectTag>(project_value, "adopt-project!", 3);
        const std::expected<void, std::string> adopted =
            host.services.adopt_project(workbench, project);
        if (!adopted) {
            raise_host_error("adopt-project!", adopted.error());
        }
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("adopt-project!", exception.what());
    } catch (...) {
        scm_misc_error("adopt-project!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

SCM expel_buffer(SCM host_object, SCM workbench_value, SCM buffer_value) {
    HostLease& host = require_host(host_object, "expel-buffer!");
    if (!host.services.expel_buffer) {
        scm_misc_error("expel-buffer!", "workbench expulsion capability is unavailable", SCM_EOL);
    }
    try {
        const WorkbenchId workbench =
            entity_id_from_scheme<WorkbenchTag>(workbench_value, "expel-buffer!", 2);
        const BufferId buffer = entity_id_from_scheme<BufferTag>(buffer_value, "expel-buffer!", 3);
        const std::expected<void, std::string> expelled =
            host.services.expel_buffer(workbench, buffer);
        if (!expelled) {
            raise_host_error("expel-buffer!", expelled.error());
        }
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("expel-buffer!", exception.what());
    } catch (...) {
        scm_misc_error("expel-buffer!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

SCM workbench_session_state(SCM host_object) {
    HostLease& host = require_host(host_object, "workbench-session-state");
    if (!host.services.workbench_session_state) {
        scm_misc_error("workbench-session-state", "workbench session capability is unavailable",
                       SCM_EOL);
    }
    try {
        const std::string state = host.services.workbench_session_state();
        return scm_from_utf8_stringn(state.data(), state.size());
    } catch (const std::exception& exception) {
        raise_host_error("workbench-session-state", exception.what());
    } catch (...) {
        scm_misc_error("workbench-session-state", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM prepare_workbench_session_restore(SCM host_object, SCM state_value) {
    if (!scm_is_string(state_value)) {
        scm_wrong_type_arg_msg("prepare-workbench-session-restore!", 2, state_value, "string");
    }
    HostLease& host = require_host(host_object, "prepare-workbench-session-restore!");
    if (!host.services.prepare_workbench_session_restore) {
        scm_misc_error("prepare-workbench-session-restore!",
                       "workbench session preparation capability is unavailable", SCM_EOL);
    }
    try {
        const std::string state = scheme_string(state_value);
        std::expected<GuileWorkbenchRestorePlan, std::string> prepared =
            host.services.prepare_workbench_session_restore(state);
        if (!prepared) {
            raise_host_error("prepare-workbench-session-restore!", prepared.error());
        }
        SCM resources = scm_c_make_vector(prepared->resources.size(), SCM_UNSPECIFIED);
        for (std::size_t index = 0; index < prepared->resources.size(); ++index) {
            const GuileWorkbenchRestoreResource& source = prepared->resources[index];
            SCM targets = scm_c_make_vector(source.targets.size(), SCM_UNSPECIFIED);
            for (std::size_t target_index = 0; target_index < source.targets.size();
                 ++target_index) {
                const GuileWorkbenchRestoreTarget& source_target = source.targets[target_index];
                SCM target = scm_c_make_vector(3, SCM_UNSPECIFIED);
                scm_c_vector_set_x(target, 0, scm_from_utf8_symbol("target"));
                scm_c_vector_set_x(
                    target, 1,
                    entity_id(source_target.window.slot, source_target.window.generation));
                scm_c_vector_set_x(target, 2, scm_from_uint32(source_target.caret));
                scm_c_vector_set_x(targets, target_index, target);
            }
            SCM resource = scm_c_make_vector(3, SCM_UNSPECIFIED);
            scm_c_vector_set_x(resource, 0, scm_from_utf8_symbol("resource"));
            scm_c_vector_set_x(resource, 1, scm_from_utf8_string(source.resource.c_str()));
            scm_c_vector_set_x(resource, 2, targets);
            scm_c_vector_set_x(resources, index, resource);
        }
        SCM mru = scm_c_make_vector(prepared->mru.size(), SCM_UNSPECIFIED);
        for (std::size_t index = 0; index < prepared->mru.size(); ++index) {
            const GuileWorkbenchRestoreMru& source = prepared->mru[index];
            SCM resource_names = scm_c_make_vector(source.resources.size(), SCM_UNSPECIFIED);
            for (std::size_t resource_index = 0; resource_index < source.resources.size();
                 ++resource_index) {
                scm_c_vector_set_x(resource_names, resource_index,
                                   scm_from_utf8_string(source.resources[resource_index].c_str()));
            }
            SCM windows = scm_c_make_vector(source.windows.size(), SCM_UNSPECIFIED);
            for (std::size_t window_index = 0; window_index < source.windows.size();
                 ++window_index) {
                scm_c_vector_set_x(windows, window_index,
                                   entity_id(source.windows[window_index].slot,
                                             source.windows[window_index].generation));
            }
            SCM entry = scm_c_make_vector(4, SCM_UNSPECIFIED);
            scm_c_vector_set_x(entry, 0, scm_from_utf8_symbol("mru"));
            scm_c_vector_set_x(entry, 1,
                               entity_id(source.workbench.slot, source.workbench.generation));
            scm_c_vector_set_x(entry, 2, resource_names);
            scm_c_vector_set_x(entry, 3, windows);
            scm_c_vector_set_x(mru, index, entry);
        }
        SCM result = scm_c_make_vector(3, SCM_UNSPECIFIED);
        scm_c_vector_set_x(result, 0, scm_from_utf8_symbol("workbench-restore-plan"));
        scm_c_vector_set_x(result, 1, resources);
        scm_c_vector_set_x(result, 2, mru);
        return result;
    } catch (const std::exception& exception) {
        raise_host_error("prepare-workbench-session-restore!", exception.what());
    } catch (...) {
        scm_misc_error("prepare-workbench-session-restore!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM show_buffer_in_window(SCM host_object, SCM window_value, SCM buffer_value, SCM caret_value) {
    HostLease& host = require_host(host_object, "show-buffer-in-window!");
    if (!host.services.show_buffer_in_window) {
        scm_misc_error("show-buffer-in-window!", "window display capability is unavailable",
                       SCM_EOL);
    }
    const WindowId window =
        entity_id_from_scheme<WindowTag>(window_value, "show-buffer-in-window!", 2);
    const BufferId buffer =
        entity_id_from_scheme<BufferTag>(buffer_value, "show-buffer-in-window!", 3);
    if (scm_is_unsigned_integer(caret_value, 0, std::numeric_limits<std::uint32_t>::max()) == 0) {
        scm_wrong_type_arg_msg("show-buffer-in-window!", 4, caret_value,
                               "non-negative 32-bit integer");
    }
    try {
        const std::expected<void, std::string> shown =
            host.services.show_buffer_in_window(window, buffer, scm_to_uint32(caret_value));
        if (!shown) {
            raise_host_error("show-buffer-in-window!", shown.error());
        }
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("show-buffer-in-window!", exception.what());
    } catch (...) {
        scm_misc_error("show-buffer-in-window!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

SCM replace_workbench_mru(SCM host_object, SCM workbench_value, SCM buffers_value) {
    HostLease& host = require_host(host_object, "replace-workbench-mru!");
    if (!host.services.replace_workbench_mru) {
        scm_misc_error("replace-workbench-mru!", "workbench MRU capability is unavailable",
                       SCM_EOL);
    }
    const WorkbenchId workbench =
        entity_id_from_scheme<WorkbenchTag>(workbench_value, "replace-workbench-mru!", 2);
    if (!scm_is_vector(buffers_value)) {
        scm_wrong_type_arg_msg("replace-workbench-mru!", 3, buffers_value, "vector");
    }
    try {
        std::vector<BufferId> buffers;
        const std::size_t count = scm_c_vector_length(buffers_value);
        buffers.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            buffers.push_back(entity_id_from_scheme<BufferTag>(
                scm_c_vector_ref(buffers_value, index), "replace-workbench-mru!", 3));
        }
        const std::expected<void, std::string> replaced =
            host.services.replace_workbench_mru(workbench, buffers);
        if (!replaced) {
            raise_host_error("replace-workbench-mru!", replaced.error());
        }
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("replace-workbench-mru!", exception.what());
    } catch (...) {
        scm_misc_error("replace-workbench-mru!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

SCM window_buffer(SCM host_object, SCM window_value) {
    HostLease& host = require_host(host_object, "window-buffer-id");
    if (!host.services.window_buffer) {
        scm_misc_error("window-buffer-id", "window buffer capability is unavailable", SCM_EOL);
    }
    const WindowId window = entity_id_from_scheme<WindowTag>(window_value, "window-buffer-id", 2);
    try {
        const BufferId buffer = host.services.window_buffer(window);
        return entity_id(buffer.slot, buffer.generation);
    } catch (const std::exception& exception) {
        raise_host_error("window-buffer-id", exception.what());
    } catch (...) {
        scm_misc_error("window-buffer-id", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM owned_user_modules(SCM host_object) {
    try {
        HostLease& host = require_host(host_object, "owned-user-modules");
        const std::shared_ptr<GuileState> state = host.state;
        if (!state || !state->active) {
            scm_misc_error("owned-user-modules", "Guile runtime has expired", SCM_EOL);
        }
        const bool has_evaluation_module = state->evaluation_module_initialized;
        SCM result =
            scm_c_make_vector(state->extensions.size() + has_evaluation_module, SCM_UNSPECIFIED);
        for (std::size_t index = 0; index < state->extensions.size(); ++index) {
            SCM entry = scm_c_make_vector(2, SCM_UNSPECIFIED);
            scm_c_vector_set_x(entry, 0,
                               scm_from_utf8_string(state->extensions[index].path.c_str()));
            scm_c_vector_set_x(entry, 1, state->extensions[index].module);
            scm_c_vector_set_x(result, index, entry);
        }
        if (has_evaluation_module) {
            SCM entry = scm_c_make_vector(2, SCM_UNSPECIFIED);
            scm_c_vector_set_x(entry, 0, scm_from_utf8_string("*scheme-user*"));
            scm_c_vector_set_x(entry, 1, state->evaluation_module);
            scm_c_vector_set_x(result, state->extensions.size(), entry);
        }
        return result;
    } catch (const std::exception& exception) {
        scm_misc_error("owned-user-modules", exception.what(), SCM_EOL);
    } catch (...) {
        scm_misc_error("owned-user-modules", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

// The Guile ABI fixes two adjacent SCM arguments; their Scheme procedure name
// and validation preserve the semantic order.
SCM project_root(SCM host_object, SCM project_value) {
    try {
        HostLease& host = require_host(host_object, "project-root");
        const ProjectId project =
            entity_id_from_scheme<ProjectTag>(project_value, "project-root", 2);
        const Project& definition = host.runtime->projects().get(project);
        return definition.roots().empty()
                   ? SCM_BOOL_F
                   : scm_from_utf8_string(definition.roots().front().c_str());
    } catch (const std::exception& exception) {
        raise_host_error("project-root", exception.what());
    } catch (...) {
        scm_misc_error("project-root", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

// The Guile ABI fixes two adjacent SCM arguments; their Scheme procedure name
// and validation preserve the semantic order.
SCM project_files(SCM host_object, SCM project_value) {
    try {
        HostLease& host = require_host(host_object, "project-files");
        const ProjectId project =
            entity_id_from_scheme<ProjectTag>(project_value, "project-files", 2);
        const std::vector<std::string>& files = host.runtime->projects().get(project).files();
        SCM result = scm_c_make_vector(files.size(), SCM_UNSPECIFIED);
        for (std::size_t index = 0; index < files.size(); ++index) {
            scm_c_vector_set_x(result, index, scm_from_utf8_string(files[index].c_str()));
        }
        return result;
    } catch (const std::exception& exception) {
        raise_host_error("project-files", exception.what());
    } catch (...) {
        scm_misc_error("project-files", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

// The Guile ABI fixes three adjacent SCM arguments; their Scheme procedure
// name and validation preserve the semantic order.
SCM path_relative(SCM host_object, SCM path_value, SCM base_value) {
    if (!scm_is_string(path_value)) {
        scm_wrong_type_arg_msg("path-relative", 2, path_value, "string");
    }
    if (!scm_is_string(base_value)) {
        scm_wrong_type_arg_msg("path-relative", 3, base_value, "string");
    }
    (void)require_host(host_object, "path-relative");
    try {
        const std::string relative =
            std::filesystem::path(scheme_string(path_value))
                .lexically_relative(std::filesystem::path(scheme_string(base_value)))
                .string();
        return scm_from_utf8_string(relative.c_str());
    } catch (const std::exception& exception) {
        raise_host_error("path-relative", exception.what());
    } catch (...) {
        scm_misc_error("path-relative", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

// The Guile ABI fixes two adjacent SCM arguments; their Scheme procedure name
// and validation preserve the semantic order.
SCM path_filename(SCM host_object, SCM path_value) {
    if (!scm_is_string(path_value)) {
        scm_wrong_type_arg_msg("path-filename", 2, path_value, "string");
    }
    (void)require_host(host_object, "path-filename");
    try {
        const std::string filename =
            std::filesystem::path(scheme_string(path_value)).filename().string();
        return scm_from_utf8_string(filename.c_str());
    } catch (const std::exception& exception) {
        raise_host_error("path-filename", exception.what());
    } catch (...) {
        scm_misc_error("path-filename", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

// The Guile ABI fixes three adjacent SCM arguments; their Scheme procedure
// name and validation preserve the semantic order.
SCM path_resolve(SCM host_object, SCM path_value, SCM base_value) {
    if (!scm_is_string(path_value)) {
        scm_wrong_type_arg_msg("path-resolve", 2, path_value, "string");
    }
    if (!scm_is_string(base_value)) {
        scm_wrong_type_arg_msg("path-resolve", 3, base_value, "string");
    }
    (void)require_host(host_object, "path-resolve");
    try {
        std::filesystem::path path(scheme_string(path_value));
        if (path.is_relative()) {
            path = std::filesystem::path(scheme_string(base_value)) / path;
        }
        const std::string resolved = path.lexically_normal().string();
        return scm_from_utf8_string(resolved.c_str());
    } catch (const std::exception& exception) {
        raise_host_error("path-resolve", exception.what());
    } catch (...) {
        scm_misc_error("path-resolve", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM active_key_bindings(SCM host_object) {
    HostLease& host = require_host(host_object, "active-key-bindings");
    if (!host.services.active_key_bindings) {
        scm_misc_error("active-key-bindings", "key-binding snapshot capability is unavailable",
                       SCM_EOL);
    }
    try {
        const std::vector<GuileKeyBindingSummary> bindings = host.services.active_key_bindings();
        SCM result = scm_c_make_vector(bindings.size(), SCM_UNSPECIFIED);
        for (std::size_t index = 0; index < bindings.size(); ++index) {
            SCM binding = scm_c_make_vector(2, SCM_UNSPECIFIED);
            scm_c_vector_set_x(binding, 0, scm_from_utf8_string(bindings[index].keys.c_str()));
            scm_c_vector_set_x(binding, 1, scm_from_utf8_string(bindings[index].command.c_str()));
            scm_c_vector_set_x(result, index, binding);
        }
        return result;
    } catch (const std::exception& exception) {
        raise_host_error("active-key-bindings", exception.what());
    } catch (...) {
        scm_misc_error("active-key-bindings", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

TextOffset text_offset_from_scheme(SCM value, const char* caller, int position) {
    if (scm_is_unsigned_integer(value, 0, std::numeric_limits<std::uint32_t>::max()) == 0) {
        scm_wrong_type_arg_msg(caller, position, value, "unsigned 32-bit text offset");
    }
    return TextOffset{scm_to_uint32(value)};
}

SCM text_range_value(TextRange range) {
    SCM result = scm_c_make_vector(2, SCM_UNSPECIFIED);
    scm_c_vector_set_x(result, 0, scm_from_uint32(range.start.value));
    scm_c_vector_set_x(result, 1, scm_from_uint32(range.end.value));
    return result;
}

SCM selection_granularity_value(SelectionGranularity granularity) {
    switch (granularity) {
    case SelectionGranularity::Character:
        return scm_from_utf8_symbol("char");
    case SelectionGranularity::Line:
        return scm_from_utf8_symbol("line");
    case SelectionGranularity::Block:
        return scm_from_utf8_symbol("block");
    case SelectionGranularity::Node:
        return scm_from_utf8_symbol("node");
    }
    throw std::logic_error("unknown selection granularity");
}

SelectionGranularity selection_granularity_from_scheme(SCM value, const char* caller,
                                                       int position) {
    if (symbol_is(value, "char")) {
        return SelectionGranularity::Character;
    }
    if (symbol_is(value, "line")) {
        return SelectionGranularity::Line;
    }
    if (symbol_is(value, "block")) {
        return SelectionGranularity::Block;
    }
    if (symbol_is(value, "node")) {
        return SelectionGranularity::Node;
    }
    scm_wrong_type_arg_msg(caller, position, value, "char, line, block, or node symbol");
    return SelectionGranularity::Character;
}

std::string scheme_datum_external(SCM value) {
    return scheme_string(scm_object_to_string(value, SCM_UNDEFINED));
}

SCM view_selection_value(const ViewSelection& selection) {
    SCM ranges = scm_c_make_vector(selection.ranges.size(), SCM_UNSPECIFIED);
    for (std::size_t index = 0; index < selection.ranges.size(); ++index) {
        const SelectionRange& range = selection.ranges[index];
        SCM value = scm_c_make_vector(3, SCM_UNSPECIFIED);
        scm_c_vector_set_x(value, 0, scm_from_uint32(range.anchor.value));
        scm_c_vector_set_x(value, 1, scm_from_uint32(range.head.value));
        scm_c_vector_set_x(value, 2, selection_granularity_value(range.granularity));
        scm_c_vector_set_x(ranges, index, value);
    }
    SCM result = scm_c_make_vector(4, SCM_UNSPECIFIED);
    scm_c_vector_set_x(result, 0, scm_from_utf8_symbol("selection"));
    scm_c_vector_set_x(result, 1, scm_from_size_t(selection.primary));
    scm_c_vector_set_x(result, 2, scm_c_read_string(selection.metadata.c_str()));
    scm_c_vector_set_x(result, 3, ranges);
    return result;
}

ViewSelection view_selection_from_scheme(SCM value, const char* caller, int position) {
    if (!scm_is_vector(value) || scm_c_vector_length(value) != 4 ||
        !symbol_is(scm_c_vector_ref(value, 0), "selection") ||
        scm_is_unsigned_integer(scm_c_vector_ref(value, 1), 0,
                                std::numeric_limits<std::size_t>::max()) == 0 ||
        !scm_is_vector(scm_c_vector_ref(value, 3))) {
        scm_wrong_type_arg_msg(caller, position, value,
                               "#(selection primary metadata ranges) value");
    }
    const SCM ranges_value = scm_c_vector_ref(value, 3);
    const std::size_t range_count = scm_c_vector_length(ranges_value);
    ViewSelection selection{.ranges = {},
                            .primary = scm_to_size_t(scm_c_vector_ref(value, 1)),
                            .metadata = scheme_datum_external(scm_c_vector_ref(value, 2))};
    if (range_count == 0 || selection.primary >= range_count) {
        scm_misc_error(caller, "selection requires a non-empty valid primary range", SCM_EOL);
    }
    selection.ranges.reserve(range_count);
    for (std::size_t index = 0; index < range_count; ++index) {
        const SCM range = scm_c_vector_ref(ranges_value, index);
        if (!scm_is_vector(range) || scm_c_vector_length(range) != 3) {
            scm_wrong_type_arg_msg(caller, position, range, "#(anchor head granularity) range");
        }
        selection.ranges.push_back(
            {.anchor = text_offset_from_scheme(scm_c_vector_ref(range, 0), caller, position),
             .head = text_offset_from_scheme(scm_c_vector_ref(range, 1), caller, position),
             .granularity =
                 selection_granularity_from_scheme(scm_c_vector_ref(range, 2), caller, position)});
    }
    return selection;
}

SCM view_caret(SCM host_object, SCM view_value) {
    try {
        HostLease& host = require_host(host_object, "view-caret");
        const ViewId view = entity_id_from_scheme<ViewTag>(view_value, "view-caret", 2);
        return scm_from_uint32(host.runtime->views().caret(view).value);
    } catch (const std::exception& exception) {
        raise_host_error("view-caret", exception.what());
    } catch (...) {
        scm_misc_error("view-caret", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM view_mark(SCM host_object, SCM view_value) {
    try {
        HostLease& host = require_host(host_object, "view-mark");
        const ViewId view = entity_id_from_scheme<ViewTag>(view_value, "view-mark", 2);
        const std::optional<TextOffset> mark = host.runtime->views().mark(view);
        return mark ? scm_from_uint32(mark->value) : SCM_BOOL_F;
    } catch (const std::exception& exception) {
        raise_host_error("view-mark", exception.what());
    } catch (...) {
        scm_misc_error("view-mark", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM view_selection(SCM host_object, SCM view_value) {
    try {
        HostLease& host = require_host(host_object, "view-selection");
        const ViewId view = entity_id_from_scheme<ViewTag>(view_value, "view-selection", 2);
        return view_selection_value(host.runtime->views().selection_model(view));
    } catch (const std::exception& exception) {
        raise_host_error("view-selection", exception.what());
    } catch (...) {
        scm_misc_error("view-selection", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM set_selection(SCM host_object, SCM view_value, SCM selection_value) {
    HostLease& host = require_host(host_object, "set-selection!");
    const ViewId view = entity_id_from_scheme<ViewTag>(view_value, "set-selection!", 2);
    if (!host.services.set_selection) {
        scm_misc_error("set-selection!", "selection mutation capability is unavailable", SCM_EOL);
    }
    try {
        ViewSelection selection = view_selection_from_scheme(selection_value, "set-selection!", 3);
        host.services.set_selection(view, std::move(selection));
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("set-selection!", exception.what());
    } catch (...) {
        scm_misc_error("set-selection!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

SCM clear_selection(SCM host_object, SCM view_value) {
    HostLease& host = require_host(host_object, "clear-selection!");
    const ViewId view = entity_id_from_scheme<ViewTag>(view_value, "clear-selection!", 2);
    if (!host.services.clear_selection) {
        scm_misc_error("clear-selection!", "selection mutation capability is unavailable", SCM_EOL);
    }
    try {
        host.services.clear_selection(view);
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("clear-selection!", exception.what());
    } catch (...) {
        scm_misc_error("clear-selection!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

SCM push_selection_history(SCM host_object, SCM view_value, SCM selection_value) {
    try {
        HostLease& host = require_host(host_object, "push-selection-history!");
        const ViewId view =
            entity_id_from_scheme<ViewTag>(view_value, "push-selection-history!", 2);
        host.runtime->views().push_selection_history(
            view, view_selection_from_scheme(selection_value, "push-selection-history!", 3));
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("push-selection-history!", exception.what());
    } catch (...) {
        scm_misc_error("push-selection-history!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

SCM pop_selection_history(SCM host_object, SCM view_value) {
    try {
        HostLease& host = require_host(host_object, "pop-selection-history!");
        const ViewId view = entity_id_from_scheme<ViewTag>(view_value, "pop-selection-history!", 2);
        const std::optional<ViewSelection> selection =
            host.runtime->views().pop_selection_history(view);
        return selection ? view_selection_value(*selection) : SCM_BOOL_F;
    } catch (const std::exception& exception) {
        raise_host_error("pop-selection-history!", exception.what());
    } catch (...) {
        scm_misc_error("pop-selection-history!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM clear_selection_history(SCM host_object, SCM view_value) {
    try {
        HostLease& host = require_host(host_object, "clear-selection-history!");
        const ViewId view =
            entity_id_from_scheme<ViewTag>(view_value, "clear-selection-history!", 2);
        host.runtime->views().clear_selection_history(view);
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("clear-selection-history!", exception.what());
    } catch (...) {
        scm_misc_error("clear-selection-history!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

SCM selection_history_depth(SCM host_object, SCM view_value) {
    try {
        HostLease& host = require_host(host_object, "selection-history-depth");
        const ViewId view =
            entity_id_from_scheme<ViewTag>(view_value, "selection-history-depth", 2);
        return scm_from_size_t(host.runtime->views().selection_history_size(view));
    } catch (const std::exception& exception) {
        raise_host_error("selection-history-depth", exception.what());
    } catch (...) {
        scm_misc_error("selection-history-depth", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

std::vector<std::string> selection_replacements_from_scheme(SCM value, std::size_t range_count,
                                                            const char* caller, int position) {
    if (scm_is_string(value)) {
        return std::vector<std::string>(range_count, scheme_string(value));
    }
    if (!scm_is_vector(value) || scm_c_vector_length(value) != range_count) {
        scm_wrong_type_arg_msg(caller, position, value,
                               "replacement string or one-string-per-range vector");
    }
    std::vector<std::string> replacements;
    replacements.reserve(range_count);
    for (std::size_t index = 0; index < range_count; ++index) {
        const SCM replacement = scm_c_vector_ref(value, index);
        if (!scm_is_string(replacement)) {
            scm_wrong_type_arg_msg(caller, position, replacement, "replacement string");
        }
        replacements.push_back(scheme_string(replacement));
    }
    return replacements;
}

std::vector<std::string> insertion_texts_from_scheme(SCM value, const char* caller, int position) {
    if (scm_is_string(value)) {
        return {scheme_string(value)};
    }
    if (!scm_is_vector(value) || scm_c_vector_length(value) == 0) {
        scm_wrong_type_arg_msg(caller, position, value,
                               "insertion string or non-empty vector of strings");
    }
    const std::size_t count = scm_c_vector_length(value);
    std::vector<std::string> texts;
    texts.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const SCM text = scm_c_vector_ref(value, index);
        if (!scm_is_string(text)) {
            scm_wrong_type_arg_msg(caller, position, text, "insertion string");
        }
        texts.push_back(scheme_string(text));
    }
    return texts;
}

SCM string_vector_value(const std::vector<std::string>& values) {
    SCM result = scm_c_make_vector(values.size(), SCM_UNSPECIFIED);
    for (std::size_t index = 0; index < values.size(); ++index) {
        scm_c_vector_set_x(result, index,
                           scm_from_utf8_stringn(values[index].data(), values[index].size()));
    }
    return result;
}

// The Guile ABI fixes four adjacent SCM arguments; their Scheme procedure
// name and validation preserve the semantic order.
SCM replace_selection(SCM host_object, SCM view_value, SCM selection_value,
                      SCM replacements_value) {
    HostLease& host = require_host(host_object, "replace-selection!");
    const ViewId view = entity_id_from_scheme<ViewTag>(view_value, "replace-selection!", 2);
    if (!host.services.replace_selection) {
        scm_misc_error("replace-selection!", "selection edit capability is unavailable", SCM_EOL);
    }
    try {
        ViewSelection selection =
            view_selection_from_scheme(selection_value, "replace-selection!", 3);
        std::vector<std::string> replacements = selection_replacements_from_scheme(
            replacements_value, selection.ranges.size(), "replace-selection!", 4);
        std::expected<ViewSelection, std::string> result =
            host.services.replace_selection(view, std::move(selection), std::move(replacements));
        if (!result) {
            raise_host_error("replace-selection!", result.error());
        }
        return view_selection_value(*result);
    } catch (const std::exception& exception) {
        raise_host_error("replace-selection!", exception.what());
    } catch (...) {
        scm_misc_error("replace-selection!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM selection_texts(SCM host_object, SCM view_value, SCM selection_value) {
    HostLease& host = require_host(host_object, "selection-texts");
    const ViewId view = entity_id_from_scheme<ViewTag>(view_value, "selection-texts", 2);
    if (!host.services.selection_texts) {
        scm_misc_error("selection-texts", "selection extraction capability is unavailable",
                       SCM_EOL);
    }
    try {
        ViewSelection selection = view_selection_from_scheme(selection_value, "selection-texts", 3);
        std::expected<std::vector<std::string>, std::string> result =
            host.services.selection_texts(view, selection);
        if (!result) {
            raise_host_error("selection-texts", result.error());
        }
        return string_vector_value(*result);
    } catch (const std::exception& exception) {
        raise_host_error("selection-texts", exception.what());
    } catch (...) {
        scm_misc_error("selection-texts", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

// The Guile ABI fixes four adjacent SCM arguments; their Scheme procedure
// name and validation preserve the semantic order.
SCM buffer_substring(SCM host_object, SCM buffer_value, SCM start_value, SCM end_value) {
    try {
        HostLease& host = require_host(host_object, "buffer-substring");
        const BufferId buffer =
            entity_id_from_scheme<BufferTag>(buffer_value, "buffer-substring", 2);
        const TextOffset start = text_offset_from_scheme(start_value, "buffer-substring", 3);
        const TextOffset end = text_offset_from_scheme(end_value, "buffer-substring", 4);
        if (end < start) {
            scm_misc_error("buffer-substring", "range end precedes range start", SCM_EOL);
        }
        const std::string text =
            host.runtime->buffers().get(buffer).snapshot().substring(TextRange{start, end});
        return scm_from_utf8_stringn(text.data(), text.size());
    } catch (const std::exception& exception) {
        raise_host_error("buffer-substring", exception.what());
    } catch (...) {
        scm_misc_error("buffer-substring", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

// The low-level text search primitive operates on UTF-8 bytes so its ranges
// share the same coordinate space as Buffer, View, and edit transactions.
SCM find_buffer_text(SCM host_object, SCM buffer_value, SCM query_value, SCM start_value,
                     SCM direction_value) {
    if (!scm_is_string(query_value)) {
        scm_wrong_type_arg_msg("find-buffer-text", 3, query_value, "string");
    }
    try {
        HostLease& host = require_host(host_object, "find-buffer-text");
        const BufferId buffer =
            entity_id_from_scheme<BufferTag>(buffer_value, "find-buffer-text", 2);
        const TextOffset start = text_offset_from_scheme(start_value, "find-buffer-text", 4);
        const std::string query = scheme_string(query_value);
        const std::string text =
            host.runtime->buffers().get(buffer).snapshot().content().to_string();
        if (start.value > text.size()) {
            scm_out_of_range("find-buffer-text", start_value);
        }

        std::size_t found = std::string::npos;
        if (symbol_is(direction_value, "forward")) {
            found = text.find(query, start.value);
        } else if (symbol_is(direction_value, "backward")) {
            found = text.rfind(query, start.value);
        } else {
            scm_wrong_type_arg_msg("find-buffer-text", 5, direction_value, "'forward or 'backward");
        }
        if (found == std::string::npos) {
            return SCM_BOOL_F;
        }
        return text_range_value(
            TextRange{TextOffset{static_cast<std::uint32_t>(found)},
                      TextOffset{static_cast<std::uint32_t>(found + query.size())}});
    } catch (const std::exception& exception) {
        raise_host_error("find-buffer-text", exception.what());
    } catch (...) {
        scm_misc_error("find-buffer-text", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

// The Guile ABI fixes four adjacent SCM arguments; their Scheme procedure
// name and validation preserve the semantic order.
SCM erase_range(SCM host_object, SCM view_value, SCM start_value, SCM end_value) {
    HostLease& host = require_host(host_object, "erase-range!");
    const ViewId view = entity_id_from_scheme<ViewTag>(view_value, "erase-range!", 2);
    const TextOffset start = text_offset_from_scheme(start_value, "erase-range!", 3);
    const TextOffset end = text_offset_from_scheme(end_value, "erase-range!", 4);
    if (end < start) {
        scm_misc_error("erase-range!", "range end precedes range start", SCM_EOL);
    }
    if (!host.services.erase_range) {
        scm_misc_error("erase-range!", "text mutation capability is unavailable", SCM_EOL);
    }
    try {
        const std::expected<void, std::string> erased =
            host.services.erase_range(view, GuileTextRange{start.value, end.value});
        if (!erased) {
            raise_host_error("erase-range!", erased.error());
        }
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("erase-range!", exception.what());
    } catch (...) {
        scm_misc_error("erase-range!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

// The Guile ABI fixes three adjacent SCM arguments; their Scheme procedure
// name and validation preserve the semantic order.
SCM insert_text(SCM host_object, SCM view_value, SCM text_value) {
    HostLease& host = require_host(host_object, "insert-text!");
    const ViewId view = entity_id_from_scheme<ViewTag>(view_value, "insert-text!", 2);
    if (!host.services.insert_text) {
        scm_misc_error("insert-text!", "text mutation capability is unavailable", SCM_EOL);
    }
    try {
        const std::expected<void, std::string> inserted = host.services.insert_text(
            view, insertion_texts_from_scheme(text_value, "insert-text!", 3));
        if (!inserted) {
            raise_host_error("insert-text!", inserted.error());
        }
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("insert-text!", exception.what());
    } catch (...) {
        scm_misc_error("insert-text!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

SCM soft_kill_range(SCM host_object, SCM view_value, SCM mode_value) {
    HostLease& host = require_host(host_object, "soft-kill-range");
    const ViewId view = entity_id_from_scheme<ViewTag>(view_value, "soft-kill-range", 2);
    if (!host.services.soft_kill_range) {
        scm_misc_error("soft-kill-range", "kill range capability is unavailable", SCM_EOL);
    }
    const bool structural = symbol_is(mode_value, "structural");
    if (!structural && !symbol_is(mode_value, "plain")) {
        scm_wrong_type_arg_msg("soft-kill-range", 3, mode_value, "'structural or 'plain");
    }
    try {
        const std::expected<std::optional<GuileTextRange>, std::string> range =
            host.services.soft_kill_range(view, structural);
        if (!range) {
            raise_host_error("soft-kill-range", range.error());
        }
        return *range ? text_range_value(
                            TextRange{TextOffset{(*range)->start}, TextOffset{(*range)->end}})
                      : SCM_BOOL_F;
    } catch (const std::exception& exception) {
        raise_host_error("soft-kill-range", exception.what());
    } catch (...) {
        scm_misc_error("soft-kill-range", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

// The Guile ABI fixes three adjacent SCM arguments; their Scheme procedure
// name and validation preserve the semantic order.
SCM set_view_caret(SCM host_object, SCM view_value, SCM offset_value) {
    try {
        HostLease& host = require_host(host_object, "set-view-caret!");
        const ViewId view = entity_id_from_scheme<ViewTag>(view_value, "set-view-caret!", 2);
        const TextOffset offset = text_offset_from_scheme(offset_value, "set-view-caret!", 3);
        if (host.services.set_view_caret) {
            host.services.set_view_caret(view, offset.value);
        } else {
            host.runtime->views().set_caret(view, offset);
        }
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("set-view-caret!", exception.what());
    } catch (...) {
        scm_misc_error("set-view-caret!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

SCM reset_preferred_column(SCM host_object, SCM view_value) {
    try {
        HostLease& host = require_host(host_object, "reset-preferred-column!");
        const ViewId view =
            entity_id_from_scheme<ViewTag>(view_value, "reset-preferred-column!", 2);
        host.runtime->views().get(view).viewport().preferred_column.reset();
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("reset-preferred-column!", exception.what());
    } catch (...) {
        scm_misc_error("reset-preferred-column!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

SCM thing_selection(SCM host_object, SCM view_value, SCM selection_value, SCM thing_value,
                    SCM extent_value) {
    HostLease& host = require_host(host_object, "thing-selection");
    const ViewId view = entity_id_from_scheme<ViewTag>(view_value, "thing-selection", 2);
    const ViewSelection source = view_selection_from_scheme(selection_value, "thing-selection", 3);
    const std::string name = scheme_name(thing_value, "thing-selection", 4);
    const bool bounds = symbol_is(extent_value, "bounds");
    if (!bounds && !symbol_is(extent_value, "inner")) {
        scm_wrong_type_arg_msg("thing-selection", 5, extent_value, "'inner or 'bounds");
    }
    if (!host.services.thing_selection) {
        scm_misc_error("thing-selection", "thing query capability is unavailable", SCM_EOL);
    }
    try {
        const std::expected<std::optional<ViewSelection>, std::string> selected =
            host.services.thing_selection(view, source, name, bounds);
        if (!selected) {
            raise_host_error("thing-selection", selected.error());
        }
        return *selected ? view_selection_value(**selected) : SCM_BOOL_F;
    } catch (const std::exception& exception) {
        raise_host_error("thing-selection", exception.what());
    } catch (...) {
        scm_misc_error("thing-selection", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM motion_selection(SCM host_object, SCM view_value, SCM selection_value, SCM motion_value,
                     SCM count_value, SCM extend_value) {
    HostLease& host = require_host(host_object, "motion-selection");
    const ViewId view = entity_id_from_scheme<ViewTag>(view_value, "motion-selection", 2);
    const ViewSelection source = view_selection_from_scheme(selection_value, "motion-selection", 3);
    const std::string name = scheme_name(motion_value, "motion-selection", 4);
    if (scm_is_signed_integer(count_value, std::numeric_limits<std::int64_t>::min(),
                              std::numeric_limits<std::int64_t>::max()) == 0) {
        scm_wrong_type_arg_msg("motion-selection", 5, count_value, "signed 64-bit integer");
    }
    if (!scheme_boolean(extend_value)) {
        scm_wrong_type_arg_msg("motion-selection", 6, extend_value, "boolean");
    }
    if (!host.services.motion_selection) {
        scm_misc_error("motion-selection", "motion query capability is unavailable", SCM_EOL);
    }
    try {
        const std::expected<ViewSelection, std::string> selected = host.services.motion_selection(
            view, source, name, scm_to_int64(count_value), scheme_true(extend_value));
        if (!selected) {
            raise_host_error("motion-selection", selected.error());
        }
        return view_selection_value(*selected);
    } catch (const std::exception& exception) {
        raise_host_error("motion-selection", exception.what());
    } catch (...) {
        scm_misc_error("motion-selection", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM expand_node_selection(SCM host_object, SCM view_value, SCM selection_value) {
    HostLease& host = require_host(host_object, "expand-node-selection");
    const ViewId view = entity_id_from_scheme<ViewTag>(view_value, "expand-node-selection", 2);
    const ViewSelection source =
        view_selection_from_scheme(selection_value, "expand-node-selection", 3);
    if (!host.services.expand_selection) {
        scm_misc_error("expand-node-selection", "node expansion capability is unavailable",
                       SCM_EOL);
    }
    try {
        const std::expected<std::optional<ViewSelection>, std::string> selected =
            host.services.expand_selection(view, source);
        if (!selected) {
            raise_host_error("expand-node-selection", selected.error());
        }
        return *selected ? view_selection_value(**selected) : SCM_BOOL_F;
    } catch (const std::exception& exception) {
        raise_host_error("expand-node-selection", exception.what());
    } catch (...) {
        scm_misc_error("expand-node-selection", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM write_clipboard(SCM host_object, SCM text_value) {
    if (!scm_is_string(text_value)) {
        scm_wrong_type_arg_msg("write-clipboard!", 2, text_value, "string");
    }
    HostLease& host = require_host(host_object, "write-clipboard!");
    if (!host.services.write_clipboard) {
        return SCM_BOOL_F;
    }
    try {
        const std::expected<void, std::string> written =
            host.services.write_clipboard(scheme_string(text_value));
        return written ? SCM_BOOL_F : scm_from_utf8_string(written.error().c_str());
    } catch (const std::exception& exception) {
        raise_host_error("write-clipboard!", exception.what());
    } catch (...) {
        scm_misc_error("write-clipboard!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM read_clipboard(SCM host_object) {
    HostLease& host = require_host(host_object, "read-clipboard");
    SCM result = scm_c_make_vector(2, SCM_BOOL_F);
    if (!host.services.read_clipboard) {
        return result;
    }
    try {
        const std::expected<std::optional<std::string>, std::string> read =
            host.services.read_clipboard();
        if (!read) {
            scm_c_vector_set_x(result, 1, scm_from_utf8_string(read.error().c_str()));
        } else if (*read) {
            scm_c_vector_set_x(result, 0, scm_from_utf8_stringn((*read)->data(), (*read)->size()));
        }
        return result;
    } catch (const std::exception& exception) {
        raise_host_error("read-clipboard", exception.what());
    } catch (...) {
        scm_misc_error("read-clipboard", "unknown C++ host failure", SCM_EOL);
    }
    return result;
}

// The Guile ABI fixes two adjacent SCM arguments; their Scheme procedure name
// and validation preserve the semantic order.
SCM buffer_id_by_name(SCM host_object, SCM name_value) {
    if (!scm_is_string(name_value)) {
        scm_wrong_type_arg_msg("buffer-id-by-name", 2, name_value, "string");
    }
    try {
        HostLease& host = require_host(host_object, "buffer-id-by-name");
        const std::optional<BufferId> buffer =
            host.runtime->buffers().find_by_name(scheme_string(name_value));
        return buffer ? entity_id(buffer->slot, buffer->generation) : SCM_BOOL_F;
    } catch (const std::exception& exception) {
        raise_host_error("buffer-id-by-name", exception.what());
    } catch (...) {
        scm_misc_error("buffer-id-by-name", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

// The Guile ABI fixes two adjacent SCM arguments; their Scheme procedure name
// and validation preserve the semantic order.
SCM buffer_resource(SCM host_object, SCM buffer_value) {
    try {
        HostLease& host = require_host(host_object, "buffer-resource");
        const BufferId buffer =
            entity_id_from_scheme<BufferTag>(buffer_value, "buffer-resource", 2);
        const std::optional<std::string>& resource =
            host.runtime->buffers().get(buffer).resource_uri();
        return resource ? scm_from_utf8_string(resource->c_str()) : SCM_BOOL_F;
    } catch (const std::exception& exception) {
        raise_host_error("buffer-resource", exception.what());
    } catch (...) {
        scm_misc_error("buffer-resource", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

// The Guile ABI fixes two adjacent SCM arguments; the Scheme-level name and
// validation preserve their semantic order.
SCM buffer_name(SCM host_object, SCM buffer_value) {
    try {
        HostLease& host = require_host(host_object, "buffer-name");
        const BufferId buffer = entity_id_from_scheme<BufferTag>(buffer_value, "buffer-name", 2);
        const std::string& name = host.runtime->buffers().get(buffer).name();
        return scm_from_utf8_stringn(name.data(), name.size());
    } catch (const std::exception& exception) {
        raise_host_error("buffer-name", exception.what());
    } catch (...) {
        scm_misc_error("buffer-name", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

// The Guile ABI fixes two adjacent SCM arguments; the Scheme-level name and
// validation preserve their semantic order.
SCM buffer_text(SCM host_object, SCM buffer_value) {
    try {
        HostLease& host = require_host(host_object, "buffer-text");
        const BufferId buffer = entity_id_from_scheme<BufferTag>(buffer_value, "buffer-text", 2);
        const std::string text =
            host.runtime->buffers().get(buffer).snapshot().content().to_string();
        return scm_from_utf8_stringn(text.data(), text.size());
    } catch (const std::exception& exception) {
        raise_host_error("buffer-text", exception.what());
    } catch (...) {
        scm_misc_error("buffer-text", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM buffer_byte_size(SCM host_object, SCM buffer_value) {
    try {
        HostLease& host = require_host(host_object, "buffer-byte-size");
        const BufferId buffer =
            entity_id_from_scheme<BufferTag>(buffer_value, "buffer-byte-size", 2);
        return scm_from_uint32(
            host.runtime->buffers().get(buffer).snapshot().content().size_bytes());
    } catch (const std::exception& exception) {
        raise_host_error("buffer-byte-size", exception.what());
    } catch (...) {
        scm_misc_error("buffer-byte-size", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM buffer_editable_start(SCM host_object, SCM buffer_value) {
    try {
        HostLease& host = require_host(host_object, "buffer-editable-start");
        const BufferId buffer =
            entity_id_from_scheme<BufferTag>(buffer_value, "buffer-editable-start", 2);
        const std::optional<TextOffset> start =
            host.runtime->buffers().get(buffer).editable_start();
        return start ? scm_from_uint32(start->value) : SCM_BOOL_F;
    } catch (const std::exception& exception) {
        raise_host_error("buffer-editable-start", exception.what());
    } catch (...) {
        scm_misc_error("buffer-editable-start", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM set_buffer_editable_start(SCM host_object, SCM buffer_value, SCM offset_value) {
    try {
        HostLease& host = require_host(host_object, "set-buffer-editable-start!");
        const BufferId buffer =
            entity_id_from_scheme<BufferTag>(buffer_value, "set-buffer-editable-start!", 2);
        std::optional<TextOffset> start;
        if (!scheme_false(offset_value)) {
            start = text_offset_from_scheme(offset_value, "set-buffer-editable-start!", 3);
        }
        host.runtime->buffers().get(buffer).set_editable_start(start);
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("set-buffer-editable-start!", exception.what());
    } catch (...) {
        scm_misc_error("set-buffer-editable-start!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

SCM create_buffer_marker(SCM host_object, SCM buffer_value, SCM offset_value, SCM affinity_value) {
    try {
        HostLease& host = require_host(host_object, "create-buffer-marker!");
        const BufferId buffer =
            entity_id_from_scheme<BufferTag>(buffer_value, "create-buffer-marker!", 2);
        const TextOffset offset = text_offset_from_scheme(offset_value, "create-buffer-marker!", 3);
        const std::string affinity = scheme_name(affinity_value, "create-buffer-marker!", 4);
        if (affinity != "before" && affinity != "after") {
            scm_misc_error("create-buffer-marker!", "marker affinity must be before or after",
                           SCM_EOL);
        }
        const AnchorId marker = host.runtime->buffers().get(buffer).create_navigation_anchor(
            offset, affinity == "before" ? AnchorAffinity::BeforeInsertion
                                         : AnchorAffinity::AfterInsertion);
        return scm_from_uint32(marker);
    } catch (const std::exception& exception) {
        raise_host_error("create-buffer-marker!", exception.what());
    } catch (...) {
        scm_misc_error("create-buffer-marker!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM buffer_marker_offset(SCM host_object, SCM buffer_value, SCM marker_value) {
    try {
        HostLease& host = require_host(host_object, "buffer-marker-offset");
        const BufferId buffer =
            entity_id_from_scheme<BufferTag>(buffer_value, "buffer-marker-offset", 2);
        if (scm_is_unsigned_integer(marker_value, 0, std::numeric_limits<std::uint32_t>::max()) ==
            0) {
            scm_wrong_type_arg_msg("buffer-marker-offset", 3, marker_value,
                                   "unsigned 32-bit marker ID");
        }
        const TextOffset offset = host.runtime->buffers().get(buffer).navigation_anchor_offset(
            scm_to_uint32(marker_value));
        return scm_from_uint32(offset.value);
    } catch (const std::exception& exception) {
        raise_host_error("buffer-marker-offset", exception.what());
    } catch (...) {
        scm_misc_error("buffer-marker-offset", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM remove_buffer_marker(SCM host_object, SCM buffer_value, SCM marker_value) {
    try {
        HostLease& host = require_host(host_object, "remove-buffer-marker!");
        const BufferId buffer =
            entity_id_from_scheme<BufferTag>(buffer_value, "remove-buffer-marker!", 2);
        if (scm_is_unsigned_integer(marker_value, 0, std::numeric_limits<std::uint32_t>::max()) ==
            0) {
            scm_wrong_type_arg_msg("remove-buffer-marker!", 3, marker_value,
                                   "unsigned 32-bit marker ID");
        }
        host.runtime->buffers().get(buffer).remove_navigation_anchor(scm_to_uint32(marker_value));
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("remove-buffer-marker!", exception.what());
    } catch (...) {
        scm_misc_error("remove-buffer-marker!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

// Returns #(source-start source-end resource target-line target-column excerpt encoding) entries.
SCM buffer_locations(SCM host_object, SCM buffer_value) {
    try {
        HostLease& host = require_host(host_object, "buffer-locations");
        const BufferId buffer =
            entity_id_from_scheme<BufferTag>(buffer_value, "buffer-locations", 2);
        const std::vector<BufferLocation>& locations =
            host.runtime->buffers().get(buffer).locations();
        SCM result = scm_c_make_vector(locations.size(), SCM_UNSPECIFIED);
        for (std::size_t index = 0; index < locations.size(); ++index) {
            const BufferLocation& location = locations[index];
            SCM entry = scm_c_make_vector(7, SCM_UNSPECIFIED);
            scm_c_vector_set_x(entry, 0, scm_from_uint32(location.source_range.start.value));
            scm_c_vector_set_x(entry, 1, scm_from_uint32(location.source_range.end.value));
            scm_c_vector_set_x(entry, 2, scm_from_utf8_string(location.resource.c_str()));
            scm_c_vector_set_x(entry, 3, scm_from_uint32(location.target.line));
            scm_c_vector_set_x(entry, 4, scm_from_uint32(location.target.column));
            scm_c_vector_set_x(
                entry, 5, scm_from_utf8_stringn(location.excerpt.data(), location.excerpt.size()));
            scm_c_vector_set_x(entry, 6, position_encoding_value(location.target.encoding));
            scm_c_vector_set_x(result, index, entry);
        }
        return result;
    } catch (const std::exception& exception) {
        raise_host_error("buffer-locations", exception.what());
    } catch (...) {
        scm_misc_error("buffer-locations", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM diagnostic_severity_value(DiagnosticSeverity severity) {
    switch (severity) {
    case DiagnosticSeverity::Error:
        return name_symbol("error");
    case DiagnosticSeverity::Warning:
        return name_symbol("warning");
    case DiagnosticSeverity::Information:
        return name_symbol("information");
    case DiagnosticSeverity::Hint:
        return name_symbol("hint");
    }
    return name_symbol("error");
}

// Returns #(start-line start-column end-line end-column severity source code message) entries.
SCM buffer_diagnostics(SCM host_object, SCM buffer_value) {
    try {
        HostLease& host = require_host(host_object, "buffer-diagnostics");
        const BufferId buffer =
            entity_id_from_scheme<BufferTag>(buffer_value, "buffer-diagnostics", 2);
        const Buffer& source = host.runtime->buffers().get(buffer);
        const DocumentSnapshot snapshot = source.snapshot();
        const Text& text = snapshot.content();
        const std::vector<Diagnostic> diagnostics = source.diagnostics();
        SCM result = scm_c_make_vector(diagnostics.size(), SCM_UNSPECIFIED);
        for (std::size_t index = 0; index < diagnostics.size(); ++index) {
            const Diagnostic& diagnostic = diagnostics[index];
            const LinePosition start = text.position(diagnostic.range.start);
            const LinePosition end = text.position(diagnostic.range.end);
            SCM entry = scm_c_make_vector(8, SCM_UNSPECIFIED);
            scm_c_vector_set_x(entry, 0, scm_from_uint32(start.line));
            scm_c_vector_set_x(entry, 1, scm_from_uint32(start.byte_column));
            scm_c_vector_set_x(entry, 2, scm_from_uint32(end.line));
            scm_c_vector_set_x(entry, 3, scm_from_uint32(end.byte_column));
            scm_c_vector_set_x(entry, 4, diagnostic_severity_value(diagnostic.severity));
            scm_c_vector_set_x(entry, 5, scm_from_utf8_string(diagnostic.source.c_str()));
            scm_c_vector_set_x(entry, 6, scm_from_utf8_string(diagnostic.code.c_str()));
            scm_c_vector_set_x(
                entry, 7,
                scm_from_utf8_stringn(diagnostic.message.data(), diagnostic.message.size()));
            scm_c_vector_set_x(result, index, entry);
        }
        return result;
    } catch (const std::exception& exception) {
        raise_host_error("buffer-diagnostics", exception.what());
    } catch (...) {
        scm_misc_error("buffer-diagnostics", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

std::vector<BufferLocation> locations_from_scheme(SCM locations_value, const char* caller,
                                                  int position) {
    if (!scm_is_vector(locations_value)) {
        scm_wrong_type_arg_msg(caller, position, locations_value, "location vector");
    }
    const std::size_t count = scm_c_vector_length(locations_value);
    std::vector<BufferLocation> locations;
    locations.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const SCM location = scm_c_vector_ref(locations_value, index);
        const std::size_t fields = scm_is_vector(location) ? scm_c_vector_length(location) : 0;
        if ((fields != 5 && fields != 6 && fields != 7) ||
            scm_is_unsigned_integer(scm_c_vector_ref(location, 0), 0,
                                    std::numeric_limits<std::uint32_t>::max()) == 0 ||
            scm_is_unsigned_integer(scm_c_vector_ref(location, 1), 0,
                                    std::numeric_limits<std::uint32_t>::max()) == 0 ||
            !scm_is_string(scm_c_vector_ref(location, 2)) ||
            scm_is_unsigned_integer(scm_c_vector_ref(location, 3), 0,
                                    std::numeric_limits<std::uint32_t>::max()) == 0 ||
            scm_is_unsigned_integer(scm_c_vector_ref(location, 4), 0,
                                    std::numeric_limits<std::uint32_t>::max()) == 0 ||
            (fields >= 6 && !scm_is_string(scm_c_vector_ref(location, 5)))) {
            scm_wrong_type_arg_msg(
                caller, position, location,
                "#(source-start source-end resource line column [excerpt [encoding]])");
        }
        const TextRange source{
            TextOffset{scm_to_uint32(scm_c_vector_ref(location, 0))},
            TextOffset{scm_to_uint32(scm_c_vector_ref(location, 1))},
        };
        if (source.end < source.start) {
            scm_misc_error(caller, "location source range is reversed", SCM_EOL);
        }
        locations.push_back(
            {.source_range = source,
             .resource = scheme_string(scm_c_vector_ref(location, 2)),
             .target = {.line = scm_to_uint32(scm_c_vector_ref(location, 3)),
                        .column = scm_to_uint32(scm_c_vector_ref(location, 4)),
                        .encoding = fields == 7
                                        ? position_encoding_from_scheme(
                                              scm_c_vector_ref(location, 6), caller, position)
                                        : PositionEncoding::Bytes},
             .excerpt = fields >= 6 ? scheme_string_with_nuls(scm_c_vector_ref(location, 5))
                                    : std::string()});
    }
    return locations;
}

SCM set_buffer_locations(SCM host_object, SCM buffer_value, SCM locations_value) {
    HostLease& host = require_host(host_object, "set-buffer-locations!");
    const BufferId buffer =
        entity_id_from_scheme<BufferTag>(buffer_value, "set-buffer-locations!", 2);
    try {
        host.runtime->buffers().set_locations(
            buffer, locations_from_scheme(locations_value, "set-buffer-locations!", 3));
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("set-buffer-locations!", exception.what());
    } catch (...) {
        scm_misc_error("set-buffer-locations!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

SCM set_location_list(SCM host_object, SCM window_value, SCM buffer_value, SCM source_value,
                      SCM locations_value) {
    HostLease& host = require_host(host_object, "set-location-list!");
    const WindowId window = entity_id_from_scheme<WindowTag>(window_value, "set-location-list!", 2);
    const BufferId buffer = entity_id_from_scheme<BufferTag>(buffer_value, "set-location-list!", 3);
    const std::string source = scheme_name(source_value, "set-location-list!", 4);
    if (!host.services.publish_location_list) {
        scm_misc_error("set-location-list!", "location list capability is unavailable", SCM_EOL);
    }
    try {
        std::expected<void, std::string> published = host.services.publish_location_list(
            window, buffer, source,
            locations_from_scheme(locations_value, "set-location-list!", 5));
        if (!published) {
            raise_host_error("set-location-list!", published.error());
        }
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("set-location-list!", exception.what());
    } catch (...) {
        scm_misc_error("set-location-list!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

SCM buffer_read_only_p(SCM host_object, SCM buffer_value) {
    try {
        HostLease& host = require_host(host_object, "buffer-read-only?");
        const BufferId buffer =
            entity_id_from_scheme<BufferTag>(buffer_value, "buffer-read-only?", 2);
        return scm_from_bool(host.runtime->buffers().get(buffer).read_only());
    } catch (const std::exception& exception) {
        raise_host_error("buffer-read-only?", exception.what());
    } catch (...) {
        scm_misc_error("buffer-read-only?", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

// The Guile ABI fixes two adjacent SCM arguments; their Scheme procedure name
// and validation preserve the semantic order.
SCM path_parent(SCM host_object, SCM path_value) {
    if (!scm_is_string(path_value)) {
        scm_wrong_type_arg_msg("path-parent", 2, path_value, "string");
    }
    (void)require_host(host_object, "path-parent");
    try {
        std::filesystem::path path(scheme_string(path_value));
        if (path != path.root_path() && path.filename().empty()) {
            path = path.parent_path();
        }
        const std::string parent = path.parent_path().string();
        return scm_from_utf8_string(parent.c_str());
    } catch (const std::exception& exception) {
        raise_host_error("path-parent", exception.what());
    } catch (...) {
        scm_misc_error("path-parent", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

// The Guile ABI fixes two adjacent SCM arguments; their Scheme procedure name
// and validation preserve the semantic order.
SCM directory_path_p(SCM host_object, SCM path_value) {
    if (!scm_is_string(path_value)) {
        scm_wrong_type_arg_msg("directory-path?", 2, path_value, "string");
    }
    (void)require_host(host_object, "directory-path?");
    try {
        const std::string path = scheme_string(path_value);
        return scm_from_bool(!path.empty() &&
                             path.back() == std::filesystem::path::preferred_separator);
    } catch (const std::exception& exception) {
        raise_host_error("directory-path?", exception.what());
    } catch (...) {
        scm_misc_error("directory-path?", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

// The Guile ABI fixes two adjacent SCM arguments; their Scheme procedure name
// and validation preserve the semantic order.
SCM path_as_directory(SCM host_object, SCM path_value) {
    if (!scm_is_string(path_value)) {
        scm_wrong_type_arg_msg("path-as-directory", 2, path_value, "string");
    }
    (void)require_host(host_object, "path-as-directory");
    try {
        std::string path =
            std::filesystem::path(scheme_string(path_value)).lexically_normal().string();
        if (path.empty() || path.back() != std::filesystem::path::preferred_separator) {
            path.push_back(std::filesystem::path::preferred_separator);
        }
        return scm_from_utf8_string(path.c_str());
    } catch (const std::exception& exception) {
        raise_host_error("path-as-directory", exception.what());
    } catch (...) {
        scm_misc_error("path-as-directory", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

// The Guile ABI fixes three adjacent SCM arguments; their Scheme procedure
// name and validation preserve the semantic order.
SCM display_buffer(SCM host_object, SCM window_value, SCM buffer_value, SCM intent_value) {
    HostLease& host = require_host(host_object, "display-buffer!");
    const WindowId window = entity_id_from_scheme<WindowTag>(window_value, "display-buffer!", 2);
    const BufferId buffer = entity_id_from_scheme<BufferTag>(buffer_value, "display-buffer!", 3);
    const std::string intent = scheme_name(intent_value, "display-buffer!", 4);
    if (!host.services.display_buffer) {
        scm_misc_error("display-buffer!", "display-buffer capability is unavailable", SCM_EOL);
    }
    try {
        const std::expected<WindowId, std::string> displayed =
            host.services.display_buffer(window, buffer, intent, std::nullopt);
        if (!displayed) {
            raise_host_error("display-buffer!", displayed.error());
        }
        return entity_id(displayed->slot, displayed->generation);
    } catch (const std::exception& exception) {
        raise_host_error("display-buffer!", exception.what());
    } catch (...) {
        scm_misc_error("display-buffer!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

SCM display_buffer_at(SCM host_object, SCM window_value, SCM buffer_value, SCM intent_value,
                      SCM line_value, SCM column_value) {
    HostLease& host = require_host(host_object, "display-buffer-at!");
    const WindowId window = entity_id_from_scheme<WindowTag>(window_value, "display-buffer-at!", 2);
    const BufferId buffer = entity_id_from_scheme<BufferTag>(buffer_value, "display-buffer-at!", 3);
    const std::string intent = scheme_name(intent_value, "display-buffer-at!", 4);
    if (scm_is_unsigned_integer(line_value, 0, std::numeric_limits<std::uint32_t>::max()) == 0) {
        scm_wrong_type_arg_msg("display-buffer-at!", 5, line_value, "unsigned 32-bit integer");
    }
    if (scm_is_unsigned_integer(column_value, 0, std::numeric_limits<std::uint32_t>::max()) == 0) {
        scm_wrong_type_arg_msg("display-buffer-at!", 6, column_value, "unsigned 32-bit integer");
    }
    if (!host.services.display_buffer) {
        scm_misc_error("display-buffer-at!", "display-buffer capability is unavailable", SCM_EOL);
    }
    try {
        const std::expected<WindowId, std::string> displayed = host.services.display_buffer(
            window, buffer, intent,
            GuileDisplayPosition{.position = {.line = scm_to_uint32(line_value),
                                              .column = scm_to_uint32(column_value),
                                              .encoding = PositionEncoding::Bytes}});
        if (!displayed) {
            raise_host_error("display-buffer-at!", displayed.error());
        }
        return entity_id(displayed->slot, displayed->generation);
    } catch (const std::exception& exception) {
        raise_host_error("display-buffer-at!", exception.what());
    } catch (...) {
        scm_misc_error("display-buffer-at!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

SCM display_buffer_position_at(SCM host_object, SCM window_value, SCM buffer_value,
                               SCM intent_value, SCM line_value, SCM column_value,
                               SCM encoding_value) {
    HostLease& host = require_host(host_object, "display-buffer-position-at!");
    const WindowId window =
        entity_id_from_scheme<WindowTag>(window_value, "display-buffer-position-at!", 2);
    const BufferId buffer =
        entity_id_from_scheme<BufferTag>(buffer_value, "display-buffer-position-at!", 3);
    const std::string intent = scheme_name(intent_value, "display-buffer-position-at!", 4);
    if (scm_is_unsigned_integer(line_value, 0, std::numeric_limits<std::uint32_t>::max()) == 0) {
        scm_wrong_type_arg_msg("display-buffer-position-at!", 5, line_value,
                               "unsigned 32-bit integer");
    }
    if (scm_is_unsigned_integer(column_value, 0, std::numeric_limits<std::uint32_t>::max()) == 0) {
        scm_wrong_type_arg_msg("display-buffer-position-at!", 6, column_value,
                               "unsigned 32-bit integer");
    }
    if (!host.services.display_buffer) {
        scm_misc_error("display-buffer-position-at!", "display-buffer capability is unavailable",
                       SCM_EOL);
    }
    try {
        const std::expected<WindowId, std::string> displayed = host.services.display_buffer(
            window, buffer, intent,
            GuileDisplayPosition{
                .position = {.line = scm_to_uint32(line_value),
                             .column = scm_to_uint32(column_value),
                             .encoding = position_encoding_from_scheme(
                                 encoding_value, "display-buffer-position-at!", 7)}});
        if (!displayed) {
            raise_host_error("display-buffer-position-at!", displayed.error());
        }
        return entity_id(displayed->slot, displayed->generation);
    } catch (const std::exception& exception) {
        raise_host_error("display-buffer-position-at!", exception.what());
    } catch (...) {
        scm_misc_error("display-buffer-position-at!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

SCM navigate_jump(SCM host_object, SCM window_value, SCM delta_value) {
    HostLease& host = require_host(host_object, "navigate-jump!");
    const WindowId window = entity_id_from_scheme<WindowTag>(window_value, "navigate-jump!", 2);
    if (scm_is_signed_integer(delta_value, std::numeric_limits<std::int64_t>::min(),
                              std::numeric_limits<std::int64_t>::max()) == 0) {
        scm_wrong_type_arg_msg("navigate-jump!", 3, delta_value, "signed 64-bit integer");
    }
    if (!host.services.navigate_jump) {
        scm_misc_error("navigate-jump!", "jump navigation capability is unavailable", SCM_EOL);
    }
    return scm_from_bool(host.services.navigate_jump(window, scm_to_int64(delta_value)));
}

SCM mark_jump(SCM host_object, SCM window_value) {
    HostLease& host = require_host(host_object, "mark-jump!");
    const WindowId window = entity_id_from_scheme<WindowTag>(window_value, "mark-jump!", 2);
    if (!host.services.mark_jump) {
        scm_misc_error("mark-jump!", "jump mark capability is unavailable", SCM_EOL);
    }
    const std::optional<std::uint64_t> node = host.services.mark_jump(window);
    return node ? scm_from_uint64(*node) : SCM_BOOL_F;
}

SCM visit_jump(SCM host_object, SCM window_value, SCM node_value) {
    HostLease& host = require_host(host_object, "visit-jump!");
    const WindowId window = entity_id_from_scheme<WindowTag>(window_value, "visit-jump!", 2);
    if (scm_is_unsigned_integer(node_value, 1, std::numeric_limits<std::uint64_t>::max()) == 0) {
        scm_wrong_type_arg_msg("visit-jump!", 3, node_value, "positive 64-bit integer");
    }
    if (!host.services.visit_jump) {
        scm_misc_error("visit-jump!", "jump visit capability is unavailable", SCM_EOL);
    }
    return scm_from_bool(host.services.visit_jump(window, scm_to_uint64(node_value)));
}

SCM link_jump(SCM host_object, SCM window_value, SCM from_value, SCM to_value, SCM kind_value,
              SCM persistent_value) {
    HostLease& host = require_host(host_object, "link-jump!");
    const WindowId window = entity_id_from_scheme<WindowTag>(window_value, "link-jump!", 2);
    if (scm_is_unsigned_integer(from_value, 1, std::numeric_limits<std::uint64_t>::max()) == 0) {
        scm_wrong_type_arg_msg("link-jump!", 3, from_value, "positive 64-bit integer");
    }
    if (scm_is_unsigned_integer(to_value, 1, std::numeric_limits<std::uint64_t>::max()) == 0) {
        scm_wrong_type_arg_msg("link-jump!", 4, to_value, "positive 64-bit integer");
    }
    const std::string kind = scheme_name(kind_value, "link-jump!", 5);
    if (!host.services.link_jump) {
        scm_misc_error("link-jump!", "jump link capability is unavailable", SCM_EOL);
    }
    return scm_from_bool(host.services.link_jump(window, scm_to_uint64(from_value),
                                                 scm_to_uint64(to_value), kind,
                                                 scm_to_bool(persistent_value) != 0));
}

SCM jump_branches(SCM host_object, SCM window_value, SCM incoming_value) {
    HostLease& host = require_host(host_object, "jump-branches");
    const WindowId window = entity_id_from_scheme<WindowTag>(window_value, "jump-branches", 2);
    if (!host.services.jump_branches) {
        scm_misc_error("jump-branches", "jump query capability is unavailable", SCM_EOL);
    }
    const std::vector<GuileJumpEdge> edges =
        host.services.jump_branches(window, scm_to_bool(incoming_value) != 0);
    SCM result = scm_c_make_vector(edges.size(), SCM_UNSPECIFIED);
    for (std::size_t index = 0; index < edges.size(); ++index) {
        const GuileJumpEdge& edge = edges[index];
        SCM value = scm_c_make_vector(5, SCM_UNSPECIFIED);
        scm_c_vector_set_x(value, 0, scm_from_uint64(edge.from));
        scm_c_vector_set_x(value, 1, scm_from_uint64(edge.to));
        scm_c_vector_set_x(value, 2, name_symbol(edge.kind));
        scm_c_vector_set_x(value, 3, scm_from_uint64(edge.at));
        scm_c_vector_set_x(value, 4, scm_from_bool(edge.persistent));
        scm_c_vector_set_x(result, index, value);
    }
    return result;
}

SCM jump_node(SCM host_object, SCM window_value, SCM node_value) {
    HostLease& host = require_host(host_object, "jump-node");
    const WindowId window = entity_id_from_scheme<WindowTag>(window_value, "jump-node", 2);
    if (scm_is_unsigned_integer(node_value, 1, std::numeric_limits<std::uint64_t>::max()) == 0) {
        scm_wrong_type_arg_msg("jump-node", 3, node_value, "positive 64-bit integer");
    }
    if (!host.services.jump_node) {
        scm_misc_error("jump-node", "jump query capability is unavailable", SCM_EOL);
    }
    const std::optional<GuileJumpNode> node =
        host.services.jump_node(window, scm_to_uint64(node_value));
    if (!node) {
        return SCM_BOOL_F;
    }
    SCM result = scm_c_make_vector(6, SCM_UNSPECIFIED);
    scm_c_vector_set_x(result, 0, scm_from_uint64(node->id));
    scm_c_vector_set_x(result, 1, scm_from_utf8_string(node->resource.c_str()));
    scm_c_vector_set_x(result, 2, scm_from_uint32(node->line));
    scm_c_vector_set_x(result, 3, scm_from_uint32(node->byte_column));
    scm_c_vector_set_x(result, 4, scm_from_utf8_string(node->excerpt.c_str()));
    scm_c_vector_set_x(result, 5, scm_from_uint64(node->last_visit));
    return result;
}

SCM evict_jumps(SCM host_object, SCM window_value, SCM maximum_value) {
    HostLease& host = require_host(host_object, "evict-jumps!");
    const WindowId window = entity_id_from_scheme<WindowTag>(window_value, "evict-jumps!", 2);
    if (scm_is_unsigned_integer(maximum_value, 0, std::numeric_limits<std::size_t>::max()) == 0) {
        scm_wrong_type_arg_msg("evict-jumps!", 3, maximum_value, "non-negative integer");
    }
    if (!host.services.evict_jumps) {
        scm_misc_error("evict-jumps!", "jump eviction capability is unavailable", SCM_EOL);
    }
    return scm_from_size_t(host.services.evict_jumps(window, scm_to_size_t(maximum_value)));
}

SCM display_generated_buffer(SCM host_object, SCM window_value, SCM name_value, SCM text_value,
                             SCM mode_value, SCM style_origin_value, SCM intent_value) {
    if (!scm_is_string(name_value)) {
        scm_wrong_type_arg_msg("display-generated-buffer!", 3, name_value, "string");
    }
    if (!scm_is_string(text_value)) {
        scm_wrong_type_arg_msg("display-generated-buffer!", 4, text_value, "string");
    }
    if (!scm_is_string(style_origin_value)) {
        scm_wrong_type_arg_msg("display-generated-buffer!", 6, style_origin_value, "string");
    }
    HostLease& host = require_host(host_object, "display-generated-buffer!");
    const WindowId window =
        entity_id_from_scheme<WindowTag>(window_value, "display-generated-buffer!", 2);
    const std::string intent = scheme_name(intent_value, "display-generated-buffer!", 7);
    if (!host.services.display_generated_buffer) {
        scm_misc_error("display-generated-buffer!", "generated-buffer capability is unavailable",
                       SCM_EOL);
    }
    try {
        const ModeId mode = require_mode(host, mode_value, "display-generated-buffer!", 5);
        const std::expected<WindowId, std::string> displayed =
            host.services.display_generated_buffer(window, scheme_string(name_value),
                                                   scheme_string_with_nuls(text_value), mode,
                                                   scheme_string(style_origin_value), intent);
        if (!displayed) {
            raise_host_error("display-generated-buffer!", displayed.error());
        }
        return entity_id(displayed->slot, displayed->generation);
    } catch (const std::exception& exception) {
        raise_host_error("display-generated-buffer!", exception.what());
    } catch (...) {
        scm_misc_error("display-generated-buffer!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

// The Guile ABI fixes three adjacent SCM arguments; the Scheme-level name and
// validation preserve their semantic order.
SCM evaluate_scheme(SCM host_object, SCM source_value, SCM source_name_value) {
    if (!scm_is_string(source_value)) {
        scm_wrong_type_arg_msg("evaluate-scheme!", 2, source_value, "string");
    }
    if (!scm_is_string(source_name_value)) {
        scm_wrong_type_arg_msg("evaluate-scheme!", 3, source_name_value, "string");
    }
    try {
        HostLease& host = require_host(host_object, "evaluate-scheme!");
        const std::string source = scheme_string(source_value);
        const std::string source_name = scheme_string(source_name_value);
        const std::expected<GuileEvaluationResult, std::string> evaluated =
            evaluate_source(host, {.source = source, .source_name = source_name});
        if (!evaluated) {
            raise_host_error("evaluate-scheme!", evaluated.error());
        }
        SCM result = scm_c_make_vector(4, SCM_UNSPECIFIED);
        if (evaluated->error) {
            scm_c_vector_set_x(result, 0, name_symbol("error"));
            scm_c_vector_set_x(result, 1, scm_from_utf8_string(evaluated->error->c_str()));
        } else {
            scm_c_vector_set_x(result, 0, name_symbol("ok"));
            scm_c_vector_set_x(result, 1, string_vector_value(evaluated->values));
        }
        scm_c_vector_set_x(
            result, 2, scm_from_utf8_stringn(evaluated->output.data(), evaluated->output.size()));
        scm_c_vector_set_x(
            result, 3,
            scm_from_utf8_stringn(evaluated->error_output.data(), evaluated->error_output.size()));
        return result;
    } catch (const std::exception& exception) {
        raise_host_error("evaluate-scheme!", exception.what());
    } catch (...) {
        scm_misc_error("evaluate-scheme!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

// The Guile ABI fixes four adjacent SCM arguments; their Scheme procedure
// name and validation preserve the semantic order.
SCM move_caret_to_line(SCM host_object, SCM view_value, SCM line_value, SCM column_value) {
    HostLease& host = require_host(host_object, "move-caret-to-line!");
    const ViewId view = entity_id_from_scheme<ViewTag>(view_value, "move-caret-to-line!", 2);
    if (scm_is_unsigned_integer(line_value, 0, std::numeric_limits<std::uint32_t>::max()) == 0) {
        scm_wrong_type_arg_msg("move-caret-to-line!", 3, line_value, "unsigned 32-bit integer");
    }
    if (scm_is_unsigned_integer(column_value, 0, std::numeric_limits<std::uint32_t>::max()) == 0) {
        scm_wrong_type_arg_msg("move-caret-to-line!", 4, column_value, "unsigned 32-bit integer");
    }
    if (!host.services.move_caret_to_line) {
        scm_misc_error("move-caret-to-line!", "caret capability is unavailable", SCM_EOL);
    }
    try {
        const std::expected<void, std::string> moved = host.services.move_caret_to_line(
            view, scm_to_uint32(line_value), scm_to_uint32(column_value));
        if (!moved) {
            raise_host_error("move-caret-to-line!", moved.error());
        }
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("move-caret-to-line!", exception.what());
    } catch (...) {
        scm_misc_error("move-caret-to-line!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

SCM scroll_view_lines(SCM host_object, SCM view_value, SCM lines_value) {
    HostLease& host = require_host(host_object, "scroll-view-lines!");
    const ViewId view = entity_id_from_scheme<ViewTag>(view_value, "scroll-view-lines!", 2);
    if (scm_is_real(lines_value) == 0) {
        scm_wrong_type_arg_msg("scroll-view-lines!", 3, lines_value, "real number");
    }
    const double lines = scm_to_double(lines_value);
    if (!std::isfinite(lines)) {
        scm_misc_error("scroll-view-lines!", "scroll delta must be finite", SCM_EOL);
    }
    if (!host.services.scroll_view_lines) {
        scm_misc_error("scroll-view-lines!", "viewport scroll capability is unavailable", SCM_EOL);
    }
    try {
        host.services.scroll_view_lines(view, lines);
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("scroll-view-lines!", exception.what());
    } catch (...) {
        scm_misc_error("scroll-view-lines!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

SCM undo_edit(SCM host_object, SCM view_value) {
    HostLease& host = require_host(host_object, "undo!");
    const ViewId view = entity_id_from_scheme<ViewTag>(view_value, "undo!", 2);
    if (!host.services.undo) {
        scm_misc_error("undo!", "undo capability is unavailable", SCM_EOL);
    }
    try {
        return scm_from_bool(host.services.undo(view));
    } catch (const std::exception& exception) {
        raise_host_error("undo!", exception.what());
    } catch (...) {
        scm_misc_error("undo!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM redo_edit(SCM host_object, SCM view_value) {
    HostLease& host = require_host(host_object, "redo!");
    const ViewId view = entity_id_from_scheme<ViewTag>(view_value, "redo!", 2);
    if (!host.services.redo) {
        scm_misc_error("redo!", "redo capability is unavailable", SCM_EOL);
    }
    try {
        return scm_from_bool(host.services.redo(view));
    } catch (const std::exception& exception) {
        raise_host_error("redo!", exception.what());
    } catch (...) {
        scm_misc_error("redo!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM move_caret_lines(SCM host_object, SCM view_value, SCM delta_value) {
    HostLease& host = require_host(host_object, "move-caret-lines!");
    const ViewId view = entity_id_from_scheme<ViewTag>(view_value, "move-caret-lines!", 2);
    if (scm_is_signed_integer(delta_value, std::numeric_limits<std::int64_t>::min(),
                              std::numeric_limits<std::int64_t>::max()) == 0) {
        scm_wrong_type_arg_msg("move-caret-lines!", 3, delta_value, "signed 64-bit integer");
    }
    if (!host.services.move_caret_lines) {
        scm_misc_error("move-caret-lines!", "vertical caret capability is unavailable", SCM_EOL);
    }
    try {
        host.services.move_caret_lines(view, scm_to_int64(delta_value));
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("move-caret-lines!", exception.what());
    } catch (...) {
        scm_misc_error("move-caret-lines!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

SCM move_caret_line_boundary(SCM host_object, SCM view_value, SCM boundary_value) {
    HostLease& host = require_host(host_object, "move-caret-line-boundary!");
    const ViewId view = entity_id_from_scheme<ViewTag>(view_value, "move-caret-line-boundary!", 2);
    bool end = false;
    if (symbol_is(boundary_value, "end")) {
        end = true;
    } else if (!symbol_is(boundary_value, "start")) {
        scm_wrong_type_arg_msg("move-caret-line-boundary!", 3, boundary_value, "'start or 'end");
    }
    if (!host.services.move_caret_line_boundary) {
        scm_misc_error("move-caret-line-boundary!", "line-boundary capability is unavailable",
                       SCM_EOL);
    }
    try {
        host.services.move_caret_line_boundary(view, end);
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("move-caret-line-boundary!", exception.what());
    } catch (...) {
        scm_misc_error("move-caret-line-boundary!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

// The Guile ABI fixes four adjacent SCM arguments; their Scheme procedure
// name and validation preserve the semantic order.
SCM delete_grapheme(SCM host_object, SCM view_value, SCM direction_value, SCM mode_value) {
    HostLease& host = require_host(host_object, "delete-grapheme!");
    const ViewId view = entity_id_from_scheme<ViewTag>(view_value, "delete-grapheme!", 2);
    const bool forward = symbol_is(direction_value, "forward");
    if (!forward && !symbol_is(direction_value, "backward")) {
        scm_wrong_type_arg_msg("delete-grapheme!", 3, direction_value, "'forward or 'backward");
    }
    const bool structural = symbol_is(mode_value, "structural");
    if (!structural && !symbol_is(mode_value, "raw")) {
        scm_wrong_type_arg_msg("delete-grapheme!", 4, mode_value, "'structural or 'raw");
    }
    if (!host.services.delete_grapheme) {
        scm_misc_error("delete-grapheme!", "grapheme deletion capability is unavailable", SCM_EOL);
    }
    try {
        switch (host.services.delete_grapheme(view, forward, structural)) {
        case GuileDeleteOutcome::Unchanged:
            return name_symbol("unchanged");
        case GuileDeleteOutcome::Deleted:
            return name_symbol("deleted");
        case GuileDeleteOutcome::MovedOverPair:
            return name_symbol("moved-over-pair");
        case GuileDeleteOutcome::MovedOverLiteral:
            return name_symbol("moved-over-literal");
        }
    } catch (const std::exception& exception) {
        raise_host_error("delete-grapheme!", exception.what());
    } catch (...) {
        scm_misc_error("delete-grapheme!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM newline_edit(SCM host_object, SCM view_value) {
    HostLease& host = require_host(host_object, "newline!");
    const ViewId view = entity_id_from_scheme<ViewTag>(view_value, "newline!", 2);
    if (!host.services.newline) {
        scm_misc_error("newline!", "newline capability is unavailable", SCM_EOL);
    }
    try {
        host.services.newline(view);
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("newline!", exception.what());
    } catch (...) {
        scm_misc_error("newline!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

SCM structural_edit(SCM host_object, SCM view_value, SCM operation_value) {
    HostLease& host = require_host(host_object, "structural-edit!");
    const ViewId view = entity_id_from_scheme<ViewTag>(view_value, "structural-edit!", 2);
    const std::string operation = scheme_name(operation_value, "structural-edit!", 3);
    if (!host.services.structural_edit) {
        scm_misc_error("structural-edit!", "structural editing capability is unavailable", SCM_EOL);
    }
    try {
        const std::expected<void, std::string> result =
            host.services.structural_edit(view, operation);
        if (!result) {
            raise_host_error("structural-edit!", result.error());
        }
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("structural-edit!", exception.what());
    } catch (...) {
        scm_misc_error("structural-edit!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

SCM indent_edit(SCM host_object, SCM view_value) {
    HostLease& host = require_host(host_object, "indent!");
    const ViewId view = entity_id_from_scheme<ViewTag>(view_value, "indent!", 2);
    if (!host.services.indent) {
        scm_misc_error("indent!", "indentation capability is unavailable", SCM_EOL);
    }
    try {
        const std::optional<std::string> role = host.services.indent(view);
        return role ? scm_from_utf8_string(role->c_str()) : SCM_BOOL_F;
    } catch (const std::exception& exception) {
        raise_host_error("indent!", exception.what());
    } catch (...) {
        scm_misc_error("indent!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM type_text(SCM host_object, SCM view_value, SCM text_value) {
    if (!scm_is_string(text_value)) {
        scm_wrong_type_arg_msg("type-text!", 3, text_value, "string");
    }
    HostLease& host = require_host(host_object, "type-text!");
    const ViewId view = entity_id_from_scheme<ViewTag>(view_value, "type-text!", 2);
    if (!host.services.type_text) {
        scm_misc_error("type-text!", "typed-text capability is unavailable", SCM_EOL);
    }
    try {
        const std::expected<void, std::string> typed =
            host.services.type_text(view, scheme_string(text_value));
        if (!typed) {
            raise_host_error("type-text!", typed.error());
        }
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("type-text!", exception.what());
    } catch (...) {
        scm_misc_error("type-text!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

SCM page_rows(SCM host_object) {
    HostLease& host = require_host(host_object, "page-rows");
    if (!host.services.page_rows) {
        scm_misc_error("page-rows", "page geometry capability is unavailable", SCM_EOL);
    }
    try {
        return scm_from_int(std::max(1, host.services.page_rows()));
    } catch (const std::exception& exception) {
        raise_host_error("page-rows", exception.what());
    } catch (...) {
        scm_misc_error("page-rows", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM project_index_state(SCM host_object, SCM project_value) {
    HostLease& host = require_host(host_object, "project-index-state");
    const ProjectId project =
        entity_id_from_scheme<ProjectTag>(project_value, "project-index-state", 2);
    try {
        const Project& definition = host.runtime->projects().get(project);
        SCM result = scm_c_make_vector(3, SCM_UNSPECIFIED);
        scm_c_vector_set_x(result, 0, scm_from_uint64(definition.index_revision()));
        scm_c_vector_set_x(result, 1, scm_from_bool(definition.indexing()));
        scm_c_vector_set_x(result, 2,
                           definition.index_error()
                               ? scm_from_utf8_string(definition.index_error()->c_str())
                               : SCM_BOOL_F);
        return result;
    } catch (const std::exception& exception) {
        raise_host_error("project-index-state", exception.what());
    } catch (...) {
        scm_misc_error("project-index-state", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM request_project_index(SCM host_object, SCM project_value) {
    HostLease& host = require_host(host_object, "request-project-index!");
    const ProjectId project =
        entity_id_from_scheme<ProjectTag>(project_value, "request-project-index!", 2);
    if (!host.services.request_project_index) {
        scm_misc_error("request-project-index!", "project-index capability is unavailable",
                       SCM_EOL);
    }
    try {
        const std::expected<void, std::string> requested =
            host.services.request_project_index(project);
        if (!requested) {
            raise_host_error("request-project-index!", requested.error());
        }
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("request-project-index!", exception.what());
    } catch (...) {
        scm_misc_error("request-project-index!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

SCM normalize_resource(SCM host_object, SCM path_value) {
    (void)require_host(host_object, "normalize-resource-path");
    if (!scm_is_string(path_value)) {
        scm_wrong_type_arg_msg("normalize-resource-path", 2, path_value, "string");
    }
    const std::expected<std::string, std::string> path =
        normalize_resource_path(scheme_string(path_value));
    if (!path) {
        raise_host_error("normalize-resource-path", path.error());
    }
    return scm_from_utf8_string(path->c_str());
}

SCM set_buffer_resource(SCM host_object, SCM buffer_value, SCM path_value) {
    if (!scm_is_string(path_value)) {
        scm_wrong_type_arg_msg("set-buffer-resource!", 3, path_value, "string");
    }
    HostLease& host = require_host(host_object, "set-buffer-resource!");
    const BufferId buffer =
        entity_id_from_scheme<BufferTag>(buffer_value, "set-buffer-resource!", 2);
    try {
        host.runtime->buffers().set_resource(buffer, scheme_string(path_value), BufferKind::File);
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("set-buffer-resource!", exception.what());
    } catch (...) {
        scm_misc_error("set-buffer-resource!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

SCM rename_buffer(SCM host_object, SCM buffer_value, SCM name_value) {
    if (!scm_is_string(name_value)) {
        scm_wrong_type_arg_msg("rename-buffer!", 3, name_value, "string");
    }
    try {
        HostLease& host = require_host(host_object, "rename-buffer!");
        const BufferId buffer = entity_id_from_scheme<BufferTag>(buffer_value, "rename-buffer!", 2);
        host.runtime->buffers().rename(buffer, scheme_string(name_value));
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("rename-buffer!", exception.what());
    } catch (...) {
        scm_misc_error("rename-buffer!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

SCM buffer_id_by_resource(SCM host_object, SCM path_value) {
    if (!scm_is_string(path_value)) {
        scm_wrong_type_arg_msg("buffer-id-by-resource", 2, path_value, "string");
    }
    try {
        HostLease& host = require_host(host_object, "buffer-id-by-resource");
        const std::optional<BufferId> buffer =
            host.runtime->buffers().find_by_resource(scheme_string(path_value));
        return buffer ? entity_id(buffer->slot, buffer->generation) : SCM_BOOL_F;
    } catch (const std::exception& exception) {
        raise_host_error("buffer-id-by-resource", exception.what());
    } catch (...) {
        scm_misc_error("buffer-id-by-resource", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM resource_mode(SCM host_object, SCM path_value) {
    if (!scm_is_string(path_value)) {
        scm_wrong_type_arg_msg("resource-mode", 2, path_value, "string");
    }
    try {
        HostLease& host = require_host(host_object, "resource-mode");
        const std::optional<ModeId> mode =
            host.runtime->resource_policies().mode_for(scheme_string(path_value));
        return mode ? name_symbol(host.runtime->modes().definition(*mode).name) : SCM_BOOL_F;
    } catch (const std::exception& exception) {
        raise_host_error("resource-mode", exception.what());
    } catch (...) {
        scm_misc_error("resource-mode", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM project_for_resource(SCM host_object, SCM path_value) {
    if (!scm_is_string(path_value)) {
        scm_wrong_type_arg_msg("project-for-resource", 2, path_value, "string");
    }
    try {
        HostLease& host = require_host(host_object, "project-for-resource");
        const std::optional<ProjectId> project =
            host.runtime->projects().find_for_resource(scheme_string(path_value));
        return project ? entity_id(project->slot, project->generation) : SCM_BOOL_F;
    } catch (const std::exception& exception) {
        raise_host_error("project-for-resource", exception.what());
    } catch (...) {
        scm_misc_error("project-for-resource", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM project_provider_definitions(SCM host_object) {
    HostLease& host = require_host(host_object, "project-provider-definitions");
    try {
        const std::vector<ProjectDiscoveryProvider>& providers =
            host.runtime->resource_policies().project_providers();
        SCM result = scm_c_make_vector(providers.size(), SCM_UNSPECIFIED);
        for (std::size_t index = 0; index < providers.size(); ++index) {
            const ProjectDiscoveryProvider& provider = providers[index];
            SCM markers = scm_c_make_vector(provider.markers.size(), SCM_UNSPECIFIED);
            for (std::size_t marker = 0; marker < provider.markers.size(); ++marker) {
                scm_c_vector_set_x(markers, marker,
                                   scm_from_utf8_string(provider.markers[marker].c_str()));
            }
            SCM definition = scm_c_make_vector(2, SCM_UNSPECIFIED);
            scm_c_vector_set_x(definition, 0, scm_from_utf8_string(provider.name.c_str()));
            scm_c_vector_set_x(definition, 1, markers);
            scm_c_vector_set_x(result, index, definition);
        }
        return result;
    } catch (const std::exception& exception) {
        raise_host_error("project-provider-definitions", exception.what());
    } catch (...) {
        scm_misc_error("project-provider-definitions", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM project_id_by_root(SCM host_object, SCM root_value) {
    HostLease& host = require_host(host_object, "project-id-by-root");
    if (!scm_is_string(root_value)) {
        scm_wrong_type_arg_msg("project-id-by-root", 2, root_value, "string");
    }
    try {
        const std::optional<ProjectId> project =
            host.runtime->projects().find_by_root(scheme_string(root_value));
        return project ? entity_id(project->slot, project->generation) : SCM_BOOL_F;
    } catch (const std::exception& exception) {
        raise_host_error("project-id-by-root", exception.what());
    } catch (...) {
        scm_misc_error("project-id-by-root", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM create_project(SCM host_object, SCM name_value, SCM roots_value, SCM provider_value,
                   SCM marker_value) {
    HostLease& host = require_host(host_object, "create-project!");
    if (!scm_is_string(name_value)) {
        scm_wrong_type_arg_msg("create-project!", 2, name_value, "string");
    }
    if (!scm_is_string(provider_value)) {
        scm_wrong_type_arg_msg("create-project!", 4, provider_value, "string");
    }
    if (!scm_is_string(marker_value)) {
        scm_wrong_type_arg_msg("create-project!", 5, marker_value, "string");
    }
    try {
        const ProjectId project = host.runtime->projects().create(
            {.name = scheme_string(name_value),
             .roots = string_sequence_from_scheme(roots_value, "create-project!", 3),
             .discovery_provider = scheme_string(provider_value),
             .discovery_marker = scheme_string(marker_value)});
        return entity_id(project.slot, project.generation);
    } catch (const std::exception& exception) {
        raise_host_error("create-project!", exception.what());
    } catch (...) {
        scm_misc_error("create-project!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM set_buffer_project(SCM host_object, SCM buffer_value, SCM project_value) {
    try {
        HostLease& host = require_host(host_object, "set-buffer-project!");
        const BufferId buffer =
            entity_id_from_scheme<BufferTag>(buffer_value, "set-buffer-project!", 2);
        std::optional<ProjectId> project;
        if (!scheme_false(project_value)) {
            project = entity_id_from_scheme<ProjectTag>(project_value, "set-buffer-project!", 3);
        }
        host.runtime->projects().assign(buffer, project);
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("set-buffer-project!", exception.what());
    } catch (...) {
        scm_misc_error("set-buffer-project!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

SCM buffer_save_snapshot(SCM host_object, SCM buffer_value) {
    try {
        HostLease& host = require_host(host_object, "buffer-save-snapshot");
        const BufferId buffer =
            entity_id_from_scheme<BufferTag>(buffer_value, "buffer-save-snapshot", 2);
        const std::string content =
            host.runtime->buffers().get(buffer).snapshot().content().to_string();
        return scm_from_utf8_stringn(content.data(), content.size());
    } catch (const std::exception& exception) {
        raise_host_error("buffer-save-snapshot", exception.what());
    } catch (...) {
        scm_misc_error("buffer-save-snapshot", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM mark_buffer_saved(SCM host_object, SCM buffer_value, SCM content_value) {
    if (!scm_is_string(content_value)) {
        scm_wrong_type_arg_msg("mark-buffer-saved!", 3, content_value, "string");
    }
    try {
        HostLease& host = require_host(host_object, "mark-buffer-saved!");
        const BufferId buffer =
            entity_id_from_scheme<BufferTag>(buffer_value, "mark-buffer-saved!", 2);
        Buffer& target = host.runtime->buffers().get(buffer);
        target.mark_saved(Text(scheme_string_with_nuls(content_value)));
        return scm_from_bool(target.modified());
    } catch (const std::exception& exception) {
        raise_host_error("mark-buffer-saved!", exception.what());
    } catch (...) {
        scm_misc_error("mark-buffer-saved!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM open_buffers(SCM host_object) {
    HostLease& host = require_host(host_object, "open-buffer-ids");
    if (!host.services.open_buffers) {
        scm_misc_error("open-buffer-ids", "open-buffer snapshot capability is unavailable",
                       SCM_EOL);
    }
    try {
        const std::vector<BufferId> buffers = host.services.open_buffers();
        SCM result = scm_c_make_vector(buffers.size(), SCM_UNSPECIFIED);
        for (std::size_t index = 0; index < buffers.size(); ++index) {
            scm_c_vector_set_x(result, index,
                               entity_id(buffers[index].slot, buffers[index].generation));
        }
        return result;
    } catch (const std::exception& exception) {
        raise_host_error("open-buffer-ids", exception.what());
    } catch (...) {
        scm_misc_error("open-buffer-ids", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

// The low-level ABI is positional; policy wrappers choose names, kinds, modes,
// and presentation metadata before entering this mechanism boundary.
SCM create_buffer(SCM host_object, SCM name_value, SCM text_value, SCM kind_value,
                  SCM resource_value, SCM read_only_value, SCM mode_value, SCM style_value,
                  SCM style_origin_value) {
    if (!scm_is_string(name_value)) {
        scm_wrong_type_arg_msg("create-buffer!", 2, name_value, "string");
    }
    if (!scm_is_string(text_value)) {
        scm_wrong_type_arg_msg("create-buffer!", 3, text_value, "string");
    }
    if (!scheme_boolean(read_only_value)) {
        scm_wrong_type_arg_msg("create-buffer!", 6, read_only_value, "boolean");
    }
    if (!scm_is_string(style_origin_value)) {
        scm_wrong_type_arg_msg("create-buffer!", 9, style_origin_value, "string");
    }
    try {
        HostLease& host = require_host(host_object, "create-buffer!");
        if (!host.services.create_buffer) {
            scm_misc_error("create-buffer!", "buffer-create capability is unavailable", SCM_EOL);
        }
        const BufferKind kind = buffer_kind_from_scheme(kind_value, "create-buffer!", 4);
        std::optional<std::string> resource;
        if (!scheme_false(resource_value)) {
            if (!scm_is_string(resource_value)) {
                scm_wrong_type_arg_msg("create-buffer!", 5, resource_value, "string or #f");
            }
            resource = scheme_string(resource_value);
        }
        std::optional<ModeId> mode;
        if (!scheme_false(mode_value)) {
            mode = require_mode(host, mode_value, "create-buffer!", 7);
        }
        const CppIndentStyle style =
            scheme_false(style_value)
                ? CppIndentStyle{}
                : cpp_indent_style_from_scheme(style_value, "create-buffer!", 8);
        std::expected<BufferId, std::string> created =
            host.services.create_buffer({.name = scheme_string(name_value),
                                         .initial_text = scheme_string_with_nuls(text_value),
                                         .kind = kind,
                                         .resource = std::move(resource),
                                         .read_only = scheme_true(read_only_value),
                                         .major_mode = mode,
                                         .style = style,
                                         .style_origin = scheme_string(style_origin_value)});
        if (!created) {
            raise_host_error("create-buffer!", created.error());
        }
        return entity_id(created->slot, created->generation);
    } catch (const std::exception& exception) {
        raise_host_error("create-buffer!", exception.what());
    } catch (...) {
        scm_misc_error("create-buffer!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM buffer_modified_p(SCM host_object, SCM buffer_value) {
    try {
        HostLease& host = require_host(host_object, "buffer-modified?");
        const BufferId buffer =
            entity_id_from_scheme<BufferTag>(buffer_value, "buffer-modified?", 2);
        return scm_from_bool(host.runtime->buffers().get(buffer).modified());
    } catch (const std::exception& exception) {
        raise_host_error("buffer-modified?", exception.what());
    } catch (...) {
        scm_misc_error("buffer-modified?", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM release_buffer(SCM host_object, SCM buffer_value, SCM replacement_value) {
    HostLease& host = require_host(host_object, "release-buffer!");
    const BufferId buffer = entity_id_from_scheme<BufferTag>(buffer_value, "release-buffer!", 2);
    const BufferId replacement =
        entity_id_from_scheme<BufferTag>(replacement_value, "release-buffer!", 3);
    if (!host.services.release_buffer) {
        scm_misc_error("release-buffer!", "buffer-release capability is unavailable", SCM_EOL);
    }
    try {
        const std::expected<void, std::string> removed =
            host.services.release_buffer(buffer, replacement);
        return removed ? SCM_BOOL_F : scm_from_utf8_string(removed.error().c_str());
    } catch (const std::exception& exception) {
        raise_host_error("release-buffer!", exception.what());
    } catch (...) {
        scm_misc_error("release-buffer!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

// The Guile ABI fixes three adjacent SCM arguments; their Scheme procedure
// name and validation preserve the semantic order.
SCM split_window(SCM host_object, SCM window_value, SCM axis_value) {
    HostLease& host = require_host(host_object, "split-window!");
    const WindowId window = entity_id_from_scheme<WindowTag>(window_value, "split-window!", 2);
    WindowSplitAxis axis;
    if (symbol_is(axis_value, "rows")) {
        axis = WindowSplitAxis::Rows;
    } else if (symbol_is(axis_value, "columns")) {
        axis = WindowSplitAxis::Columns;
    } else {
        scm_wrong_type_arg_msg("split-window!", 3, axis_value, "'rows or 'columns");
    }
    if (!host.services.split_window) {
        scm_misc_error("split-window!", "window-split capability is unavailable", SCM_EOL);
    }
    try {
        const std::expected<void, std::string> split = host.services.split_window(window, axis);
        return split ? SCM_BOOL_F : scm_from_utf8_string(split.error().c_str());
    } catch (const std::exception& exception) {
        raise_host_error("split-window!", exception.what());
    } catch (...) {
        scm_misc_error("split-window!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

// The Guile ABI fixes two adjacent SCM arguments; their Scheme procedure name
// and validation preserve the semantic order.
SCM delete_window(SCM host_object, SCM window_value) {
    HostLease& host = require_host(host_object, "delete-window!");
    const WindowId window = entity_id_from_scheme<WindowTag>(window_value, "delete-window!", 2);
    if (!host.services.delete_window) {
        scm_misc_error("delete-window!", "window-delete capability is unavailable", SCM_EOL);
    }
    try {
        const std::expected<void, std::string> removed = host.services.delete_window(window);
        return removed ? SCM_BOOL_F : scm_from_utf8_string(removed.error().c_str());
    } catch (const std::exception& exception) {
        raise_host_error("delete-window!", exception.what());
    } catch (...) {
        scm_misc_error("delete-window!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

// The Guile ABI fixes two adjacent SCM arguments; their Scheme procedure name
// and validation preserve the semantic order.
SCM delete_other_windows(SCM host_object, SCM window_value) {
    HostLease& host = require_host(host_object, "delete-other-windows!");
    const WindowId window =
        entity_id_from_scheme<WindowTag>(window_value, "delete-other-windows!", 2);
    if (!host.services.delete_other_windows) {
        scm_misc_error("delete-other-windows!", "window-retain capability is unavailable", SCM_EOL);
    }
    try {
        const std::expected<void, std::string> retained =
            host.services.delete_other_windows(window);
        return retained ? SCM_BOOL_F : scm_from_utf8_string(retained.error().c_str());
    } catch (const std::exception& exception) {
        raise_host_error("delete-other-windows!", exception.what());
    } catch (...) {
        scm_misc_error("delete-other-windows!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM open_windows(SCM host_object) {
    HostLease& host = require_host(host_object, "open-window-ids");
    if (!host.services.open_windows) {
        scm_misc_error("open-window-ids", "window-list capability is unavailable", SCM_EOL);
    }
    try {
        const std::vector<WindowId> windows = host.services.open_windows();
        SCM result = scm_c_make_vector(windows.size(), SCM_UNSPECIFIED);
        for (std::size_t index = 0; index < windows.size(); ++index) {
            scm_c_vector_set_x(result, index,
                               entity_id(windows[index].slot, windows[index].generation));
        }
        return result;
    } catch (const std::exception& exception) {
        raise_host_error("open-window-ids", exception.what());
    } catch (...) {
        scm_misc_error("open-window-ids", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM active_window(SCM host_object) {
    HostLease& host = require_host(host_object, "active-window-id");
    if (!host.services.active_window) {
        scm_misc_error("active-window-id", "active-window capability is unavailable", SCM_EOL);
    }
    try {
        const WindowId window = host.services.active_window();
        (void)host.runtime->windows().get(window);
        return entity_id(window.slot, window.generation);
    } catch (const std::exception& exception) {
        raise_host_error("active-window-id", exception.what());
    } catch (...) {
        scm_misc_error("active-window-id", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM window_view(SCM host_object, SCM window_value) {
    HostLease& host = require_host(host_object, "window-view-id");
    const WindowId window = entity_id_from_scheme<WindowTag>(window_value, "window-view-id", 2);
    try {
        const ViewId view = host.runtime->windows().get(window).view_id();
        return entity_id(view.slot, view.generation);
    } catch (const std::exception& exception) {
        raise_host_error("window-view-id", exception.what());
    } catch (...) {
        scm_misc_error("window-view-id", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM window_role(SCM host_object, SCM window_value) {
    HostLease& host = require_host(host_object, "window-role");
    const WindowId window = entity_id_from_scheme<WindowTag>(window_value, "window-role", 2);
    try {
        const std::optional<std::string>& role = host.runtime->windows().get(window).role();
        return role ? name_symbol(*role) : SCM_BOOL_F;
    } catch (const std::exception& exception) {
        raise_host_error("window-role", exception.what());
    } catch (...) {
        scm_misc_error("window-role", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM set_window_role(SCM host_object, SCM window_value, SCM role_value) {
    HostLease& host = require_host(host_object, "set-window-role!");
    const WindowId window = entity_id_from_scheme<WindowTag>(window_value, "set-window-role!", 2);
    if (!scheme_false(role_value) && !scheme_true(scm_symbol_p(role_value))) {
        scm_wrong_type_arg_msg("set-window-role!", 3, role_value, "symbol or #f");
    }
    if (!host.services.set_window_role) {
        scm_misc_error("set-window-role!", "window role capability is unavailable", SCM_EOL);
    }
    try {
        const std::optional<std::string> role =
            scheme_false(role_value)
                ? std::nullopt
                : std::optional(scheme_name(role_value, "set-window-role!", 3));
        const std::expected<void, std::string> changed =
            host.services.set_window_role(window, role);
        if (!changed) {
            raise_host_error("set-window-role!", changed.error());
        }
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("set-window-role!", exception.what());
    } catch (...) {
        scm_misc_error("set-window-role!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

SCM window_pinned(SCM host_object, SCM window_value) {
    HostLease& host = require_host(host_object, "window-pinned?");
    const WindowId window = entity_id_from_scheme<WindowTag>(window_value, "window-pinned?", 2);
    try {
        return scm_from_bool(host.runtime->windows().get(window).pinned());
    } catch (const std::exception& exception) {
        raise_host_error("window-pinned?", exception.what());
    } catch (...) {
        scm_misc_error("window-pinned?", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM set_window_pinned(SCM host_object, SCM window_value, SCM pinned_value) {
    if (!scheme_boolean(pinned_value)) {
        scm_wrong_type_arg_msg("set-window-pinned!", 3, pinned_value, "boolean");
    }
    HostLease& host = require_host(host_object, "set-window-pinned!");
    const WindowId window = entity_id_from_scheme<WindowTag>(window_value, "set-window-pinned!", 2);
    if (!host.services.set_window_pinned) {
        scm_misc_error("set-window-pinned!", "window pin capability is unavailable", SCM_EOL);
    }
    try {
        const std::expected<void, std::string> changed =
            host.services.set_window_pinned(window, scheme_true(pinned_value));
        if (!changed) {
            raise_host_error("set-window-pinned!", changed.error());
        }
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("set-window-pinned!", exception.what());
    } catch (...) {
        scm_misc_error("set-window-pinned!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

SCM window_created_by_policy(SCM host_object, SCM window_value) {
    HostLease& host = require_host(host_object, "window-created-by-policy?");
    const WindowId window =
        entity_id_from_scheme<WindowTag>(window_value, "window-created-by-policy?", 2);
    try {
        return scm_from_bool(host.runtime->windows().get(window).created_by_policy());
    } catch (const std::exception& exception) {
        raise_host_error("window-created-by-policy?", exception.what());
    } catch (...) {
        scm_misc_error("window-created-by-policy?", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM workbench_slot(SCM host_object, SCM workbench_value, SCM role_value) {
    if (!scheme_true(scm_symbol_p(role_value))) {
        scm_wrong_type_arg_msg("workbench-slot", 3, role_value, "symbol");
    }
    HostLease& host = require_host(host_object, "workbench-slot");
    const WorkbenchId workbench =
        entity_id_from_scheme<WorkbenchTag>(workbench_value, "workbench-slot", 2);
    if (!host.services.workbench_slot) {
        scm_misc_error("workbench-slot", "workbench slot capability is unavailable", SCM_EOL);
    }
    try {
        const std::optional<WindowId> window =
            host.services.workbench_slot(workbench, scheme_name(role_value, "workbench-slot", 3));
        return window ? entity_id(window->slot, window->generation) : SCM_BOOL_F;
    } catch (const std::exception& exception) {
        raise_host_error("workbench-slot", exception.what());
    } catch (...) {
        scm_misc_error("workbench-slot", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM focus_window(SCM host_object, SCM window_value) {
    HostLease& host = require_host(host_object, "focus-window!");
    const WindowId window = entity_id_from_scheme<WindowTag>(window_value, "focus-window!", 2);
    if (!host.services.focus_window) {
        scm_misc_error("focus-window!", "window-focus capability is unavailable", SCM_EOL);
    }
    try {
        const std::expected<void, std::string> selected = host.services.focus_window(window);
        return selected ? SCM_BOOL_F : scm_from_utf8_string(selected.error().c_str());
    } catch (const std::exception& exception) {
        raise_host_error("focus-window!", exception.what());
    } catch (...) {
        scm_misc_error("focus-window!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM interaction_mechanism_status(SCM host_object) {
    HostLease& host = require_host(host_object, "interaction-mechanism-status");
    try {
        const GuileInteractionMechanismStatus status =
            host.services.interaction_mechanism_status
                ? host.services.interaction_mechanism_status()
                : GuileInteractionMechanismStatus{};
        SCM result = scm_c_make_vector(5, SCM_UNSPECIFIED);
        scm_c_vector_set_x(result, 0, scm_from_bool(status.active));
        scm_c_vector_set_x(result, 1, scm_from_size_t(status.candidate_count));
        scm_c_vector_set_x(result, 2,
                           status.buffer ? entity_id(status.buffer->slot, status.buffer->generation)
                                         : SCM_BOOL_F);
        scm_c_vector_set_x(result, 3,
                           status.view ? entity_id(status.view->slot, status.view->generation)
                                       : SCM_BOOL_F);
        scm_c_vector_set_x(result, 4, scm_from_uint64(status.candidate_revision));
        return result;
    } catch (const std::exception& exception) {
        raise_host_error("interaction-mechanism-status", exception.what());
    } catch (...) {
        scm_misc_error("interaction-mechanism-status", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM interaction_origin_project(SCM host_object) {
    HostLease& host = require_host(host_object, "interaction-origin-project");
    try {
        const std::optional<ProjectId> project = host.services.interaction_origin_project
                                                     ? host.services.interaction_origin_project()
                                                     : std::nullopt;
        return project ? entity_id(project->slot, project->generation) : SCM_BOOL_F;
    } catch (const std::exception& exception) {
        raise_host_error("interaction-origin-project", exception.what());
    } catch (...) {
        scm_misc_error("interaction-origin-project", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM refresh_interaction_mechanism(SCM host_object, SCM provider_value) {
    if (!scm_is_string(provider_value)) {
        scm_wrong_type_arg_msg("refresh-interaction-mechanism!", 2, provider_value, "string");
    }
    HostLease& host = require_host(host_object, "refresh-interaction-mechanism!");
    if (!host.services.refresh_interaction) {
        scm_misc_error("refresh-interaction-mechanism!",
                       "interaction refresh capability is unavailable", SCM_EOL);
    }
    try {
        const std::expected<void, std::string> refreshed =
            host.services.refresh_interaction(scheme_string(provider_value));
        if (!refreshed) {
            raise_host_error("refresh-interaction-mechanism!", refreshed.error());
        }
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("refresh-interaction-mechanism!", exception.what());
    } catch (...) {
        scm_misc_error("refresh-interaction-mechanism!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

SCM submit_interaction_mechanism(SCM host_object, SCM selected_value,
                                 SCM allow_custom_input_value) {
    HostLease& host = require_host(host_object, "submit-interaction-mechanism!");
    if (!host.services.submit_interaction) {
        scm_misc_error("submit-interaction-mechanism!",
                       "interaction submission capability is unavailable", SCM_EOL);
    }
    try {
        if (!scheme_boolean(allow_custom_input_value)) {
            scm_wrong_type_arg_msg("submit-interaction-mechanism!", 3, allow_custom_input_value,
                                   "boolean");
        }
        std::optional<std::size_t> selected;
        if (!scheme_false(selected_value)) {
            if (scm_is_unsigned_integer(selected_value, 0,
                                        std::numeric_limits<std::size_t>::max()) == 0) {
                scm_wrong_type_arg_msg("submit-interaction-mechanism!", 2, selected_value,
                                       "non-negative integer or #f");
            }
            selected = scm_to_size_t(selected_value);
        }
        std::expected<std::string, std::string> submitted =
            host.services.submit_interaction(selected, scheme_true(allow_custom_input_value));
        if (!submitted) {
            raise_host_error("submit-interaction-mechanism!", submitted.error());
        }
        return scm_from_utf8_stringn(submitted->data(), submitted->size());
    } catch (const std::exception& exception) {
        raise_host_error("submit-interaction-mechanism!", exception.what());
    } catch (...) {
        scm_misc_error("submit-interaction-mechanism!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM replace_interaction_input(SCM host_object, SCM input_value) {
    if (!scm_is_string(input_value)) {
        scm_wrong_type_arg_msg("replace-interaction-input!", 2, input_value, "string");
    }
    HostLease& host = require_host(host_object, "replace-interaction-input!");
    if (!host.services.replace_interaction_input) {
        scm_misc_error("replace-interaction-input!",
                       "interaction input replacement capability is unavailable", SCM_EOL);
    }
    try {
        const std::expected<RevisionId, std::string> revision =
            host.services.replace_interaction_input(scheme_string(input_value));
        if (!revision) {
            raise_host_error("replace-interaction-input!", revision.error());
        }
        return scm_from_uint64(*revision);
    } catch (const std::exception& exception) {
        raise_host_error("replace-interaction-input!", exception.what());
    } catch (...) {
        scm_misc_error("replace-interaction-input!", "unknown C++ host failure", SCM_EOL);
    }
    return scm_from_uint64(0);
}

SCM cancel_interaction_mechanism(SCM host_object) {
    HostLease& host = require_host(host_object, "cancel-interaction-mechanism!");
    if (!host.services.cancel_interaction) {
        scm_misc_error("cancel-interaction-mechanism!",
                       "interaction cancellation capability is unavailable", SCM_EOL);
    }
    try {
        return scm_from_bool(host.services.cancel_interaction());
    } catch (const std::exception& exception) {
        raise_host_error("cancel-interaction-mechanism!", exception.what());
    } catch (...) {
        scm_misc_error("cancel-interaction-mechanism!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM completion_active(SCM host_object) {
    HostLease& host = require_host(host_object, "completion-active?");
    if (!host.services.completion_active) {
        return SCM_BOOL_F;
    }
    try {
        return scm_from_bool(host.services.completion_active());
    } catch (const std::exception& exception) {
        raise_host_error("completion-active?", exception.what());
    } catch (...) {
        scm_misc_error("completion-active?", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM refresh_completion(SCM host_object) {
    HostLease& host = require_host(host_object, "refresh-completion!");
    if (!host.services.refresh_completion) {
        return SCM_BOOL_F;
    }
    try {
        const std::expected<void, std::string> refreshed = host.services.refresh_completion();
        return refreshed ? SCM_BOOL_F : scm_from_utf8_string(refreshed.error().c_str());
    } catch (const std::exception& exception) {
        raise_host_error("refresh-completion!", exception.what());
    } catch (...) {
        scm_misc_error("refresh-completion!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM start_completion(SCM host_object, SCM context_value, SCM anchor_value, SCM providers_value,
                     SCM trigger_value) {
    HostLease& host = require_host(host_object, "start-completion!");
    if (!host.services.start_completion) {
        scm_misc_error("start-completion!", "completion capability is unavailable", SCM_EOL);
    }
    try {
        const CommandContext context =
            command_context_from_scheme(host, context_value, "start-completion!");
        const TextOffset anchor = text_offset_from_scheme(anchor_value, "start-completion!", 3);
        CompletionTrigger trigger;
        if (symbol_is(trigger_value, "manual")) {
            trigger.kind = CompletionTriggerKind::Manual;
        } else if (symbol_is(trigger_value, "automatic")) {
            trigger.kind = CompletionTriggerKind::Automatic;
        } else {
            scm_wrong_type_arg_msg("start-completion!", 5, trigger_value, "'manual or 'automatic");
        }
        if (!scm_is_vector(providers_value)) {
            scm_wrong_type_arg_msg("start-completion!", 4, providers_value,
                                   "completion provider vector");
        }
        const CommandTarget target{.window = context.window_id(),
                                   .buffer = context.buffer_id(),
                                   .view = context.view_id()};
        std::vector<CompletionProvider> providers;
        const std::size_t provider_count = scm_c_vector_length(providers_value);
        providers.reserve(provider_count);
        for (std::size_t index = 0; index < provider_count; ++index) {
            const SCM value = scm_c_vector_ref(providers_value, index);
            if (scm_is_string(value)) {
                const std::string name = scheme_string(value);
                if (const auto provider = host.state->completion_providers_by_name.find(name);
                    provider != host.state->completion_providers_by_name.end()) {
                    providers.push_back(CompletionProvider::scripted(provider->second));
                } else if (name == "snippet") {
                    providers.push_back(CompletionProvider::snippet());
                } else {
                    raise_host_error("start-completion!",
                                     std::format("unknown completion provider '{}'", name));
                }
            } else if (scm_is_vector(value)) {
                if (!host.services.resolve_lsp_completion_provider) {
                    raise_host_error("start-completion!",
                                     "LSP completion capability is unavailable");
                }
                std::expected<CompletionProvider, std::string> resolved =
                    host.services.resolve_lsp_completion_provider(
                        target, script_lsp_provider_from_scheme(value, "start-completion!", 4));
                if (!resolved) {
                    raise_host_error("start-completion!", resolved.error());
                }
                providers.push_back(*resolved);
            } else {
                scm_wrong_type_arg_msg("start-completion!", 4, value,
                                       "completion provider name or LSP provider specification");
            }
        }
        std::expected<void, std::string> started = host.services.start_completion(
            target, anchor, std::move(providers), std::move(trigger));
        if (!started) {
            raise_host_error("start-completion!", started.error());
        }
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("start-completion!", exception.what());
    } catch (...) {
        scm_misc_error("start-completion!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

SCM move_completion(SCM host_object, SCM delta_value) {
    if (scm_is_integer(delta_value) == 0) {
        scm_wrong_type_arg_msg("move-completion!", 2, delta_value, "integer");
    }
    HostLease& host = require_host(host_object, "move-completion!");
    if (!host.services.move_completion) {
        scm_misc_error("move-completion!", "completion navigation capability is unavailable",
                       SCM_EOL);
    }
    try {
        return scm_from_bool(host.services.move_completion(scm_to_int64(delta_value)));
    } catch (const std::exception& exception) {
        raise_host_error("move-completion!", exception.what());
    } catch (...) {
        scm_misc_error("move-completion!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM apply_completion(SCM host_object, SCM replace_value) {
    HostLease& host = require_host(host_object, "apply-completion!");
    if (!host.services.apply_completion) {
        scm_misc_error("apply-completion!", "completion application capability is unavailable",
                       SCM_EOL);
    }
    try {
        std::expected<void, std::string> applied =
            host.services.apply_completion(scheme_true(replace_value));
        if (!applied) {
            raise_host_error("apply-completion!", applied.error());
        }
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("apply-completion!", exception.what());
    } catch (...) {
        scm_misc_error("apply-completion!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

SCM cancel_completion(SCM host_object) {
    HostLease& host = require_host(host_object, "cancel-completion!");
    if (!host.services.cancel_completion) {
        scm_misc_error("cancel-completion!", "completion cancellation capability is unavailable",
                       SCM_EOL);
    }
    try {
        return scm_from_bool(host.services.cancel_completion());
    } catch (const std::exception& exception) {
        raise_host_error("cancel-completion!", exception.what());
    } catch (...) {
        scm_misc_error("cancel-completion!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM cancel_pending_input(SCM host_object) {
    HostLease& host = require_host(host_object, "cancel-pending-input!");
    if (!host.services.cancel_pending_input) {
        scm_misc_error("cancel-pending-input!", "command input capability is unavailable", SCM_EOL);
    }
    try {
        host.services.cancel_pending_input();
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("cancel-pending-input!", exception.what());
    } catch (...) {
        scm_misc_error("cancel-pending-input!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

SCM view_position(SCM host_object, SCM view_value) {
    HostLease& host = require_host(host_object, "view-position");
    const ViewId view = entity_id_from_scheme<ViewTag>(view_value, "view-position", 2);
    if (!host.services.view_position) {
        scm_misc_error("view-position", "view position capability is unavailable", SCM_EOL);
    }
    try {
        const GuileViewPosition position = host.services.view_position(view);
        SCM result = scm_c_make_vector(5, SCM_UNSPECIFIED);
        scm_c_vector_set_x(result, 0, scm_from_uint32(position.line));
        scm_c_vector_set_x(result, 1, scm_from_uint32(position.line_count));
        scm_c_vector_set_x(result, 2, scm_from_uint32(position.display_column));
        scm_c_vector_set_x(result, 3, scm_from_uint32(position.byte));
        scm_c_vector_set_x(result, 4, scm_from_uint32(position.byte_count));
        return result;
    } catch (const std::exception& exception) {
        raise_host_error("view-position", exception.what());
    } catch (...) {
        scm_misc_error("view-position", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM view_line_prefix(SCM host_object, SCM view_value) {
    HostLease& host = require_host(host_object, "view-line-prefix");
    const ViewId view = entity_id_from_scheme<ViewTag>(view_value, "view-line-prefix", 2);
    if (!host.services.view_line_prefix) {
        scm_misc_error("view-line-prefix", "view line query capability is unavailable", SCM_EOL);
    }
    try {
        const GuileViewLinePrefix prefix = host.services.view_line_prefix(view);
        SCM result = scm_c_make_vector(3, SCM_UNSPECIFIED);
        scm_c_vector_set_x(result, 0, scm_from_uint32(prefix.line_start));
        scm_c_vector_set_x(result, 1, scm_from_uint32(prefix.caret));
        scm_c_vector_set_x(result, 2,
                           scm_from_utf8_stringn(prefix.text.data(), prefix.text.size()));
        return result;
    } catch (const std::exception& exception) {
        raise_host_error("view-line-prefix", exception.what());
    } catch (...) {
        scm_misc_error("view-line-prefix", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM view_syntax_token(SCM host_object, SCM view_value, SCM offset_value) {
    HostLease& host = require_host(host_object, "view-syntax-token");
    const ViewId view = entity_id_from_scheme<ViewTag>(view_value, "view-syntax-token", 2);
    const TextOffset offset = text_offset_from_scheme(offset_value, "view-syntax-token", 3);
    if (!host.services.view_syntax_token) {
        scm_misc_error("view-syntax-token", "syntax query capability is unavailable", SCM_EOL);
    }
    try {
        const std::optional<GuileSyntaxToken> token = host.services.view_syntax_token(view, offset);
        if (!token) {
            return SCM_BOOL_F;
        }
        SCM result = scm_c_make_vector(3, SCM_UNSPECIFIED);
        scm_c_vector_set_x(result, 0, scm_from_utf8_symbol(token->kind.c_str()));
        scm_c_vector_set_x(result, 1, scm_from_uint32(token->start));
        scm_c_vector_set_x(result, 2, scm_from_uint32(token->end));
        return result;
    } catch (const std::exception& exception) {
        raise_host_error("view-syntax-token", exception.what());
    } catch (...) {
        scm_misc_error("view-syntax-token", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM view_identifier_words(SCM host_object, SCM view_value) {
    HostLease& host = require_host(host_object, "view-identifier-words");
    const ViewId view = entity_id_from_scheme<ViewTag>(view_value, "view-identifier-words", 2);
    if (!host.services.view_identifier_words) {
        scm_misc_error("view-identifier-words", "identifier query capability is unavailable",
                       SCM_EOL);
    }
    try {
        return string_vector_value(host.services.view_identifier_words(view));
    } catch (const std::exception& exception) {
        raise_host_error("view-identifier-words", exception.what());
    } catch (...) {
        scm_misc_error("view-identifier-words", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM location_navigation(SCM host_object) {
    HostLease& host = require_host(host_object, "location-navigation");
    if (!host.services.location_navigation) {
        SCM empty = scm_c_make_vector(3, SCM_BOOL_F);
        scm_c_vector_set_x(empty, 2, scm_from_size_t(0));
        return empty;
    }
    try {
        const GuileLocationNavigation navigation = host.services.location_navigation();
        SCM result = scm_c_make_vector(3, SCM_BOOL_F);
        if (navigation.buffer) {
            scm_c_vector_set_x(result, 0,
                               entity_id(navigation.buffer->slot, navigation.buffer->generation));
        }
        if (navigation.selected_index) {
            scm_c_vector_set_x(result, 1, scm_from_size_t(*navigation.selected_index));
        }
        scm_c_vector_set_x(result, 2, scm_from_size_t(navigation.location_count));
        return result;
    } catch (const std::exception& exception) {
        raise_host_error("location-navigation", exception.what());
    } catch (...) {
        scm_misc_error("location-navigation", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM set_location_navigation(SCM host_object, SCM buffer_value, SCM selected_value) {
    HostLease& host = require_host(host_object, "set-location-navigation!");
    if (!host.services.set_location_navigation) {
        scm_misc_error("set-location-navigation!", "location navigation capability is unavailable",
                       SCM_EOL);
    }
    try {
        std::optional<BufferId> buffer;
        if (!scheme_false(buffer_value)) {
            buffer = entity_id_from_scheme<BufferTag>(buffer_value, "set-location-navigation!", 2);
        }
        std::optional<std::size_t> selected;
        if (!scheme_false(selected_value)) {
            selected = scm_to_size_t(selected_value);
        }
        const std::expected<void, std::string> updated =
            host.services.set_location_navigation(buffer, selected);
        if (!updated) {
            raise_host_error("set-location-navigation!", updated.error());
        }
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("set-location-navigation!", exception.what());
    } catch (...) {
        scm_misc_error("set-location-navigation!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

SCM location_list_target(SCM host_object, SCM buffer_value, SCM index_value) {
    HostLease& host = require_host(host_object, "location-list-target");
    std::optional<BufferId> buffer;
    if (!scheme_false(buffer_value)) {
        buffer = entity_id_from_scheme<BufferTag>(buffer_value, "location-list-target", 2);
    }
    if (scm_is_unsigned_integer(index_value, 0, std::numeric_limits<std::size_t>::max()) == 0) {
        scm_wrong_type_arg_msg("location-list-target", 3, index_value, "non-negative integer");
    }
    if (!host.services.location_target) {
        scm_misc_error("location-list-target", "location target capability is unavailable",
                       SCM_EOL);
    }
    try {
        const std::optional<GuileLocationTarget> target =
            host.services.location_target(buffer, scm_to_size_t(index_value));
        if (!target) {
            return SCM_BOOL_F;
        }
        SCM result = scm_c_make_vector(5, SCM_UNSPECIFIED);
        scm_c_vector_set_x(result, 0, scm_from_utf8_string(target->resource.c_str()));
        scm_c_vector_set_x(result, 1, scm_from_uint32(target->position.line));
        scm_c_vector_set_x(result, 2, scm_from_uint32(target->position.column));
        scm_c_vector_set_x(result, 3, scm_from_bool(target->stale));
        scm_c_vector_set_x(result, 4, position_encoding_value(target->position.encoding));
        return result;
    } catch (const std::exception& exception) {
        raise_host_error("location-list-target", exception.what());
    } catch (...) {
        scm_misc_error("location-list-target", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM move_location_list(SCM host_object, SCM delta_value) {
    HostLease& host = require_host(host_object, "move-location-list!");
    if (scm_is_signed_integer(delta_value, std::numeric_limits<int>::min(),
                              std::numeric_limits<int>::max()) == 0) {
        scm_wrong_type_arg_msg("move-location-list!", 2, delta_value, "integer");
    }
    if (!host.services.move_location_list) {
        scm_misc_error("move-location-list!", "location list stack capability is unavailable",
                       SCM_EOL);
    }
    return scm_from_bool(host.services.move_location_list(scm_to_int(delta_value)));
}

SCM position_buffer_view(SCM host_object, SCM window_value, SCM buffer_value, SCM offset_value) {
    HostLease& host = require_host(host_object, "position-buffer-view!");
    const WindowId window =
        entity_id_from_scheme<WindowTag>(window_value, "position-buffer-view!", 2);
    const BufferId buffer =
        entity_id_from_scheme<BufferTag>(buffer_value, "position-buffer-view!", 3);
    const TextOffset offset = text_offset_from_scheme(offset_value, "position-buffer-view!", 4);
    if (!host.services.position_buffer_view) {
        scm_misc_error("position-buffer-view!", "buffer view capability is unavailable", SCM_EOL);
    }
    try {
        const std::expected<void, std::string> positioned =
            host.services.position_buffer_view(window, buffer, offset.value);
        if (!positioned) {
            raise_host_error("position-buffer-view!", positioned.error());
        }
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("position-buffer-view!", exception.what());
    } catch (...) {
        scm_misc_error("position-buffer-view!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

void initialize_host_module(void*) {
    host_type = scm_make_foreign_object_type(scm_from_utf8_symbol("cind-editor-host"),
                                             scm_list_1(scm_from_utf8_symbol("implementation")),
                                             finalize_host);
    (void)scm_gc_protect_object(host_type);
    (void)scm_c_define_gsubr("define-command!", 4, 0, 0,
                             reinterpret_cast<scm_t_subr>(define_command));
    (void)scm_c_define_gsubr("set-command-documentation!", 3, 0, 0,
                             reinterpret_cast<scm_t_subr>(set_command_documentation));
    (void)scm_c_define_gsubr("define-interaction-provider!", 3, 0, 0,
                             reinterpret_cast<scm_t_subr>(define_interaction_provider));
    (void)scm_c_define_gsubr("define-completion-provider!", 3, 1, 0,
                             reinterpret_cast<scm_t_subr>(define_completion_provider));
    (void)scm_c_define_gsubr("bind-key-if-command!", 4, 0, 0,
                             reinterpret_cast<scm_t_subr>(bind_key_if_command));
    (void)scm_c_define_gsubr("define-keymap!", 3, 0, 0,
                             reinterpret_cast<scm_t_subr>(define_keymap));
    (void)scm_c_define_gsubr("bind-key!", 4, 0, 0, reinterpret_cast<scm_t_subr>(bind_key));
    (void)scm_c_define_gsubr("bind-remap!", 4, 0, 0, reinterpret_cast<scm_t_subr>(bind_remap));
    (void)scm_c_define_gsubr("keymap-bindings", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(keymap_bindings));
    (void)scm_c_define_gsubr("resolve-key-sequence", 3, 0, 0,
                             reinterpret_cast<scm_t_subr>(resolve_key_sequence));
    (void)scm_c_define_gsubr("keymap-context-snapshot", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(keymap_context_snapshot));
    (void)scm_c_define_gsubr("key-sequence-completions", 3, 0, 0,
                             reinterpret_cast<scm_t_subr>(key_sequence_completions));
    (void)scm_c_define_gsubr("set-input-feedback!", 4, 0, 0,
                             reinterpret_cast<scm_t_subr>(set_input_feedback));
    (void)scm_c_define_gsubr("clear-input-feedback!", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(clear_input_feedback));
    (void)scm_c_define_gsubr("%define-input-state!", 8, 0, 0,
                             reinterpret_cast<scm_t_subr>(define_input_state));
    (void)scm_c_define_gsubr("set-input-state-lifecycle!", 4, 0, 0,
                             reinterpret_cast<scm_t_subr>(set_input_state_lifecycle));
    (void)scm_c_define_gsubr("set-input-state-position-hints!", 3, 0, 0,
                             reinterpret_cast<scm_t_subr>(set_input_state_position_hints));
    (void)scm_c_define_gsubr("define-input-strategy!", 5, 0, 0,
                             reinterpret_cast<scm_t_subr>(define_input_strategy));
    (void)scm_c_define_gsubr("set-default-input-strategy!", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(set_default_input_strategy));
    (void)scm_c_define_gsubr("set-view-input-strategy!", 3, 0, 0,
                             reinterpret_cast<scm_t_subr>(set_view_input_strategy));
    (void)scm_c_define_gsubr("view-input-strategy", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(view_input_strategy));
    (void)scm_c_define_gsubr("set-base-input-state!", 3, 0, 0,
                             reinterpret_cast<scm_t_subr>(set_base_input_state));
    (void)scm_c_define_gsubr("push-input-state!", 3, 0, 0,
                             reinterpret_cast<scm_t_subr>(push_input_state));
    (void)scm_c_define_gsubr("pop-input-state!", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(pop_input_state));
    (void)scm_c_define_gsubr("reset-input-states!", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(reset_input_states));
    (void)scm_c_define_gsubr("view-input-states", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(view_input_states));
    (void)scm_c_define_gsubr("observe-input-state-changes!", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(observe_input_state_changes));
    (void)scm_c_define_gsubr("define-thing!", 3, 0, 0, reinterpret_cast<scm_t_subr>(define_thing));
    (void)scm_c_define_gsubr("define-motion!", 3, 0, 0,
                             reinterpret_cast<scm_t_subr>(define_motion));
    (void)scm_c_define_gsubr("define-language-profile!", 4, 0, 0,
                             reinterpret_cast<scm_t_subr>(define_language_profile));
    (void)scm_c_define_gsubr("%define-mode!", 10, 0, 0, reinterpret_cast<scm_t_subr>(define_mode));
    (void)scm_c_define_gsubr("set-mode-completion-auto!", 3, 0, 0,
                             reinterpret_cast<scm_t_subr>(set_mode_completion_auto));
    (void)scm_c_define_gsubr("define-file-mode-rule!", 5, 0, 0,
                             reinterpret_cast<scm_t_subr>(define_file_mode_rule));
    (void)scm_c_define_gsubr("define-project-provider!", 3, 0, 0,
                             reinterpret_cast<scm_t_subr>(define_project_provider));
    (void)scm_c_define_gsubr("mode-properties", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(mode_properties));
    (void)scm_c_define_gsubr("buffer-language-facet?", 3, 0, 0,
                             reinterpret_cast<scm_t_subr>(buffer_language_facet_p));
    (void)scm_c_define_gsubr("set-buffer-major-mode!", 3, 0, 0,
                             reinterpret_cast<scm_t_subr>(set_buffer_major_mode));
    (void)scm_c_define_gsubr("set-buffer-minor-mode!", 4, 0, 0,
                             reinterpret_cast<scm_t_subr>(set_buffer_minor_mode));
    (void)scm_c_define_gsubr("buffer-mode-policy", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(buffer_mode_policy));
    (void)scm_c_define_gsubr("buffer-mode-summary", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(buffer_mode_summary));
    (void)scm_c_define_gsubr("observe-mode-policy-changes!", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(observe_mode_policy_changes));
    (void)scm_c_define_gsubr("enabled-command-names", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(enabled_command_names));
    (void)scm_c_define_gsubr("command-properties", 3, 0, 0,
                             reinterpret_cast<scm_t_subr>(command_properties));
    (void)scm_c_define_gsubr("open-buffer-summaries", 1, 0, 0,
                             reinterpret_cast<scm_t_subr>(open_buffer_summaries));
    (void)scm_c_define_gsubr("workbench-list", 1, 0, 0,
                             reinterpret_cast<scm_t_subr>(workbench_list));
    (void)scm_c_define_gsubr("current-workbench", 1, 0, 0,
                             reinterpret_cast<scm_t_subr>(current_workbench));
    (void)scm_c_define_gsubr("workbench-scope", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(workbench_scope));
    (void)scm_c_define_gsubr("workbench-mru", 2, 0, 0, reinterpret_cast<scm_t_subr>(workbench_mru));
    (void)scm_c_define_gsubr("workbench-buffer-summaries", 3, 0, 0,
                             reinterpret_cast<scm_t_subr>(workbench_buffer_summaries));
    (void)scm_c_define_gsubr("workbench-buffer-ids", 3, 0, 0,
                             reinterpret_cast<scm_t_subr>(workbench_buffer_ids));
    (void)scm_c_define_gsubr("new-workbench!", 3, 0, 0,
                             reinterpret_cast<scm_t_subr>(create_workbench));
    (void)scm_c_define_gsubr("switch-workbench!", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(switch_workbench));
    (void)scm_c_define_gsubr("close-workbench!", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(close_workbench));
    (void)scm_c_define_gsubr("adopt-project!", 3, 0, 0,
                             reinterpret_cast<scm_t_subr>(adopt_project));
    (void)scm_c_define_gsubr("expel-buffer!", 3, 0, 0, reinterpret_cast<scm_t_subr>(expel_buffer));
    (void)scm_c_define_gsubr("workbench-session-state", 1, 0, 0,
                             reinterpret_cast<scm_t_subr>(workbench_session_state));
    (void)scm_c_define_gsubr("prepare-workbench-session-restore!", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(prepare_workbench_session_restore));
    (void)scm_c_define_gsubr("show-buffer-in-window!", 4, 0, 0,
                             reinterpret_cast<scm_t_subr>(show_buffer_in_window));
    (void)scm_c_define_gsubr("replace-workbench-mru!", 3, 0, 0,
                             reinterpret_cast<scm_t_subr>(replace_workbench_mru));
    (void)scm_c_define_gsubr("project-list", 1, 0, 0, reinterpret_cast<scm_t_subr>(project_list));
    (void)scm_c_define_gsubr("owned-user-modules", 1, 0, 0,
                             reinterpret_cast<scm_t_subr>(owned_user_modules));
    (void)scm_c_define_gsubr("project-root", 2, 0, 0, reinterpret_cast<scm_t_subr>(project_root));
    (void)scm_c_define_gsubr("project-files", 2, 0, 0, reinterpret_cast<scm_t_subr>(project_files));
    (void)scm_c_define_gsubr("path-relative", 3, 0, 0, reinterpret_cast<scm_t_subr>(path_relative));
    (void)scm_c_define_gsubr("path-filename", 2, 0, 0, reinterpret_cast<scm_t_subr>(path_filename));
    (void)scm_c_define_gsubr("path-resolve", 3, 0, 0, reinterpret_cast<scm_t_subr>(path_resolve));
    (void)scm_c_define_gsubr("active-key-bindings", 1, 0, 0,
                             reinterpret_cast<scm_t_subr>(active_key_bindings));
    (void)scm_c_define_gsubr("view-caret", 2, 0, 0, reinterpret_cast<scm_t_subr>(view_caret));
    (void)scm_c_define_gsubr("view-mark", 2, 0, 0, reinterpret_cast<scm_t_subr>(view_mark));
    (void)scm_c_define_gsubr("view-selection", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(view_selection));
    (void)scm_c_define_gsubr("set-selection!", 3, 0, 0,
                             reinterpret_cast<scm_t_subr>(set_selection));
    (void)scm_c_define_gsubr("clear-selection!", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(clear_selection));
    (void)scm_c_define_gsubr("push-selection-history!", 3, 0, 0,
                             reinterpret_cast<scm_t_subr>(push_selection_history));
    (void)scm_c_define_gsubr("pop-selection-history!", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(pop_selection_history));
    (void)scm_c_define_gsubr("clear-selection-history!", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(clear_selection_history));
    (void)scm_c_define_gsubr("selection-history-depth", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(selection_history_depth));
    (void)scm_c_define_gsubr("replace-selection!", 4, 0, 0,
                             reinterpret_cast<scm_t_subr>(replace_selection));
    (void)scm_c_define_gsubr("selection-texts", 3, 0, 0,
                             reinterpret_cast<scm_t_subr>(selection_texts));
    (void)scm_c_define_gsubr("buffer-substring", 4, 0, 0,
                             reinterpret_cast<scm_t_subr>(buffer_substring));
    (void)scm_c_define_gsubr("find-buffer-text", 5, 0, 0,
                             reinterpret_cast<scm_t_subr>(find_buffer_text));
    (void)scm_c_define_gsubr("erase-range!", 4, 0, 0, reinterpret_cast<scm_t_subr>(erase_range));
    (void)scm_c_define_gsubr("insert-text!", 3, 0, 0, reinterpret_cast<scm_t_subr>(insert_text));
    (void)scm_c_define_gsubr("soft-kill-range", 3, 0, 0,
                             reinterpret_cast<scm_t_subr>(soft_kill_range));
    (void)scm_c_define_gsubr("set-view-caret!", 3, 0, 0,
                             reinterpret_cast<scm_t_subr>(set_view_caret));
    (void)scm_c_define_gsubr("reset-preferred-column!", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(reset_preferred_column));
    (void)scm_c_define_gsubr("thing-selection", 5, 0, 0,
                             reinterpret_cast<scm_t_subr>(thing_selection));
    (void)scm_c_define_gsubr("motion-selection", 6, 0, 0,
                             reinterpret_cast<scm_t_subr>(motion_selection));
    (void)scm_c_define_gsubr("expand-node-selection", 3, 0, 0,
                             reinterpret_cast<scm_t_subr>(expand_node_selection));
    (void)scm_c_define_gsubr("write-clipboard!", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(write_clipboard));
    (void)scm_c_define_gsubr("read-clipboard", 1, 0, 0,
                             reinterpret_cast<scm_t_subr>(read_clipboard));
    (void)scm_c_define_gsubr("buffer-id-by-name", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(buffer_id_by_name));
    (void)scm_c_define_gsubr("buffer-name", 2, 0, 0, reinterpret_cast<scm_t_subr>(buffer_name));
    (void)scm_c_define_gsubr("buffer-resource", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(buffer_resource));
    (void)scm_c_define_gsubr("buffer-text", 2, 0, 0, reinterpret_cast<scm_t_subr>(buffer_text));
    (void)scm_c_define_gsubr("buffer-byte-size", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(buffer_byte_size));
    (void)scm_c_define_gsubr("buffer-editable-start", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(buffer_editable_start));
    (void)scm_c_define_gsubr("set-buffer-editable-start!", 3, 0, 0,
                             reinterpret_cast<scm_t_subr>(set_buffer_editable_start));
    (void)scm_c_define_gsubr("create-buffer-marker!", 4, 0, 0,
                             reinterpret_cast<scm_t_subr>(create_buffer_marker));
    (void)scm_c_define_gsubr("buffer-marker-offset", 3, 0, 0,
                             reinterpret_cast<scm_t_subr>(buffer_marker_offset));
    (void)scm_c_define_gsubr("remove-buffer-marker!", 3, 0, 0,
                             reinterpret_cast<scm_t_subr>(remove_buffer_marker));
    (void)scm_c_define_gsubr("buffer-locations", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(buffer_locations));
    (void)scm_c_define_gsubr("buffer-diagnostics", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(buffer_diagnostics));
    (void)scm_c_define_gsubr("set-buffer-locations!", 3, 0, 0,
                             reinterpret_cast<scm_t_subr>(set_buffer_locations));
    (void)scm_c_define_gsubr("set-location-list!", 5, 0, 0,
                             reinterpret_cast<scm_t_subr>(set_location_list));
    (void)scm_c_define_gsubr("buffer-read-only?", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(buffer_read_only_p));
    (void)scm_c_define_gsubr("path-parent", 2, 0, 0, reinterpret_cast<scm_t_subr>(path_parent));
    (void)scm_c_define_gsubr("directory-path?", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(directory_path_p));
    (void)scm_c_define_gsubr("path-as-directory", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(path_as_directory));
    (void)scm_c_define_gsubr("display-buffer!", 4, 0, 0,
                             reinterpret_cast<scm_t_subr>(display_buffer));
    (void)scm_c_define_gsubr("display-buffer-at!", 6, 0, 0,
                             reinterpret_cast<scm_t_subr>(display_buffer_at));
    (void)scm_c_define_gsubr("display-buffer-position-at!", 7, 0, 0,
                             reinterpret_cast<scm_t_subr>(display_buffer_position_at));
    (void)scm_c_define_gsubr("navigate-jump!", 3, 0, 0,
                             reinterpret_cast<scm_t_subr>(navigate_jump));
    (void)scm_c_define_gsubr("mark-jump!", 2, 0, 0, reinterpret_cast<scm_t_subr>(mark_jump));
    (void)scm_c_define_gsubr("visit-jump!", 3, 0, 0, reinterpret_cast<scm_t_subr>(visit_jump));
    (void)scm_c_define_gsubr("link-jump!", 6, 0, 0, reinterpret_cast<scm_t_subr>(link_jump));
    (void)scm_c_define_gsubr("jump-branches", 3, 0, 0, reinterpret_cast<scm_t_subr>(jump_branches));
    (void)scm_c_define_gsubr("jump-node", 3, 0, 0, reinterpret_cast<scm_t_subr>(jump_node));
    (void)scm_c_define_gsubr("evict-jumps!", 3, 0, 0, reinterpret_cast<scm_t_subr>(evict_jumps));
    (void)scm_c_define_gsubr("display-generated-buffer!", 7, 0, 0,
                             reinterpret_cast<scm_t_subr>(display_generated_buffer));
    (void)scm_c_define_gsubr("evaluate-scheme!", 3, 0, 0,
                             reinterpret_cast<scm_t_subr>(evaluate_scheme));
    (void)scm_c_define_gsubr("move-caret-to-line!", 4, 0, 0,
                             reinterpret_cast<scm_t_subr>(move_caret_to_line));
    (void)scm_c_define_gsubr("scroll-view-lines!", 3, 0, 0,
                             reinterpret_cast<scm_t_subr>(scroll_view_lines));
    (void)scm_c_define_gsubr("undo!", 2, 0, 0, reinterpret_cast<scm_t_subr>(undo_edit));
    (void)scm_c_define_gsubr("redo!", 2, 0, 0, reinterpret_cast<scm_t_subr>(redo_edit));
    (void)scm_c_define_gsubr("move-caret-lines!", 3, 0, 0,
                             reinterpret_cast<scm_t_subr>(move_caret_lines));
    (void)scm_c_define_gsubr("move-caret-line-boundary!", 3, 0, 0,
                             reinterpret_cast<scm_t_subr>(move_caret_line_boundary));
    (void)scm_c_define_gsubr("delete-grapheme!", 4, 0, 0,
                             reinterpret_cast<scm_t_subr>(delete_grapheme));
    (void)scm_c_define_gsubr("newline!", 2, 0, 0, reinterpret_cast<scm_t_subr>(newline_edit));
    (void)scm_c_define_gsubr("indent!", 2, 0, 0, reinterpret_cast<scm_t_subr>(indent_edit));
    (void)scm_c_define_gsubr("type-text!", 3, 0, 0, reinterpret_cast<scm_t_subr>(type_text));
    (void)scm_c_define_gsubr("structural-edit!", 3, 0, 0,
                             reinterpret_cast<scm_t_subr>(structural_edit));
    (void)scm_c_define_gsubr("page-rows", 1, 0, 0, reinterpret_cast<scm_t_subr>(page_rows));
    (void)scm_c_define_gsubr("interaction-mechanism-status", 1, 0, 0,
                             reinterpret_cast<scm_t_subr>(interaction_mechanism_status));
    (void)scm_c_define_gsubr("interaction-origin-project", 1, 0, 0,
                             reinterpret_cast<scm_t_subr>(interaction_origin_project));
    (void)scm_c_define_gsubr("refresh-interaction-mechanism!", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(refresh_interaction_mechanism));
    (void)scm_c_define_gsubr("submit-interaction-mechanism!", 3, 0, 0,
                             reinterpret_cast<scm_t_subr>(submit_interaction_mechanism));
    (void)scm_c_define_gsubr("replace-interaction-input!", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(replace_interaction_input));
    (void)scm_c_define_gsubr("cancel-interaction-mechanism!", 1, 0, 0,
                             reinterpret_cast<scm_t_subr>(cancel_interaction_mechanism));
    (void)scm_c_define_gsubr("completion-active?", 1, 0, 0,
                             reinterpret_cast<scm_t_subr>(completion_active));
    (void)scm_c_define_gsubr("refresh-completion!", 1, 0, 0,
                             reinterpret_cast<scm_t_subr>(refresh_completion));
    (void)scm_c_define_gsubr("start-completion!", 5, 0, 0,
                             reinterpret_cast<scm_t_subr>(start_completion));
    (void)scm_c_define_gsubr("move-completion!", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(move_completion));
    (void)scm_c_define_gsubr("apply-completion!", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(apply_completion));
    (void)scm_c_define_gsubr("cancel-completion!", 1, 0, 0,
                             reinterpret_cast<scm_t_subr>(cancel_completion));
    (void)scm_c_define_gsubr("cancel-pending-input!", 1, 0, 0,
                             reinterpret_cast<scm_t_subr>(cancel_pending_input));
    (void)scm_c_define_gsubr("view-position", 2, 0, 0, reinterpret_cast<scm_t_subr>(view_position));
    (void)scm_c_define_gsubr("view-line-prefix", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(view_line_prefix));
    (void)scm_c_define_gsubr("view-syntax-token", 3, 0, 0,
                             reinterpret_cast<scm_t_subr>(view_syntax_token));
    (void)scm_c_define_gsubr("view-identifier-words", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(view_identifier_words));
    (void)scm_c_define_gsubr("location-navigation", 1, 0, 0,
                             reinterpret_cast<scm_t_subr>(location_navigation));
    (void)scm_c_define_gsubr("set-location-navigation!", 3, 0, 0,
                             reinterpret_cast<scm_t_subr>(set_location_navigation));
    (void)scm_c_define_gsubr("location-list-target", 3, 0, 0,
                             reinterpret_cast<scm_t_subr>(location_list_target));
    (void)scm_c_define_gsubr("move-location-list!", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(move_location_list));
    (void)scm_c_define_gsubr("position-buffer-view!", 4, 0, 0,
                             reinterpret_cast<scm_t_subr>(position_buffer_view));
    (void)scm_c_define_gsubr("project-index-state", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(project_index_state));
    (void)scm_c_define_gsubr("request-project-index!", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(request_project_index));
    (void)scm_c_define_gsubr("normalize-resource-path", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(normalize_resource));
    (void)scm_c_define_gsubr("set-buffer-resource!", 3, 0, 0,
                             reinterpret_cast<scm_t_subr>(set_buffer_resource));
    (void)scm_c_define_gsubr("rename-buffer!", 3, 0, 0,
                             reinterpret_cast<scm_t_subr>(rename_buffer));
    (void)scm_c_define_gsubr("buffer-id-by-resource", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(buffer_id_by_resource));
    (void)scm_c_define_gsubr("resource-mode", 2, 0, 0, reinterpret_cast<scm_t_subr>(resource_mode));
    (void)scm_c_define_gsubr("project-for-resource", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(project_for_resource));
    (void)scm_c_define_gsubr("project-provider-definitions", 1, 0, 0,
                             reinterpret_cast<scm_t_subr>(project_provider_definitions));
    (void)scm_c_define_gsubr("project-id-by-root", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(project_id_by_root));
    (void)scm_c_define_gsubr("create-project!", 5, 0, 0,
                             reinterpret_cast<scm_t_subr>(create_project));
    (void)scm_c_define_gsubr("set-buffer-project!", 3, 0, 0,
                             reinterpret_cast<scm_t_subr>(set_buffer_project));
    (void)scm_c_define_gsubr("buffer-save-snapshot", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(buffer_save_snapshot));
    (void)scm_c_define_gsubr("mark-buffer-saved!", 3, 0, 0,
                             reinterpret_cast<scm_t_subr>(mark_buffer_saved));
    (void)scm_c_define_gsubr("open-buffer-ids", 1, 0, 0,
                             reinterpret_cast<scm_t_subr>(open_buffers));
    (void)scm_c_define_gsubr("create-buffer!", 9, 0, 0,
                             reinterpret_cast<scm_t_subr>(create_buffer));
    (void)scm_c_define_gsubr("buffer-modified?", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(buffer_modified_p));
    (void)scm_c_define_gsubr("release-buffer!", 3, 0, 0,
                             reinterpret_cast<scm_t_subr>(release_buffer));
    (void)scm_c_define_gsubr("split-window!", 3, 0, 0, reinterpret_cast<scm_t_subr>(split_window));
    (void)scm_c_define_gsubr("delete-window!", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(delete_window));
    (void)scm_c_define_gsubr("delete-other-windows!", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(delete_other_windows));
    (void)scm_c_define_gsubr("open-window-ids", 1, 0, 0,
                             reinterpret_cast<scm_t_subr>(open_windows));
    (void)scm_c_define_gsubr("active-window-id", 1, 0, 0,
                             reinterpret_cast<scm_t_subr>(active_window));
    (void)scm_c_define_gsubr("window-view-id", 2, 0, 0, reinterpret_cast<scm_t_subr>(window_view));
    (void)scm_c_define_gsubr("window-buffer-id", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(window_buffer));
    (void)scm_c_define_gsubr("window-role", 2, 0, 0, reinterpret_cast<scm_t_subr>(window_role));
    (void)scm_c_define_gsubr("set-window-role!", 3, 0, 0,
                             reinterpret_cast<scm_t_subr>(set_window_role));
    (void)scm_c_define_gsubr("window-pinned?", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(window_pinned));
    (void)scm_c_define_gsubr("set-window-pinned!", 3, 0, 0,
                             reinterpret_cast<scm_t_subr>(set_window_pinned));
    (void)scm_c_define_gsubr("window-created-by-policy?", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(window_created_by_policy));
    (void)scm_c_define_gsubr("workbench-slot", 3, 0, 0,
                             reinterpret_cast<scm_t_subr>(workbench_slot));
    (void)scm_c_define_gsubr("focus-window!", 2, 0, 0, reinterpret_cast<scm_t_subr>(focus_window));
    scm_c_export(
        "define-command!", "set-command-documentation!", "define-interaction-provider!",
        "define-completion-provider!", "define-keymap!", "bind-key!", "bind-key-if-command!",
        "bind-remap!", "keymap-bindings", "resolve-key-sequence", "keymap-context-snapshot",
        "key-sequence-completions", "set-input-feedback!", "clear-input-feedback!",
        "%define-input-state!", "set-input-state-lifecycle!", "set-input-state-position-hints!",
        "define-input-strategy!", "set-default-input-strategy!", "set-view-input-strategy!",
        "view-input-strategy", "set-base-input-state!", "push-input-state!", "pop-input-state!",
        "reset-input-states!", "view-input-states", "observe-input-state-changes!", "define-thing!",
        "define-motion!", "define-language-profile!", "%define-mode!", "set-mode-completion-auto!",
        "define-file-mode-rule!", "define-project-provider!", "mode-properties",
        "buffer-language-facet?", "set-buffer-major-mode!", "set-buffer-minor-mode!",
        "buffer-mode-policy", "buffer-mode-summary", "observe-mode-policy-changes!",
        "enabled-command-names", "command-properties", "open-buffer-summaries", "workbench-list",
        "current-workbench", "workbench-scope", "workbench-mru", "workbench-buffer-summaries",
        "workbench-buffer-ids", "new-workbench!", "switch-workbench!", "close-workbench!",
        "adopt-project!", "expel-buffer!", "workbench-session-state",
        "prepare-workbench-session-restore!", "show-buffer-in-window!", "replace-workbench-mru!",
        "project-list", "owned-user-modules", "project-root", "project-files", "path-relative",
        "path-filename", "path-resolve", "active-key-bindings", "buffer-id-by-name", "buffer-name",
        "buffer-resource", "buffer-text", "buffer-byte-size", "buffer-editable-start",
        "set-buffer-editable-start!", "create-buffer-marker!", "buffer-marker-offset",
        "remove-buffer-marker!", "buffer-locations", "buffer-diagnostics", "set-buffer-locations!",
        "set-location-list!", "buffer-read-only?", "path-parent", "directory-path?",
        "path-as-directory", "view-caret", "view-mark", "view-selection", "set-selection!",
        "clear-selection!", "push-selection-history!", "pop-selection-history!",
        "clear-selection-history!", "selection-history-depth", "replace-selection!",
        "selection-texts", "buffer-substring", "find-buffer-text", "erase-range!", "insert-text!",
        "soft-kill-range", "set-view-caret!", "reset-preferred-column!", "thing-selection",
        "motion-selection", "expand-node-selection", "write-clipboard!", "read-clipboard",
        "display-buffer!", "display-buffer-at!", "display-buffer-position-at!",
        "display-generated-buffer!", "navigate-jump!", "mark-jump!", "visit-jump!", "link-jump!",
        "jump-branches", "evaluate-scheme!", "move-caret-to-line!", "scroll-view-lines!", "undo!",
        "redo!", "move-caret-lines!", "move-caret-line-boundary!", "delete-grapheme!", "newline!",
        "indent!", "type-text!", "structural-edit!", "page-rows", "interaction-mechanism-status",
        "interaction-origin-project", "refresh-interaction-mechanism!",
        "submit-interaction-mechanism!", "replace-interaction-input!",
        "cancel-interaction-mechanism!", "cancel-pending-input!", "completion-active?",
        "refresh-completion!", "start-completion!", "move-completion!", "apply-completion!",
        "cancel-completion!", "view-position", "view-line-prefix", "view-syntax-token",
        "view-identifier-words", "location-navigation", "set-location-navigation!",
        "location-list-target", "move-location-list!", "position-buffer-view!",
        "project-index-state", "request-project-index!", "normalize-resource-path",
        "set-buffer-resource!", "rename-buffer!", "buffer-id-by-resource", "resource-mode",
        "project-for-resource", "project-provider-definitions", "project-id-by-root",
        "create-project!", "set-buffer-project!", "buffer-save-snapshot", "mark-buffer-saved!",
        "open-buffer-ids", "create-buffer!", "buffer-modified?", "release-buffer!", "split-window!",
        "delete-window!", "delete-other-windows!", "open-window-ids", "active-window-id",
        "window-view-id", "window-buffer-id", "window-role", "set-window-role!", "window-pinned?",
        "set-window-pinned!", "window-created-by-policy?", "workbench-slot", "focus-window!",
        nullptr);
    initialize_guile_async_host_bindings(require_async_bridge);
}

enum class GuileSearchPath : std::uint8_t {
    Source,
    Compiled,
};

void prepend_guile_search_path(GuileSearchPath kind, const char* path) {
    const char* variable_name =
        kind == GuileSearchPath::Source ? "%load-path" : "%load-compiled-path";
    const SCM variable = scm_c_lookup(variable_name);
    scm_variable_set_x(variable, scm_cons(scm_from_utf8_string(path), scm_variable_ref(variable)));
}

void initialize_guile() {
    scm_init_guile();
    prepend_guile_search_path(GuileSearchPath::Source, CIND_GUILE_SITE_DIR);
    prepend_guile_search_path(GuileSearchPath::Source, CIND_ARES_SCHEME_DIR);
    prepend_guile_search_path(GuileSearchPath::Compiled, CIND_GUILE_SITE_CCACHE_DIR);
    (void)scm_c_define_module("cind host", initialize_host_module, nullptr);
}

GuileKeymapPolicy keymap_policy_from_scheme(HostLease& host, SCM value, const char* caller) {
    if (!scm_is_vector(value) || scm_c_vector_length(value) != 3 ||
        !symbol_is(scm_c_vector_ref(value, 0), "keymap-policy") ||
        !scm_is_vector(scm_c_vector_ref(value, 1)) || !scm_is_vector(scm_c_vector_ref(value, 2))) {
        scm_wrong_type_arg_msg(caller, 0, value, "#(keymap-policy layers overrides)");
    }
    const SCM layer_values = scm_c_vector_ref(value, 1);
    const SCM override_values = scm_c_vector_ref(value, 2);
    GuileKeymapPolicy policy;
    policy.layers.reserve(scm_c_vector_length(layer_values));
    for (std::size_t index = 0; index < scm_c_vector_length(layer_values); ++index) {
        const SCM layer = scm_c_vector_ref(layer_values, index);
        if (!scm_is_vector(layer) || scm_c_vector_length(layer) != 3 ||
            !symbol_is(scm_c_vector_ref(layer, 0), "keymap-layer") ||
            !scm_is_string(scm_c_vector_ref(layer, 2))) {
            scm_wrong_type_arg_msg(caller, 0, layer, "#(keymap-layer keymap scope)");
        }
        std::string scope = scheme_string(scm_c_vector_ref(layer, 2));
        if (scope.empty()) {
            scm_misc_error(caller, "keymap layer scope must not be empty", SCM_EOL);
        }
        policy.layers.push_back(
            {.keymap = require_keymap(host, scm_c_vector_ref(layer, 1), caller, 0),
             .scope = std::move(scope)});
    }
    policy.overrides.reserve(scm_c_vector_length(override_values));
    for (std::size_t index = 0; index < scm_c_vector_length(override_values); ++index) {
        policy.overrides.push_back(
            require_keymap(host, scm_c_vector_ref(override_values, index), caller, 0));
    }
    return policy;
}

SCM modeline_facts_value(const ModelineFacts& facts) {
    SCM value = scm_c_make_vector(11, SCM_UNSPECIFIED);
    scm_c_vector_set_x(value, 0, scm_from_utf8_symbol("modeline-facts"));
    scm_c_vector_set_x(value, 1, scm_from_utf8_string(facts.buffer_name.c_str()));
    scm_c_vector_set_x(value, 2,
                       facts.resource.empty() ? SCM_BOOL_F
                                              : scm_from_utf8_string(facts.resource.c_str()));
    scm_c_vector_set_x(value, 3, scm_from_bool(facts.dirty));
    scm_c_vector_set_x(value, 4, scm_from_uint32(facts.line));
    scm_c_vector_set_x(value, 5, scm_from_uint32(facts.column));
    scm_c_vector_set_x(value, 6, scm_from_uint32(facts.line_count));
    scm_c_vector_set_x(value, 7, scm_from_uint64(facts.revision));
    scm_c_vector_set_x(value, 8, scm_from_utf8_string(facts.style_origin.c_str()));
    scm_c_vector_set_x(value, 9, scm_from_utf8_string(""));
    scm_c_vector_set_x(value, 10, scm_from_utf8_string(facts.input_state.c_str()));
    return value;
}

ModelineGroup modeline_group_from_scheme(SCM value, const char* caller) {
    if (symbol_is(value, "chip")) {
        return ModelineGroup::Chip;
    }
    if (symbol_is(value, "left")) {
        return ModelineGroup::Left;
    }
    if (symbol_is(value, "right")) {
        return ModelineGroup::Right;
    }
    scm_wrong_type_arg_msg(caller, 0, value, "chip, left, or right symbol");
}

ModelineTone modeline_tone_from_scheme(SCM value, const char* caller) {
    if (symbol_is(value, "strong")) {
        return ModelineTone::Strong;
    }
    if (symbol_is(value, "normal")) {
        return ModelineTone::Normal;
    }
    if (symbol_is(value, "faded")) {
        return ModelineTone::Faded;
    }
    if (symbol_is(value, "faint")) {
        return ModelineTone::Faint;
    }
    if (symbol_is(value, "salient")) {
        return ModelineTone::Salient;
    }
    if (symbol_is(value, "critical")) {
        return ModelineTone::Critical;
    }
    scm_wrong_type_arg_msg(caller, 0, value, "modeline tone symbol");
}

ModelineWeight modeline_weight_from_scheme(SCM value, const char* caller) {
    if (symbol_is(value, "regular")) {
        return ModelineWeight::Regular;
    }
    if (symbol_is(value, "strong")) {
        return ModelineWeight::Strong;
    }
    scm_wrong_type_arg_msg(caller, 0, value, "regular or strong symbol");
}

ModelineContent modeline_content_from_scheme(SCM value, const char* caller) {
    if (!scm_is_vector(value) || scm_c_vector_length(value) != 2 ||
        !symbol_is(scm_c_vector_ref(value, 0), "modeline") ||
        !scm_is_vector(scm_c_vector_ref(value, 1))) {
        scm_wrong_type_arg_msg(caller, 0, value, "#(modeline segments)");
    }
    const SCM segments = scm_c_vector_ref(value, 1);
    ModelineContent content;
    content.segments.reserve(scm_c_vector_length(segments));
    for (std::size_t index = 0; index < scm_c_vector_length(segments); ++index) {
        const SCM segment = scm_c_vector_ref(segments, index);
        const SCM debug_value = scm_is_vector(segment) && scm_c_vector_length(segment) == 6
                                    ? scm_c_vector_ref(segment, 4)
                                    : SCM_UNDEFINED;
        if (!scm_is_vector(segment) || scm_c_vector_length(segment) != 6 ||
            !symbol_is(scm_c_vector_ref(segment, 0), "modeline-segment") ||
            !scheme_boolean(debug_value) || !scm_is_string(scm_c_vector_ref(segment, 5))) {
            scm_wrong_type_arg_msg(caller, 0, segment,
                                   "#(modeline-segment group tone weight debug? text)");
        }
        std::string text = scheme_string(scm_c_vector_ref(segment, 5));
        if (text.empty()) {
            scm_misc_error(caller, "modeline segment text must not be empty", SCM_EOL);
        }
        content.segments.push_back(
            {.text = std::move(text),
             .group = modeline_group_from_scheme(scm_c_vector_ref(segment, 1), caller),
             .tone = modeline_tone_from_scheme(scm_c_vector_ref(segment, 2), caller),
             .weight = modeline_weight_from_scheme(scm_c_vector_ref(segment, 3), caller),
             .debug = scheme_true(debug_value)});
    }
    return content;
}

SCM chrome_item_value(const ChromeItem& item) {
    SCM value = scm_c_make_vector(3, SCM_UNSPECIFIED);
    scm_c_vector_set_x(value, 0, scm_from_utf8_symbol("chrome-item"));
    scm_c_vector_set_x(value, 1, scm_from_utf8_string(item.label.c_str()));
    scm_c_vector_set_x(value, 2, scm_from_utf8_string(item.detail.c_str()));
    return value;
}

SCM chrome_facts_value(const ChromeFacts& facts) {
    SCM candidates = scm_c_make_vector(facts.candidates.size(), SCM_UNSPECIFIED);
    for (std::size_t index = 0; index < facts.candidates.size(); ++index) {
        scm_c_vector_set_x(candidates, index, chrome_item_value(facts.candidates[index]));
    }
    SCM hints = scm_c_make_vector(facts.hints.size(), SCM_UNSPECIFIED);
    for (std::size_t index = 0; index < facts.hints.size(); ++index) {
        const ChromeHint& hint = facts.hints[index];
        SCM value = scm_c_make_vector(4, SCM_UNSPECIFIED);
        scm_c_vector_set_x(value, 0, scm_from_utf8_symbol("chrome-hint"));
        scm_c_vector_set_x(value, 1, scm_from_utf8_string(hint.key.c_str()));
        scm_c_vector_set_x(value, 2, scm_from_utf8_string(hint.detail.c_str()));
        scm_c_vector_set_x(value, 3, scm_from_bool(hint.prefix));
        scm_c_vector_set_x(hints, index, value);
    }
    SCM value = scm_c_make_vector(13, SCM_UNSPECIFIED);
    scm_c_vector_set_x(value, 0, scm_from_utf8_symbol("chrome-facts"));
    scm_c_vector_set_x(value, 1,
                       scm_from_utf8_symbol(!facts.interaction ? "none"
                                            : *facts.interaction == ChromeInteractionKind::Picker
                                                ? "picker"
                                                : "text"));
    scm_c_vector_set_x(value, 2, scm_from_utf8_string(facts.prompt.c_str()));
    scm_c_vector_set_x(value, 3, scm_from_utf8_string(facts.input.c_str()));
    scm_c_vector_set_x(value, 4, scm_from_size_t(facts.input_caret));
    scm_c_vector_set_x(value, 5, candidates);
    scm_c_vector_set_x(value, 6, facts.selection ? scm_from_size_t(*facts.selection) : SCM_BOOL_F);
    scm_c_vector_set_x(value, 7, scm_from_utf8_string(""));
    scm_c_vector_set_x(value, 8, scm_from_utf8_string(facts.preedit.c_str()));
    scm_c_vector_set_x(value, 9, scm_from_utf8_string(facts.pending_sequence.c_str()));
    scm_c_vector_set_x(value, 10, scm_from_utf8_string(facts.pending_prefix.c_str()));
    scm_c_vector_set_x(value, 11, hints);
    scm_c_vector_set_x(value, 12, scm_from_size_t(facts.prompt.size()));
    return value;
}

ChromeItem chrome_item_from_scheme(SCM value, const char* caller) {
    if (!scm_is_vector(value) || scm_c_vector_length(value) != 3 ||
        !symbol_is(scm_c_vector_ref(value, 0), "chrome-item") ||
        !scm_is_string(scm_c_vector_ref(value, 1)) || !scm_is_string(scm_c_vector_ref(value, 2))) {
        scm_wrong_type_arg_msg(caller, 0, value, "#(chrome-item label detail)");
    }
    return {.label = scheme_string(scm_c_vector_ref(value, 1)),
            .detail = scheme_string(scm_c_vector_ref(value, 2)),
            .kind = {}};
}

std::optional<std::size_t> optional_size_from_scheme(SCM value, const char* caller) {
    if (scheme_false(value)) {
        return std::nullopt;
    }
    if (scm_is_unsigned_integer(value, 0, std::numeric_limits<std::size_t>::max()) == 0) {
        scm_wrong_type_arg_msg(caller, 0, value, "non-negative exact integer or #f");
    }
    return scm_to_size_t(value);
}

ChromeContent chrome_content_from_scheme(SCM value, const char* caller) {
    if (!scm_is_vector(value) || scm_c_vector_length(value) != 10 ||
        !symbol_is(scm_c_vector_ref(value, 0), "chrome") ||
        !scm_is_string(scm_c_vector_ref(value, 1)) || !scm_is_string(scm_c_vector_ref(value, 2)) ||
        !scm_is_string(scm_c_vector_ref(value, 4)) || !scm_is_vector(scm_c_vector_ref(value, 5))) {
        scm_wrong_type_arg_msg(caller, 0, value,
                               "#(chrome pending-key echo echo-caret popup-title items capacity "
                               "selection input input-caret)");
    }
    const SCM capacity = scm_c_vector_ref(value, 6);
    if (scm_is_unsigned_integer(capacity, 0, std::numeric_limits<std::size_t>::max()) == 0) {
        scm_wrong_type_arg_msg(caller, 0, capacity, "non-negative exact integer");
    }
    ChromeContent content;
    content.pending_key = scheme_string(scm_c_vector_ref(value, 1));
    content.echo = scheme_string(scm_c_vector_ref(value, 2));
    content.echo_cursor_byte = optional_size_from_scheme(scm_c_vector_ref(value, 3), caller);
    content.popup_title = scheme_string(scm_c_vector_ref(value, 4));
    content.popup_capacity = scm_to_size_t(capacity);
    content.popup_selection = optional_size_from_scheme(scm_c_vector_ref(value, 7), caller);
    const SCM items = scm_c_vector_ref(value, 5);
    content.popup_items.reserve(scm_c_vector_length(items));
    for (std::size_t index = 0; index < scm_c_vector_length(items); ++index) {
        content.popup_items.push_back(
            chrome_item_from_scheme(scm_c_vector_ref(items, index), caller));
    }
    const SCM input = scm_c_vector_ref(value, 8);
    if (!scheme_false(input)) {
        if (!scm_is_string(input)) {
            scm_wrong_type_arg_msg(caller, 0, input, "string or #f");
        }
        content.popup_input = scheme_string(input);
    }
    content.popup_input_cursor = optional_size_from_scheme(scm_c_vector_ref(value, 9), caller);
    if (content.echo_cursor_byte && *content.echo_cursor_byte > content.echo.size()) {
        scm_misc_error(caller, "echo caret exceeds the UTF-8 byte length", SCM_EOL);
    }
    if (content.popup_selection && *content.popup_selection >= content.popup_items.size()) {
        scm_misc_error(caller, "popup selection is outside the item vector", SCM_EOL);
    }
    if (content.popup_input_cursor &&
        (!content.popup_input || *content.popup_input_cursor > content.popup_input->size())) {
        scm_misc_error(caller, "popup input caret is invalid", SCM_EOL);
    }
    return content;
}

SCM startup_facts_value(const StartupFacts& facts) {
    SCM value = scm_c_make_vector(3, SCM_UNSPECIFIED);
    scm_c_vector_set_x(value, 0, scm_from_utf8_symbol("startup-facts"));
    scm_c_vector_set_x(value, 1, scm_from_utf8_string(facts.requested_resource.c_str()));
    scm_c_vector_set_x(value, 2, scm_from_bool(facts.has_initial_text));
    return value;
}

SCM session_facts_value(const SessionFacts& facts) {
    SCM value = scm_c_make_vector(2, SCM_UNSPECIFIED);
    scm_c_vector_set_x(value, 0, scm_from_utf8_symbol("session-facts"));
    scm_c_vector_set_x(value, 1, scm_from_bool(facts.has_initial_text));
    return value;
}

SCM pointer_target_symbol(PointerTargetKind target) {
    switch (target) {
    case PointerTargetKind::DocumentText:
        return scm_from_utf8_symbol("document-text");
    case PointerTargetKind::DocumentGutter:
        return scm_from_utf8_symbol("document-gutter");
    case PointerTargetKind::PopupHeader:
        return scm_from_utf8_symbol("popup-header");
    case PointerTargetKind::PopupItem:
        return scm_from_utf8_symbol("popup-item");
    case PointerTargetKind::Status:
        return scm_from_utf8_symbol("status");
    case PointerTargetKind::Echo:
        return scm_from_utf8_symbol("echo");
    case PointerTargetKind::Region:
        return scm_from_utf8_symbol("region");
    }
    throw std::logic_error("unknown pointer target kind");
}

SCM pointer_event_value(const PointerEvent& event, bool pending_key_sequence) {
    SCM value = scm_c_make_vector(7, SCM_UNSPECIFIED);
    scm_c_vector_set_x(value, 0, scm_from_utf8_symbol("pointer-event"));
    scm_c_vector_set_x(value, 1, pointer_target_symbol(event.target));
    scm_c_vector_set_x(value, 2,
                       event.window ? entity_id(event.window->slot, event.window->generation)
                                    : SCM_BOOL_F);
    scm_c_vector_set_x(value, 3,
                       event.document_line ? scm_from_uint32(*event.document_line) : SCM_BOOL_F);
    scm_c_vector_set_x(value, 4,
                       event.display_column ? scm_from_uint32(*event.display_column) : SCM_BOOL_F);
    scm_c_vector_set_x(value, 5,
                       event.popup_item ? scm_from_size_t(*event.popup_item) : SCM_BOOL_F);
    scm_c_vector_set_x(value, 6, scm_from_bool(pending_key_sequence));
    return value;
}

SCM scroll_input_value(ScrollInput input) {
    SCM value = scm_c_make_vector(3, SCM_UNSPECIFIED);
    scm_c_vector_set_x(value, 0, scm_from_utf8_symbol("scroll-input"));
    scm_c_vector_set_x(value, 1, scm_from_double(input.amount));
    scm_c_vector_set_x(value, 2,
                       scm_from_utf8_symbol(input.unit == ScrollUnit::Lines ? "lines" : "steps"));
    return value;
}

PresentationTheme presentation_theme_from_scheme(SCM value, const char* caller) {
    constexpr std::size_t color_count = 16;
    if (!scm_is_vector(value) || scm_c_vector_length(value) != color_count + 1 ||
        !symbol_is(scm_c_vector_ref(value, 0), "presentation-theme")) {
        scm_wrong_type_arg_msg(caller, 0, value, "#(presentation-theme 16-argb-colors)");
    }
    std::uint32_t colors[color_count]{};
    for (std::size_t index = 0; index < color_count; ++index) {
        const SCM color = scm_c_vector_ref(value, index + 1);
        if (scm_is_unsigned_integer(color, 0, std::numeric_limits<std::uint32_t>::max()) == 0) {
            scm_wrong_type_arg_msg(caller, static_cast<int>(index + 1), color,
                                   "unsigned 32-bit ARGB color");
        }
        colors[index] = scm_to_uint32(color);
    }
    return {.canvas = colors[0],
            .highlight = colors[1],
            .band = colors[2],
            .selection = colors[3],
            .divider = colors[4],
            .text = colors[5],
            .strong = colors[6],
            .faded = colors[7],
            .faint = colors[8],
            .salient = colors[9],
            .popout = colors[10],
            .critical = colors[11],
            .cursor = colors[12],
            .sign_added = colors[13],
            .sign_modified = colors[14],
            .sign_deleted = colors[15]};
}

std::uint32_t presentation_color_from_scheme(SCM value, const char* caller, int position) {
    if (scm_is_unsigned_integer(value, 0, std::numeric_limits<std::uint32_t>::max()) == 0) {
        scm_wrong_type_arg_msg(caller, position, value, "unsigned 32-bit ARGB color");
    }
    return scm_to_uint32(value);
}

PresentationTextRole presentation_text_role_from_scheme(SCM value, const char* caller,
                                                        int position) {
    const std::string name = scheme_name(value, caller, position);
    const auto found = std::ranges::find(presentation_text_role_names, name);
    if (found == presentation_text_role_names.end()) {
        scm_wrong_type_arg_msg(caller, position, value, "known presentation text role");
    }
    return static_cast<PresentationTextRole>(
        std::distance(presentation_text_role_names.begin(), found));
}

PresentationStyleSheet presentation_styles_from_scheme(SCM value, const char* caller) {
    if (!scm_is_vector(value) || scm_c_vector_length(value) != 5 ||
        !symbol_is(scm_c_vector_ref(value, 0), "presentation-styles")) {
        scm_wrong_type_arg_msg(
            caller, 0, value,
            "#(presentation-styles inactive-alpha secondary-alpha text-styles modeline-colors)");
    }
    PresentationStyleSheet result;
    const auto alpha = [&](std::size_t index) {
        const SCM input = scm_c_vector_ref(value, index);
        if (scm_is_unsigned_integer(input, 0, 255) == 0) {
            scm_wrong_type_arg_msg(caller, static_cast<int>(index), input, "unsigned 8-bit alpha");
        }
        return static_cast<std::uint8_t>(scm_to_uint8(input));
    };
    result.inactive_alpha = alpha(1);
    result.secondary_alpha = alpha(2);

    const SCM styles = scm_c_vector_ref(value, 3);
    if (!scm_is_vector(styles) ||
        scm_c_vector_length(styles) != PresentationStyleSheet::text_role_count) {
        scm_wrong_type_arg_msg(caller, 3, styles,
                               "vector containing every presentation text role exactly once");
    }
    std::array<bool, PresentationStyleSheet::text_role_count> seen{};
    for (std::size_t index = 0; index < PresentationStyleSheet::text_role_count; ++index) {
        const SCM entry = scm_c_vector_ref(styles, index);
        if (!scm_is_vector(entry) || scm_c_vector_length(entry) != 5 ||
            !symbol_is(scm_c_vector_ref(entry, 0), "presentation-style")) {
            scm_wrong_type_arg_msg(
                caller, 3, entry,
                "#(presentation-style role foreground background-or-#f regular-or-strong)");
        }
        const PresentationTextRole role =
            presentation_text_role_from_scheme(scm_c_vector_ref(entry, 1), caller, 3);
        const std::size_t role_index = static_cast<std::size_t>(role);
        if (seen[role_index]) {
            scm_misc_error(caller, "presentation text role is duplicated", SCM_EOL);
        }
        seen[role_index] = true;
        PresentationTextStyle& style = result.text[role_index];
        style.foreground = presentation_color_from_scheme(scm_c_vector_ref(entry, 2), caller, 3);
        const SCM background = scm_c_vector_ref(entry, 3);
        if (!scheme_false(background)) {
            style.background = presentation_color_from_scheme(background, caller, 3);
        }
        const SCM weight = scm_c_vector_ref(entry, 4);
        if (symbol_is(weight, "regular")) {
            style.weight = PresentationWeight::Regular;
        } else if (symbol_is(weight, "strong")) {
            style.weight = PresentationWeight::Strong;
        } else {
            scm_wrong_type_arg_msg(caller, 3, weight, "regular or strong symbol");
        }
    }
    constexpr std::array background_roles = {
        PresentationTextRole::StatusBar,
        PresentationTextRole::StatusKey,
        PresentationTextRole::Popup,
        PresentationTextRole::PositionHint,
        PresentationTextRole::PopupSelected,
        PresentationTextRole::ModelineInactive,
        PresentationTextRole::ModelineInactiveChip,
    };
    for (const PresentationTextRole role : background_roles) {
        if (!result.style(role).background) {
            scm_misc_error(caller, "presentation role ~A requires a background color",
                           scm_list_1(name_symbol(presentation_text_role_name(role))));
        }
    }

    const SCM modeline = scm_c_vector_ref(value, 4);
    if (!scm_is_vector(modeline) ||
        scm_c_vector_length(modeline) != PresentationStyleSheet::modeline_tone_count) {
        scm_wrong_type_arg_msg(caller, 4, modeline, "six modeline colors in semantic tone order");
    }
    for (std::size_t index = 0; index < PresentationStyleSheet::modeline_tone_count; ++index) {
        result.modeline[index] =
            presentation_color_from_scheme(scm_c_vector_ref(modeline, index), caller, 4);
    }
    return result;
}

PresentationMotion presentation_motion_from_scheme(SCM value, const char* caller) {
    if (!scm_is_vector(value) || scm_c_vector_length(value) != 5 ||
        !symbol_is(scm_c_vector_ref(value, 0), "presentation-motion")) {
        scm_wrong_type_arg_msg(
            caller, 0, value,
            "#(presentation-motion duration-ms spring-frequency position-tolerance "
            "velocity-tolerance)");
    }
    const SCM duration = scm_c_vector_ref(value, 1);
    if (scm_is_unsigned_integer(duration, 1, std::numeric_limits<std::uint32_t>::max()) == 0) {
        scm_wrong_type_arg_msg(caller, 1, duration, "positive unsigned 32-bit milliseconds");
    }
    float values[3]{};
    for (std::size_t index = 0; index < 3; ++index) {
        const SCM input = scm_c_vector_ref(value, index + 2);
        if (!scheme_true(scm_real_p(input))) {
            scm_wrong_type_arg_msg(caller, static_cast<int>(index + 2), input,
                                   "positive finite real number");
        }
        const double converted = scm_to_double(input);
        if (!std::isfinite(converted) || converted <= 0.0 ||
            converted > std::numeric_limits<float>::max()) {
            scm_misc_error(caller, "motion parameter must be a positive finite float", SCM_EOL);
        }
        values[index] = static_cast<float>(converted);
    }
    return {.view_duration_ms = scm_to_uint32(duration),
            .scroll_spring_frequency = values[0],
            .scroll_position_tolerance = values[1],
            .scroll_velocity_tolerance = values[2]};
}

PresentationMetrics presentation_metrics_from_scheme(SCM value, const char* caller) {
    if (!scm_is_vector(value) || scm_c_vector_length(value) != 11 ||
        !symbol_is(scm_c_vector_ref(value, 0), "presentation-metrics")) {
        scm_wrong_type_arg_msg(
            caller, 0, value,
            "#(presentation-metrics modeline-extra-height echo-extra-height footer-padding-x "
            "segment-gap chip-padding-x minibuffer-padding-x minibuffer-detail-gap "
            "cursor-stroke minimum-columns minimum-rows)");
    }
    float values[8]{};
    for (std::size_t index = 0; index < 8; ++index) {
        const SCM input = scm_c_vector_ref(value, index + 1);
        if (!scheme_true(scm_real_p(input))) {
            scm_wrong_type_arg_msg(caller, static_cast<int>(index + 1), input,
                                   "finite real number");
        }
        const double converted = scm_to_double(input);
        const bool zero_allowed = index < 2 || index == 3 || index == 6;
        if (!std::isfinite(converted) || converted < 0.0 || (!zero_allowed && converted == 0.0) ||
            converted > std::numeric_limits<float>::max()) {
            scm_misc_error(caller, "presentation metric is outside its valid range", SCM_EOL);
        }
        values[index] = static_cast<float>(converted);
    }
    std::uint32_t dimensions[2]{};
    for (std::size_t index = 0; index < 2; ++index) {
        const SCM input = scm_c_vector_ref(value, index + 9);
        if (scm_is_unsigned_integer(input, 1, std::numeric_limits<int>::max()) == 0) {
            scm_wrong_type_arg_msg(caller, static_cast<int>(index + 9), input,
                                   "positive integer representable by the frontend");
        }
        dimensions[index] = scm_to_uint32(input);
    }
    return {.modeline_extra_height = values[0],
            .echo_extra_height = values[1],
            .footer_padding_x = values[2],
            .segment_gap = values[3],
            .chip_padding_x = values[4],
            .minibuffer_padding_x = values[5],
            .minibuffer_detail_gap = values[6],
            .cursor_stroke = values[7],
            .minimum_columns = dimensions[0],
            .minimum_rows = dimensions[1]};
}

PresentationTypography presentation_typography_from_scheme(SCM value, const char* caller) {
    if (!scm_is_vector(value) || scm_c_vector_length(value) != 3 ||
        !symbol_is(scm_c_vector_ref(value, 0), "presentation-typography") ||
        !scm_is_string(scm_c_vector_ref(value, 1)) ||
        scm_is_real(scm_c_vector_ref(value, 2)) == 0) {
        scm_wrong_type_arg_msg(caller, 0, value,
                               "#(presentation-typography font-family font-size)");
    }
    PresentationTypography typography{
        .font_family = scheme_string(scm_c_vector_ref(value, 1)),
        .font_size = static_cast<float>(scm_to_double(scm_c_vector_ref(value, 2)))};
    if (typography.font_family.empty()) {
        scm_misc_error(caller, "presentation font family must not be empty", SCM_EOL);
    }
    if (!std::isfinite(typography.font_size) || typography.font_size <= 0.0F) {
        scm_misc_error(caller, "presentation font size must be positive and finite", SCM_EOL);
    }
    return typography;
}

PresentationProfile presentation_profile_from_scheme(SCM value, const char* caller) {
    if (!scm_is_vector(value) || scm_c_vector_length(value) != 6 ||
        !symbol_is(scm_c_vector_ref(value, 0), "presentation-profile")) {
        scm_wrong_type_arg_msg(caller, 0, value,
                               "#(presentation-profile theme styles motion metrics typography)");
    }
    return {
        .theme = presentation_theme_from_scheme(scm_c_vector_ref(value, 1), caller),
        .styles = presentation_styles_from_scheme(scm_c_vector_ref(value, 2), caller),
        .motion = presentation_motion_from_scheme(scm_c_vector_ref(value, 3), caller),
        .metrics = presentation_metrics_from_scheme(scm_c_vector_ref(value, 4), caller),
        .typography = presentation_typography_from_scheme(scm_c_vector_ref(value, 5), caller),
    };
}

StartupBufferPlan startup_buffer_plan_from_scheme(HostLease& host, SCM buffer_value,
                                                  bool has_initial_text, const char* caller) {
    if (!scm_is_vector(buffer_value) || scm_c_vector_length(buffer_value) != 7 ||
        !symbol_is(scm_c_vector_ref(buffer_value, 0), "startup-buffer") ||
        !scm_is_string(scm_c_vector_ref(buffer_value, 1)) ||
        !scheme_boolean(scm_c_vector_ref(buffer_value, 5))) {
        scm_wrong_type_arg_msg(
            caller, 0, buffer_value,
            "#(startup-buffer name contents kind resource-or-#f read-only? mode)");
    }

    StartupBufferPlan plan;
    plan.name = scheme_string(scm_c_vector_ref(buffer_value, 1));
    if (plan.name.empty()) {
        scm_misc_error(caller, "startup buffer name must not be empty", SCM_EOL);
    }
    const SCM contents = scm_c_vector_ref(buffer_value, 2);
    if (symbol_is(contents, "initial-text")) {
        if (!has_initial_text) {
            scm_misc_error(caller, "startup plan requested unavailable initial text", SCM_EOL);
        }
        plan.use_initial_text = true;
    } else if (!symbol_is(contents, "empty")) {
        scm_wrong_type_arg_msg(caller, 0, contents, "'initial-text or 'empty");
    }
    plan.kind = buffer_kind_from_scheme(scm_c_vector_ref(buffer_value, 3), caller, 0);

    const SCM resource = scm_c_vector_ref(buffer_value, 4);
    if (!scheme_false(resource)) {
        if (!scm_is_string(resource)) {
            scm_wrong_type_arg_msg(caller, 0, resource, "string or #f");
        }
        plan.resource = scheme_string(resource);
        if (plan.resource->empty()) {
            scm_misc_error(caller, "startup buffer resource must not be empty", SCM_EOL);
        }
    }
    if (plan.kind == BufferKind::File && !plan.resource) {
        scm_misc_error(caller, "startup file buffer requires a resource", SCM_EOL);
    }
    plan.read_only = scheme_true(scm_c_vector_ref(buffer_value, 5));
    plan.major_mode = require_mode(host, scm_c_vector_ref(buffer_value, 6), caller, 0);
    return plan;
}

StartupPlan startup_plan_from_scheme(HostLease& host, SCM value, const StartupFacts& facts,
                                     const char* caller) {
    if (!scm_is_vector(value) || scm_c_vector_length(value) != 6 ||
        !symbol_is(scm_c_vector_ref(value, 0), "startup-plan") ||
        !scm_is_string(scm_c_vector_ref(value, 3)) || !scheme_boolean(scm_c_vector_ref(value, 5))) {
        scm_wrong_type_arg_msg(caller, 0, value,
                               "#(startup-plan startup-buffer cpp-indent-style-or-#f style-origin "
                               "resource-to-open-or-#f startup-placeholder?)");
    }
    StartupPlan plan;
    plan.buffer = startup_buffer_plan_from_scheme(host, scm_c_vector_ref(value, 1),
                                                  facts.has_initial_text, caller);
    const SCM style = scm_c_vector_ref(value, 2);
    plan.style =
        scheme_false(style) ? CppIndentStyle{} : cpp_indent_style_from_scheme(style, caller, 0);
    plan.style_origin = scheme_string(scm_c_vector_ref(value, 3));

    const SCM resource_to_open = scm_c_vector_ref(value, 4);
    if (!scheme_false(resource_to_open)) {
        if (!scm_is_string(resource_to_open)) {
            scm_wrong_type_arg_msg(caller, 0, resource_to_open, "string or #f");
        }
        plan.resource_to_open = scheme_string(resource_to_open);
        if (plan.resource_to_open->empty()) {
            scm_misc_error(caller, "startup resource to open must not be empty", SCM_EOL);
        }
    }
    plan.startup_placeholder = scheme_true(scm_c_vector_ref(value, 5));
    if (plan.startup_placeholder && !plan.resource_to_open) {
        scm_misc_error(caller, "startup placeholder requires a deferred resource", SCM_EOL);
    }
    return plan;
}

SessionPlan session_plan_from_scheme(HostLease& host, SCM value, const SessionFacts& facts,
                                     const char* caller) {
    if (!scm_is_vector(value) || scm_c_vector_length(value) != 2 ||
        !symbol_is(scm_c_vector_ref(value, 0), "session-plan")) {
        scm_wrong_type_arg_msg(caller, 0, value, "#(session-plan startup-buffer)");
    }
    return {.buffer = startup_buffer_plan_from_scheme(host, scm_c_vector_ref(value, 1),
                                                      facts.has_initial_text, caller)};
}

SCM display_facts_value(const GuileDisplayFacts& facts) {
    SCM windows = scm_c_make_vector(facts.windows.size(), SCM_UNSPECIFIED);
    for (std::size_t index = 0; index < facts.windows.size(); ++index) {
        const GuileDisplayWindow& source = facts.windows[index];
        SCM window = scm_c_make_vector(5, SCM_UNSPECIFIED);
        scm_c_vector_set_x(window, 0, name_symbol("display-window"));
        scm_c_vector_set_x(window, 1, entity_id(source.window.slot, source.window.generation));
        scm_c_vector_set_x(window, 2, source.role ? name_symbol(*source.role) : SCM_BOOL_F);
        scm_c_vector_set_x(window, 3, scm_from_bool(source.pinned));
        scm_c_vector_set_x(window, 4, scm_from_bool(source.created_by_policy));
        scm_c_vector_set_x(windows, index, window);
    }
    SCM slots = scm_c_make_vector(facts.slots.size(), SCM_UNSPECIFIED);
    for (std::size_t index = 0; index < facts.slots.size(); ++index) {
        const GuileDisplaySlot& source = facts.slots[index];
        SCM slot = scm_c_make_vector(3, SCM_UNSPECIFIED);
        scm_c_vector_set_x(slot, 0, name_symbol("display-slot"));
        scm_c_vector_set_x(slot, 1, name_symbol(source.role));
        scm_c_vector_set_x(slot, 2, entity_id(source.window.slot, source.window.generation));
        scm_c_vector_set_x(slots, index, slot);
    }
    SCM result = scm_c_make_vector(6, SCM_UNSPECIFIED);
    scm_c_vector_set_x(result, 0, name_symbol("display-facts"));
    scm_c_vector_set_x(result, 1, name_symbol(facts.intent));
    scm_c_vector_set_x(result, 2, entity_id(facts.origin.slot, facts.origin.generation));
    scm_c_vector_set_x(result, 3, entity_id(facts.active.slot, facts.active.generation));
    scm_c_vector_set_x(result, 4, windows);
    scm_c_vector_set_x(result, 5, slots);
    return result;
}

GuileDisplayPlan display_plan_from_scheme(SCM value, const char* caller) {
    if (!scm_is_vector(value) || scm_c_vector_length(value) < 2) {
        scm_wrong_type_arg_msg(caller, 0, value, "display plan vector");
    }
    GuileDisplayPlan plan;
    if (symbol_is(scm_c_vector_ref(value, 0), "display-reuse") && scm_c_vector_length(value) == 2) {
        plan.action = GuileDisplayPlan::Action::Reuse;
        plan.target = entity_id_from_scheme<WindowTag>(scm_c_vector_ref(value, 1), caller, 0);
        return plan;
    }
    if (!symbol_is(scm_c_vector_ref(value, 0), "display-split") ||
        scm_c_vector_length(value) != 5 || scm_is_real(scm_c_vector_ref(value, 3)) == 0) {
        scm_wrong_type_arg_msg(
            caller, 0, value, "#(display-reuse window) or #(display-split window axis ratio role)");
    }
    plan.action = GuileDisplayPlan::Action::Split;
    plan.target = entity_id_from_scheme<WindowTag>(scm_c_vector_ref(value, 1), caller, 0);
    const SCM axis = scm_c_vector_ref(value, 2);
    if (symbol_is(axis, "rows")) {
        plan.axis = WindowSplitAxis::Rows;
    } else if (symbol_is(axis, "columns")) {
        plan.axis = WindowSplitAxis::Columns;
    } else {
        scm_wrong_type_arg_msg(caller, 0, axis, "'rows or 'columns");
    }
    plan.ratio = static_cast<float>(scm_to_double(scm_c_vector_ref(value, 3)));
    if (!std::isfinite(plan.ratio) || plan.ratio <= 0.0F || plan.ratio >= 1.0F) {
        scm_misc_error(caller, "display split ratio must be between zero and one", SCM_EOL);
    }
    const SCM role = scm_c_vector_ref(value, 4);
    if (!scheme_false(role)) {
        plan.role = scheme_name(role, caller, 0);
    }
    return plan;
}

struct GuileCall {
    enum class Operation : std::uint8_t {
        Load,
        LoadExtension,
        MakeEvaluationModule,
        Evaluate,
        InstallCommands,
        InstallProviders,
        InstallKeymaps,
        InstallInputStates,
        InstallModes,
        InstallResourcePolicies,
        InstallBufferLifecyclePolicies,
        InstallPointerPolicies,
        InstallPresentationPolicies,
        InstallDisplayPolicy,
        ResolveStartupPlan,
        ResolveSessionPlan,
        SetStartupPlaceholder,
        ResolveCloseCommand,
        HandlePointer,
        HandleScroll,
        OpenResource,
        RestoreWorkbenchSession,
        BufferEdited,
        InteractionStarted,
        InteractionPolicyState,
        MinibufferHistoryState,
        InteractionSelection,
        CommandFeedbackState,
        ApplicationState,
        SetCaretReveal,
        BufferSavingState,
        CommandInput,
        CommandResultFeedback,
        RecordCommand,
        SetMessage,
        ResolveKeymapPolicy,
        ResolveBaseKeymapPolicy,
        ChromeContent,
        ModelineContent,
        PresentationProfile,
        DisplayPlan,
        FallbackDisplayPlan,
        ProjectSearchRunning,
        ProjectIndexUpdated,
        InvokeCommand,
        InvokeProvider,
        InvokeCompletionProvider,
        InvokeCompletionResolver,
        TransformProviderResult,
        TransformCompletionProviderResult,
        InvokeInputHandler,
        InvokePositionHints,
        InvokeInputStateObserver,
        InvokeModePolicyObserver,
        CheckEnabled,
    };

    SCM host = SCM_UNDEFINED;
    SCM module = SCM_UNDEFINED;
    SCM procedure = SCM_UNDEFINED;
    SCM argument = SCM_UNDEFINED;
    SCM result = SCM_UNDEFINED;
    std::size_t count = 0;
    const CommandContext* context = nullptr;
    const CommandInvocation* invocation = nullptr;
    const CompletionRequest* completion_request = nullptr;
    const CompletionItem* completion_item = nullptr;
    const InputStateChange* input_state_change = nullptr;
    const BufferModePolicyChange* mode_policy_change = nullptr;
    EditorRuntime* runtime = nullptr;
    const StartupFacts* startup_facts = nullptr;
    const SessionFacts* session_facts = nullptr;
    const PointerEvent* pointer_event = nullptr;
    const ModelineFacts* modeline_facts = nullptr;
    const ChromeFacts* chrome_facts = nullptr;
    const GuileDisplayFacts* display_facts = nullptr;
    const InteractionRequest* interaction_request = nullptr;
    std::exception_ptr cpp_failure;
    ScrollInput scroll_input;
    std::string path;
    std::string source;
    std::string source_name;
    std::string history;
    std::string query;
    std::vector<InteractionCandidate> provider_candidates;
    CompletionProvider completion_provider;
    CompletionProviderResponse completion_response;
    CompletionItem resolved_completion_item;
    std::vector<PositionHint> position_hints;
    ModelineContent modeline_content;
    std::string error;
    GuileKeymapPolicy keymap_policy;
    StartupPlan startup_plan;
    SessionPlan session_plan;
    ChromeContent chrome_content;
    std::optional<CommandResult> command_result;
    CommandId command;
    WindowId window;
    BufferId buffer;
    ViewId view;
    RevisionId revision = 0;
    ProjectId project;
    std::optional<std::uint32_t> line;
    std::optional<std::uint32_t> column;
    std::string intent;
    KeyStroke key;
    PresentationProfile presentation_profile;
    GuileDisplayPlan display_plan;
    GuileMinibufferHistoryState minibuffer_history;
    std::optional<GuileInteractionPolicyState> interaction_policy_state;
    std::optional<std::size_t> interaction_selection;
    GuileCommandFeedbackState command_feedback;
    GuileApplicationState application_state;
    CommandLoopStatus command_status = CommandLoopStatus::NotHandled;
    std::optional<std::string> command_name;
    Operation operation = Operation::Load;
    bool pending_key_sequence = false;
    bool completion_needs_resolution = false;
    bool force = false;
    bool enabled = false;
    bool clear_message = false;
    bool interaction_started = false;
    CommandTarget interaction_origin;
};

void discard_state_tail(GuileState& state, const GuileState& checkpoint) {
    while (state.mode_policy_observers.size() > checkpoint.mode_policy_observers.size()) {
        (void)scm_gc_unprotect_object(state.mode_policy_observers.back().procedure);
        state.mode_policy_observers.pop_back();
    }
    while (state.input_state_observers.size() > checkpoint.input_state_observers.size()) {
        (void)scm_gc_unprotect_object(state.input_state_observers.back().procedure);
        state.input_state_observers.pop_back();
    }
    while (state.input_state_lifecycles.size() > checkpoint.input_state_lifecycles.size()) {
        const ScriptInputStateLifecycle& lifecycle = state.input_state_lifecycles.back();
        if (!scheme_false(lifecycle.on_enter)) {
            (void)scm_gc_unprotect_object(lifecycle.on_enter);
        }
        if (!scheme_false(lifecycle.on_exit)) {
            (void)scm_gc_unprotect_object(lifecycle.on_exit);
        }
        state.input_state_lifecycles.pop_back();
    }
    while (state.position_hint_providers.size() > checkpoint.position_hint_providers.size()) {
        (void)scm_gc_unprotect_object(state.position_hint_providers.back().procedure);
        state.position_hint_providers.pop_back();
    }
    while (state.input_states.size() > checkpoint.input_states.size()) {
        const SCM handler = state.input_states.back().handler;
        if (!scheme_false(handler)) {
            (void)scm_gc_unprotect_object(handler);
        }
        state.input_states.pop_back();
    }
    while (state.providers.size() > checkpoint.providers.size()) {
        (void)scm_gc_unprotect_object(state.providers.back().complete);
        state.providers.pop_back();
    }
    while (state.completion_providers.size() > checkpoint.completion_providers.size()) {
        const ScriptCompletionProvider& provider = state.completion_providers.back();
        (void)scm_gc_unprotect_object(provider.complete);
        if (!scheme_false(provider.resolve)) {
            (void)scm_gc_unprotect_object(provider.resolve);
        }
        state.completion_providers.pop_back();
    }
    while (state.commands.size() > checkpoint.commands.size()) {
        const ScriptCommand& command = state.commands.back();
        (void)scm_gc_unprotect_object(command.execute);
        if (!scheme_false(command.enabled)) {
            (void)scm_gc_unprotect_object(command.enabled);
        }
        state.commands.pop_back();
    }
}

SCM command_context_value(const CommandContext& context) {
    const std::optional<ProjectId> project = context.project_id();
    return scm_list_4(
        scm_cons(scm_from_utf8_symbol("window"),
                 entity_id(context.window_id().slot, context.window_id().generation)),
        scm_cons(scm_from_utf8_symbol("buffer"),
                 entity_id(context.buffer_id().slot, context.buffer_id().generation)),
        scm_cons(scm_from_utf8_symbol("view"),
                 entity_id(context.view_id().slot, context.view_id().generation)),
        scm_cons(scm_from_utf8_symbol("project"),
                 project ? entity_id(project->slot, project->generation) : SCM_BOOL_F));
}

SCM setting_value(const SettingValue& value) {
    if (const bool* boolean = std::get_if<bool>(&value)) {
        return scm_from_bool(*boolean);
    }
    if (const std::int64_t* integer = std::get_if<std::int64_t>(&value)) {
        return scm_from_int64(*integer);
    }
    if (const double* real = std::get_if<double>(&value)) {
        return scm_from_double(*real);
    }
    return scm_from_utf8_string(std::get<std::string>(value).c_str());
}

SCM command_invocation_value(const CommandInvocation& invocation) {
    SCM arguments = scm_c_make_vector(invocation.arguments.size(), SCM_UNSPECIFIED);
    for (std::size_t index = 0; index < invocation.arguments.size(); ++index) {
        scm_c_vector_set_x(arguments, index, setting_value(invocation.arguments[index]));
    }
    SCM extra = SCM_EOL;
    for (auto iterator = invocation.prefix.extra.rbegin();
         iterator != invocation.prefix.extra.rend(); ++iterator) {
        extra = scm_cons(
            scm_cons(scm_from_utf8_string(iterator->name.c_str()), setting_value(iterator->value)),
            extra);
    }
    SCM result = scm_c_make_vector(5, SCM_UNSPECIFIED);
    scm_c_vector_set_x(result, 0, scm_from_utf8_symbol("invocation"));
    scm_c_vector_set_x(result, 1, arguments);
    scm_c_vector_set_x(
        result, 2, invocation.prefix.count ? scm_from_int64(*invocation.prefix.count) : SCM_BOOL_F);
    scm_c_vector_set_x(result, 3,
                       invocation.prefix.register_name
                           ? scm_from_utf8_string(invocation.prefix.register_name->c_str())
                           : SCM_BOOL_F);
    scm_c_vector_set_x(result, 4, extra);
    return result;
}

bool setting_value_p(SCM value) {
    if (scheme_boolean(value) || scm_is_string(value)) {
        return true;
    }
    if (scm_is_integer(value)) {
        return scm_is_signed_integer(value, std::numeric_limits<std::int64_t>::min(),
                                     std::numeric_limits<std::int64_t>::max()) != 0;
    }
    return scm_is_real(value) != 0;
}

SettingValue setting_from_scheme(SCM value) {
    if (scheme_boolean(value)) {
        return scheme_true(value);
    }
    if (scm_is_integer(value)) {
        return scm_to_int64(value);
    }
    if (scm_is_real(value)) {
        return scm_to_double(value);
    }
    return scheme_string(value);
}

std::vector<SettingValue> arguments_from_scheme(SCM values, const char* caller) {
    if (!scm_is_vector(values)) {
        scm_wrong_type_arg_msg(caller, 0, values, "vector of command arguments");
    }
    const std::size_t size = scm_c_vector_length(values);
    for (std::size_t index = 0; index < size; ++index) {
        if (!setting_value_p(scm_c_vector_ref(values, index))) {
            scm_misc_error(caller, "command argument has an unsupported type", SCM_EOL);
        }
    }
    std::vector<SettingValue> result;
    result.reserve(size);
    for (std::size_t index = 0; index < size; ++index) {
        result.push_back(setting_from_scheme(scm_c_vector_ref(values, index)));
    }
    return result;
}

bool symbol_is(SCM value, const char* expected) {
    return scheme_true(scm_symbol_p(value)) &&
           scheme_true(scm_eq_p(value, scm_from_utf8_symbol(expected)));
}

CommandPrefix command_prefix_from_scheme(SCM count_value, SCM register_value, SCM extra_value,
                                         const char* caller) {
    CommandPrefix prefix;
    if (!scheme_false(count_value)) {
        if (scm_is_signed_integer(count_value, std::numeric_limits<std::int64_t>::min(),
                                  std::numeric_limits<std::int64_t>::max()) == 0) {
            scm_wrong_type_arg_msg(caller, 1, count_value, "integer or #f");
        }
        prefix.count = scm_to_int64(count_value);
    }
    if (!scheme_false(register_value)) {
        prefix.register_name = scheme_name(register_value, caller, 2);
    }
    const long extra_count = scm_ilength(extra_value);
    if (extra_count < 0) {
        scm_wrong_type_arg_msg(caller, 3, extra_value, "proper association list");
    }
    prefix.extra.reserve(static_cast<std::size_t>(extra_count));
    for (long index = 0; index < extra_count; ++index) {
        const SCM entry = scm_car(extra_value);
        if (!scm_is_pair(entry) || !setting_value_p(scm_cdr(entry))) {
            scm_wrong_type_arg_msg(caller, 3, entry, "(name . setting-value) entry");
        }
        prefix.extra.push_back({.name = scheme_name(scm_car(entry), caller, 3),
                                .value = setting_from_scheme(scm_cdr(entry))});
        extra_value = scm_cdr(extra_value);
    }
    return prefix;
}

CommandResult command_result_from_scheme(SCM value, const CommandContext& context) {
    if (!scm_is_vector(value) || scm_c_vector_length(value) == 0) {
        return std::unexpected(CommandError{"Guile command returned an invalid result"});
    }
    const std::size_t size = scm_c_vector_length(value);
    const SCM tag = scm_c_vector_ref(value, 0);
    const auto completed = [&](CommandSelectionResult selection,
                               std::size_t value_index) -> CommandResult {
        if ((size != value_index && size != value_index + 1) ||
            (size == value_index + 1 && !setting_value_p(scm_c_vector_ref(value, value_index)))) {
            return std::unexpected(CommandError{"Guile completed result is malformed"});
        }
        CommandCompleted result{.value = std::nullopt, .selection = std::move(selection)};
        if (size == value_index + 1) {
            result.value = setting_from_scheme(scm_c_vector_ref(value, value_index));
        }
        return result;
    };
    if (symbol_is(tag, "completed")) {
        return completed(CommandSelectionDefault{}, 1);
    }
    if (symbol_is(tag, "completed-preserve")) {
        return completed(CommandSelectionPreserve{}, 1);
    }
    if (symbol_is(tag, "completed-collapse")) {
        return completed(CommandSelectionCollapse{}, 1);
    }
    if (symbol_is(tag, "completed-selection")) {
        if (size != 2 && size != 3) {
            return std::unexpected(CommandError{"Guile completed selection result is malformed"});
        }
        try {
            return completed(view_selection_from_scheme(scm_c_vector_ref(value, 1),
                                                        "command-completed/selection", 1),
                             2);
        } catch (const std::exception& exception) {
            return std::unexpected(CommandError{exception.what()});
        }
    }
    if (symbol_is(tag, "prefix")) {
        if (size != 4) {
            return std::unexpected(CommandError{"Guile command prefix result is malformed"});
        }
        return CommandPrefixUpdate{.prefix = command_prefix_from_scheme(
                                       scm_c_vector_ref(value, 1), scm_c_vector_ref(value, 2),
                                       scm_c_vector_ref(value, 3), "command-prefix")};
    }
    if (symbol_is(tag, "error")) {
        if (size != 2 || !scm_is_string(scm_c_vector_ref(value, 1))) {
            return std::unexpected(CommandError{"Guile error result is malformed"});
        }
        return std::unexpected(CommandError{scheme_string(scm_c_vector_ref(value, 1))});
    }
    if (symbol_is(tag, "dispatch") || symbol_is(tag, "dispatch-target")) {
        const bool targeted = symbol_is(tag, "dispatch-target");
        if (size != (targeted ? 4U : 3U) || !scm_is_string(scm_c_vector_ref(value, 1)) ||
            !scm_is_vector(scm_c_vector_ref(value, 2))) {
            return std::unexpected(CommandError{"Guile dispatch result is malformed"});
        }
        std::vector<SettingValue> arguments =
            arguments_from_scheme(scm_c_vector_ref(value, 2), "command-dispatch");
        const std::string name = scheme_string(scm_c_vector_ref(value, 1));
        const std::optional<CommandId> command = context.runtime().commands().find(name);
        if (!command) {
            return std::unexpected(CommandError{std::format("unknown command '{}'", name)});
        }
        std::optional<CommandTarget> target;
        if (targeted) {
            const SCM target_value = scm_c_vector_ref(value, 3);
            if (!scm_is_vector(target_value) || scm_c_vector_length(target_value) != 3) {
                return std::unexpected(CommandError{"Guile dispatch target is malformed"});
            }
            try {
                target = CommandTarget{
                    .window = entity_id_from_scheme<WindowTag>(scm_c_vector_ref(target_value, 0),
                                                               "command-dispatch-to", 3),
                    .buffer = entity_id_from_scheme<BufferTag>(scm_c_vector_ref(target_value, 1),
                                                               "command-dispatch-to", 3),
                    .view = entity_id_from_scheme<ViewTag>(scm_c_vector_ref(target_value, 2),
                                                           "command-dispatch-to", 3)};
            } catch (const std::exception& exception) {
                return std::unexpected(CommandError{exception.what()});
            }
        }
        return CommandDispatch{.command = *command,
                               .invocation = {.arguments = std::move(arguments), .prefix = {}},
                               .target = target};
    }
    if (symbol_is(tag, "interaction")) {
        if (size != 12 || !scheme_true(scm_symbol_p(scm_c_vector_ref(value, 1))) ||
            !scheme_true(scm_symbol_p(scm_c_vector_ref(value, 2))) ||
            !scheme_true(scm_symbol_p(scm_c_vector_ref(value, 3))) ||
            !scm_is_string(scm_c_vector_ref(value, 4)) ||
            !scm_is_string(scm_c_vector_ref(value, 5)) ||
            !scm_is_string(scm_c_vector_ref(value, 6)) ||
            !scm_is_string(scm_c_vector_ref(value, 7)) ||
            !scm_is_string(scm_c_vector_ref(value, 8)) ||
            !scheme_boolean(scm_c_vector_ref(value, 9)) ||
            !scm_is_string(scm_c_vector_ref(value, 10)) ||
            !scm_is_vector(scm_c_vector_ref(value, 11))) {
            return std::unexpected(CommandError{"Guile interaction result is malformed"});
        }
        const SCM kind_value = scm_c_vector_ref(value, 1);
        InteractionKind kind;
        if (symbol_is(kind_value, "text")) {
            kind = InteractionKind::Text;
        } else if (symbol_is(kind_value, "picker")) {
            kind = InteractionKind::Picker;
        } else {
            return std::unexpected(CommandError{"Guile interaction kind is unknown"});
        }
        std::vector<SettingValue> arguments =
            arguments_from_scheme(scm_c_vector_ref(value, 11), "command-interaction");
        const std::string accept_name = scheme_string(scm_c_vector_ref(value, 10));
        const std::optional<CommandId> accept = context.runtime().commands().find(accept_name);
        if (!accept) {
            return std::unexpected(
                CommandError{std::format("unknown accept command '{}'", accept_name)});
        }
        return InteractionRequest{
            .kind = kind,
            .keymap = scheme_name(scm_c_vector_ref(value, 2), "command-interaction", 2),
            .input_state = scheme_name(scm_c_vector_ref(value, 3), "command-interaction", 3),
            .buffer_name = scheme_string(scm_c_vector_ref(value, 4)),
            .prompt = scheme_string(scm_c_vector_ref(value, 5)),
            .initial_input = scheme_string(scm_c_vector_ref(value, 6)),
            .history = scheme_string(scm_c_vector_ref(value, 7)),
            .provider = scheme_string(scm_c_vector_ref(value, 8)),
            .allow_custom_input = scheme_true(scm_c_vector_ref(value, 9)),
            .accept_command = *accept,
            .arguments = std::move(arguments)};
    }
    return std::unexpected(CommandError{"Guile command returned an unknown result kind"});
}

std::vector<InteractionCandidate> provider_candidates_from_scheme(SCM value) {
    if (!scm_is_vector(value)) {
        throw std::invalid_argument("Guile provider must return a vector of candidates");
    }
    const std::size_t count = scm_c_vector_length(value);
    for (std::size_t index = 0; index < count; ++index) {
        const SCM candidate = scm_c_vector_ref(value, index);
        if (!scm_is_vector(candidate) || scm_c_vector_length(candidate) != 4) {
            throw std::invalid_argument("Guile provider candidate must be a four-element vector");
        }
        for (std::size_t field = 0; field < 4; ++field) {
            if (!scm_is_string(scm_c_vector_ref(candidate, field))) {
                throw std::invalid_argument("Guile provider candidate fields must be strings");
            }
        }
    }
    std::vector<InteractionCandidate> result;
    result.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const SCM candidate = scm_c_vector_ref(value, index);
        result.push_back({.value = scheme_string(scm_c_vector_ref(candidate, 0)),
                          .label = scheme_string(scm_c_vector_ref(candidate, 1)),
                          .detail = scheme_string(scm_c_vector_ref(candidate, 2)),
                          .filter_text = scheme_string(scm_c_vector_ref(candidate, 3))});
    }
    return result;
}

SCM completion_request_value(const CompletionRequest& request) {
    const char* trigger = "manual";
    switch (request.trigger.kind) {
    case CompletionTriggerKind::Manual:
        break;
    case CompletionTriggerKind::Character:
        trigger = "character";
        break;
    case CompletionTriggerKind::Automatic:
        trigger = "automatic";
        break;
    case CompletionTriggerKind::Incomplete:
        trigger = "incomplete";
        break;
    }
    SCM result = scm_c_make_vector(7, SCM_UNSPECIFIED);
    scm_c_vector_set_x(result, 0, scm_from_utf8_symbol("completion-request"));
    scm_c_vector_set_x(result, 1,
                       scm_from_utf8_stringn(request.query.data(), request.query.size()));
    scm_c_vector_set_x(result, 2, scm_from_uint32(request.anchor.value));
    scm_c_vector_set_x(result, 3, scm_from_uint32(request.caret.value));
    scm_c_vector_set_x(result, 4, scm_from_uint32(request.line));
    scm_c_vector_set_x(result, 5, scm_from_utf8_symbol(trigger));
    scm_c_vector_set_x(
        result, 6,
        scm_from_utf8_stringn(request.trigger.character.data(), request.trigger.character.size()));
    return result;
}

SCM completion_item_value(const CompletionItem& item) {
    SCM result = scm_c_make_vector(10, SCM_UNSPECIFIED);
    scm_c_vector_set_x(result, 0, scm_from_utf8_symbol("completion-item"));
    const auto set_text = [&](std::size_t index, const std::string& value) {
        scm_c_vector_set_x(result, index, scm_from_utf8_stringn(value.data(), value.size()));
    };
    set_text(1, item.label);
    set_text(2, item.kind);
    set_text(3, item.detail);
    set_text(4, item.filter_text);
    set_text(5, item.sort_text);
    set_text(6, item.edit ? item.edit->new_text : item.label);
    set_text(7, item.documentation);
    if (item.edit) {
        scm_c_vector_set_x(result, 8, scm_from_uint32(item.edit->replace_range.start.value));
        scm_c_vector_set_x(result, 9, scm_from_uint32(item.edit->replace_range.end.value));
    } else {
        scm_c_vector_set_x(result, 8, SCM_BOOL_F);
        scm_c_vector_set_x(result, 9, SCM_BOOL_F);
    }
    return result;
}

CompletionProviderResponse completion_response_from_scheme(SCM value, CompletionProvider provider,
                                                           const CompletionRequest& request,
                                                           bool needs_resolution = false) {
    if (!scm_is_vector(value) || scm_c_vector_length(value) != 3 ||
        !symbol_is(scm_c_vector_ref(value, 0), "completion-result") ||
        !scm_is_vector(scm_c_vector_ref(value, 1)) || !scheme_boolean(scm_c_vector_ref(value, 2))) {
        throw std::invalid_argument(
            "Guile completion provider must return #(completion-result candidates incomplete?)");
    }
    const SCM candidates = scm_c_vector_ref(value, 1);
    const std::size_t count = scm_c_vector_length(candidates);
    std::vector<CompletionItem> items;
    items.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const SCM candidate = scm_c_vector_ref(candidates, index);
        if (!scm_is_vector(candidate) || scm_c_vector_length(candidate) != 10 ||
            !symbol_is(scm_c_vector_ref(candidate, 0), "completion-item")) {
            throw std::invalid_argument(
                "Guile completion candidate must be a completion-item vector");
        }
        for (std::size_t field = 1; field <= 7; ++field) {
            if (!scm_is_string(scm_c_vector_ref(candidate, field))) {
                throw std::invalid_argument(
                    "Guile completion candidate text fields must be strings");
            }
        }
        const SCM start_value = scm_c_vector_ref(candidate, 8);
        const SCM end_value = scm_c_vector_ref(candidate, 9);
        if (scheme_false(start_value) != scheme_false(end_value) ||
            (!scheme_false(start_value) &&
             (scm_is_unsigned_integer(start_value, 0, std::numeric_limits<std::uint32_t>::max()) ==
                  0 ||
              scm_is_unsigned_integer(end_value, 0, std::numeric_limits<std::uint32_t>::max()) ==
                  0))) {
            throw std::invalid_argument(
                "Guile completion candidate range must contain two byte offsets or two #f values");
        }
        TextRange replacement{request.anchor, request.caret};
        if (!scheme_false(start_value)) {
            replacement = {TextOffset{scm_to_uint32(start_value)},
                           TextOffset{scm_to_uint32(end_value)}};
        }
        const std::string label = scheme_string(scm_c_vector_ref(candidate, 1));
        items.push_back(
            {.provider = provider,
             .filter_text = scheme_string(scm_c_vector_ref(candidate, 4)),
             .label = label,
             .kind = scheme_string(scm_c_vector_ref(candidate, 2)),
             .detail = scheme_string(scm_c_vector_ref(candidate, 3)),
             .edit = CompletionEdit{.insert_range = replacement,
                                    .replace_range = replacement,
                                    .new_text = scheme_string(scm_c_vector_ref(candidate, 6))},
             .sort_text = scheme_string(scm_c_vector_ref(candidate, 5)),
             .is_snippet = false,
             .resolved = !needs_resolution,
             .resolving = false,
             .resolve_error = {},
             .documentation = scheme_string(scm_c_vector_ref(candidate, 7)),
             .additional_edits = {},
             .raw = needs_resolution ? label : std::string{}});
    }
    return {.provider = provider,
            .items = std::move(items),
            .is_incomplete = scheme_true(scm_c_vector_ref(value, 2))};
}

SCM call_body(void* data) {
    auto& call = *static_cast<GuileCall*>(data);
    try {
        switch (call.operation) {
        case GuileCall::Operation::Load:
            call.result = scm_c_primitive_load(call.path.c_str());
            break;
        case GuileCall::Operation::LoadExtension:
            call.result = scm_call_2(scm_c_public_ref("cind extension", "load-extension-file"),
                                     scm_from_utf8_string(call.path.c_str()), call.host);
            break;
        case GuileCall::Operation::MakeEvaluationModule:
            call.result = scm_call_1(scm_c_public_ref("cind development", "make-evaluation-module"),
                                     call.host);
            break;
        case GuileCall::Operation::Evaluate:
            call.result =
                scm_call_3(scm_c_public_ref("cind development", "evaluate-source"), call.module,
                           scm_from_utf8_stringn(call.source.data(), call.source.size()),
                           scm_from_utf8_stringn(call.source_name.data(), call.source_name.size()));
            break;
        case GuileCall::Operation::InstallCommands:
            call.result =
                scm_call_1(scm_c_public_ref("cind core", "install-core-commands!"), call.host);
            call.count = scm_to_size_t(call.result);
            break;
        case GuileCall::Operation::InstallProviders:
            call.result =
                scm_call_1(scm_c_public_ref("cind core", "install-core-providers!"), call.host);
            call.count = scm_to_size_t(call.result);
            break;
        case GuileCall::Operation::InstallKeymaps:
            call.result =
                scm_call_1(scm_c_public_ref("cind core", "install-default-keymaps!"), call.host);
            call.count = scm_to_size_t(call.result);
            break;
        case GuileCall::Operation::InstallInputStates:
            call.result =
                scm_call_1(scm_c_public_ref("cind core", "install-input-states!"), call.host);
            call.count = scm_to_size_t(call.result);
            break;
        case GuileCall::Operation::InstallModes:
            call.result =
                scm_call_1(scm_c_public_ref("cind core", "install-core-modes!"), call.host);
            call.count = scm_to_size_t(call.result);
            break;
        case GuileCall::Operation::InstallResourcePolicies:
            call.result = scm_call_1(
                scm_c_public_ref("cind core", "install-core-resource-policies!"), call.host);
            call.count = scm_to_size_t(call.result);
            break;
        case GuileCall::Operation::InstallBufferLifecyclePolicies:
            call.result = scm_call_1(
                scm_c_public_ref("cind core", "install-buffer-lifecycle-policies!"), call.host);
            break;
        case GuileCall::Operation::InstallPointerPolicies:
            call.result =
                scm_call_1(scm_c_public_ref("cind core", "install-pointer-policies!"), call.host);
            call.count = scm_to_size_t(call.result);
            break;
        case GuileCall::Operation::InstallPresentationPolicies:
            call.result = scm_call_1(
                scm_c_public_ref("cind core", "install-presentation-policies!"), call.host);
            call.count = scm_to_size_t(call.result);
            break;
        case GuileCall::Operation::InstallDisplayPolicy:
            call.result =
                scm_call_1(scm_c_public_ref("cind core", "install-display-policy!"), call.host);
            call.count = scm_to_size_t(call.result);
            break;
        case GuileCall::Operation::ResolveStartupPlan:
            call.result = scm_call_2(scm_c_public_ref("cind lifecycle", "resolve-startup-plan"),
                                     call.host, startup_facts_value(*call.startup_facts));
            call.startup_plan =
                startup_plan_from_scheme(require_host(call.host, "resolve-startup-plan"),
                                         call.result, *call.startup_facts, "resolve-startup-plan");
            break;
        case GuileCall::Operation::ResolveSessionPlan:
            call.result = scm_call_2(scm_c_public_ref("cind lifecycle", "resolve-session-plan"),
                                     call.host, session_facts_value(*call.session_facts));
            call.session_plan =
                session_plan_from_scheme(require_host(call.host, "resolve-session-plan"),
                                         call.result, *call.session_facts, "resolve-session-plan");
            break;
        case GuileCall::Operation::SetStartupPlaceholder:
            call.result = scm_call_2(
                scm_c_public_ref("cind lifecycle", "set-startup-placeholder!"), call.host,
                call.enabled ? entity_id(call.buffer.slot, call.buffer.generation) : SCM_BOOL_F);
            break;
        case GuileCall::Operation::ResolveCloseCommand:
            call.result =
                scm_call_3(scm_c_public_ref("cind lifecycle", "resolve-close-command"), call.host,
                           command_context_value(*call.context), scm_from_bool(call.force));
            call.command = require_command(require_host(call.host, "resolve-close-command"),
                                           call.result, "resolve-close-command", 0);
            break;
        case GuileCall::Operation::HandlePointer:
            call.result =
                scm_call_3(scm_c_public_ref("cind pointer", "handle-pointer!"), call.host,
                           command_context_value(*call.context),
                           pointer_event_value(*call.pointer_event, call.pending_key_sequence));
            if (!scheme_boolean(call.result)) {
                scm_wrong_type_arg_msg("handle-pointer!", 0, call.result, "boolean");
            }
            call.enabled = scheme_true(call.result);
            break;
        case GuileCall::Operation::HandleScroll:
            call.result = scm_call_3(scm_c_public_ref("cind pointer", "handle-scroll!"), call.host,
                                     command_context_value(*call.context),
                                     scroll_input_value(call.scroll_input));
            if (!scheme_boolean(call.result)) {
                scm_wrong_type_arg_msg("handle-scroll!", 0, call.result, "boolean");
            }
            call.enabled = scheme_true(call.result);
            break;
        case GuileCall::Operation::OpenResource:
            call.result = scm_call_6(scm_c_public_ref("cind core", "open-resource-with-intent!"),
                                     call.host, entity_id(call.window.slot, call.window.generation),
                                     scm_from_utf8_string(call.path.c_str()),
                                     call.line ? scm_from_uint32(*call.line) : SCM_BOOL_F,
                                     call.column ? scm_from_uint32(*call.column) : SCM_BOOL_F,
                                     name_symbol(call.intent));
            break;
        case GuileCall::Operation::RestoreWorkbenchSession:
            call.result =
                scm_call_2(scm_c_public_ref("cind core", "restore-workbench-session!"), call.host,
                           scm_from_utf8_stringn(call.source.data(), call.source.size()));
            break;
        case GuileCall::Operation::BufferEdited:
            call.result = scm_call_4(scm_c_public_ref("cind lifecycle", "buffer-edited!"),
                                     call.host, entity_id(call.buffer.slot, call.buffer.generation),
                                     entity_id(call.view.slot, call.view.generation),
                                     scm_from_uint64(call.revision));
            break;
        case GuileCall::Operation::InteractionStarted: {
            const InteractionRequest& request = *call.interaction_request;
            SCM arguments = scm_c_make_vector(request.arguments.size(), SCM_UNSPECIFIED);
            for (std::size_t index = 0; index < request.arguments.size(); ++index) {
                scm_c_vector_set_x(arguments, index, setting_value(request.arguments[index]));
            }
            SCM target = scm_c_make_vector(3, SCM_UNSPECIFIED);
            scm_c_vector_set_x(target, 0,
                               entity_id(call.interaction_origin.window.slot,
                                         call.interaction_origin.window.generation));
            scm_c_vector_set_x(target, 1,
                               entity_id(call.interaction_origin.buffer.slot,
                                         call.interaction_origin.buffer.generation));
            scm_c_vector_set_x(target, 2,
                               entity_id(call.interaction_origin.view.slot,
                                         call.interaction_origin.view.generation));
            SCM state = scm_c_make_vector(11, SCM_UNSPECIFIED);
            scm_c_vector_set_x(
                state, 0,
                scm_from_utf8_symbol(request.kind == InteractionKind::Picker ? "picker" : "text"));
            scm_c_vector_set_x(state, 1, scm_from_utf8_string(request.keymap.c_str()));
            scm_c_vector_set_x(state, 2, scm_from_utf8_string(request.input_state.c_str()));
            scm_c_vector_set_x(state, 3, scm_from_utf8_string(request.buffer_name.c_str()));
            scm_c_vector_set_x(state, 4, scm_from_utf8_string(request.prompt.c_str()));
            scm_c_vector_set_x(state, 5, scm_from_utf8_string(request.history.c_str()));
            scm_c_vector_set_x(state, 6, scm_from_bool(request.allow_custom_input));
            scm_c_vector_set_x(state, 7, scm_from_utf8_string(request.provider.c_str()));
            const std::string& accept_name =
                call.runtime->commands().definition(request.accept_command).name;
            scm_c_vector_set_x(state, 8, scm_from_utf8_string(accept_name.c_str()));
            scm_c_vector_set_x(state, 9, arguments);
            scm_c_vector_set_x(state, 10, target);
            call.result = scm_call_2(scm_c_public_ref("cind minibuffer", "interaction-started!"),
                                     call.host, state);
            break;
        }
        case GuileCall::Operation::InteractionPolicyState: {
            call.result = scm_call_1(
                scm_c_public_ref("cind minibuffer", "interaction-policy-state"), call.host);
            if (scheme_false(call.result)) {
                call.interaction_policy_state.reset();
                break;
            }
            if (!scm_is_vector(call.result) || scm_c_vector_length(call.result) != 8 ||
                !scheme_true(scm_symbol_p(scm_c_vector_ref(call.result, 0))) ||
                !scm_is_string(scm_c_vector_ref(call.result, 1)) ||
                !scm_is_string(scm_c_vector_ref(call.result, 2)) ||
                !scm_is_string(scm_c_vector_ref(call.result, 3)) ||
                !scm_is_string(scm_c_vector_ref(call.result, 4)) ||
                !scm_is_string(scm_c_vector_ref(call.result, 5)) ||
                !scheme_boolean(scm_c_vector_ref(call.result, 6)) ||
                !scm_is_string(scm_c_vector_ref(call.result, 7))) {
                scm_misc_error("interaction-policy-state", "interaction policy state is malformed",
                               SCM_EOL);
            }
            const SCM kind = scm_c_vector_ref(call.result, 0);
            if (!symbol_is(kind, "text") && !symbol_is(kind, "picker")) {
                scm_misc_error("interaction-policy-state", "interaction kind is unknown", SCM_EOL);
            }
            call.interaction_policy_state = GuileInteractionPolicyState{
                .kind = symbol_is(kind, "picker") ? InteractionKind::Picker : InteractionKind::Text,
                .keymap = scheme_string(scm_c_vector_ref(call.result, 1)),
                .input_state = scheme_string(scm_c_vector_ref(call.result, 2)),
                .buffer_name = scheme_string(scm_c_vector_ref(call.result, 3)),
                .prompt = scheme_string(scm_c_vector_ref(call.result, 4)),
                .history = scheme_string(scm_c_vector_ref(call.result, 5)),
                .allow_custom_input = scheme_true(scm_c_vector_ref(call.result, 6)),
                .provider = scheme_string(scm_c_vector_ref(call.result, 7))};
            break;
        }
        case GuileCall::Operation::MinibufferHistoryState: {
            call.result =
                scm_call_3(scm_c_public_ref("cind minibuffer", "minibuffer-history-state"),
                           call.host, entity_id(call.buffer.slot, call.buffer.generation),
                           scm_from_utf8_stringn(call.history.data(), call.history.size()));
            if (!scm_is_vector(call.result) || scm_c_vector_length(call.result) != 3 ||
                scm_is_unsigned_integer(scm_c_vector_ref(call.result, 0), 0,
                                        std::numeric_limits<std::size_t>::max()) == 0 ||
                (!scheme_false(scm_c_vector_ref(call.result, 1)) &&
                 scm_is_unsigned_integer(scm_c_vector_ref(call.result, 1), 0,
                                         std::numeric_limits<std::size_t>::max()) == 0) ||
                !scm_is_string(scm_c_vector_ref(call.result, 2))) {
                scm_misc_error("minibuffer-history-state",
                               "history state must be #(entry-count index-or-#f draft)", SCM_EOL);
            }
            call.minibuffer_history.entries = scm_to_size_t(scm_c_vector_ref(call.result, 0));
            if (!scheme_false(scm_c_vector_ref(call.result, 1))) {
                call.minibuffer_history.index = scm_to_size_t(scm_c_vector_ref(call.result, 1));
            }
            call.minibuffer_history.draft = scheme_string(scm_c_vector_ref(call.result, 2));
            break;
        }
        case GuileCall::Operation::InteractionSelection:
            call.result =
                scm_call_1(scm_c_public_ref("cind minibuffer", "interaction-selection"), call.host);
            if (!scheme_false(call.result)) {
                if (scm_is_unsigned_integer(call.result, 0,
                                            std::numeric_limits<std::size_t>::max()) == 0) {
                    scm_misc_error("interaction-selection",
                                   "selection must be a non-negative integer or #f", SCM_EOL);
                }
                call.interaction_selection = scm_to_size_t(call.result);
            }
            break;
        case GuileCall::Operation::CommandFeedbackState:
            call.result =
                scm_call_1(scm_c_public_ref("cind command", "command-feedback-state"), call.host);
            if (!scm_is_vector(call.result) || scm_c_vector_length(call.result) != 3 ||
                !scm_is_string(scm_c_vector_ref(call.result, 0)) ||
                !scm_is_string(scm_c_vector_ref(call.result, 1)) ||
                !scm_is_string(scm_c_vector_ref(call.result, 2))) {
                scm_misc_error("command-feedback-state",
                               "command feedback state must be #(message last-key last-command)",
                               SCM_EOL);
            }
            call.command_feedback.message = scheme_string(scm_c_vector_ref(call.result, 0));
            call.command_feedback.last_key = scheme_string(scm_c_vector_ref(call.result, 1));
            call.command_feedback.last_command = scheme_string(scm_c_vector_ref(call.result, 2));
            break;
        case GuileCall::Operation::ApplicationState:
            call.result =
                scm_call_1(scm_c_public_ref("cind application", "application-state"), call.host);
            if (!scm_is_vector(call.result) || scm_c_vector_length(call.result) != 2 ||
                !scheme_boolean(scm_c_vector_ref(call.result, 0)) ||
                !scheme_boolean(scm_c_vector_ref(call.result, 1))) {
                scm_misc_error("application-state",
                               "application state must be #(exit-requested? reveal-caret?)",
                               SCM_EOL);
            }
            call.application_state.exit_requested = scheme_true(scm_c_vector_ref(call.result, 0));
            call.application_state.reveal_caret = scheme_true(scm_c_vector_ref(call.result, 1));
            break;
        case GuileCall::Operation::SetCaretReveal:
            call.result = scm_call_2(scm_c_public_ref("cind application", "set-caret-reveal!"),
                                     call.host, scm_from_bool(call.enabled));
            break;
        case GuileCall::Operation::BufferSavingState:
            call.result =
                scm_call_2(scm_c_public_ref("cind lifecycle", "buffer-saving?"), call.host,
                           entity_id(call.buffer.slot, call.buffer.generation));
            call.enabled = scheme_true(call.result);
            break;
        case GuileCall::Operation::CommandInput:
            call.result = scm_call_3(scm_c_public_ref("cind command", "command-input!"), call.host,
                                     scm_from_utf8_stringn(call.source.data(), call.source.size()),
                                     scm_from_bool(call.clear_message));
            break;
        case GuileCall::Operation::CommandResultFeedback:
            call.result =
                scm_call_6(scm_c_public_ref("cind command", "command-result!"), call.host,
                           scm_from_utf8_symbol(command_loop_status_name(call.command_status)),
                           scm_from_bool(call.enabled),
                           call.command_name ? scm_from_utf8_stringn(call.command_name->data(),
                                                                     call.command_name->size())
                                             : SCM_BOOL_F,
                           scm_from_bool(call.interaction_started),
                           scm_from_utf8_stringn(call.source.data(), call.source.size()));
            break;
        case GuileCall::Operation::RecordCommand:
            call.result = scm_call_2(scm_c_public_ref("cind command", "record-command!"), call.host,
                                     scm_from_utf8_stringn(call.source.data(), call.source.size()));
            break;
        case GuileCall::Operation::SetMessage:
            call.result = scm_call_2(scm_c_public_ref("cind command", "set-message!"), call.host,
                                     scm_from_utf8_stringn(call.source.data(), call.source.size()));
            break;
        case GuileCall::Operation::ResolveKeymapPolicy:
            call.result = scm_call_2(scm_c_public_ref("cind command", "resolve-keymap-policy"),
                                     call.host, command_context_value(*call.context));
            call.keymap_policy =
                keymap_policy_from_scheme(require_host(call.host, "resolve-keymap-policy"),
                                          call.result, "resolve-keymap-policy");
            break;
        case GuileCall::Operation::ResolveBaseKeymapPolicy:
            call.result = scm_call_2(scm_c_public_ref("cind command", "resolve-base-keymap-policy"),
                                     call.host, command_context_value(*call.context));
            call.keymap_policy =
                keymap_policy_from_scheme(require_host(call.host, "resolve-base-keymap-policy"),
                                          call.result, "resolve-base-keymap-policy");
            break;
        case GuileCall::Operation::ChromeContent:
            call.result = scm_call_3(scm_c_public_ref("cind command", "resolve-chrome-content"),
                                     call.host, command_context_value(*call.context),
                                     chrome_facts_value(*call.chrome_facts));
            call.chrome_content = chrome_content_from_scheme(call.result, "resolve-chrome-content");
            break;
        case GuileCall::Operation::ModelineContent:
            call.result = scm_call_3(scm_c_public_ref("cind command", "resolve-modeline-content"),
                                     call.host, command_context_value(*call.context),
                                     modeline_facts_value(*call.modeline_facts));
            call.modeline_content =
                modeline_content_from_scheme(call.result, "resolve-modeline-content");
            break;
        case GuileCall::Operation::PresentationProfile:
            call.result = scm_call_1(
                scm_c_public_ref("cind command", "resolve-presentation-profile"), call.host);
            call.presentation_profile =
                presentation_profile_from_scheme(call.result, "resolve-presentation-profile");
            break;
        case GuileCall::Operation::DisplayPlan:
            call.result = scm_call_2(scm_c_public_ref("cind command", "resolve-display-plan"),
                                     call.host, display_facts_value(*call.display_facts));
            call.display_plan = display_plan_from_scheme(call.result, "resolve-display-plan");
            break;
        case GuileCall::Operation::FallbackDisplayPlan:
            call.result = scm_call_2(scm_c_public_ref("cind core", "fallback-display-plan"),
                                     call.host, display_facts_value(*call.display_facts));
            call.display_plan = display_plan_from_scheme(call.result, "fallback-display-plan");
            break;
        case GuileCall::Operation::ProjectSearchRunning:
            call.result =
                scm_call_1(scm_c_public_ref("cind core", "project-search-running?"), call.host);
            call.enabled = scheme_true(call.result);
            break;
        case GuileCall::Operation::ProjectIndexUpdated:
            call.result =
                scm_call_2(scm_c_public_ref("cind core", "project-index-updated!"), call.host,
                           entity_id(call.project.slot, call.project.generation));
            break;
        case GuileCall::Operation::InvokeCommand:
            call.result = scm_call_2(call.procedure, command_context_value(*call.context),
                                     command_invocation_value(*call.invocation));
            call.command_result = command_result_from_scheme(call.result, *call.context);
            break;
        case GuileCall::Operation::InvokeProvider:
            call.result = scm_call_2(call.procedure, command_context_value(*call.context),
                                     scm_from_utf8_string(call.query.c_str()));
            break;
        case GuileCall::Operation::InvokeCompletionProvider:
            call.result = scm_call_2(call.procedure, command_context_value(*call.context),
                                     completion_request_value(*call.completion_request));
            if (!scm_is_vector(call.result) || scm_c_vector_length(call.result) == 0 ||
                !symbol_is(scm_c_vector_ref(call.result, 0), "async-completion-provider")) {
                call.completion_response = completion_response_from_scheme(
                    call.result, call.completion_provider, *call.completion_request,
                    call.completion_needs_resolution);
            }
            break;
        case GuileCall::Operation::InvokeCompletionResolver: {
            call.result = scm_call_3(call.procedure, command_context_value(*call.context),
                                     completion_request_value(*call.completion_request),
                                     completion_item_value(*call.completion_item));
            SCM candidates = scm_c_make_vector(1, SCM_UNSPECIFIED);
            scm_c_vector_set_x(candidates, 0, call.result);
            SCM response = scm_c_make_vector(3, SCM_UNSPECIFIED);
            scm_c_vector_set_x(response, 0, scm_from_utf8_symbol("completion-result"));
            scm_c_vector_set_x(response, 1, candidates);
            scm_c_vector_set_x(response, 2, SCM_BOOL_F);
            CompletionProviderResponse converted = completion_response_from_scheme(
                response, call.completion_provider, *call.completion_request);
            call.resolved_completion_item = std::move(converted.items.front());
            break;
        }
        case GuileCall::Operation::TransformProviderResult:
            call.result = scm_call_1(call.procedure, call.argument);
            call.provider_candidates = provider_candidates_from_scheme(call.result);
            break;
        case GuileCall::Operation::TransformCompletionProviderResult:
            call.result = scm_call_1(call.procedure, call.argument);
            call.completion_response = completion_response_from_scheme(
                call.result, call.completion_provider, *call.completion_request,
                call.completion_needs_resolution);
            break;
        case GuileCall::Operation::InvokeInputHandler:
            call.result = scm_call_2(call.procedure, command_context_value(*call.context),
                                     scm_from_utf8_string(format_key_stroke(call.key).c_str()));
            break;
        case GuileCall::Operation::InvokePositionHints:
            call.result = scm_call_1(call.procedure, command_context_value(*call.context));
            call.position_hints =
                position_hints_from_scheme(call.result, "input state position-hints provider");
            break;
        case GuileCall::Operation::InvokeInputStateObserver: {
            const InputStateChange& change = *call.input_state_change;
            const auto state_name = [&](std::optional<InputStateId> state) {
                return state ? name_symbol(call.runtime->input_states().definition(*state).name)
                             : SCM_BOOL_F;
            };
            SCM event = scm_c_make_vector(4, SCM_UNSPECIFIED);
            const char* kind = change.kind == InputStateChangeKind::Push  ? "push"
                               : change.kind == InputStateChangeKind::Pop ? "pop"
                                                                          : "base";
            scm_c_vector_set_x(event, 0, scm_from_utf8_symbol(kind));
            scm_c_vector_set_x(event, 1, entity_id(change.view.slot, change.view.generation));
            scm_c_vector_set_x(event, 2, state_name(change.from));
            scm_c_vector_set_x(event, 3, state_name(change.to));
            call.result = scm_call_1(call.procedure, event);
            break;
        }
        case GuileCall::Operation::InvokeModePolicyObserver: {
            const BufferModePolicyChange& change = *call.mode_policy_change;
            SCM event = scm_c_make_vector(5, SCM_BOOL_F);
            const char* kind = change.kind == BufferModeChangeKind::Major ? "major"
                               : change.kind == BufferModeChangeKind::MinorEnabled
                                   ? "minor-enabled"
                                   : "minor-disabled";
            scm_c_vector_set_x(event, 0, scm_from_utf8_symbol(kind));
            scm_c_vector_set_x(event, 1, entity_id(change.buffer.slot, change.buffer.generation));
            if (change.mode) {
                scm_c_vector_set_x(
                    event, 2, name_symbol(call.runtime->modes().definition(*change.mode).name));
            }
            scm_c_vector_set_x(event, 3, mode_policy_value(change.before, *call.runtime));
            scm_c_vector_set_x(event, 4, mode_policy_value(change.after, *call.runtime));
            call.result = scm_call_1(call.procedure, event);
            break;
        }
        case GuileCall::Operation::CheckEnabled:
            call.result = scm_call_1(call.procedure, command_context_value(*call.context));
            call.enabled = scheme_true(call.result);
            break;
        }
    } catch (...) {
        call.cpp_failure = std::current_exception();
    }
    return call.result;
}

SCM call_handler(void* data, SCM tag, SCM arguments) {
    auto& call = *static_cast<GuileCall*>(data);
    try {
        const SCM message = scm_simple_format(SCM_BOOL_F, scm_from_utf8_string("~S: ~S"),
                                              scm_list_2(tag, arguments));
        call.error = scheme_string(message);
    } catch (...) {
        call.cpp_failure = std::current_exception();
    }
    return SCM_UNSPECIFIED;
}

std::expected<SCM, std::string> run_guile_call(GuileCall& call) {
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

std::expected<GuileEvaluationResult, std::string> evaluation_result_from_scheme(SCM value) {
    try {
        if (!scm_is_vector(value) || scm_c_vector_length(value) != 4) {
            return std::unexpected("Scheme evaluator returned a malformed result");
        }
        const SCM status = scm_c_vector_ref(value, 0);
        const SCM payload = scm_c_vector_ref(value, 1);
        const SCM output = scm_c_vector_ref(value, 2);
        const SCM error_output = scm_c_vector_ref(value, 3);
        if (!scm_is_string(output) || !scm_is_string(error_output)) {
            return std::unexpected("Scheme evaluator returned malformed output streams");
        }
        GuileEvaluationResult result{.values = {},
                                     .output = scheme_string(output),
                                     .error_output = scheme_string(error_output),
                                     .error = std::nullopt};
        if (symbol_is(status, "error")) {
            if (!scm_is_string(payload)) {
                return std::unexpected("Scheme evaluator returned a malformed error");
            }
            result.error = scheme_string(payload);
            return result;
        }
        if (!symbol_is(status, "ok") || !scm_is_vector(payload)) {
            return std::unexpected("Scheme evaluator returned an unknown result kind");
        }
        const std::size_t count = scm_c_vector_length(payload);
        result.values.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            const SCM rendered = scm_c_vector_ref(payload, index);
            if (!scm_is_string(rendered)) {
                return std::unexpected("Scheme evaluator returned a non-string value");
            }
            result.values.push_back(scheme_string(rendered));
        }
        return result;
    } catch (const std::exception& exception) {
        return std::unexpected(
            std::format("failed to decode Scheme evaluation result: {}", exception.what()));
    }
}

std::expected<GuileEvaluationResult, std::string> evaluate_source(HostLease& host,
                                                                  GuileEvaluationRequest request) {
    const std::shared_ptr<GuileState> state = host.state;
    if (!state || !state->active) {
        return std::unexpected("Guile runtime has expired");
    }
    if (std::this_thread::get_id() != state->owner) {
        return std::unexpected("Scheme evaluation must run on the editor thread");
    }
    if (request.source_name.empty()) {
        return std::unexpected("Scheme evaluation source name is empty");
    }
    if (!state->evaluation_module_initialized) {
        GuileCall make_module;
        make_module.operation = GuileCall::Operation::MakeEvaluationModule;
        make_module.host = host.capability;
        const std::expected<SCM, std::string> created = run_guile_call(make_module);
        if (!created) {
            state->last_error = created.error();
            return std::unexpected(*state->last_error);
        }
        state->evaluation_module = *created;
        (void)scm_gc_protect_object(state->evaluation_module);
        state->evaluation_module_initialized = true;
    }

    const std::size_t commands = host.commands_installed;
    const std::size_t providers = host.providers_installed;
    const std::size_t bindings = host.bindings_installed;
    const std::size_t input_states = host.input_states_installed;
    const std::size_t input_strategies = host.input_strategies_installed;
    const std::size_t modes = host.modes_installed;
    const std::size_t resource_policies = host.resource_policies_installed;
    const std::string previous_source = state->definition_source;
    state->definition_source = std::format("scheme:{}", request.source_name);

    GuileCall call;
    call.operation = GuileCall::Operation::Evaluate;
    call.source = std::string(request.source);
    call.source_name = std::string(request.source_name);
    call.module = state->evaluation_module;
    std::expected<SCM, std::string> evaluated = run_guile_call(call);
    state->definition_source = previous_source;

    if (host.commands_installed != commands) {
        ++state->command_revision;
    }
    if (host.providers_installed != providers) {
        ++state->provider_revision;
    }
    if (host.bindings_installed != bindings) {
        ++state->binding_revision;
    }
    if (host.input_states_installed != input_states ||
        host.input_strategies_installed != input_strategies) {
        ++state->input_state_revision;
    }
    if (host.modes_installed != modes) {
        ++state->mode_revision;
    }
    if (host.resource_policies_installed != resource_policies) {
        ++state->resource_policy_revision;
    }

    if (!evaluated) {
        state->last_error = evaluated.error();
        return std::unexpected(*state->last_error);
    }
    std::expected<GuileEvaluationResult, std::string> result =
        evaluation_result_from_scheme(*evaluated);
    if (!result) {
        state->last_error = result.error();
        return std::unexpected(*state->last_error);
    }
    state->last_error = result->error;
    return result;
}

std::filesystem::path bundled_module_path(std::string_view name) {
    return std::filesystem::path(CIND_BUNDLED_SCHEME_DIR) / "cind" / std::format("{}.scm", name);
}

CommandResult invoke_script_command(const std::shared_ptr<GuileState>& state,
                                    std::size_t command_index, CommandContext& context,
                                    const CommandInvocation& invocation) {
    if (std::this_thread::get_id() != state->owner) {
        return std::unexpected(CommandError{"Guile command invoked outside its editor thread"});
    }
    if (!state->active || command_index >= state->commands.size()) {
        return std::unexpected(CommandError{"Guile command runtime has expired"});
    }
    GuileCall call;
    call.operation = GuileCall::Operation::InvokeCommand;
    call.procedure = state->commands[command_index].execute;
    call.context = &context;
    call.invocation = &invocation;
    std::expected<SCM, std::string> result = run_guile_call(call);
    if (!result) {
        state->last_error = result.error();
        return std::unexpected(
            CommandError{std::format("Guile command failed: {}", result.error())});
    }
    if (!call.command_result) {
        state->last_error = "Guile command produced no result";
        return std::unexpected(CommandError{*state->last_error});
    }
    state->last_error.reset();
    return std::move(*call.command_result);
}

struct GuileAsyncTransformTask {
    GuileAsyncTransformTask(std::weak_ptr<GuileState> runtime_value,
                            ScriptAsyncRequest request_value, SCM transform_value,
                            GuileAsyncBridge* bridge_value, std::thread::id owner_value)
        : runtime(std::move(runtime_value)), request(std::move(request_value)),
          transform(transform_value), bridge(bridge_value), owner(owner_value) {}

    GuileAsyncTransformTask(const GuileAsyncTransformTask&) = delete;
    GuileAsyncTransformTask& operator=(const GuileAsyncTransformTask&) = delete;

    std::weak_ptr<GuileState> runtime;
    std::optional<ScriptAsyncRequest> request;
    SCM transform = SCM_UNDEFINED;
    GuileAsyncBridge* bridge = nullptr;
    std::thread::id owner;

    ~GuileAsyncTransformTask() {
        if (std::this_thread::get_id() != owner) {
            std::terminate();
        }
        (void)scm_gc_unprotect_object(transform);
    }
};

struct GuileCompletionTransformTask {
    std::shared_ptr<GuileAsyncTransformTask> async;
    CompletionProvider provider;
    CompletionRequest request;
    bool needs_resolution = false;
};

std::shared_ptr<GuileAsyncTransformTask>
make_guile_async_transform_task(const std::shared_ptr<GuileState>& state, SCM value,
                                const char* caller, GuileAsyncBridge* async_bridge) {
    ScriptAsyncRequest request =
        script_async_request_from_scheme(scm_c_vector_ref(value, 1), caller, 1);
    SCM transform = scm_c_vector_ref(value, 2);
    (void)scm_gc_protect_object(transform);
    try {
        return std::make_shared<GuileAsyncTransformTask>(state, std::move(request), transform,
                                                         async_bridge, state->owner);
    } catch (...) {
        (void)scm_gc_unprotect_object(transform);
        throw;
    }
}

InteractionProviderResult invoke_script_provider(const std::shared_ptr<GuileState>& state,
                                                 std::size_t provider_index,
                                                 CommandContext& context, std::string_view query,
                                                 GuileAsyncBridge* async_bridge) {
    if (std::this_thread::get_id() != state->owner) {
        throw std::logic_error("Guile provider invoked outside its editor thread");
    }
    if (!state->active || provider_index >= state->providers.size()) {
        throw std::runtime_error("Guile provider runtime has expired");
    }
    GuileCall call;
    call.operation = GuileCall::Operation::InvokeProvider;
    call.procedure = state->providers[provider_index].complete;
    call.context = &context;
    call.query = std::string(query);
    std::expected<SCM, std::string> result = run_guile_call(call);
    if (!result) {
        state->last_error = result.error();
        throw std::runtime_error(std::format("Guile provider failed: {}", result.error()));
    }
    state->last_error.reset();
    if (!scm_is_vector(*result) || scm_c_vector_length(*result) == 0 ||
        !symbol_is(scm_c_vector_ref(*result, 0), "async-provider")) {
        return provider_candidates_from_scheme(*result);
    }
    if (scm_c_vector_length(*result) != 3 ||
        !scheme_true(scm_procedure_p(scm_c_vector_ref(*result, 2)))) {
        throw std::invalid_argument(
            "Guile async provider result must be #(async-provider request transform)");
    }

    auto task =
        make_guile_async_transform_task(state, *result, "interaction-provider-task", async_bridge);
    return InteractionCandidateAsync{
        .start = [task](InteractionCandidateAsync::Completed completed,
                        InteractionCandidateAsync::Failed failed,
                        InteractionCandidateAsync::Cancelled cancelled)
            -> std::expected<InteractionCandidateAsync::Cancel, std::string> {
            const std::shared_ptr<GuileState> runtime = task->runtime.lock();
            if (!runtime || !runtime->active || std::this_thread::get_id() != task->owner) {
                return std::unexpected("Guile provider runtime has expired");
            }
            if (!task->request) {
                return std::unexpected("Guile async provider task was already started");
            }
            if (task->bridge == nullptr) {
                return std::unexpected("Guile async provider host capability is unavailable");
            }
            std::expected<std::uint64_t, std::string> started = task->bridge->start_native_task(
                std::move(*task->request),
                {.completed =
                     [task, completed = std::move(completed),
                      failed](std::uint64_t, ScriptAsyncResult async_result) mutable {
                         const std::shared_ptr<GuileState> state = task->runtime.lock();
                         if (!state || !state->active ||
                             std::this_thread::get_id() != task->owner) {
                             failed("Guile provider runtime has expired");
                             return;
                         }
                         GuileCall transform_call;
                         transform_call.operation = GuileCall::Operation::TransformProviderResult;
                         transform_call.procedure = task->transform;
                         transform_call.argument =
                             script_async_result_to_scheme(std::move(async_result));
                         std::expected<SCM, std::string> transformed =
                             run_guile_call(transform_call);
                         if (!transformed) {
                             state->last_error = transformed.error();
                             failed(std::format("Guile async provider transform failed: {}",
                                                transformed.error()));
                             return;
                         }
                         state->last_error.reset();
                         completed(std::move(transform_call.provider_candidates));
                     },
                 .cancelled = [cancelled =
                                   std::move(cancelled)](std::uint64_t) mutable { cancelled(); },
                 .failed =
                     [failed = std::move(failed)](std::uint64_t, std::string message) mutable {
                         failed(std::move(message));
                     }});
            task->request.reset();
            if (!started) {
                return std::unexpected(std::move(started.error()));
            }
            const std::uint64_t id = *started;
            return InteractionCandidateAsync::Cancel{[task, id] {
                const std::shared_ptr<GuileState> runtime = task->runtime.lock();
                if (runtime && runtime->active && task->bridge != nullptr) {
                    (void)task->bridge->cancel_native_task(id);
                }
            }};
        }};
}

CompletionProviderResult invoke_script_completion_provider(
    const std::shared_ptr<GuileState>& state, std::size_t provider_index, CommandContext& context,
    const CompletionProvider& provider, const CompletionRequest& completion_request,
    GuileAsyncBridge* async_bridge) {
    if (std::this_thread::get_id() != state->owner) {
        throw std::logic_error("Guile completion provider invoked outside its editor thread");
    }
    if (!state->active || provider_index >= state->completion_providers.size()) {
        throw std::runtime_error("Guile completion provider runtime has expired");
    }
    const ScriptCompletionProvider& definition = state->completion_providers[provider_index];
    GuileCall call;
    call.operation = GuileCall::Operation::InvokeCompletionProvider;
    call.procedure = definition.complete;
    call.context = &context;
    call.completion_request = &completion_request;
    call.completion_provider = provider;
    call.completion_needs_resolution = !scheme_false(definition.resolve);
    std::expected<SCM, std::string> result = run_guile_call(call);
    if (!result) {
        state->last_error = result.error();
        throw std::runtime_error(
            std::format("Guile completion provider failed: {}", result.error()));
    }
    state->last_error.reset();
    if (!scm_is_vector(*result) || scm_c_vector_length(*result) == 0 ||
        !symbol_is(scm_c_vector_ref(*result, 0), "async-completion-provider")) {
        return std::move(call.completion_response);
    }
    if (scm_c_vector_length(*result) != 3 ||
        !scheme_true(scm_procedure_p(scm_c_vector_ref(*result, 2)))) {
        throw std::invalid_argument("Guile async completion provider result must be "
                                    "#(async-completion-provider request transform)");
    }

    auto task =
        make_guile_async_transform_task(state, *result, "completion-provider-task", async_bridge);
    auto completion_task = std::make_shared<GuileCompletionTransformTask>(
        GuileCompletionTransformTask{.async = std::move(task),
                                     .provider = provider,
                                     .request = completion_request,
                                     .needs_resolution = !scheme_false(definition.resolve)});
    return CompletionProviderAsync{
        .start = [completion_task](CompletionProviderAsync::Completed completed,
                                   CompletionProviderAsync::Failed failed,
                                   CompletionProviderAsync::Cancelled cancelled)
            -> std::expected<CompletionProviderAsync::Cancel, std::string> {
            const std::shared_ptr<GuileAsyncTransformTask>& task = completion_task->async;
            const std::shared_ptr<GuileState> runtime = task->runtime.lock();
            if (!runtime || !runtime->active || std::this_thread::get_id() != task->owner) {
                return std::unexpected("Guile completion provider runtime has expired");
            }
            if (!task->request) {
                return std::unexpected("Guile async completion provider task was already started");
            }
            if (task->bridge == nullptr) {
                return std::unexpected(
                    "Guile async completion provider host capability is unavailable");
            }
            std::expected<std::uint64_t, std::string> started = task->bridge->start_native_task(
                std::move(*task->request),
                {.completed =
                     [completion_task, completed = std::move(completed),
                      failed](std::uint64_t, ScriptAsyncResult async_result) mutable {
                         const std::shared_ptr<GuileAsyncTransformTask>& task =
                             completion_task->async;
                         const std::shared_ptr<GuileState> state = task->runtime.lock();
                         if (!state || !state->active ||
                             std::this_thread::get_id() != task->owner) {
                             failed("Guile completion provider runtime has expired");
                             return;
                         }
                         GuileCall transform_call;
                         transform_call.operation =
                             GuileCall::Operation::TransformCompletionProviderResult;
                         transform_call.procedure = task->transform;
                         transform_call.argument =
                             script_async_result_to_scheme(std::move(async_result));
                         transform_call.completion_provider = completion_task->provider;
                         transform_call.completion_request = &completion_task->request;
                         transform_call.completion_needs_resolution =
                             completion_task->needs_resolution;
                         std::expected<SCM, std::string> transformed =
                             run_guile_call(transform_call);
                         if (!transformed) {
                             state->last_error = transformed.error();
                             failed(
                                 std::format("Guile async completion provider transform failed: {}",
                                             transformed.error()));
                             return;
                         }
                         state->last_error.reset();
                         completed(std::move(transform_call.completion_response));
                     },
                 .cancelled = [cancelled =
                                   std::move(cancelled)](std::uint64_t) mutable { cancelled(); },
                 .failed =
                     [failed = std::move(failed)](std::uint64_t, std::string message) mutable {
                         failed(std::move(message));
                     }});
            task->request.reset();
            if (!started) {
                return std::unexpected(std::move(started.error()));
            }
            const std::uint64_t id = *started;
            return CompletionProviderAsync::Cancel{[completion_task, id] {
                const std::shared_ptr<GuileAsyncTransformTask>& task = completion_task->async;
                const std::shared_ptr<GuileState> runtime = task->runtime.lock();
                if (runtime && runtime->active && task->bridge != nullptr) {
                    (void)task->bridge->cancel_native_task(id);
                }
            }};
        }};
}

InputStateHandlerResult invoke_script_input_handler(const std::shared_ptr<GuileState>& state,
                                                    std::size_t state_index, EditorRuntime& runtime,
                                                    CommandContext& context, KeyStroke key) {
    if (std::this_thread::get_id() != state->owner || !state->active ||
        state_index >= state->input_states.size()) {
        return std::unexpected("Guile input state handler used outside its runtime");
    }
    GuileCall call;
    call.operation = GuileCall::Operation::InvokeInputHandler;
    call.procedure = state->input_states[state_index].handler;
    call.context = &context;
    call.key = key;
    const std::expected<SCM, std::string> result = run_guile_call(call);
    if (!result) {
        state->last_error = result.error();
        return std::unexpected(std::format("Guile input state handler failed: {}", result.error()));
    }
    state->last_error.reset();
    if (symbol_is(*result, "pass")) {
        return InputStateHandlerAction{.kind = InputStateHandlerActionKind::Pass,
                                       .command = {},
                                       .invocation = {},
                                       .feedback = std::nullopt};
    }
    if (symbol_is(*result, "consume")) {
        return InputStateHandlerAction{.kind = InputStateHandlerActionKind::Consume,
                                       .command = {},
                                       .invocation = {},
                                       .feedback = std::nullopt};
    }
    if (scm_is_vector(*result) &&
        (scm_c_vector_length(*result) == 2 || scm_c_vector_length(*result) == 3) &&
        symbol_is(scm_c_vector_ref(*result, 0), "dispatch")) {
        const SCM command_value = scm_c_vector_ref(*result, 1);
        const std::string name = scheme_name(command_value, "input-state-handler", 2);
        const std::optional<CommandId> command = runtime.commands().find(name);
        if (!command) {
            return std::unexpected(std::format("unknown input state command '{}'", name));
        }
        std::vector<SettingValue> arguments;
        if (scm_c_vector_length(*result) == 3) {
            arguments = arguments_from_scheme(scm_c_vector_ref(*result, 2), "input-state-handler");
        }
        return InputStateHandlerAction{
            .kind = InputStateHandlerActionKind::Dispatch,
            .command = *command,
            .invocation = {.arguments = std::move(arguments), .prefix = {}},
            .feedback = std::nullopt};
    }
    if (scm_is_vector(*result) && scm_c_vector_length(*result) == 3 &&
        symbol_is(scm_c_vector_ref(*result, 0), "pending")) {
        const SCM sequence_value = scm_c_vector_ref(*result, 1);
        const SCM hints_value = scm_c_vector_ref(*result, 2);
        if (!scm_is_string(sequence_value) || !scm_is_vector(hints_value)) {
            return std::unexpected(
                "input state pending result requires a string and a vector of hints");
        }
        std::vector<InputHint> hints =
            input_hints_from_scheme(hints_value, "input-state-handler", 3);
        return InputStateHandlerAction{.kind = InputStateHandlerActionKind::Pending,
                                       .command = {},
                                       .invocation = {},
                                       .feedback =
                                           InputFeedback{.sequence = scheme_string(sequence_value),
                                                         .hints = std::move(hints)}};
    }
    return std::unexpected("Guile input state handler must return pass, consume, "
                           "#(dispatch command [arguments]), or #(pending sequence hints)");
}

PositionHintProviderResult invoke_script_position_hints(const std::shared_ptr<GuileState>& state,
                                                        std::size_t provider_index,
                                                        CommandContext& context) {
    if (std::this_thread::get_id() != state->owner || !state->active ||
        provider_index >= state->position_hint_providers.size()) {
        return std::unexpected(
            "Guile input state position-hints provider used outside its runtime");
    }
    GuileCall call;
    call.operation = GuileCall::Operation::InvokePositionHints;
    call.procedure = state->position_hint_providers[provider_index].procedure;
    call.context = &context;
    const std::expected<SCM, std::string> result = run_guile_call(call);
    if (!result) {
        state->last_error = result.error();
        return std::unexpected(
            std::format("Guile input state position-hints provider failed: {}", result.error()));
    }
    state->last_error.reset();
    return std::move(call.position_hints);
}

void invoke_script_input_state_observer(const std::shared_ptr<GuileState>& state,
                                        std::size_t observer_index, EditorRuntime& runtime,
                                        const InputStateChange& change) {
    if (std::this_thread::get_id() != state->owner || !state->active ||
        observer_index >= state->input_state_observers.size()) {
        state->last_error = "Guile input state observer used outside its runtime";
        return;
    }
    GuileCall call;
    call.operation = GuileCall::Operation::InvokeInputStateObserver;
    call.procedure = state->input_state_observers[observer_index].procedure;
    call.input_state_change = &change;
    call.runtime = &runtime;
    const std::expected<SCM, std::string> result = run_guile_call(call);
    if (!result) {
        state->last_error = std::format("Guile input state observer failed: {}", result.error());
    }
}

void invoke_script_input_state_lifecycle(const std::shared_ptr<GuileState>& state,
                                         std::size_t lifecycle_index, EditorRuntime& runtime,
                                         const InputStateChange& change, bool entering) {
    if (std::this_thread::get_id() != state->owner || !state->active ||
        lifecycle_index >= state->input_state_lifecycles.size()) {
        state->last_error = "Guile input state lifecycle used outside its runtime";
        return;
    }
    const ScriptInputStateLifecycle& lifecycle = state->input_state_lifecycles[lifecycle_index];
    const SCM procedure = entering ? lifecycle.on_enter : lifecycle.on_exit;
    if (scheme_false(procedure)) {
        return;
    }
    GuileCall call;
    call.operation = GuileCall::Operation::InvokeInputStateObserver;
    call.procedure = procedure;
    call.input_state_change = &change;
    call.runtime = &runtime;
    const std::expected<SCM, std::string> result = run_guile_call(call);
    if (!result) {
        state->last_error = std::format("Guile input state {} callback failed: {}",
                                        entering ? "on-enter" : "on-exit", result.error());
    }
}

void invoke_script_mode_policy_observer(const std::shared_ptr<GuileState>& state,
                                        std::size_t observer_index, EditorRuntime& runtime,
                                        const BufferModePolicyChange& change) {
    if (std::this_thread::get_id() != state->owner || !state->active ||
        observer_index >= state->mode_policy_observers.size()) {
        state->last_error = "Guile mode policy observer used outside its runtime";
        return;
    }
    GuileCall call;
    call.operation = GuileCall::Operation::InvokeModePolicyObserver;
    call.procedure = state->mode_policy_observers[observer_index].procedure;
    call.mode_policy_change = &change;
    call.runtime = &runtime;
    const std::expected<SCM, std::string> result = run_guile_call(call);
    if (!result) {
        state->last_error = std::format("Guile mode policy observer failed: {}", result.error());
    }
}

bool script_command_enabled(const std::shared_ptr<GuileState>& state, std::size_t command_index,
                            const CommandContext& context) {
    if (std::this_thread::get_id() != state->owner || !state->active ||
        command_index >= state->commands.size()) {
        state->last_error = "Guile command predicate used outside its runtime";
        return false;
    }
    GuileCall call;
    call.operation = GuileCall::Operation::CheckEnabled;
    call.procedure = state->commands[command_index].enabled;
    call.context = &context;
    std::expected<SCM, std::string> result = run_guile_call(call);
    if (!result) {
        state->last_error = result.error();
        return false;
    }
    state->last_error.reset();
    return call.enabled;
}

} // namespace

class GuileRuntime::Impl {
public:
    explicit Impl(EditorRuntime& runtime, GuileHostServices services)
        : state_(std::make_shared<GuileState>()),
          async_bridge_(services.start_async_task, services.cancel_async_task, services.async_tasks,
                        [state = std::weak_ptr<GuileState>(state_)](std::string message) {
                            if (const std::shared_ptr<GuileState> locked = state.lock();
                                locked && locked->active &&
                                std::this_thread::get_id() == locked->owner) {
                                locked->last_error = std::move(message);
                            }
                        }) {
        (void)ensure_c_family_mechanisms(runtime);
        (void)ensure_scheme_mechanisms(runtime);
        state_->owner = std::this_thread::get_id();
        std::call_once(guile_once, initialize_guile);
        for (const std::string_view module : bundled_guile_modules) {
            GuileCall load;
            load.operation = GuileCall::Operation::Load;
            load.path = bundled_module_path(module).string();
            if (std::expected<SCM, std::string> loaded = run_guile_call(load); !loaded) {
                state_->last_error = loaded.error();
                throw std::runtime_error(std::format("failed to load bundled Guile module '{}': {}",
                                                     module, *state_->last_error));
            }
        }
        version_ = scheme_string(scm_version());
        lease_ = new HostLease{.runtime = &runtime,
                               .state = state_,
                               .services = std::move(services),
                               .async_bridge = &async_bridge_};
        host_ = scm_make_foreign_object_1(host_type, lease_);
        lease_->capability = host_;
        (void)scm_gc_protect_object(host_);
    }

    ~Impl() {
        for (const ScriptModePolicyObserver& observer : state_->mode_policy_observers) {
            (void)lease_->runtime->modes().unsubscribe(observer.listener);
            (void)scm_gc_unprotect_object(observer.procedure);
        }
        state_->mode_policy_observers.clear();
        for (const ScriptInputStateObserver& observer : state_->input_state_observers) {
            (void)lease_->runtime->input_states().unsubscribe(observer.listener);
            (void)scm_gc_unprotect_object(observer.procedure);
        }
        state_->input_state_observers.clear();
        state_->active = false;
        for (const ScriptCommand& command : state_->commands) {
            (void)scm_gc_unprotect_object(command.execute);
            if (!scheme_false(command.enabled)) {
                (void)scm_gc_unprotect_object(command.enabled);
            }
        }
        state_->commands.clear();
        for (const ScriptProvider& provider : state_->providers) {
            (void)scm_gc_unprotect_object(provider.complete);
        }
        state_->providers.clear();
        for (const ScriptCompletionProvider& provider : state_->completion_providers) {
            (void)scm_gc_unprotect_object(provider.complete);
            if (!scheme_false(provider.resolve)) {
                (void)scm_gc_unprotect_object(provider.resolve);
            }
        }
        state_->completion_providers.clear();
        state_->completion_providers_by_name.clear();
        for (const ScriptInputState& input_state : state_->input_states) {
            if (!scheme_false(input_state.handler)) {
                (void)scm_gc_unprotect_object(input_state.handler);
            }
        }
        state_->input_states.clear();
        for (const ScriptPositionHintProvider& provider : state_->position_hint_providers) {
            (void)scm_gc_unprotect_object(provider.procedure);
        }
        state_->position_hint_providers.clear();
        for (const ScriptInputStateLifecycle& lifecycle : state_->input_state_lifecycles) {
            if (!scheme_false(lifecycle.on_enter)) {
                (void)scm_gc_unprotect_object(lifecycle.on_enter);
            }
            if (!scheme_false(lifecycle.on_exit)) {
                (void)scm_gc_unprotect_object(lifecycle.on_exit);
            }
        }
        state_->input_state_lifecycles.clear();
        for (const LoadedExtension& extension : state_->extensions) {
            (void)scm_gc_unprotect_object(extension.module);
        }
        state_->extensions.clear();
        if (state_->evaluation_module_initialized) {
            (void)scm_gc_unprotect_object(state_->evaluation_module);
            state_->evaluation_module = SCM_UNDEFINED;
            state_->evaluation_module_initialized = false;
        }
        lease_->runtime = nullptr;
        lease_->state.reset();
        scm_foreign_object_set_x(host_, 0, nullptr);
        (void)scm_gc_unprotect_object(host_);
        delete std::exchange(lease_, nullptr);
    }

    std::expected<std::size_t, std::string> install_core_commands() {
        require_owner_thread();
        lease_->commands_installed = 0;
        state_->definition_source = "scheme:(cind core)";
        GuileCall call;
        call.operation = GuileCall::Operation::InstallCommands;
        call.host = host_;
        std::expected<SCM, std::string> result = run_guile_call(call);
        state_->definition_source = "scheme";
        if (!result) {
            state_->last_error = result.error();
            return std::unexpected(*state_->last_error);
        }
        if (call.count != lease_->commands_installed || call.count != state_->commands.size()) {
            state_->last_error = "Guile command policy returned an inconsistent command count";
            return std::unexpected(*state_->last_error);
        }
        ++state_->command_revision;
        state_->last_error.reset();
        return call.count;
    }

    std::expected<std::size_t, std::string> install_core_providers() {
        require_owner_thread();
        lease_->providers_installed = 0;
        GuileCall call;
        call.operation = GuileCall::Operation::InstallProviders;
        call.host = host_;
        std::expected<SCM, std::string> result = run_guile_call(call);
        if (!result) {
            state_->last_error = result.error();
            return std::unexpected(*state_->last_error);
        }
        if (call.count != lease_->providers_installed ||
            call.count != state_->providers.size() + state_->completion_providers.size()) {
            state_->last_error = "Guile provider policy returned an inconsistent provider count";
            return std::unexpected(*state_->last_error);
        }
        ++state_->provider_revision;
        state_->last_error.reset();
        return call.count;
    }

    std::expected<std::size_t, std::string> install_default_keymaps() {
        require_owner_thread();
        lease_->bindings_installed = 0;
        GuileCall call;
        call.operation = GuileCall::Operation::InstallKeymaps;
        call.host = host_;
        std::expected<SCM, std::string> result = run_guile_call(call);
        if (!result) {
            state_->last_error = result.error();
            return std::unexpected(*state_->last_error);
        }
        const std::size_t installed = call.count;
        if (installed != lease_->bindings_installed) {
            state_->last_error = "Guile keymap policy returned an inconsistent binding count";
            return std::unexpected(*state_->last_error);
        }
        ++state_->binding_revision;
        state_->last_error.reset();
        return installed;
    }

    std::expected<void, std::string> install_presentation_policies() {
        require_owner_thread();
        GuileCall call;
        call.operation = GuileCall::Operation::InstallPresentationPolicies;
        call.host = host_;
        if (std::expected<SCM, std::string> result = run_guile_call(call); !result) {
            state_->last_error = result.error();
            return std::unexpected(*state_->last_error);
        }
        if (call.count != 7) {
            state_->last_error = "Guile presentation policy returned an inconsistent policy count";
            return std::unexpected(*state_->last_error);
        }
        state_->last_error.reset();
        return {};
    }

    std::expected<void, std::string> install_display_policy() {
        require_owner_thread();
        GuileCall call;
        call.operation = GuileCall::Operation::InstallDisplayPolicy;
        call.host = host_;
        if (std::expected<SCM, std::string> result = run_guile_call(call); !result) {
            state_->last_error = result.error();
            return std::unexpected(*state_->last_error);
        }
        if (call.count != 1) {
            state_->last_error = "Guile display policy returned an inconsistent policy count";
            return std::unexpected(*state_->last_error);
        }
        state_->last_error.reset();
        return {};
    }

    std::expected<void, std::string> restore_workbench_session(std::string_view serialized) {
        require_owner_thread();
        GuileCall call;
        call.operation = GuileCall::Operation::RestoreWorkbenchSession;
        call.host = host_;
        call.source = serialized;
        if (std::expected<SCM, std::string> result = run_guile_call(call); !result) {
            state_->last_error = result.error();
            return std::unexpected(*state_->last_error);
        }
        state_->last_error.reset();
        return {};
    }

    std::expected<void, std::string> buffer_edited(BufferId buffer, ViewId view,
                                                   RevisionId revision) {
        require_owner_thread();
        const Buffer& target_buffer = lease_->runtime->buffers().get(buffer);
        const View& target_view = lease_->runtime->views().get(view);
        if (target_view.buffer_id() != buffer) {
            return std::unexpected("edited view does not display the edited buffer");
        }
        if (target_buffer.snapshot().revision() != revision) {
            return std::unexpected("edited buffer revision is stale");
        }
        GuileCall call;
        call.operation = GuileCall::Operation::BufferEdited;
        call.host = host_;
        call.buffer = buffer;
        call.view = view;
        call.revision = revision;
        if (std::expected<SCM, std::string> result = run_guile_call(call); !result) {
            state_->last_error = result.error();
            return std::unexpected(*state_->last_error);
        }
        state_->last_error.reset();
        return {};
    }

    std::expected<void, std::string> interaction_started(const InteractionRequest& request,
                                                         CommandTarget origin) {
        require_owner_thread();
        (void)lease_->runtime->windows().get(origin.window);
        (void)lease_->runtime->buffers().get(origin.buffer);
        (void)lease_->runtime->views().get(origin.view);
        GuileCall call;
        call.operation = GuileCall::Operation::InteractionStarted;
        call.host = host_;
        call.runtime = lease_->runtime;
        call.interaction_request = &request;
        call.interaction_origin = origin;
        if (std::expected<SCM, std::string> result = run_guile_call(call); !result) {
            state_->last_error = result.error();
            return std::unexpected(*state_->last_error);
        }
        state_->last_error.reset();
        return {};
    }

    std::expected<std::optional<GuileInteractionPolicyState>, std::string>
    interaction_policy_state() const {
        require_owner_thread();
        std::optional<std::string> previous_error = state_->last_error;
        GuileCall call;
        call.operation = GuileCall::Operation::InteractionPolicyState;
        call.host = host_;
        if (std::expected<SCM, std::string> result = run_guile_call(call); !result) {
            state_->last_error = result.error();
            return std::unexpected(*state_->last_error);
        }
        state_->last_error = std::move(previous_error);
        return std::move(call.interaction_policy_state);
    }

    std::expected<GuileMinibufferHistoryState, std::string>
    minibuffer_history_state(BufferId buffer, std::string_view history) const {
        require_owner_thread();
        (void)lease_->runtime->buffers().get(buffer);
        std::optional<std::string> previous_error = state_->last_error;
        GuileCall call;
        call.operation = GuileCall::Operation::MinibufferHistoryState;
        call.host = host_;
        call.buffer = buffer;
        call.history = history;
        if (std::expected<SCM, std::string> result = run_guile_call(call); !result) {
            state_->last_error = result.error();
            return std::unexpected(*state_->last_error);
        }
        state_->last_error = std::move(previous_error);
        return std::move(call.minibuffer_history);
    }

    std::expected<std::optional<std::size_t>, std::string> interaction_selection() const {
        require_owner_thread();
        std::optional<std::string> previous_error = state_->last_error;
        GuileCall call;
        call.operation = GuileCall::Operation::InteractionSelection;
        call.host = host_;
        if (std::expected<SCM, std::string> result = run_guile_call(call); !result) {
            state_->last_error = result.error();
            return std::unexpected(*state_->last_error);
        }
        state_->last_error = std::move(previous_error);
        return call.interaction_selection;
    }

    std::expected<GuileCommandFeedbackState, std::string> command_feedback_state() const {
        require_owner_thread();
        std::optional<std::string> previous_error = state_->last_error;
        GuileCall call;
        call.operation = GuileCall::Operation::CommandFeedbackState;
        call.host = host_;
        if (std::expected<SCM, std::string> result = run_guile_call(call); !result) {
            state_->last_error = result.error();
            return std::unexpected(*state_->last_error);
        }
        state_->last_error = std::move(previous_error);
        return std::move(call.command_feedback);
    }

    std::expected<GuileApplicationState, std::string> application_state() const {
        require_owner_thread();
        std::optional<std::string> previous_error = state_->last_error;
        GuileCall call;
        call.operation = GuileCall::Operation::ApplicationState;
        call.host = host_;
        if (std::expected<SCM, std::string> result = run_guile_call(call); !result) {
            state_->last_error = result.error();
            return std::unexpected(*state_->last_error);
        }
        state_->last_error = std::move(previous_error);
        return call.application_state;
    }

    std::expected<void, std::string> set_caret_reveal(bool reveal) {
        require_owner_thread();
        std::optional<std::string> previous_error = state_->last_error;
        GuileCall call;
        call.operation = GuileCall::Operation::SetCaretReveal;
        call.host = host_;
        call.enabled = reveal;
        if (std::expected<SCM, std::string> result = run_guile_call(call); !result) {
            state_->last_error = result.error();
            return std::unexpected(*state_->last_error);
        }
        state_->last_error = std::move(previous_error);
        return {};
    }

    std::expected<bool, std::string> buffer_saving(BufferId buffer) const {
        require_owner_thread();
        (void)lease_->runtime->buffers().get(buffer);
        std::optional<std::string> previous_error = state_->last_error;
        GuileCall call;
        call.operation = GuileCall::Operation::BufferSavingState;
        call.host = host_;
        call.buffer = buffer;
        if (std::expected<SCM, std::string> result = run_guile_call(call); !result) {
            state_->last_error = result.error();
            return std::unexpected(*state_->last_error);
        }
        state_->last_error = std::move(previous_error);
        return call.enabled;
    }

    std::expected<void, std::string> command_input(std::string_view key, bool clear_message) {
        require_owner_thread();
        std::optional<std::string> previous_error = state_->last_error;
        GuileCall call;
        call.operation = GuileCall::Operation::CommandInput;
        call.host = host_;
        call.source = key;
        call.clear_message = clear_message;
        if (std::expected<SCM, std::string> result = run_guile_call(call); !result) {
            state_->last_error = result.error();
            return std::unexpected(*state_->last_error);
        }
        state_->last_error = std::move(previous_error);
        return {};
    }

    std::expected<void, std::string> record_command(std::string_view command) {
        require_owner_thread();
        std::optional<std::string> previous_error = state_->last_error;
        GuileCall call;
        call.operation = GuileCall::Operation::RecordCommand;
        call.host = host_;
        call.source = command;
        if (std::expected<SCM, std::string> result = run_guile_call(call); !result) {
            state_->last_error = result.error();
            return std::unexpected(*state_->last_error);
        }
        state_->last_error = std::move(previous_error);
        return {};
    }

    std::expected<void, std::string>
    command_result_feedback(CommandLoopStatus status, bool consumed,
                            std::optional<std::string_view> command, bool interaction_started,
                            std::string_view message) {
        require_owner_thread();
        std::optional<std::string> previous_error = state_->last_error;
        GuileCall call;
        call.operation = GuileCall::Operation::CommandResultFeedback;
        call.host = host_;
        call.command_status = status;
        call.enabled = consumed;
        if (command) {
            call.command_name = *command;
        }
        call.interaction_started = interaction_started;
        call.source = message;
        if (std::expected<SCM, std::string> result = run_guile_call(call); !result) {
            state_->last_error = result.error();
            return std::unexpected(*state_->last_error);
        }
        state_->last_error = std::move(previous_error);
        return {};
    }

    std::expected<void, std::string> set_message(std::string_view message) {
        require_owner_thread();
        std::optional<std::string> previous_error = state_->last_error;
        GuileCall call;
        call.operation = GuileCall::Operation::SetMessage;
        call.host = host_;
        call.source = message;
        if (std::expected<SCM, std::string> result = run_guile_call(call); !result) {
            state_->last_error = result.error();
            return std::unexpected(*state_->last_error);
        }
        state_->last_error = std::move(previous_error);
        return {};
    }

    std::expected<std::size_t, std::string> install_input_states() {
        require_owner_thread();
        lease_->input_states_installed = 0;
        lease_->input_strategies_installed = 0;
        GuileCall call;
        call.operation = GuileCall::Operation::InstallInputStates;
        call.host = host_;
        std::expected<SCM, std::string> result = run_guile_call(call);
        if (!result) {
            state_->last_error = result.error();
            return std::unexpected(*state_->last_error);
        }
        if (call.count != lease_->input_states_installed) {
            state_->last_error =
                "Guile input state policy returned an inconsistent definition count";
            return std::unexpected(*state_->last_error);
        }
        if (lease_->input_strategies_installed == 0) {
            state_->last_error = "Guile input policy did not define an input strategy";
            return std::unexpected(*state_->last_error);
        }
        ++state_->input_state_revision;
        state_->last_error.reset();
        return call.count;
    }

    std::expected<std::size_t, std::string> install_core_modes() {
        require_owner_thread();
        lease_->modes_installed = 0;
        GuileCall call;
        call.operation = GuileCall::Operation::InstallModes;
        call.host = host_;
        std::expected<SCM, std::string> result = run_guile_call(call);
        if (!result) {
            state_->last_error = result.error();
            return std::unexpected(*state_->last_error);
        }
        if (call.count != lease_->modes_installed) {
            state_->last_error =
                std::format("Guile mode policy returned {} definitions after installing {} modes",
                            call.count, lease_->modes_installed);
            return std::unexpected(*state_->last_error);
        }
        ++state_->mode_revision;
        state_->last_error.reset();
        return call.count;
    }

    std::expected<std::size_t, std::string> install_core_resource_policies() {
        require_owner_thread();
        lease_->resource_policies_installed = 0;
        GuileCall call;
        call.operation = GuileCall::Operation::InstallResourcePolicies;
        call.host = host_;
        std::expected<SCM, std::string> result = run_guile_call(call);
        if (!result) {
            state_->last_error = result.error();
            return std::unexpected(*state_->last_error);
        }
        if (call.count != lease_->resource_policies_installed) {
            state_->last_error = "Guile resource policy returned an inconsistent definition count";
            return std::unexpected(*state_->last_error);
        }
        ++state_->resource_policy_revision;
        state_->last_error.reset();
        return call.count;
    }

    std::expected<void, std::string> install_buffer_lifecycle_policies() {
        require_owner_thread();
        GuileCall call;
        call.operation = GuileCall::Operation::InstallBufferLifecyclePolicies;
        call.host = host_;
        if (std::expected<SCM, std::string> result = run_guile_call(call); !result) {
            state_->last_error = result.error();
            return std::unexpected(*state_->last_error);
        }
        state_->last_error.reset();
        return {};
    }

    std::expected<void, std::string> install_pointer_policies() {
        require_owner_thread();
        GuileCall call;
        call.operation = GuileCall::Operation::InstallPointerPolicies;
        call.host = host_;
        if (std::expected<SCM, std::string> result = run_guile_call(call); !result) {
            state_->last_error = result.error();
            return std::unexpected(*state_->last_error);
        }
        if (call.count != 2) {
            state_->last_error = "Guile pointer policy returned an inconsistent policy count";
            return std::unexpected(*state_->last_error);
        }
        state_->last_error.reset();
        return {};
    }

    std::expected<StartupPlan, std::string> startup_plan(const StartupFacts& facts) const {
        require_owner_thread();
        GuileCall call;
        call.operation = GuileCall::Operation::ResolveStartupPlan;
        call.host = host_;
        call.startup_facts = &facts;
        if (std::expected<SCM, std::string> result = run_guile_call(call); !result) {
            state_->last_error = result.error();
            return std::unexpected(result.error());
        }
        state_->last_error.reset();
        return std::move(call.startup_plan);
    }

    std::expected<SessionPlan, std::string> session_plan(const SessionFacts& facts) const {
        require_owner_thread();
        GuileCall call;
        call.operation = GuileCall::Operation::ResolveSessionPlan;
        call.host = host_;
        call.session_facts = &facts;
        if (std::expected<SCM, std::string> result = run_guile_call(call); !result) {
            state_->last_error = result.error();
            return std::unexpected(result.error());
        }
        state_->last_error.reset();
        return std::move(call.session_plan);
    }

    std::expected<void, std::string> set_startup_placeholder(std::optional<BufferId> buffer) {
        require_owner_thread();
        GuileCall call;
        call.operation = GuileCall::Operation::SetStartupPlaceholder;
        call.host = host_;
        call.enabled = buffer.has_value();
        if (buffer) {
            (void)lease_->runtime->buffers().get(*buffer);
            call.buffer = *buffer;
        }
        if (std::expected<SCM, std::string> result = run_guile_call(call); !result) {
            state_->last_error = result.error();
            return std::unexpected(result.error());
        }
        state_->last_error.reset();
        return {};
    }

    std::expected<CommandId, std::string> close_command(const CommandContext& context,
                                                        bool force) const {
        require_owner_thread();
        GuileCall call;
        call.operation = GuileCall::Operation::ResolveCloseCommand;
        call.host = host_;
        call.context = &context;
        call.force = force;
        if (std::expected<SCM, std::string> result = run_guile_call(call); !result) {
            state_->last_error = result.error();
            return std::unexpected(result.error());
        }
        state_->last_error.reset();
        return call.command;
    }

    std::expected<bool, std::string> handle_pointer(const CommandContext& context,
                                                    const PointerEvent& event,
                                                    bool pending_key_sequence) const {
        require_owner_thread();
        GuileCall call;
        call.operation = GuileCall::Operation::HandlePointer;
        call.host = host_;
        call.context = &context;
        call.pointer_event = &event;
        call.pending_key_sequence = pending_key_sequence;
        if (std::expected<SCM, std::string> result = run_guile_call(call); !result) {
            state_->last_error = result.error();
            return std::unexpected(result.error());
        }
        state_->last_error.reset();
        return call.enabled;
    }

    std::expected<bool, std::string> handle_scroll(const CommandContext& context,
                                                   ScrollInput input) const {
        require_owner_thread();
        if (!std::isfinite(input.amount)) {
            return std::unexpected("scroll delta must be finite");
        }
        GuileCall call;
        call.operation = GuileCall::Operation::HandleScroll;
        call.host = host_;
        call.context = &context;
        call.scroll_input = input;
        if (std::expected<SCM, std::string> result = run_guile_call(call); !result) {
            state_->last_error = result.error();
            return std::unexpected(result.error());
        }
        state_->last_error.reset();
        return call.enabled;
    }

    std::expected<void, std::string> open_resource(WindowId window, std::string_view path,
                                                   std::optional<std::uint32_t> line,
                                                   std::optional<std::uint32_t> column,
                                                   std::string_view intent) {
        require_owner_thread();
        if (intent.empty()) {
            return std::unexpected("resource open intent must not be empty");
        }
        GuileCall call;
        call.operation = GuileCall::Operation::OpenResource;
        call.host = host_;
        call.window = window;
        call.path = path;
        call.line = line;
        call.column = column;
        call.intent = intent;
        if (std::expected<SCM, std::string> result = run_guile_call(call); !result) {
            state_->last_error = result.error();
            return std::unexpected(*state_->last_error);
        }
        state_->last_error.reset();
        return {};
    }

    bool project_search_running() const {
        require_owner_thread();
        GuileCall call;
        call.operation = GuileCall::Operation::ProjectSearchRunning;
        call.host = host_;
        if (std::expected<SCM, std::string> result = run_guile_call(call); !result) {
            state_->last_error = result.error();
            return false;
        }
        return call.enabled;
    }

    std::expected<GuileKeymapPolicy, std::string> keymap_policy(const CommandContext& context,
                                                                bool base) const {
        require_owner_thread();
        GuileCall call;
        call.operation = base ? GuileCall::Operation::ResolveBaseKeymapPolicy
                              : GuileCall::Operation::ResolveKeymapPolicy;
        call.host = host_;
        call.context = &context;
        if (std::expected<SCM, std::string> result = run_guile_call(call); !result) {
            state_->last_error = result.error();
            return std::unexpected(result.error());
        }
        return std::move(call.keymap_policy);
    }

    std::expected<ChromeContent, std::string> chrome_content(const CommandContext& context,
                                                             const ChromeFacts& facts) const {
        require_owner_thread();
        GuileCall call;
        call.operation = GuileCall::Operation::ChromeContent;
        call.host = host_;
        call.context = &context;
        call.chrome_facts = &facts;
        if (std::expected<SCM, std::string> result = run_guile_call(call); !result) {
            state_->last_error = result.error();
            return std::unexpected(result.error());
        }
        return std::move(call.chrome_content);
    }

    std::expected<ModelineContent, std::string> modeline_content(const CommandContext& context,
                                                                 const ModelineFacts& facts) const {
        require_owner_thread();
        GuileCall call;
        call.operation = GuileCall::Operation::ModelineContent;
        call.host = host_;
        call.context = &context;
        call.modeline_facts = &facts;
        if (std::expected<SCM, std::string> result = run_guile_call(call); !result) {
            state_->last_error = result.error();
            return std::unexpected(result.error());
        }
        return std::move(call.modeline_content);
    }

    std::expected<PresentationProfile, std::string> presentation_profile() const {
        require_owner_thread();
        GuileCall call;
        call.operation = GuileCall::Operation::PresentationProfile;
        call.host = host_;
        if (std::expected<SCM, std::string> result = run_guile_call(call); !result) {
            state_->last_error = result.error();
            return std::unexpected(result.error());
        }
        return call.presentation_profile;
    }

    std::expected<GuileDisplayPlan, std::string>
    display_plan(const GuileDisplayFacts& facts) const {
        require_owner_thread();
        GuileCall call;
        call.operation = GuileCall::Operation::DisplayPlan;
        call.host = host_;
        call.display_facts = &facts;
        if (std::expected<SCM, std::string> result = run_guile_call(call); !result) {
            state_->last_error = result.error();
            return std::unexpected(result.error());
        }
        state_->last_error.reset();
        return call.display_plan;
    }

    std::expected<GuileDisplayPlan, std::string>
    fallback_display_plan(const GuileDisplayFacts& facts) const {
        require_owner_thread();
        GuileCall call;
        call.operation = GuileCall::Operation::FallbackDisplayPlan;
        call.host = host_;
        call.display_facts = &facts;
        if (std::expected<SCM, std::string> result = run_guile_call(call); !result) {
            state_->last_error = result.error();
            return std::unexpected(result.error());
        }
        state_->last_error.reset();
        return call.display_plan;
    }

    void project_index_updated(ProjectId project) {
        require_owner_thread();
        GuileCall call;
        call.operation = GuileCall::Operation::ProjectIndexUpdated;
        call.host = host_;
        call.project = project;
        if (std::expected<SCM, std::string> result = run_guile_call(call); !result) {
            state_->last_error = result.error();
        }
    }

    std::expected<void, std::string> load_extension(const std::string& input_path) {
        require_owner_thread();
        std::error_code error;
        const std::filesystem::path path =
            std::filesystem::absolute(input_path, error).lexically_normal();
        if (error) {
            return std::unexpected(std::format("invalid extension path: {}", error.message()));
        }
        if (!std::filesystem::is_regular_file(path, error)) {
            return std::unexpected(
                error ? std::format("cannot inspect extension '{}': {}", path.string(),
                                    error.message())
                      : std::format("extension '{}' is not a file", path.string()));
        }
        std::string extension_path = path.string();
        state_->extensions.reserve(state_->extensions.size() + 1);

        EditorRuntime::ExtensionCheckpoint registries = lease_->runtime->checkpoint_extensions();
        const GuileState state_checkpoint = *state_;
        const std::vector<std::uint64_t> async_checkpoint = async_bridge_.checkpoint();
        const std::size_t commands_installed = lease_->commands_installed;
        const std::size_t providers_installed = lease_->providers_installed;
        const std::size_t bindings_installed = lease_->bindings_installed;
        const std::size_t input_states_installed = lease_->input_states_installed;
        const std::size_t input_strategies_installed = lease_->input_strategies_installed;
        const std::size_t modes_installed = lease_->modes_installed;
        const std::size_t resource_policies_installed = lease_->resource_policies_installed;

        GuileCall call;
        call.operation = GuileCall::Operation::LoadExtension;
        call.path = extension_path;
        call.host = host_;
        state_->definition_source = std::format("scheme:{}", extension_path);
        std::expected<SCM, std::string> loaded = run_guile_call(call);
        state_->definition_source = "scheme";
        if (!loaded) {
            async_bridge_.rollback_to(async_checkpoint);
            discard_state_tail(*state_, state_checkpoint);
            lease_->runtime->restore_extensions(registries);
            *state_ = state_checkpoint;
            lease_->commands_installed = commands_installed;
            lease_->providers_installed = providers_installed;
            lease_->bindings_installed = bindings_installed;
            lease_->input_states_installed = input_states_installed;
            lease_->input_strategies_installed = input_strategies_installed;
            lease_->modes_installed = modes_installed;
            lease_->resource_policies_installed = resource_policies_installed;
            state_->last_error = loaded.error();
            return std::unexpected(*state_->last_error);
        }

        (void)scm_gc_protect_object(*loaded);
        state_->extensions.push_back({.path = std::move(extension_path), .module = *loaded});
        if (lease_->commands_installed != commands_installed) {
            ++state_->command_revision;
        }
        if (lease_->providers_installed != providers_installed) {
            ++state_->provider_revision;
        }
        if (lease_->bindings_installed != bindings_installed) {
            ++state_->binding_revision;
        }
        if (lease_->input_states_installed != input_states_installed ||
            lease_->input_strategies_installed != input_strategies_installed) {
            ++state_->input_state_revision;
        }
        if (lease_->modes_installed != modes_installed) {
            ++state_->mode_revision;
        }
        if (lease_->resource_policies_installed != resource_policies_installed) {
            ++state_->resource_policy_revision;
        }
        state_->last_error.reset();
        return {};
    }

    std::expected<GuileEvaluationResult, std::string> evaluate(GuileEvaluationRequest request) {
        require_owner_thread();
        return evaluate_source(*lease_, request);
    }

    std::expected<CompletionProviderResult, std::string>
    complete(const CompletionProvider& provider, const CompletionRequest& request) {
        require_owner_thread();
        if (provider.kind != CompletionProviderKind::Scripted ||
            provider.session >= state_->completion_providers.size()) {
            return std::unexpected("unknown scripted completion provider");
        }
        CommandContext context(*lease_->runtime, request.target.window, request.target.buffer,
                               request.target.view);
        try {
            return invoke_script_completion_provider(state_, provider.session, context, provider,
                                                     request, &async_bridge_);
        } catch (const std::exception& exception) {
            state_->last_error = exception.what();
            return std::unexpected(*state_->last_error);
        } catch (...) {
            state_->last_error = "unknown Guile completion provider failure";
            return std::unexpected(*state_->last_error);
        }
    }

    std::expected<CompletionItem, std::string> resolve(const CompletionProvider& provider,
                                                       const CompletionRequest& request,
                                                       const CompletionItem& item) {
        require_owner_thread();
        if (provider.kind != CompletionProviderKind::Scripted ||
            provider.session >= state_->completion_providers.size()) {
            return std::unexpected("unknown scripted completion provider");
        }
        const SCM procedure = state_->completion_providers[provider.session].resolve;
        if (scheme_false(procedure)) {
            return std::unexpected("scripted completion provider has no resolver");
        }
        CommandContext context(*lease_->runtime, request.target.window, request.target.buffer,
                               request.target.view);
        GuileCall call;
        call.operation = GuileCall::Operation::InvokeCompletionResolver;
        call.procedure = procedure;
        call.context = &context;
        call.completion_request = &request;
        call.completion_item = &item;
        call.completion_provider = provider;
        if (std::expected<SCM, std::string> result = run_guile_call(call); !result) {
            state_->last_error = result.error();
            return std::unexpected(
                std::format("Guile completion resolver failed: {}", result.error()));
        }
        state_->last_error.reset();
        return std::move(call.resolved_completion_item);
    }

    GuileRuntimeSnapshot snapshot() const {
        return {.engine = "guile",
                .version = version_,
                .modules =
                    [] {
                        std::vector<std::string> modules;
                        modules.reserve(bundled_guile_modules.size());
                        for (const std::string_view module : bundled_guile_modules) {
                            modules.push_back(std::format("cind {}", module));
                        }
                        return modules;
                    }(),
                .extensions =
                    [&] {
                        std::vector<std::string> paths;
                        paths.reserve(state_->extensions.size());
                        for (const LoadedExtension& extension : state_->extensions) {
                            paths.push_back(extension.path);
                        }
                        return paths;
                    }(),
                .command_revision = state_->command_revision,
                .scripted_commands = state_->commands.size(),
                .provider_revision = state_->provider_revision,
                .scripted_providers =
                    state_->providers.size() + state_->completion_providers.size(),
                .binding_revision = state_->binding_revision,
                .input_state_revision = state_->input_state_revision,
                .scripted_input_states = state_->input_state_definitions,
                .scripted_input_strategies = state_->input_strategy_definitions,
                .mode_revision = state_->mode_revision,
                .scripted_modes = state_->mode_definitions,
                .resource_policy_revision = state_->resource_policy_revision,
                .scripted_file_mode_rules =
                    lease_->runtime->resource_policies().file_mode_rules().size(),
                .scripted_project_providers =
                    lease_->runtime->resource_policies().project_providers().size(),
                .outstanding_async_tasks = async_bridge_.outstanding(),
                .last_error = state_->last_error};
    }

    void shutdown_async_tasks() noexcept {
        if (std::this_thread::get_id() != state_->owner) {
            return;
        }
        async_bridge_.shutdown();
    }

private:
    void require_owner_thread() const {
        if (std::this_thread::get_id() != state_->owner) {
            throw std::logic_error("Guile runtime must run on its editor thread");
        }
    }

    std::shared_ptr<GuileState> state_;
    GuileAsyncBridge async_bridge_;
    HostLease* lease_ = nullptr;
    SCM host_ = SCM_UNDEFINED;
    std::string version_;
};

GuileRuntime::GuileRuntime(EditorRuntime& runtime, GuileHostServices services)
    : impl_(std::make_unique<Impl>(runtime, std::move(services))) {}

GuileRuntime::~GuileRuntime() = default;

std::expected<std::size_t, std::string> GuileRuntime::install_core_commands() {
    return impl_->install_core_commands();
}

std::expected<std::size_t, std::string> GuileRuntime::install_core_providers() {
    return impl_->install_core_providers();
}

std::expected<std::size_t, std::string> GuileRuntime::install_default_keymaps() {
    return impl_->install_default_keymaps();
}

std::expected<std::size_t, std::string> GuileRuntime::install_input_states() {
    return impl_->install_input_states();
}

std::expected<std::size_t, std::string> GuileRuntime::install_core_modes() {
    return impl_->install_core_modes();
}

std::expected<std::size_t, std::string> GuileRuntime::install_core_resource_policies() {
    return impl_->install_core_resource_policies();
}

std::expected<void, std::string> GuileRuntime::install_buffer_lifecycle_policies() {
    return impl_->install_buffer_lifecycle_policies();
}

std::expected<void, std::string> GuileRuntime::install_pointer_policies() {
    return impl_->install_pointer_policies();
}

std::expected<void, std::string> GuileRuntime::install_presentation_policies() {
    return impl_->install_presentation_policies();
}

std::expected<void, std::string> GuileRuntime::install_display_policy() {
    return impl_->install_display_policy();
}

std::expected<void, std::string>
GuileRuntime::restore_workbench_session(std::string_view serialized) {
    return impl_->restore_workbench_session(serialized);
}

std::expected<void, std::string> GuileRuntime::buffer_edited(BufferId buffer, ViewId view,
                                                             RevisionId revision) {
    return impl_->buffer_edited(buffer, view, revision);
}

std::expected<void, std::string>
GuileRuntime::interaction_started(const InteractionRequest& request, CommandTarget origin) {
    return impl_->interaction_started(request, origin);
}

std::expected<std::optional<GuileInteractionPolicyState>, std::string>
GuileRuntime::interaction_policy_state() const {
    return impl_->interaction_policy_state();
}

std::expected<GuileMinibufferHistoryState, std::string>
GuileRuntime::minibuffer_history_state(BufferId buffer, std::string_view history) const {
    return impl_->minibuffer_history_state(buffer, history);
}

std::expected<std::optional<std::size_t>, std::string> GuileRuntime::interaction_selection() const {
    return impl_->interaction_selection();
}

std::expected<GuileCommandFeedbackState, std::string> GuileRuntime::command_feedback_state() const {
    return impl_->command_feedback_state();
}

std::expected<GuileApplicationState, std::string> GuileRuntime::application_state() const {
    return impl_->application_state();
}

std::expected<void, std::string> GuileRuntime::set_caret_reveal(bool reveal) {
    return impl_->set_caret_reveal(reveal);
}

std::expected<bool, std::string> GuileRuntime::buffer_saving(BufferId buffer) const {
    return impl_->buffer_saving(buffer);
}

std::expected<void, std::string> GuileRuntime::command_input(std::string_view key,
                                                             bool clear_message) {
    return impl_->command_input(key, clear_message);
}

std::expected<void, std::string> GuileRuntime::record_command(std::string_view command) {
    return impl_->record_command(command);
}

std::expected<void, std::string>
GuileRuntime::command_result_feedback(CommandLoopStatus status, bool consumed,
                                      std::optional<std::string_view> command,
                                      bool interaction_started, std::string_view message) {
    return impl_->command_result_feedback(status, consumed, command, interaction_started, message);
}

std::expected<void, std::string> GuileRuntime::set_message(std::string_view message) {
    return impl_->set_message(message);
}

std::expected<StartupPlan, std::string>
GuileRuntime::startup_plan(const StartupFacts& facts) const {
    return impl_->startup_plan(facts);
}

std::expected<SessionPlan, std::string>
GuileRuntime::session_plan(const SessionFacts& facts) const {
    return impl_->session_plan(facts);
}

std::expected<void, std::string>
GuileRuntime::set_startup_placeholder(std::optional<BufferId> buffer) {
    return impl_->set_startup_placeholder(buffer);
}

std::expected<CommandId, std::string> GuileRuntime::close_command(const CommandContext& context,
                                                                  bool force) const {
    return impl_->close_command(context, force);
}

std::expected<bool, std::string> GuileRuntime::handle_pointer(const CommandContext& context,
                                                              const PointerEvent& event,
                                                              bool pending_key_sequence) const {
    return impl_->handle_pointer(context, event, pending_key_sequence);
}

std::expected<bool, std::string> GuileRuntime::handle_scroll(const CommandContext& context,
                                                             ScrollInput input) const {
    return impl_->handle_scroll(context, input);
}

std::expected<void, std::string> GuileRuntime::open_resource(WindowId window, std::string_view path,
                                                             std::optional<std::uint32_t> line,
                                                             std::optional<std::uint32_t> column,
                                                             std::string_view intent) {
    return impl_->open_resource(window, path, line, column, intent);
}

bool GuileRuntime::project_search_running() const {
    return impl_->project_search_running();
}

std::expected<GuileKeymapPolicy, std::string>
GuileRuntime::keymap_policy(const CommandContext& context) const {
    return impl_->keymap_policy(context, false);
}

std::expected<GuileKeymapPolicy, std::string>
GuileRuntime::base_keymap_policy(const CommandContext& context) const {
    return impl_->keymap_policy(context, true);
}

std::expected<ChromeContent, std::string>
GuileRuntime::chrome_content(const CommandContext& context, const ChromeFacts& facts) const {
    return impl_->chrome_content(context, facts);
}

std::expected<ModelineContent, std::string>
GuileRuntime::modeline_content(const CommandContext& context, const ModelineFacts& facts) const {
    return impl_->modeline_content(context, facts);
}

std::expected<PresentationProfile, std::string> GuileRuntime::presentation_profile() const {
    return impl_->presentation_profile();
}

std::expected<GuileDisplayPlan, std::string>
GuileRuntime::display_plan(const GuileDisplayFacts& facts) const {
    return impl_->display_plan(facts);
}

std::expected<GuileDisplayPlan, std::string>
GuileRuntime::fallback_display_plan(const GuileDisplayFacts& facts) const {
    return impl_->fallback_display_plan(facts);
}

void GuileRuntime::project_index_updated(ProjectId project) {
    impl_->project_index_updated(project);
}

std::expected<void, std::string> GuileRuntime::load_extension(const std::string& path) {
    return impl_->load_extension(path);
}

std::expected<GuileEvaluationResult, std::string>
GuileRuntime::evaluate(GuileEvaluationRequest request) {
    return impl_->evaluate(request);
}

std::expected<CompletionProviderResult, std::string>
GuileRuntime::complete(const CompletionProvider& provider, const CompletionRequest& request) {
    return impl_->complete(provider, request);
}

std::expected<CompletionItem, std::string> GuileRuntime::resolve(const CompletionProvider& provider,
                                                                 const CompletionRequest& request,
                                                                 const CompletionItem& item) {
    return impl_->resolve(provider, request, item);
}

GuileRuntimeSnapshot GuileRuntime::snapshot() const {
    return impl_->snapshot();
}

void GuileRuntime::shutdown_async_tasks() noexcept {
    impl_->shutdown_async_tasks();
}

std::optional<std::string> discover_user_init_file() {
    const char* config_home = std::getenv("XDG_CONFIG_HOME");
    std::filesystem::path path;
    if (config_home != nullptr && *config_home != '\0') {
        path = std::filesystem::path(config_home) / "cind" / "init.scm";
    } else {
        const char* home = std::getenv("HOME");
        if (home == nullptr || *home == '\0') {
            return std::nullopt;
        }
        path = std::filesystem::path(home) / ".config" / "cind" / "init.scm";
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) {
        return std::nullopt;
    }
    return path.lexically_normal().string();
}

} // namespace cind
