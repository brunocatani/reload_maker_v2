#include "HkxReader.h"

#include <QCoreApplication>
#include <QFile>
#include <QTextStream>
#include <QtEndian>

#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

using namespace reloadmaker;

namespace
{
    void require(bool condition, const char* message)
    {
        if (!condition) { throw std::runtime_error(message); }
    }

    void u32(QByteArray& bytes, qsizetype offset, std::uint32_t value)
    {
        if (bytes.size() < offset + 4) { bytes.resize(offset + 4, '\0'); }
        qToLittleEndian(value, reinterpret_cast<uchar*>(bytes.data() + offset));
    }

    void f32(QByteArray& bytes, qsizetype offset, float value)
    {
        u32(bytes, offset, std::bit_cast<std::uint32_t>(value));
    }

    void quaternion(QByteArray& bytes, const Quaternionf& value)
    {
        const qsizetype offset = bytes.size();
        f32(bytes, offset, value.x);
        f32(bytes, offset + 4, value.y);
        f32(bytes, offset + 8, value.z);
        f32(bytes, offset + 12, value.w);
    }

    Quaternionf axisZ(float degrees)
    {
        const float angle = degrees * 3.14159265358979323846f / 360.0f;
        return { 0, 0, std::sin(angle), std::cos(angle) };
    }

    Quaternionf negate(Quaternionf value)
    {
        return { -value.x, -value.y, -value.z, -value.w };
    }

    void expectRotation(const Quaternionf& actual, const Quaternionf& expected)
    {
        const float dot = actual.x * expected.x + actual.y * expected.y +
                          actual.z * expected.z + actual.w * expected.w;
        require(std::isfinite(dot) && std::abs(std::abs(dot) - 1.0f) < 2.0e-6f,
                "sampled rotation does not match the independent expected orientation");
    }

    // Minimal in-memory packfile fixture. Expected transforms below are analytic;
    // no production parser or spline implementation constructs their values.
    struct PackfileFixture
    {
        QByteArray data = QByteArray(0x120, '\0');
        QVector<std::array<std::uint32_t, 2>> localFixups;
        QString animationClass;

        explicit PackfileFixture(QString name) : animationClass(std::move(name))
        {
            std::memcpy(data.data() + 0x110, "Root", 5);
            localFixups.append({ 0xC0, 0x110 });
            f32(data, 0x14, 1.0f);
            u32(data, 0x18, 1);
        }

        void array(std::uint32_t field, const QByteArray& content, std::uint32_t count)
        {
            while (data.size() % 16) { data.append('\0'); }
            localFixups.append({ field, static_cast<std::uint32_t>(data.size()) });
            u32(data, field + 8, count);
            data.append(content);
        }

        QByteArray finish()
        {
            const QByteArray firstClass = animationClass.toLatin1() + '\0';
            const QByteArray names = firstClass + QByteArray("hkaAnimationBinding\0", 20);
            const std::uint32_t local = static_cast<std::uint32_t>(data.size());
            for (const auto& fixup : localFixups) {
                u32(data, data.size(), fixup[0]);
                u32(data, data.size(), fixup[1]);
            }
            const std::uint32_t global = static_cast<std::uint32_t>(data.size());
            for (const std::uint32_t value : { 0xC8U, 2U, 0U }) {
                u32(data, data.size(), value);
            }
            const std::uint32_t virtuals = static_cast<std::uint32_t>(data.size());
            for (const std::uint32_t value : { 0U, 0U, 0U, 0xB0U, 0U,
                                               static_cast<std::uint32_t>(firstClass.size()) }) {
                u32(data, data.size(), value);
            }
            const std::uint32_t end = static_cast<std::uint32_t>(data.size());
            QByteArray result(0x100, '\0');
            u32(result, 0, 0x57E0E057);
            u32(result, 4, 0x10C0C010);
            u32(result, 0x0C, 11);
            result[0x10] = 8;
            result[0x11] = 1;
            u32(result, 0x14, 3);
            std::memcpy(result.data() + 0x28, "hk_2014.1.0-r1", 14);
            std::memcpy(result.data() + 0x40, "__classnames__", 14);
            std::memcpy(result.data() + 0x80, "__types__", 9);
            std::memcpy(result.data() + 0xC0, "__data__", 8);
            u32(result, 0x54, 0x100);
            for (qsizetype field = 0x58; field <= 0x6C; field += 4) {
                u32(result, field, static_cast<std::uint32_t>(names.size()));
            }
            const std::uint32_t start = 0x100 + static_cast<std::uint32_t>(names.size());
            u32(result, 0x94, start);
            u32(result, 0xD4, start);
            u32(result, 0xD8, local);
            u32(result, 0xDC, global);
            u32(result, 0xE0, virtuals);
            u32(result, 0xE4, end);
            u32(result, 0xE8, end);
            u32(result, 0xEC, end);
            result.append(names);
            result.append(data);
            return result;
        }
    };

    QByteArray spline(const QVector<QVector<Quaternionf>>& blockControls,
                      std::uint8_t degree = 1, bool staticRotation = false)
    {
        PackfileFixture file(QStringLiteral("hkaSplineCompressedAnimation"));
        const auto blocks = static_cast<std::uint32_t>(blockControls.size());
        f32(file.data, 0x14, static_cast<float>(blocks));
        u32(file.data, 0x38, blocks * 2U + 1U);
        u32(file.data, 0x3C, blocks);
        u32(file.data, 0x40, 3);
        u32(file.data, 0x44, 4);
        f32(file.data, 0x48, 1);
        f32(file.data, 0x50, 0.5f);
        QByteArray offsets;
        QByteArray blob;
        for (const auto& controls : blockControls) {
            u32(offsets, offsets.size(), static_cast<std::uint32_t>(blob.size()));
            blob.append(char(5U << 2U)); // Uncompressed quaternion components.
            blob.append(char(7));       // Static XYZ translation.
            blob.append(staticRotation ? char(15) : char(0xF0));
            blob.append(char(0));       // Identity scale.
            f32(blob, blob.size(), 3.0f);
            f32(blob, blob.size(), -7.0f);
            f32(blob, blob.size(), 11.0f);
            if (!staticRotation) {
                const auto count = static_cast<std::uint16_t>(controls.size() - 1);
                blob.append(char(count & 255));
                blob.append(char(count >> 8));
                blob.append(char(degree));
                for (int i = 0; i <= degree; ++i) { blob.append(char(0)); }
                for (int i = 0; i <= degree; ++i) { blob.append(char(2)); }
                while (blob.size() % 4) { blob.append('\0'); }
            }
            for (const auto& control : controls) { quaternion(blob, control); }
        }
        file.array(0x58, offsets, blocks);
        file.array(0x98, blob, static_cast<std::uint32_t>(blob.size()));
        return file.finish();
    }

    QByteArray interleaved(Quaternionf first, Quaternionf last)
    {
        PackfileFixture file(QStringLiteral("hkaInterleavedUncompressedAnimation"));
        QByteArray values;
        for (int index = 0; index < 2; ++index) {
            f32(values, values.size(), static_cast<float>(index) * 10.0f);
            for (int i = 0; i < 3; ++i) { f32(values, values.size(), 0); }
            quaternion(values, index == 0 ? first : last);
            for (int i = 0; i < 4; ++i) { f32(values, values.size(), 1); }
        }
        file.array(0x38, values, 2);
        return file.finish();
    }

    HkxClip decode(const QByteArray& bytes)
    {
        QString error;
        HkxClip clip = HkxReader::readClip(bytes, error);
        if (!clip.valid()) { throw std::runtime_error(error.toStdString()); }
        require(clip.trackToBone == QVector<std::int16_t>{ 0 }, "identity binding map failed");
        return clip;
    }

    Transformf sample(const HkxClip& clip, float time)
    {
        QVector<Transformf> pose;
        clip.sample(time, pose);
        require(pose.size() == 1, "wrong output track count");
        return pose.front();
    }

    void syntheticChecks()
    {
        const Quaternionf ninety = axisZ(90);
        for (std::uint8_t degree : { 1, 2, 3 }) {
            for (const auto& rotation : { ninety, negate(ninety) }) {
                const HkxClip clip = decode(spline({ QVector<Quaternionf>(degree + 1, rotation) }, degree));
                for (float t : { -0.5f, 0.0f, 0.25f, 0.5f, 0.75f, 1.0f, 1.5f }) {
                    const auto pose = sample(clip, t);
                    expectRotation(pose.rotation, ninety);
                    require(pose.translation.x == 3 && pose.translation.y == -7 &&
                            pose.translation.z == 11, "rotation repair changed translation");
                    require(pose.scale.x == 1 && pose.scale.y == 1 && pose.scale.z == 1,
                            "rotation repair changed scale");
                }
            }
        }
        expectRotation(sample(decode(spline({ { ninety } }, 0, true)), 0.5f).rotation, ninety);
        const HkxClip linear = decode(spline({ { axisZ(0), negate(ninety) } }));
        expectRotation(sample(linear, 0).rotation, axisZ(0));
        expectRotation(sample(linear, 0.5f).rotation, axisZ(45));
        expectRotation(sample(linear, 1).rotation, ninety);
        const HkxClip seam = decode(spline({ { ninety, ninety }, { negate(ninety), negate(ninety) } }));
        for (float t : { 0.9999f, 1.0f, 1.0001f, 2.0f }) {
            expectRotation(sample(seam, t).rotation, ninety);
        }
        const HkxClip raw = decode(interleaved(axisZ(0), negate(ninety)));
        expectRotation(sample(raw, 0).rotation, axisZ(0));
        expectRotation(sample(raw, 0.5f).rotation, axisZ(45));
        expectRotation(sample(raw, 1).rotation, ninety);
        require(std::abs(sample(raw, 0.5f).translation.x - 5) < 1e-6f,
                "interleaved midpoint translation failed");
    }
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);
    try {
        syntheticChecks();
        out << "PASS analytic spline degrees 1-3, quaternion signs, static/dynamic equality, endpoints, block seam and interleaved sampling\n";
    } catch (const std::exception& error) {
        out << "FAIL " << error.what() << '\n';
        return 1;
    }
    return 0;
}
