#include "viewport/Pins.h"

#include <QJsonArray>
#include <QUuid>

#include <cmath>

namespace reloadmaker
{
    namespace
    {
        constexpr qsizetype kMaximumNameLength = 512;

        bool readAuthor(const QString& name, PinAuthor& author)
        {
            if (name == QLatin1String("human")) {
                author = PinAuthor::Human;
                return true;
            }
            if (name == QLatin1String("ai")) {
                author = PinAuthor::Ai;
                return true;
            }
            return false;
        }
    }

    QString pinAuthorName(PinAuthor author)
    {
        return author == PinAuthor::Ai ? QStringLiteral("ai") : QStringLiteral("human");
    }

    QString pinText(const QString& text)
    {
        const auto trimmed = text.trimmed();
        return trimmed.size() > kMaximumPinText ? QString() : trimmed;
    }

    QJsonObject pinJson(const Pin& pin)
    {
        QJsonObject json{ { "id", pin.id },
                          { "slot", QString::number(pin.slot) },
                          { "part", QString::number(pin.part) },
                          { "shape", pin.shape },
                          { "shapeIndex", pin.shapeIndex },
                          { "offset", QJsonArray{ pin.offset.x, pin.offset.y, pin.offset.z } },
                          { "text", pin.text },
                          { "author", pinAuthorName(pin.author) },
                          { "state", pin.resolved ? "resolved" : "open" } };
        if (!pin.resolution.isEmpty()) {
            json["resolution"] = pin.resolution;
        }
        return json;
    }

    bool readPin(const QJsonObject& json, Pin& pin, QString& error)
    {
        const auto fail = [&error](const QString& message) {
            error = message;
            return false;
        };
        Pin read;
        read.id = json["id"].toString();
        if (QUuid::fromString(read.id).isNull()) {
            return fail(QStringLiteral("pin without a valid id"));
        }
        bool slotValid = false, partValid = false;
        read.slot = json["slot"].toString().toULongLong(&slotValid);
        read.part = json["part"].toString().toUInt(&partValid);
        if (!slotValid || !partValid) {
            return fail(QStringLiteral("pin %1 has no part").arg(read.id));
        }
        read.shape = json["shape"].toString();
        read.shapeIndex = json["shapeIndex"].toInt(-1);
        if (read.shape.isEmpty() || read.shape.size() > kMaximumNameLength || read.shapeIndex < 0) {
            return fail(QStringLiteral("pin %1 has no anchor shape").arg(read.id));
        }
        const auto offset = json["offset"].toArray();
        if (offset.size() != 3) {
            return fail(QStringLiteral("pin %1 has no offset").arg(read.id));
        }
        for (const auto& value : offset) {
            if (!value.isDouble() || !std::isfinite(value.toDouble())) {
                return fail(QStringLiteral("pin %1 has an invalid offset").arg(read.id));
            }
        }
        read.offset = { float(offset[0].toDouble()), float(offset[1].toDouble()), float(offset[2].toDouble()) };
        read.text = pinText(json["text"].toString());
        if (read.text.isEmpty()) {
            return fail(QStringLiteral("pin %1 has no note").arg(read.id));
        }
        const auto state = json["state"].toString();
        if (!readAuthor(json["author"].toString(), read.author) || (state != QLatin1String("open") && state != QLatin1String("resolved"))) {
            return fail(QStringLiteral("pin %1 has an unknown author or state").arg(read.id));
        }
        read.resolved = state == QLatin1String("resolved");
        read.resolution = json["resolution"].toString();
        if (read.resolution.size() > kMaximumPinText) {
            return fail(QStringLiteral("pin %1 has an over-long resolution").arg(read.id));
        }
        pin = std::move(read);
        return true;
    }

    bool readPins(const QJsonValue& value, QVector<Pin>& pins, QString& error)
    {
        pins.clear();
        if (value.isUndefined()) {
            return true;
        }
        const auto list = value.toArray();
        if (!value.isArray() || list.size() > kMaximumPins) {
            error = QStringLiteral("the pin list is invalid");
            return false;
        }
        for (const auto& item : list) {
            Pin pin;
            if (!item.isObject() || !readPin(item.toObject(), pin, error)) {
                if (!item.isObject()) {
                    error = QStringLiteral("a pin is not an object");
                }
                pins.clear();
                return false;
            }
            pins.append(std::move(pin));
        }
        return true;
    }
}
