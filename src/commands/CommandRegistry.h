#pragma once

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

namespace reloadmaker
{
    // Whether a command may change the document. Edits reject a caller's stale
    // expected revision; reads ignore it so clients can poll while editing.
    enum class CommandAccess
    {
        Read,
        Edit,
    };

    // Whether a command may run while a source load, assembly rebuild or reload
    // capture is in progress. Idle-only commands also start with a cleared
    // last error, so their failure report belongs to this command alone.
    enum class CommandGate
    {
        Always,
        WhenIdle,
    };

    // A handler returns its response, or nullopt to finish as a completed edit:
    // fail on a recorded last error, advance the revision and answer with the
    // studio state. Handlers report rejections by throwing std::runtime_error.
    using CommandHandler = std::function<std::optional<QJsonObject>(const QJsonObject& params)>;

    struct CommandSpec
    {
        QString name;
        CommandAccess access{ CommandAccess::Edit };
        CommandGate gate{ CommandGate::WhenIdle };
        CommandHandler run;
    };

    // The single list of editor operations. The command pipe dispatches through
    // it, and every operation exposed to MCP clients is registered here.
    class CommandRegistry
    {
    public:
        // Rejects duplicate names and missing handlers at registration time.
        void add(CommandSpec spec);
        [[nodiscard]] const CommandSpec* find(const QString& name) const;
        [[nodiscard]] QStringList names() const;

    private:
        QHash<QString, CommandSpec> _commands;
    };
}
