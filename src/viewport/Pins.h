#pragma once

#include "HkxReader.h"
#include "WeaponConfiguration.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QVector>

#include <cstdint>

namespace reloadmaker
{
    // Who wrote a pin: the person in the editor, or a command client (the AI).
    enum class PinAuthor
    {
        Human,
        Ai,
    };

    // A note pointed at a part (rework 03 §4). It is anchored to one shape of
    // the part installed in a slot, with an offset in that shape's frame, so it
    // follows the part through animation. A pin whose part is not installed in
    // the current configuration stays listed but is not drawn.
    struct Pin
    {
        QString id;
        SlotKey slot{ 0 };
        std::uint32_t part{ 0 };  // the installed attachment when the pin was placed
        QString shape;            // the anchor shape's name in the part's model
        int shapeIndex{ 0 };      // among the part's shapes with that name
        Vec3f offset{};           // in the anchor shape's frame
        QString text;
        PinAuthor author{ PinAuthor::Human };
        bool resolved{ false };
        QString resolution;       // what resolved it, for example the changed property
    };

    inline constexpr int kMaximumPins = 512;
    inline constexpr int kMaximumPinText = 2000;

    [[nodiscard]] QString pinAuthorName(PinAuthor author);
    [[nodiscard]] QJsonObject pinJson(const Pin& pin);
    // Validates a saved pin; on failure error says why.
    [[nodiscard]] bool readPin(const QJsonObject& json, Pin& pin, QString& error);
    // A project's saved pins; an absent value is no pins.
    [[nodiscard]] bool readPins(const QJsonValue& value, QVector<Pin>& pins, QString& error);
    // Trimmed note text; empty when the note is blank or too long.
    [[nodiscard]] QString pinText(const QString& text);
}
