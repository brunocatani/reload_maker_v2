// Source compatibility rules: when a weapon's reload applies, which
// conversions block it and which magazine groups have no physical body. Saved
// rules live in the session; otherwise the bundled defaults apply.
#include "ReloadMakerController.h"
#include "SourcePolicy.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QSet>

#include <algorithm>
#include <functional>
#include <stdexcept>

namespace reloadmaker
{
    namespace
    {
        constexpr qint64 kDefaultsSizeLimit = 1024 * 1024;
        constexpr qsizetype kDefaultsEntryLimit = 256;

        struct PolicyDefault
        {
            QJsonObject policy;
            QString id; // empty when no bundled default applies
        };

        // The bundled default for `weapon`. An entry with a source override
        // applies only when its marker plugin's presence matches. Throws when
        // the defaults are unreadable, invalid or ambiguous.
        PolicyDefault bundledPolicy(const QString& weapon, const std::function<bool(const QString&)>& pluginLoaded)
        {
            QFile file(QCoreApplication::applicationDirPath() + "/assets/source-policies.json");
            if (!file.open(QIODevice::ReadOnly) || file.size() > kDefaultsSizeLimit) {
                throw std::runtime_error("Bundled source compatibility defaults are unavailable");
            }
            QJsonParseError parse;
            const QJsonObject document = QJsonDocument::fromJson(file.readAll(), &parse).object();
            const bool valid = parse.error == QJsonParseError::NoError && document["version"] == 1 &&
                               document["policies"].isArray() && document["policies"].toArray().size() <= kDefaultsEntryLimit;
            if (!valid) {
                throw std::runtime_error("Bundled source compatibility defaults are invalid");
            }
            PolicyDefault result;
            for (const auto& value : document["policies"].toArray()) {
                const QJsonObject entry = value.toObject();
                if (entry["weapon"].toString().compare(weapon, Qt::CaseInsensitive) != 0) {
                    continue;
                }
                const QJsonObject candidate = entry["policy"].toObject();
                QString error;
                if (!validSourcePolicy(candidate, error)) {
                    throw std::runtime_error(error.toStdString());
                }
                if (candidate.contains("sourceOverride")) {
                    const QJsonObject marker = candidate["sourceOverride"].toObject();
                    const QString plugin = marker["marker"].toString().section(':', 0, 0);
                    if (pluginLoaded(plugin) != marker["present"].toBool()) {
                        continue;
                    }
                }
                if (!result.id.isEmpty()) {
                    throw std::runtime_error("Several compatibility defaults match this source; resolve the policy definitions");
                }
                result.policy = candidate;
                result.id = entry["id"].toString();
            }
            return result;
        }
    }

    QString ReloadMakerController::sourcePolicyKey() const
    {
        if (!currentWeapon()) {
            return {};
        }
        // One ESP project owns a weapon's rules, whatever source loaded it.
        return poseRecordKey(currentWeapon()->formId).toLower();
    }

    bool ReloadMakerController::sourcePluginLoaded(const QString& plugin) const
    {
        if (_libraryMode && _launchSnapshot.plugins.contains(plugin, Qt::CaseInsensitive)) {
            return true;
        }
        for (const QString& path : _package.plugins) {
            if (QFileInfo(path).fileName().compare(plugin, Qt::CaseInsensitive) == 0) {
                return true;
            }
        }
        for (const PluginContent& content : _plugins) {
            if (content.fileName.compare(plugin, Qt::CaseInsensitive) == 0) {
                return true;
            }
        }
        return false;
    }

    QJsonObject ReloadMakerController::sourcePolicyState() const
    {
        if (!currentWeapon()) {
            throw std::runtime_error("Select the source weapon first");
        }
        const QString key = sourcePolicyKey();
        const QString weapon = poseRecordKey(currentWeapon()->formId);
        PolicyDefault policy;
        if (_sourcePolicies.contains(key)) {
            policy.policy = _sourcePolicies.value(key).toObject();
        } else {
            policy = bundledPolicy(weapon, [this](const QString& plugin) { return sourcePluginLoaded(plugin); });
        }

        // Every modification this weapon can take, by record, for the rule editor.
        QMap<QString, QString> modifications;
        if (const auto* config = configuration()) {
            for (const auto& slot : config->entries()) {
                for (const auto id : slot.choices) {
                    if (const auto* mod = config->mod(id)) {
                        modifications[poseRecordKey(id)] = mod->name.isEmpty() ? mod->editorId : mod->name;
                    }
                }
            }
        }
        // The game retains a levelled OMOD container beside its selected leaf.
        // Derive these identities from NONE-type Includes records; a name or
        // an empty model alone must never exempt an unknown physical magazine.
        QJsonArray collections = policy.policy.value("magazineCollections").toArray();
        const auto* plugin = currentPlugin();
        const auto* config = configuration();
        QSet<std::uint32_t> magazineLeaves;
        if (plugin && config) {
            for (const auto& slot : config->entries()) {
                if (poseRecordKey(slot.attachPoint).compare("Fallout4.esm:05d4d7", Qt::CaseInsensitive) != 0) continue;
                for (const auto id : slot.choices) {
                    const auto* mod = config->mod(id);
                    if (mod && mod->weaponModification) magazineLeaves.insert(id);
                }
            }
            std::function<bool(std::uint32_t, int)> containsMagazine = [&](std::uint32_t id, int depth) {
                if (magazineLeaves.contains(id)) return true;
                const auto* mod = plugin->findObjectMod(id);
                if (depth >= 8 || !mod || mod->weaponModification || !mod->modelPath.isEmpty()) return false;
                for (const auto& child : mod->includes) if (containsMagazine(child.formId, depth + 1)) return true;
                return false;
            };
            for (const auto& mod : plugin->objectMods) {
                if (mod.weaponModification || !mod.modelPath.isEmpty() || mod.includes.isEmpty() ||
                    poseRecordKey(mod.attachPoint).compare("Fallout4.esm:05d4d7", Qt::CaseInsensitive) != 0 ||
                    !containsMagazine(mod.formId, 0)) continue;
                const auto record = poseRecordKey(mod.formId);
                if (std::none_of(collections.cbegin(), collections.cend(), [&](const auto& item) {
                    return item.toString().compare(record, Qt::CaseInsensitive) == 0;
                })) collections.append(record);
            }
        }
        if (!collections.isEmpty()) policy.policy["magazineCollections"] = collections;
        QJsonArray blocked;
        for (const auto& entry : policy.policy.value("blockedMods").toArray()) {
            if (!auxiliaryReloadMode(entry.toString())) blocked.append(entry);
        }
        if (blocked.isEmpty()) policy.policy.remove("blockedMods");
        else policy.policy["blockedMods"] = blocked;
        QJsonArray choices;
        for (auto it = modifications.cbegin(); it != modifications.cend(); ++it) {
            choices.append(QJsonObject{ { "record", it.key() }, { "name", it.value() } });
        }
        return {
            { "key", key },
            { "weapon", weapon },
            { "policy", policy.policy },
            { "saved", _sourcePolicies.contains(key) },
            { "preset", policy.id },
            { "modifications", choices },
        };
    }

    QJsonObject ReloadMakerController::configureSourcePolicy(const QJsonObject& request)
    {
        if (_loadThread || _reloadCapturing || !currentWeapon()) {
            throw std::runtime_error("Wait for the current source load");
        }
        const QString key = sourcePolicyKey();
        if (request["key"] != key) {
            throw std::runtime_error("Source context changed; reopen compatibility rules");
        }
        const QString operation = request["operation"].toString();
        if (operation == "reset") {
            _sourcePolicies.remove(key);
        } else if (operation == "save") {
            QString error;
            if (!request["policy"].isObject() || !validSourcePolicy(request["policy"].toObject(), error)) {
                throw std::runtime_error(error.isEmpty() ? "Choose valid compatibility rules" : error.toStdString());
            }
            _sourcePolicies[key] = request["policy"];
        } else {
            throw std::runtime_error("Choose save or reset for source compatibility");
        }
        noteDocumentChange(QStringLiteral("source compatibility"));
        return sourcePolicyState();
    }
}
