#pragma once

#include "cli/session.hpp"
#include "editor/command.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace cind {

class EditorRuntime;

class SearchCommands {
public:
    using MessageSink = std::function<void(std::string)>;

    SearchCommands(EditorRuntime& runtime, EditSession& session, MessageSink message_sink);

    std::string_view query() const { return query_; }
    void set_query(std::string query) { query_ = std::move(query); }
    bool move(bool forward);

private:
    CommandResult begin(CommandContext&, const CommandInvocation&) const;
    CommandResult accept(CommandContext&, const CommandInvocation& invocation);

    EditorRuntime* runtime_;
    EditSession* session_;
    MessageSink message_sink_;
    CommandId accept_command_;
    std::string query_;
};

} // namespace cind
