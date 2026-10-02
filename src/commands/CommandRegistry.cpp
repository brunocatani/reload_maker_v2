#include "CommandRegistry.h"

#include <algorithm>
#include <stdexcept>

namespace reloadmaker
{
    void CommandRegistry::add(CommandSpec spec)
    {
        // Registration happens once at startup; a mistake here is a programming
        // error that must stop the editor before it accepts any request.
        if (spec.name.isEmpty() || !spec.run) {
            throw std::logic_error("A command needs a name and a handler");
        }
        if (_commands.contains(spec.name)) {
            throw std::logic_error("Command registered twice: " + spec.name.toStdString());
        }
        const QString name = spec.name;
        _commands.insert(name, std::move(spec));
    }

    const CommandSpec* CommandRegistry::find(const QString& name) const
    {
        const auto found = _commands.constFind(name);
        return found == _commands.cend() ? nullptr : &found.value();
    }

    QStringList CommandRegistry::names() const
    {
        QStringList result = _commands.keys();
        std::sort(result.begin(), result.end());
        return result;
    }
}
