#pragma once

#include <QString>
#include <QStringList>

namespace reloadmaker
{
    // Lower-case words of a shape, node or material name: split at
    // punctuation, camel case and letter/digit changes ("MagBullet:0" gives
    // mag, bullet, 0; "338BrassProjectile" gives 338, brass, projectile).
    [[nodiscard]] QStringList nameWords(const QString& text);

    // Whether a magazine piece reads as a cartridge, from its shape name or
    // its material's file and folder names (rework 14, item 4). The words
    // are bullet, casing, cartridge, round, ammo, shell, brass and
    // projectile, singular or plural; abbreviations such as "rds" are not
    // read, so "magazine_10rds" stays a body. A saved magazine setup
    // overrides the proposal.
    struct PieceRoleProposal
    {
        bool cartridge{ false };
        QString evidence; // which name and word, empty for a body
    };

    [[nodiscard]] PieceRoleProposal proposeMagazinePieceRole(const QString& shapeName, const QString& materialPath);
}
