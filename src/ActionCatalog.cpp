#include "ActionCatalog.h"
#include <QRegularExpression>

namespace reloadmaker {
namespace {
// Readable suffix for variant clips: "ReloadStart" -> "start", "ReadyA" -> "alternate A".
QString variantText(const QString& suffix) {
    if (suffix.isEmpty()) return {};
    if (suffix.size() == 1 && suffix[0].isUpper()) return QStringLiteral("alternate %1").arg(suffix);
    QString text;
    for (const QChar c : suffix) {
        if (c.isUpper() && !text.isEmpty()) text += QLatin1Char(' ');
        text += c.toLower();
    }
    return text;
}
}

QString actionFamilyName(ActionFamily family) {
    switch (family) {
    case ActionFamily::Idle: return QStringLiteral("Idle");
    case ActionFamily::Equip: return QStringLiteral("Equip");
    case ActionFamily::Fire: return QStringLiteral("Fire");
    case ActionFamily::Cycle: return QStringLiteral("Cycle");
    case ActionFamily::Reload: return QStringLiteral("Reload");
    }
    return {};
}

std::optional<ActionClass> classifyWeaponClip(const QString& baseName) {
    static const QRegularExpression equip(QStringLiteral("^WPNEquip(Fast)?$"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression single(QStringLiteral("^WPNFireSingleReady([A-Z])?$"), QRegularExpression::CaseInsensitiveOption);
    // Forward is the canonical automatic clip; Back/plain are movement-direction blends
    // of the same mechanical action and are not separate authoring sources.
    static const QRegularExpression automatic(QStringLiteral("^WPNFireAutoReadyForward$"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression cycle(QStringLiteral("^WPNBoltChargeReady$"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression reload(QStringLiteral("^WPNReload(\\w*)$"), QRegularExpression::CaseInsensitiveOption);

    if (const auto m = equip.match(baseName); m.hasMatch()) {
        const bool fast = !m.captured(1).isEmpty();
        return ActionClass{ActionFamily::Equip, fast ? QStringLiteral("Equip — fast") : QStringLiteral("Equip"), false, false, fast ? 1 : 0};
    }
    if (const auto m = single.match(baseName); m.hasMatch()) {
        const auto variant = m.captured(1).toUpper();
        return ActionClass{ActionFamily::Fire, variant.isEmpty() ? QStringLiteral("Fire — single") : QStringLiteral("Fire — single · %1").arg(variantText(variant)),
                           false, false, variant.isEmpty() ? 0 : 1 + variant[0].unicode()};
    }
    if (automatic.match(baseName).hasMatch())
        return ActionClass{ActionFamily::Fire, QStringLiteral("Fire — automatic"), true, false, 200};
    if (cycle.match(baseName).hasMatch())
        return ActionClass{ActionFamily::Cycle, QStringLiteral("Cycle — bolt / charge"), false, false, 0};
    if (const auto m = reload.match(baseName); m.hasMatch()) {
        const auto suffix = m.captured(1);
        // Sighted reloads duplicate the ready reload for a flat-screen aim stance.
        if (suffix.contains(QStringLiteral("Sighted"), Qt::CaseInsensitive)) return std::nullopt;
        return ActionClass{ActionFamily::Reload, suffix.isEmpty() ? QStringLiteral("Reload") : QStringLiteral("Reload — %1").arg(variantText(suffix)),
                           false, suffix.isEmpty(), suffix.isEmpty() ? 0 : 1};
    }
    return std::nullopt;
}
}
