#include "domain/detectors/MagazinePieces.h"

#include <QSet>

namespace reloadmaker
{
    QStringList nameWords(const QString& text)
    {
        QStringList words;
        QString word;
        const auto flush = [&] {
            if (!word.isEmpty()) {
                words.append(word.toLower());
                word.clear();
            }
        };
        for (int i = 0; i < text.size(); ++i) {
            const QChar c = text[i];
            if (!c.isLetterOrNumber()) {
                flush();
                continue;
            }
            if (!word.isEmpty()) {
                const QChar previous = text[i - 1];
                const bool digitChange = previous.isDigit() != c.isDigit();
                const bool camel = previous.isLower() && c.isUpper();
                // "ARReceiver": the last capital of a run starts the next word.
                const bool acronymEnd = previous.isUpper() && c.isUpper() && i + 1 < text.size() && text[i + 1].isLower();
                if (digitChange || camel || acronymEnd) {
                    flush();
                }
            }
            word.append(c);
        }
        flush();
        return words;
    }

    namespace
    {
        QString cartridgeWord(const QString& text)
        {
            static const QSet<QString> words{ QStringLiteral("bullet"),    QStringLiteral("bullets"),    QStringLiteral("casing"),
                                              QStringLiteral("casings"),   QStringLiteral("cartridge"),  QStringLiteral("cartridges"),
                                              QStringLiteral("round"),     QStringLiteral("rounds"),     QStringLiteral("ammo"),
                                              QStringLiteral("shell"),     QStringLiteral("shells"),     QStringLiteral("brass"),
                                              QStringLiteral("projectile"), QStringLiteral("projectiles") };
            for (const auto& word : nameWords(text)) {
                if (words.contains(word)) {
                    return word;
                }
            }
            return {};
        }
    }

    PieceRoleProposal proposeMagazinePieceRole(const QString& shapeName, const QString& materialPath)
    {
        if (const auto word = cartridgeWord(shapeName); !word.isEmpty()) {
            return { true, QStringLiteral("shape name \"%1\" (%2)").arg(shapeName, word) };
        }
        // The material file and the folder holding it ("…/MK18/Ammo/x.bgsm").
        QString path = materialPath;
        path.replace('\\', '/');
        const auto parts = path.split('/', Qt::SkipEmptyParts);
        for (int i = parts.size() - 1; i >= 0 && i >= parts.size() - 2; --i) {
            QString part = parts[i];
            if (i == parts.size() - 1) {
                part = part.section('.', 0, 0);
            }
            if (const auto word = cartridgeWord(part); !word.isEmpty()) {
                return { true, QStringLiteral("material \"%1\" (%2)").arg(materialPath, word) };
            }
        }
        return {};
    }
}
