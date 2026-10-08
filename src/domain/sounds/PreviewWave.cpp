#include "domain/sounds/PreviewWave.h"

#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace reloadmaker
{
    namespace
    {
        constexpr std::uint16_t kPcm = 1;
        constexpr std::uint16_t kFloat = 3;
        constexpr std::uint16_t kExtensible = 0xFFFE;
        // KSDATAFORMAT_SUBTYPE_* share this tail after the 2-byte format code.
        constexpr unsigned char kSubtypeTail[14]{ 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 };

        std::int16_t toPcm16(const uchar* sample, std::uint16_t format, std::uint16_t bits)
        {
            if (format == kFloat) {
                float value;
                std::memcpy(&value, sample, sizeof value);
                value = std::isfinite(value) ? std::clamp(value, -1.f, 1.f) : 0.f;
                return std::int16_t(std::lround(value * 32767.f));
            }
            switch (bits) {
            case 8: return std::int16_t((int(sample[0]) - 128) << 8);
            case 16: return qFromLittleEndian<std::int16_t>(sample);
            case 24: return qFromLittleEndian<std::int16_t>(sample + 1);
            default: return std::int16_t(qFromLittleEndian<std::int32_t>(sample) >> 16);
            }
        }
    }

    bool decodePreviewWave(const QByteArray& wav, PreviewWave& result, QString& error)
    {
        error.clear();
        result = {};
        const auto fail = [&](const char* message) {
            error = QString::fromLatin1(message);
            return false;
        };
        if (wav.size() < 12 || wav.size() > 64 * 1024 * 1024 || wav.left(4) != "RIFF" || wav.mid(8, 4) != "WAVE") {
            return fail("Preview requires a PCM WAV sample (up to 64 MiB).");
        }
        std::uint16_t tag = 0, channels = 0, align = 0, bits = 0;
        std::uint32_t rate = 0, bytesPerSecond = 0;
        bool format = false;
        QByteArray data;
        for (qsizetype at = 12; at <= wav.size() - 8;) {
            const auto count = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(wav.constData() + at + 4));
            const auto start = at + 8;
            if (count > quint32(wav.size() - start)) {
                return fail("Truncated WAV chunk.");
            }
            if (wav.mid(at, 4) == "fmt ") {
                if (count < 16) {
                    return fail("Truncated WAV format.");
                }
                const auto* f = reinterpret_cast<const uchar*>(wav.constData() + start);
                tag = qFromLittleEndian<quint16>(f);
                channels = qFromLittleEndian<quint16>(f + 2);
                rate = qFromLittleEndian<quint32>(f + 4);
                bytesPerSecond = qFromLittleEndian<quint32>(f + 8);
                align = qFromLittleEndian<quint16>(f + 12);
                bits = qFromLittleEndian<quint16>(f + 14);
                if (tag == kExtensible) {
                    if (count < 40 || std::memcmp(f + 26, kSubtypeTail, sizeof kSubtypeTail) != 0) {
                        return fail("Unsupported WAV extensible subformat.");
                    }
                    if (qFromLittleEndian<quint16>(f + 18) != bits) {
                        return fail("Unsupported WAV container: valid bits differ from sample size.");
                    }
                    tag = qFromLittleEndian<quint16>(f + 24);
                }
                format = true;
            } else if (wav.mid(at, 4) == "data") {
                data = wav.mid(start, count);
            }
            at = start + count + (count & 1);
        }
        const bool pcm = tag == kPcm && (bits == 8 || bits == 16 || bits == 24 || bits == 32);
        const bool floating = tag == kFloat && bits == 32;
        if (!format || data.isEmpty() || !(pcm || floating) || channels < 1 || channels > 2 || rate < 8000 || rate > 192000 ||
            align != channels * bits / 8 || bytesPerSecond != rate * align || data.size() % align) {
            return fail("Unsupported or invalid PCM sample format.");
        }
        const qsizetype samples = data.size() / (bits / 8);
        result.channels = channels;
        result.sampleRate = rate;
        result.pcm16.resize(samples * 2);
        const auto* in = reinterpret_cast<const uchar*>(data.constData());
        auto* out = reinterpret_cast<uchar*>(result.pcm16.data());
        for (qsizetype i = 0; i < samples; ++i) {
            qToLittleEndian<std::int16_t>(toPcm16(in + i * (bits / 8), tag, bits), out + i * 2);
        }
        return true;
    }
}
