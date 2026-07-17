#pragma once

#include "editor/basic_commands.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace cind {

class EditorRuntime;

class SearchCommands {
public:
    using MessageSink = std::function<void(std::string)>;

    SearchCommands(EditorRuntime& runtime, EditSessionResolver session, MessageSink message_sink);

    std::string_view query() const { return query_; }
    void set_query(std::string query) { query_ = std::move(query); }
    bool move(bool forward, ViewId view);

private:
    CommandResult accept(CommandContext&, const CommandInvocation& invocation);

    EditSessionResolver session_;
    MessageSink message_sink_;
    std::string query_;
};

} // namespace cind
