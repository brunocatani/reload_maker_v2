#pragma once
#include <QByteArray>
#include <QString>
#include <cstdint>

namespace reloadmaker
{
    // A WAV sample as interleaved 16-bit PCM for preview playback.
    struct PreviewWave
    {
        std::uint16_t channels{};
        std::uint32_t sampleRate{};
        QByteArray pcm16;
    };

    // Reads plain PCM (8/16/24/32-bit) and WAVE_FORMAT_EXTENSIBLE PCM or
    // IEEE float (32-bit) samples, mono or stereo, and converts them to
    // 16-bit PCM. Mods ship 24/32-bit extensible files the game plays (the
    // G3's foley); refusing them hid every reload sound of such weapons.
    bool decodePreviewWave(const QByteArray& wav, PreviewWave& result, QString& error);
}
