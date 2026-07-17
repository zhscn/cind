#include "script/guile_runtime.hpp"

#include "editor/runtime.hpp"

#include <libguile.h>

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
#include <utility>

namespace cind {

namespace {

struct ScriptCommand {
    SCM execute = SCM_UNDEFINED;
    SCM enabled = SCM_BOOL_F;
};

struct ScriptProvider {
    SCM complete = SCM_UNDEFINED;
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
    return *host;
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
                                                 CommandContext& context, std::string_view query);
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
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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
            InteractionProviderRegistry::Complete complete =
                [weak, provider_index](CommandContext& context,
                                       std::string_view query) -> InteractionProviderResult {
                const std::shared_ptr<GuileState> locked = weak.lock();
                if (!locked || !locked->active) {
                    throw std::runtime_error("Guile provider runtime has expired");
                }
                return invoke_script_provider(locked, provider_index, context, query);
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

// The Guile ABI fixes four adjacent SCM arguments; their Scheme procedure
// names and validation preserve the semantic order.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
SCM base_keymap_layers(SCM host_object, SCM context_value) {
    try {
        HostLease& host = require_host(host_object, "base-keymap-layers");
        if (!host.services.base_keymap_layers) {
            scm_misc_error("base-keymap-layers", "base keymap layer service is unavailable",
                           SCM_EOL);
        }
        const CommandContext context =
            command_context_from_scheme(host, context_value, "base-keymap-layers");
        const std::vector<KeymapId> layers = host.services.base_keymap_layers(context.window_id());
        SCM result = scm_c_make_vector(layers.size(), SCM_UNSPECIFIED);
        for (std::size_t index = 0; index < layers.size(); ++index) {
            scm_c_vector_set_x(result, index,
                               name_symbol(host.runtime->keymaps().definition(layers[index]).name));
        }
        return result;
    } catch (const std::exception& exception) {
        scm_misc_error("base-keymap-layers", exception.what(), SCM_EOL);
    } catch (...) {
        scm_misc_error("base-keymap-layers", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
SCM active_keymap_layers(SCM host_object, SCM context_value) {
    try {
        HostLease& host = require_host(host_object, "active-keymap-layers");
        if (!host.services.active_keymap_layers) {
            scm_misc_error("active-keymap-layers", "active keymap layer service is unavailable",
                           SCM_EOL);
        }
        const CommandContext context =
            command_context_from_scheme(host, context_value, "active-keymap-layers");
        const std::vector<KeymapId> layers =
            host.services.active_keymap_layers(context.window_id());
        SCM result = scm_c_make_vector(layers.size(), SCM_UNSPECIFIED);
        for (std::size_t index = 0; index < layers.size(); ++index) {
            scm_c_vector_set_x(result, index,
                               name_symbol(host.runtime->keymaps().definition(layers[index]).name));
        }
        return result;
    } catch (const std::exception& exception) {
        scm_misc_error("active-keymap-layers", exception.what(), SCM_EOL);
    } catch (...) {
        scm_misc_error("active-keymap-layers", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

// The Guile ABI fixes three adjacent SCM arguments; validation preserves their semantic order.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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

// The low-level Guile ABI fixes seven adjacent SCM arguments. The `(cind
// input)` wrapper supplies the public keyword interface and state-local
// optional capabilities.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
SCM define_input_state(SCM host_object, SCM name_value, SCM keymaps_value, SCM text_input_value,
                       SCM cursor_value, SCM indicator_value, SCM handler_value) {
    if (!scm_is_string(indicator_value)) {
        scm_wrong_type_arg_msg("%define-input-state!", 6, indicator_value, "string");
    }
    if (!scheme_false(handler_value) && !scheme_true(scm_procedure_p(handler_value))) {
        scm_wrong_type_arg_msg("%define-input-state!", 7, handler_value, "procedure or #f");
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
        CursorShape cursor;
        if (symbol_is(cursor_value, "beam")) {
            cursor = CursorShape::Beam;
        } else if (symbol_is(cursor_value, "block")) {
            cursor = CursorShape::Block;
        } else if (symbol_is(cursor_value, "underline")) {
            cursor = CursorShape::Underline;
        } else {
            scm_wrong_type_arg_msg("%define-input-state!", 5, cursor_value,
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

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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
    SCM result = scm_c_make_vector(3, SCM_BOOL_F);
    scm_c_vector_set_x(result, 0, interaction_class_symbol(policy.interaction_class));
    if (policy.initial_state) {
        scm_c_vector_set_x(
            result, 1, name_symbol(runtime.input_states().definition(*policy.initial_state).name));
    }
    scm_c_vector_set_x(result, 2, mode_things_value(policy.things));
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

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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

// The Guile ABI fixes eight adjacent SCM arguments; the public Scheme wrappers
// provide keyword arguments and preserve this normalized host boundary.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
SCM define_mode(SCM host_object, SCM name_value, SCM kind_value, SCM parent_value, SCM keymap_value,
                SCM interaction_class_value, SCM initial_state_value, SCM things_value) {
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
        std::optional<KeymapId> keymap;
        if (!scheme_false(keymap_value)) {
            keymap = require_keymap(host, keymap_value, "%define-mode!", 5);
        }
        std::optional<InteractionClass> interaction_class;
        if (!scheme_false(interaction_class_value)) {
            interaction_class =
                interaction_class_from_scheme(interaction_class_value, "%define-mode!", 6);
        }
        std::optional<InputStateId> initial_state;
        if (!scheme_false(initial_state_value)) {
            initial_state = require_input_state(host, initial_state_value, "%define-mode!", 7);
        }
        std::vector<ModeThingBinding> things =
            mode_things_from_scheme(things_value, "%define-mode!", 8);

        const std::optional<ModeId> existing = host.runtime->modes().find(name);
        const ModeId mode = existing ? *existing : host.runtime->modes().define(name, kind);
        if (host.runtime->modes().definition(mode).kind != kind) {
            throw std::invalid_argument("mode definition cannot change its kind");
        }
        host.runtime->modes().set_parent(mode, parent);
        host.runtime->modes().set_interaction_class(mode, interaction_class);
        host.runtime->modes().set_initial_state(mode, initial_state);
        host.runtime->modes().set_things(mode, std::move(things));
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
// NOLINTEND(bugprone-easily-swappable-parameters)

// The Guile ABI fixes five adjacent SCM arguments; the public Scheme policy
// supplies declarative matcher lists.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
SCM mode_properties(SCM host_object, SCM mode_value) {
    try {
        HostLease& host = require_host(host_object, "mode-properties");
        const ModeId mode = require_mode(host, mode_value, "mode-properties", 2);
        const ModeRegistry::Definition& definition = host.runtime->modes().definition(mode);
        SCM result = scm_c_make_vector(7, SCM_BOOL_F);
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
        return result;
    } catch (const std::exception& exception) {
        scm_misc_error("mode-properties", exception.what(), SCM_EOL);
    } catch (...) {
        scm_misc_error("mode-properties", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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

SCM open_buffer_summaries(SCM host_object) {
    HostLease& host = require_host(host_object, "open-buffer-summaries");
    if (!host.services.open_buffers) {
        scm_misc_error("open-buffer-summaries", "open-buffer snapshot capability is unavailable",
                       SCM_EOL);
    }
    try {
        const std::vector<BufferId> buffers = host.services.open_buffers();
        SCM result = scm_c_make_vector(buffers.size(), SCM_UNSPECIFIED);
        for (std::size_t index = 0; index < buffers.size(); ++index) {
            const Buffer& buffer = host.runtime->buffers().get(buffers[index]);
            SCM summary = scm_c_make_vector(3, SCM_UNSPECIFIED);
            scm_c_vector_set_x(summary, 0, scm_from_utf8_string(buffer.name().c_str()));
            scm_c_vector_set_x(summary, 1,
                               buffer.resource_uri()
                                   ? scm_from_utf8_string(buffer.resource_uri()->c_str())
                                   : SCM_BOOL_F);
            scm_c_vector_set_x(summary, 2, scm_from_bool(buffer.modified()));
            scm_c_vector_set_x(result, index, summary);
        }
        return result;
    } catch (const std::exception& exception) {
        raise_host_error("open-buffer-summaries", exception.what());
    } catch (...) {
        scm_misc_error("open-buffer-summaries", "unknown C++ host failure", SCM_EOL);
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
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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

// The Guile ABI fixes four adjacent SCM arguments; their Scheme procedure
// name and validation preserve the semantic order.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
SCM soft_kill_range(SCM host_object, SCM view_value) {
    HostLease& host = require_host(host_object, "soft-kill-range");
    const ViewId view = entity_id_from_scheme<ViewTag>(view_value, "soft-kill-range", 2);
    if (!host.services.soft_kill_range) {
        scm_misc_error("soft-kill-range", "structural range capability is unavailable", SCM_EOL);
    }
    try {
        const std::optional<GuileTextRange> range = host.services.soft_kill_range(view);
        return range ? text_range_value(TextRange{TextOffset{range->start}, TextOffset{range->end}})
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
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
SCM set_view_caret(SCM host_object, SCM view_value, SCM offset_value) {
    try {
        HostLease& host = require_host(host_object, "set-view-caret!");
        const ViewId view = entity_id_from_scheme<ViewTag>(view_value, "set-view-caret!", 2);
        const TextOffset offset = text_offset_from_scheme(offset_value, "set-view-caret!", 3);
        host.runtime->views().set_caret(view, offset);
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("set-view-caret!", exception.what());
    } catch (...) {
        scm_misc_error("set-view-caret!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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

// The Guile ABI fixes two adjacent SCM arguments; their Scheme procedure name
// and validation preserve the semantic order.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
SCM path_parent(SCM host_object, SCM path_value) {
    if (!scm_is_string(path_value)) {
        scm_wrong_type_arg_msg("path-parent", 2, path_value, "string");
    }
    (void)require_host(host_object, "path-parent");
    try {
        const std::string parent =
            std::filesystem::path(scheme_string(path_value)).parent_path().string();
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
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
SCM display_buffer(SCM host_object, SCM window_value, SCM buffer_value) {
    HostLease& host = require_host(host_object, "display-buffer!");
    const WindowId window = entity_id_from_scheme<WindowTag>(window_value, "display-buffer!", 2);
    const BufferId buffer = entity_id_from_scheme<BufferTag>(buffer_value, "display-buffer!", 3);
    if (!host.services.display_buffer) {
        scm_misc_error("display-buffer!", "display-buffer capability is unavailable", SCM_EOL);
    }
    try {
        const std::expected<void, std::string> displayed =
            host.services.display_buffer(window, buffer);
        if (!displayed) {
            raise_host_error("display-buffer!", displayed.error());
        }
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("display-buffer!", exception.what());
    } catch (...) {
        scm_misc_error("display-buffer!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
SCM display_generated_buffer(SCM host_object, SCM window_value, SCM name_value, SCM text_value) {
    if (!scm_is_string(name_value)) {
        scm_wrong_type_arg_msg("display-generated-buffer!", 3, name_value, "string");
    }
    if (!scm_is_string(text_value)) {
        scm_wrong_type_arg_msg("display-generated-buffer!", 4, text_value, "string");
    }
    HostLease& host = require_host(host_object, "display-generated-buffer!");
    const WindowId window =
        entity_id_from_scheme<WindowTag>(window_value, "display-generated-buffer!", 2);
    if (!host.services.display_generated_buffer) {
        scm_misc_error("display-generated-buffer!", "generated-buffer capability is unavailable",
                       SCM_EOL);
    }
    try {
        const std::expected<void, std::string> displayed = host.services.display_generated_buffer(
            window, scheme_string(name_value), scheme_string(text_value));
        if (!displayed) {
            raise_host_error("display-generated-buffer!", displayed.error());
        }
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("display-generated-buffer!", exception.what());
    } catch (...) {
        scm_misc_error("display-generated-buffer!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

// The Guile ABI fixes three adjacent SCM arguments; the Scheme-level name and
// validation preserve their semantic order.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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

// The Guile ABI fixes two adjacent SCM arguments; their Scheme procedure name
// and validation preserve the semantic order.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
SCM set_message(SCM host_object, SCM message_value) {
    if (!scm_is_string(message_value)) {
        scm_wrong_type_arg_msg("set-message!", 2, message_value, "string");
    }
    try {
        HostLease& host = require_host(host_object, "set-message!");
        if (!host.services.set_message) {
            scm_misc_error("set-message!", "message capability is unavailable", SCM_EOL);
        }
        host.services.set_message(scheme_string(message_value));
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("set-message!", exception.what());
    } catch (...) {
        scm_misc_error("set-message!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

// The Guile ABI fixes two adjacent SCM arguments; their Scheme procedure name
// and validation preserve the semantic order.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
SCM ensure_project_index(SCM host_object, SCM project_value) {
    HostLease& host = require_host(host_object, "ensure-project-index!");
    const ProjectId project =
        entity_id_from_scheme<ProjectTag>(project_value, "ensure-project-index!", 2);
    if (!host.services.ensure_project_index) {
        scm_misc_error("ensure-project-index!", "project-index capability is unavailable", SCM_EOL);
    }
    try {
        const std::expected<void, std::string> requested =
            host.services.ensure_project_index(project);
        if (!requested) {
            raise_host_error("ensure-project-index!", requested.error());
        }
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("ensure-project-index!", exception.what());
    } catch (...) {
        scm_misc_error("ensure-project-index!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

// The Guile ABI fixes three adjacent SCM arguments; their Scheme procedure
// name and validation preserve the semantic order.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
SCM open_file(SCM host_object, SCM window_value, SCM path_value) {
    if (!scm_is_string(path_value)) {
        scm_wrong_type_arg_msg("open-file!", 3, path_value, "string");
    }
    HostLease& host = require_host(host_object, "open-file!");
    const WindowId window = entity_id_from_scheme<WindowTag>(window_value, "open-file!", 2);
    if (!host.services.open_file) {
        scm_misc_error("open-file!", "file-open capability is unavailable", SCM_EOL);
    }
    try {
        const std::expected<void, std::string> opened =
            host.services.open_file(window, scheme_string(path_value));
        if (!opened) {
            raise_host_error("open-file!", opened.error());
        }
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("open-file!", exception.what());
    } catch (...) {
        scm_misc_error("open-file!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

// The Guile ABI fixes four adjacent SCM arguments; their Scheme procedure
// name and validation preserve the semantic order.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
SCM start_project_search(SCM host_object, SCM project_value, SCM window_value, SCM query_value) {
    if (!scm_is_string(query_value)) {
        scm_wrong_type_arg_msg("start-project-search!", 4, query_value, "string");
    }
    HostLease& host = require_host(host_object, "start-project-search!");
    const ProjectId project =
        entity_id_from_scheme<ProjectTag>(project_value, "start-project-search!", 2);
    const WindowId window =
        entity_id_from_scheme<WindowTag>(window_value, "start-project-search!", 3);
    if (!host.services.start_project_search) {
        scm_misc_error("start-project-search!", "project-search capability is unavailable",
                       SCM_EOL);
    }
    try {
        const std::expected<void, std::string> started =
            host.services.start_project_search(project, window, scheme_string(query_value));
        if (!started) {
            raise_host_error("start-project-search!", started.error());
        }
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("start-project-search!", exception.what());
    } catch (...) {
        scm_misc_error("start-project-search!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

// The Guile ABI fixes three adjacent SCM arguments; their Scheme procedure
// name and validation preserve the semantic order.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
SCM set_buffer_resource(SCM host_object, SCM buffer_value, SCM path_value) {
    if (!scm_is_string(path_value)) {
        scm_wrong_type_arg_msg("set-buffer-resource!", 3, path_value, "string");
    }
    HostLease& host = require_host(host_object, "set-buffer-resource!");
    const BufferId buffer =
        entity_id_from_scheme<BufferTag>(buffer_value, "set-buffer-resource!", 2);
    if (!host.services.set_buffer_resource) {
        scm_misc_error("set-buffer-resource!", "buffer-resource capability is unavailable",
                       SCM_EOL);
    }
    try {
        const std::expected<void, std::string> updated =
            host.services.set_buffer_resource(buffer, scheme_string(path_value));
        if (!updated) {
            raise_host_error("set-buffer-resource!", updated.error());
        }
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("set-buffer-resource!", exception.what());
    } catch (...) {
        scm_misc_error("set-buffer-resource!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

// The Guile ABI fixes two adjacent SCM arguments; their Scheme procedure name
// and validation preserve the semantic order.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
SCM save_buffer(SCM host_object, SCM buffer_value) {
    HostLease& host = require_host(host_object, "save-buffer!");
    const BufferId buffer = entity_id_from_scheme<BufferTag>(buffer_value, "save-buffer!", 2);
    if (!host.services.save_buffer) {
        scm_misc_error("save-buffer!", "buffer-save capability is unavailable", SCM_EOL);
    }
    try {
        host.services.save_buffer(buffer);
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("save-buffer!", exception.what());
    } catch (...) {
        scm_misc_error("save-buffer!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
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

// The Guile ABI fixes three adjacent SCM arguments; their Scheme procedure
// name and validation preserve the semantic order.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
SCM kill_buffer(SCM host_object, SCM buffer_value, SCM force_value) {
    if (!scheme_boolean(force_value)) {
        scm_wrong_type_arg_msg("kill-buffer!", 3, force_value, "boolean");
    }
    HostLease& host = require_host(host_object, "kill-buffer!");
    const BufferId buffer = entity_id_from_scheme<BufferTag>(buffer_value, "kill-buffer!", 2);
    if (!host.services.kill_buffer) {
        scm_misc_error("kill-buffer!", "buffer-kill capability is unavailable", SCM_EOL);
    }
    try {
        const std::expected<void, std::string> removed =
            host.services.kill_buffer(buffer, scheme_true(force_value));
        if (!removed) {
            return scm_from_utf8_string(removed.error().c_str());
        }
        return SCM_BOOL_F;
    } catch (const std::exception& exception) {
        raise_host_error("kill-buffer!", exception.what());
    } catch (...) {
        scm_misc_error("kill-buffer!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

// The Guile ABI fixes two adjacent SCM arguments; their Scheme procedure name
// and validation preserve the semantic order.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
SCM request_quit(SCM host_object, SCM force_value) {
    if (!scheme_boolean(force_value)) {
        scm_wrong_type_arg_msg("request-quit!", 2, force_value, "boolean");
    }
    HostLease& host = require_host(host_object, "request-quit!");
    if (!host.services.request_quit) {
        scm_misc_error("request-quit!", "application-quit capability is unavailable", SCM_EOL);
    }
    try {
        host.services.request_quit(scheme_true(force_value));
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("request-quit!", exception.what());
    } catch (...) {
        scm_misc_error("request-quit!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

// The Guile ABI fixes three adjacent SCM arguments; their Scheme procedure
// name and validation preserve the semantic order.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
SCM delete_other_windows(SCM host_object, SCM window_value) {
    HostLease& host = require_host(host_object, "delete-other-windows!");
    const WindowId window =
        entity_id_from_scheme<WindowTag>(window_value, "delete-other-windows!", 2);
    if (!host.services.delete_other_windows) {
        scm_misc_error("delete-other-windows!", "window-retain capability is unavailable", SCM_EOL);
    }
    try {
        host.services.delete_other_windows(window);
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("delete-other-windows!", exception.what());
    } catch (...) {
        scm_misc_error("delete-other-windows!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_UNSPECIFIED;
}

// The Guile ABI fixes three adjacent SCM arguments; their Scheme procedure
// name and validation preserve the semantic order.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
SCM select_other_window(SCM host_object, SCM window_value, SCM delta_value) {
    if (scm_is_signed_integer(delta_value, std::numeric_limits<int>::min(),
                              std::numeric_limits<int>::max()) == 0) {
        scm_wrong_type_arg_msg("select-other-window!", 3, delta_value, "integer");
    }
    HostLease& host = require_host(host_object, "select-other-window!");
    const WindowId window =
        entity_id_from_scheme<WindowTag>(window_value, "select-other-window!", 2);
    if (!host.services.select_other_window) {
        scm_misc_error("select-other-window!", "window-focus capability is unavailable", SCM_EOL);
    }
    try {
        const std::expected<void, std::string> selected =
            host.services.select_other_window(window, scm_to_int(delta_value));
        return selected ? SCM_BOOL_F : scm_from_utf8_string(selected.error().c_str());
    } catch (const std::exception& exception) {
        raise_host_error("select-other-window!", exception.what());
    } catch (...) {
        scm_misc_error("select-other-window!", "unknown C++ host failure", SCM_EOL);
    }
    return SCM_BOOL_F;
}

SCM request_redraw(SCM host_object) {
    HostLease& host = require_host(host_object, "request-redraw!");
    if (!host.services.request_redraw) {
        scm_misc_error("request-redraw!", "redraw capability is unavailable", SCM_EOL);
    }
    try {
        host.services.request_redraw();
        return SCM_UNSPECIFIED;
    } catch (const std::exception& exception) {
        raise_host_error("request-redraw!", exception.what());
    } catch (...) {
        scm_misc_error("request-redraw!", "unknown C++ host failure", SCM_EOL);
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
    (void)scm_c_define_gsubr("base-keymap-layers", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(base_keymap_layers));
    (void)scm_c_define_gsubr("active-keymap-layers", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(active_keymap_layers));
    (void)scm_c_define_gsubr("key-sequence-completions", 3, 0, 0,
                             reinterpret_cast<scm_t_subr>(key_sequence_completions));
    (void)scm_c_define_gsubr("set-input-feedback!", 4, 0, 0,
                             reinterpret_cast<scm_t_subr>(set_input_feedback));
    (void)scm_c_define_gsubr("clear-input-feedback!", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(clear_input_feedback));
    (void)scm_c_define_gsubr("%define-input-state!", 7, 0, 0,
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
    (void)scm_c_define_gsubr("%define-mode!", 8, 0, 0, reinterpret_cast<scm_t_subr>(define_mode));
    (void)scm_c_define_gsubr("define-file-mode-rule!", 5, 0, 0,
                             reinterpret_cast<scm_t_subr>(define_file_mode_rule));
    (void)scm_c_define_gsubr("define-project-provider!", 3, 0, 0,
                             reinterpret_cast<scm_t_subr>(define_project_provider));
    (void)scm_c_define_gsubr("mode-properties", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(mode_properties));
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
    (void)scm_c_define_gsubr("owned-user-modules", 1, 0, 0,
                             reinterpret_cast<scm_t_subr>(owned_user_modules));
    (void)scm_c_define_gsubr("project-root", 2, 0, 0, reinterpret_cast<scm_t_subr>(project_root));
    (void)scm_c_define_gsubr("project-files", 2, 0, 0, reinterpret_cast<scm_t_subr>(project_files));
    (void)scm_c_define_gsubr("path-relative", 3, 0, 0, reinterpret_cast<scm_t_subr>(path_relative));
    (void)scm_c_define_gsubr("path-filename", 2, 0, 0, reinterpret_cast<scm_t_subr>(path_filename));
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
    (void)scm_c_define_gsubr("erase-range!", 4, 0, 0, reinterpret_cast<scm_t_subr>(erase_range));
    (void)scm_c_define_gsubr("insert-text!", 3, 0, 0, reinterpret_cast<scm_t_subr>(insert_text));
    (void)scm_c_define_gsubr("soft-kill-range", 2, 0, 0,
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
    (void)scm_c_define_gsubr("path-parent", 2, 0, 0, reinterpret_cast<scm_t_subr>(path_parent));
    (void)scm_c_define_gsubr("directory-path?", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(directory_path_p));
    (void)scm_c_define_gsubr("path-as-directory", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(path_as_directory));
    (void)scm_c_define_gsubr("display-buffer!", 3, 0, 0,
                             reinterpret_cast<scm_t_subr>(display_buffer));
    (void)scm_c_define_gsubr("display-generated-buffer!", 4, 0, 0,
                             reinterpret_cast<scm_t_subr>(display_generated_buffer));
    (void)scm_c_define_gsubr("evaluate-scheme!", 3, 0, 0,
                             reinterpret_cast<scm_t_subr>(evaluate_scheme));
    (void)scm_c_define_gsubr("move-caret-to-line!", 4, 0, 0,
                             reinterpret_cast<scm_t_subr>(move_caret_to_line));
    (void)scm_c_define_gsubr("set-message!", 2, 0, 0, reinterpret_cast<scm_t_subr>(set_message));
    (void)scm_c_define_gsubr("ensure-project-index!", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(ensure_project_index));
    (void)scm_c_define_gsubr("open-file!", 3, 0, 0, reinterpret_cast<scm_t_subr>(open_file));
    (void)scm_c_define_gsubr("start-project-search!", 4, 0, 0,
                             reinterpret_cast<scm_t_subr>(start_project_search));
    (void)scm_c_define_gsubr("set-buffer-resource!", 3, 0, 0,
                             reinterpret_cast<scm_t_subr>(set_buffer_resource));
    (void)scm_c_define_gsubr("save-buffer!", 2, 0, 0, reinterpret_cast<scm_t_subr>(save_buffer));
    (void)scm_c_define_gsubr("open-buffer-ids", 1, 0, 0,
                             reinterpret_cast<scm_t_subr>(open_buffers));
    (void)scm_c_define_gsubr("kill-buffer!", 3, 0, 0, reinterpret_cast<scm_t_subr>(kill_buffer));
    (void)scm_c_define_gsubr("request-quit!", 2, 0, 0, reinterpret_cast<scm_t_subr>(request_quit));
    (void)scm_c_define_gsubr("split-window!", 3, 0, 0, reinterpret_cast<scm_t_subr>(split_window));
    (void)scm_c_define_gsubr("delete-window!", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(delete_window));
    (void)scm_c_define_gsubr("delete-other-windows!", 2, 0, 0,
                             reinterpret_cast<scm_t_subr>(delete_other_windows));
    (void)scm_c_define_gsubr("select-other-window!", 3, 0, 0,
                             reinterpret_cast<scm_t_subr>(select_other_window));
    (void)scm_c_define_gsubr("request-redraw!", 1, 0, 0,
                             reinterpret_cast<scm_t_subr>(request_redraw));
    scm_c_export(
        "define-command!", "set-command-documentation!", "define-interaction-provider!",
        "define-keymap!", "bind-key!", "bind-key-if-command!", "bind-remap!", "keymap-bindings",
        "resolve-key-sequence", "base-keymap-layers", "active-keymap-layers",
        "key-sequence-completions", "set-input-feedback!", "clear-input-feedback!",
        "%define-input-state!", "set-input-state-lifecycle!", "set-input-state-position-hints!",
        "define-input-strategy!", "set-default-input-strategy!", "set-view-input-strategy!",
        "view-input-strategy", "set-base-input-state!", "push-input-state!", "pop-input-state!",
        "reset-input-states!", "view-input-states", "observe-input-state-changes!", "define-thing!",
        "define-motion!", "%define-mode!", "define-file-mode-rule!", "define-project-provider!",
        "mode-properties", "set-buffer-major-mode!", "set-buffer-minor-mode!", "buffer-mode-policy",
        "buffer-mode-summary", "observe-mode-policy-changes!", "enabled-command-names",
        "command-properties", "open-buffer-summaries", "owned-user-modules", "project-root",
        "project-files", "path-relative", "path-filename", "active-key-bindings",
        "buffer-id-by-name", "buffer-name", "buffer-resource", "buffer-text", "path-parent",
        "directory-path?", "path-as-directory", "view-caret", "view-mark", "view-selection",
        "set-selection!", "clear-selection!", "push-selection-history!", "pop-selection-history!",
        "clear-selection-history!", "selection-history-depth", "replace-selection!",
        "selection-texts", "buffer-substring", "erase-range!", "insert-text!", "soft-kill-range",
        "set-view-caret!", "reset-preferred-column!", "thing-selection", "motion-selection",
        "expand-node-selection", "write-clipboard!", "read-clipboard", "display-buffer!",
        "display-generated-buffer!", "evaluate-scheme!", "move-caret-to-line!", "set-message!",
        "ensure-project-index!", "open-file!", "start-project-search!", "set-buffer-resource!",
        "save-buffer!", "open-buffer-ids", "kill-buffer!", "request-quit!", "split-window!",
        "delete-window!", "delete-other-windows!", "select-other-window!", "request-redraw!",
        nullptr);
}

void initialize_guile() {
    scm_init_guile();
    (void)scm_c_define_module("cind host", initialize_host_module, nullptr);
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
        InvokeCommand,
        InvokeProvider,
        InvokeInputHandler,
        InvokePositionHints,
        InvokeInputStateObserver,
        InvokeModePolicyObserver,
        CheckEnabled,
    };

    Operation operation = Operation::Load;
    std::string path;
    std::string source;
    std::string source_name;
    SCM host = SCM_UNDEFINED;
    SCM module = SCM_UNDEFINED;
    SCM procedure = SCM_UNDEFINED;
    SCM result = SCM_UNDEFINED;
    std::size_t count = 0;
    std::string query;
    const CommandContext* context = nullptr;
    const CommandInvocation* invocation = nullptr;
    KeyStroke key;
    const InputStateChange* input_state_change = nullptr;
    const BufferModePolicyChange* mode_policy_change = nullptr;
    EditorRuntime* runtime = nullptr;
    std::optional<CommandResult> command_result;
    std::vector<InteractionCandidate> provider_candidates;
    std::vector<PositionHint> position_hints;
    bool enabled = false;
    std::exception_ptr cpp_failure;
    std::string error;
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
    if (symbol_is(tag, "dispatch")) {
        if (size != 3 || !scm_is_string(scm_c_vector_ref(value, 1)) ||
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
        return CommandDispatch{.command = *command,
                               .invocation = {.arguments = std::move(arguments), .prefix = {}}};
    }
    if (symbol_is(tag, "interaction")) {
        if (size != 9 || !scheme_true(scm_symbol_p(scm_c_vector_ref(value, 1))) ||
            !scm_is_string(scm_c_vector_ref(value, 2)) ||
            !scm_is_string(scm_c_vector_ref(value, 3)) ||
            !scm_is_string(scm_c_vector_ref(value, 4)) ||
            !scm_is_string(scm_c_vector_ref(value, 5)) ||
            !scheme_boolean(scm_c_vector_ref(value, 6)) ||
            !scm_is_string(scm_c_vector_ref(value, 7)) ||
            !scm_is_vector(scm_c_vector_ref(value, 8))) {
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
            arguments_from_scheme(scm_c_vector_ref(value, 8), "command-interaction");
        const std::string accept_name = scheme_string(scm_c_vector_ref(value, 7));
        const std::optional<CommandId> accept = context.runtime().commands().find(accept_name);
        if (!accept) {
            return std::unexpected(
                CommandError{std::format("unknown accept command '{}'", accept_name)});
        }
        return InteractionRequest{.kind = kind,
                                  .prompt = scheme_string(scm_c_vector_ref(value, 2)),
                                  .initial_input = scheme_string(scm_c_vector_ref(value, 3)),
                                  .history = scheme_string(scm_c_vector_ref(value, 4)),
                                  .provider = scheme_string(scm_c_vector_ref(value, 5)),
                                  .allow_custom_input = scheme_true(scm_c_vector_ref(value, 6)),
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
        case GuileCall::Operation::InvokeCommand:
            call.result = scm_call_2(call.procedure, command_context_value(*call.context),
                                     command_invocation_value(*call.invocation));
            call.command_result = command_result_from_scheme(call.result, *call.context);
            break;
        case GuileCall::Operation::InvokeProvider:
            call.result = scm_call_2(call.procedure, command_context_value(*call.context),
                                     scm_from_utf8_string(call.query.c_str()));
            call.provider_candidates = provider_candidates_from_scheme(call.result);
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

InteractionProviderResult invoke_script_provider(const std::shared_ptr<GuileState>& state,
                                                 std::size_t provider_index,
                                                 CommandContext& context, std::string_view query) {
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
    return std::move(call.provider_candidates);
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
        : state_(std::make_shared<GuileState>()) {
        state_->owner = std::this_thread::get_id();
        std::call_once(guile_once, initialize_guile);
        for (std::string_view module :
             {"command", "input", "extension", "emacs", "toy-modal", "meow", "vim", "helix",
              "structural", "development", "introspect", "core"}) {
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
        lease_ =
            new HostLease{.runtime = &runtime, .state = state_, .services = std::move(services)};
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
        if (call.count != lease_->providers_installed || call.count != state_->providers.size()) {
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
            state_->last_error = "Guile mode policy returned an inconsistent definition count";
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

    GuileRuntimeSnapshot snapshot() const {
        return {.engine = "guile",
                .version = version_,
                .modules = {"cind command", "cind input", "cind extension", "cind emacs",
                            "cind toy-modal", "cind meow", "cind vim", "cind helix",
                            "cind structural", "cind development", "cind introspect", "cind core"},
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
                .scripted_providers = state_->providers.size(),
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
                .last_error = state_->last_error};
    }

private:
    void require_owner_thread() const {
        if (std::this_thread::get_id() != state_->owner) {
            throw std::logic_error("Guile runtime must run on its editor thread");
        }
    }

    std::shared_ptr<GuileState> state_;
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

std::expected<void, std::string> GuileRuntime::load_extension(const std::string& path) {
    return impl_->load_extension(path);
}

std::expected<GuileEvaluationResult, std::string>
GuileRuntime::evaluate(GuileEvaluationRequest request) {
    return impl_->evaluate(request);
}

GuileRuntimeSnapshot GuileRuntime::snapshot() const {
    return impl_->snapshot();
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
