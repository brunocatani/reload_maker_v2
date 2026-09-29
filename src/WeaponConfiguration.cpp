#include "WeaponConfiguration.h"

#include <QRegularExpression>
#include <algorithm>

namespace reloadmaker
{
    namespace
    {
        QString slotLabel(const ObjectModRecord& mod, const PluginContent& plugin)
        {
            // Labels are presentation only. Exclusivity/availability always use
            // record identities, never these human-readable names.
            static const QRegularExpression bracket(QStringLiteral("^\\[([^\\]]+)\\]"));
            const auto match = bracket.match(mod.name.isEmpty() ? mod.editorId : mod.name);
            QString label = match.hasMatch() ? match.captured(1) : plugin.keywordNames.value(mod.attachPoint);
            if (label.startsWith(QStringLiteral("ap_"), Qt::CaseInsensitive)) { label = label.section('_', -1); }
            const auto keywordBracket = bracket.match(label);
            if (keywordBracket.hasMatch()) { label = keywordBracket.captured(1); }
            if (label.compare(QStringLiteral("Mag"), Qt::CaseInsensitive) == 0) { label = QStringLiteral("Magazine"); }
            if (label.isEmpty()) { label = QStringLiteral("Slot %1").arg(mod.attachPoint, 8, 16, QLatin1Char('0')); }
            if (!mod.attachPoint) { label = QStringLiteral("Unclassified: %1").arg(mod.name.isEmpty() ? mod.editorId : mod.name); }
            return label;
        }
    }

    void WeaponConfiguration::initialize(const PluginContent& plugin, const WeaponRecord& weapon)
    {
        _weapon = weapon;
        _mods.clear(); _slots.clear(); _selected.clear(); _warnings.clear();
        for (const auto& candidate : plugin.objectMods) {
            if (!candidate.targetType.isEmpty() && candidate.targetType != QStringLiteral("WEAP")) { continue; }
            const bool isDefault = weapon.defaultObjectMods.contains(candidate.formId);
            if (!candidate.weaponModification && candidate.metadataWarning.isEmpty()) { continue; }
            const bool targetMatches = std::any_of(candidate.targetKeywords.cbegin(), candidate.targetKeywords.cend(),
                [&weapon](auto id) { return weapon.keywords.contains(id); });
            if (!isDefault && !candidate.targetKeywords.isEmpty() && !targetMatches) { continue; }
            _mods.append(candidate);
            const SlotKey key = configurationSlotKey(candidate);
            auto found = std::find_if(_slots.begin(), _slots.end(), [key](const auto& s) { return s.key == key; });
            if (found == _slots.end()) {
                _slots.append({key, candidate.attachPoint, slotLabel(candidate, plugin), {}, false, false});
                found = _slots.end() - 1;
            }
            found->choices.append(candidate.formId);
            found->hasModels |= !candidate.modelPath.isEmpty();
            found->unverified |= weapon.attachParentSlots.isEmpty() || candidate.attachPoint == 0 || candidate.targetKeywords.isEmpty() ||
                                 !candidate.metadataWarning.isEmpty();
        }
        for (auto id : weapon.defaultObjectMods) {
            const auto* candidate = mod(id);
            if (!candidate) {
                _warnings.append(QStringLiteral("Default modification %1 is not available in this plugin").arg(id, 8, 16, QLatin1Char('0')));
                continue;
            }
            const SlotKey key = configurationSlotKey(*candidate);
            if (_selected.contains(key) && _selected.value(key) != id) {
                _warnings.append(QStringLiteral("Multiple default choices for slot %1; first declared default retained").arg(key, 0, 16));
                continue;
            }
            _selected.insert(key, id);
        }
        std::stable_sort(_slots.begin(), _slots.end(), [](const auto& a, const auto& b) { return a.label.localeAwareCompare(b.label) < 0; });
    }

    const ConfigurationSlot* WeaponConfiguration::slot(SlotKey key) const
    {
        const auto it = std::find_if(_slots.cbegin(), _slots.cend(), [key](const auto& s) { return s.key == key; });
        return it == _slots.cend() ? nullptr : &*it;
    }

    const ObjectModRecord* WeaponConfiguration::mod(std::uint32_t id) const
    {
        const auto it = std::find_if(_mods.cbegin(), _mods.cend(), [id](const auto& m) { return m.formId == id; });
        return it == _mods.cend() ? nullptr : &*it;
    }

    bool WeaponConfiguration::select(SlotKey key, std::uint32_t id)
    {
        const auto* entry = slot(key);
        if (!entry || (id && !entry->choices.contains(id))) { return false; }
        if (_selected.value(key) == id) { return false; }
        _selected.insert(key, id);
        return true;
    }

    QHash<SlotKey, SlotKey> WeaponConfiguration::availableSlots() const
    {
        QHash<SlotKey, SlotKey> exposed;
        for (auto id : _weapon.attachParentSlots) { if (slot(id)) { exposed.insert(id, 0); } }
        for (const auto& entry : _slots) {
            // A missing root-slot contract stays explicitly unverified in the UI.
            if (!entry.attachPoint || _weapon.attachParentSlots.isEmpty()) { exposed.insert(entry.key, 0); }
        }
        for (int pass = 0; pass < _slots.size(); ++pass) {
            bool changed = false;
            for (const auto& entry : _slots) {
                if (!exposed.contains(entry.key)) { continue; }
                const auto* selection = mod(selected(entry.key));
                if (!selection) { continue; }
                for (auto child : selection->attachParentSlots) {
                    if (slot(child) && !exposed.contains(child)) { exposed.insert(child, entry.key); changed = true; }
                }
            }
            if (!changed) { break; }
        }
        return exposed;
    }

    QVector<std::uint32_t> WeaponConfiguration::activeMods() const
    {
        const auto exposed = availableSlots();
        QVector<std::uint32_t> result;
        for (const auto& entry : _slots) {
            const auto id = selected(entry.key);
            if (id && exposed.contains(entry.key)) { result.append(id); }
        }
        return result;
    }
}
