#pragma once

#include "EspReader.h"

namespace reloadmaker
{
    using SlotKey = std::uint64_t;

    [[nodiscard]] inline SlotKey configurationSlotKey(const ObjectModRecord& mod)
    {
        return mod.attachPoint ? mod.attachPoint : (SlotKey{1} << 32) | mod.formId;
    }

    struct ConfigurationSlot
    {
        SlotKey key{0};
        std::uint32_t attachPoint{0};
        QString label;
        QVector<std::uint32_t> choices;
        bool hasModels{false};
        bool unverified{false};
    };

    // Session-owned value data. Choices are retained when a parent temporarily
    // removes a slot, but unavailable choices do not enter the assembled weapon.
    class WeaponConfiguration final
    {
    public:
        void initialize(const PluginContent& plugin, const WeaponRecord& weapon);
        [[nodiscard]] bool select(SlotKey slot, std::uint32_t mod);
        [[nodiscard]] QHash<SlotKey, SlotKey> availableSlots() const;
        [[nodiscard]] QVector<std::uint32_t> activeMods() const;
        [[nodiscard]] const ConfigurationSlot* slot(SlotKey key) const;
        [[nodiscard]] std::uint32_t selected(SlotKey key) const { return _selected.value(key); }
        [[nodiscard]] const QVector<ConfigurationSlot>& entries() const { return _slots; }
        [[nodiscard]] const QStringList& warnings() const { return _warnings; }
        [[nodiscard]] const ObjectModRecord* mod(std::uint32_t id) const;

    private:
        WeaponRecord _weapon;
        QVector<ObjectModRecord> _mods;
        QVector<ConfigurationSlot> _slots;
        QHash<SlotKey, std::uint32_t> _selected;
        QStringList _warnings;
    };
}
