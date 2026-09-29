#include "HkxReader.h"

#include <QtEndian>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <unordered_map>

namespace reloadmaker
{
    namespace
    {
        constexpr std::uint32_t kMagic0 = 0x57E0E057U;
        constexpr std::uint32_t kMagic1 = 0x10C0C010U;
        constexpr std::uint32_t kPackfileVersion = 11U;
        constexpr qsizetype kFileHeaderSize = 0x40;
        constexpr qsizetype kSectionHeaderSize = 0x40;
        constexpr std::uint32_t kMaximumObjects = 65'536U;
        constexpr std::uint32_t kMaximumBones = 2'048U;
        constexpr std::uint32_t kMaximumTracks = 2'048U;
        constexpr std::uint32_t kMaximumAnnotations = 16'384U;
        constexpr std::uint32_t kMaximumBlocks = 4'096U;
        constexpr std::uint32_t kMaximumAnimationDataBytes = 256U * 1024U * 1024U;
        constexpr std::uint32_t kInterleavedTransformSize = 0x30U;
        constexpr std::uint8_t kMaximumSplineDegree = 16U;

        class ByteReader final
        {
        public:
            explicit ByteReader(const QByteArray& data) : _data(data) {}

            [[nodiscard]] qsizetype size() const noexcept { return _data.size(); }

            void require(const qsizetype offset, const qsizetype count) const
            {
                if (offset < 0 || count < 0 || offset > _data.size() - count) {
                    throw std::runtime_error("HKX field is outside the file");
                }
            }

            [[nodiscard]] std::uint8_t u8(const qsizetype offset) const
            {
                require(offset, 1);
                return static_cast<std::uint8_t>(_data.at(offset));
            }

            [[nodiscard]] Q_NEVER_INLINE std::uint16_t u16(const qsizetype offset) const
            {
                require(offset, 2);
                std::uint16_t value = 0;
                std::memcpy(&value, _data.constData() + offset, sizeof(value));
                return qFromLittleEndian(value);
            }

            [[nodiscard]] std::int16_t i16(const qsizetype offset) const
            {
                return static_cast<std::int16_t>(u16(offset));
            }

            [[nodiscard]] Q_NEVER_INLINE std::uint32_t u32(const qsizetype offset) const
            {
                require(offset, 4);
                std::uint32_t value = 0;
                std::memcpy(&value, _data.constData() + offset, sizeof(value));
                return qFromLittleEndian(value);
            }

            [[nodiscard]] std::int32_t i32(const qsizetype offset) const
            {
                return static_cast<std::int32_t>(u32(offset));
            }

            [[nodiscard]] float f32(const qsizetype offset) const
            {
                return std::bit_cast<float>(u32(offset));
            }

            [[nodiscard]] QString ascii(const qsizetype offset, const qsizetype maximum = 256) const
            {
                require(offset, 1);
                const qsizetype limit = (std::min)(_data.size(), offset + maximum);
                qsizetype end = offset;
                while (end < limit && _data.at(end) != '\0') {
                    ++end;
                }
                if (end == limit) {
                    throw std::runtime_error("HKX string is not terminated");
                }
                return QString::fromLatin1(_data.constData() + offset, end - offset);
            }

            [[nodiscard]] QByteArray bytes(const qsizetype offset, const qsizetype count) const
            {
                require(offset, count);
                return _data.mid(offset, count);
            }

        private:
            const QByteArray& _data;
        };

        struct Section
        {
            QString name;
            std::uint32_t dataStart{ 0 };
            std::uint32_t localFixups{ 0 };
            std::uint32_t globalFixups{ 0 };
            std::uint32_t virtualFixups{ 0 };
            std::uint32_t exports{ 0 };
            std::uint32_t imports{ 0 };
            std::uint32_t end{ 0 };
        };

        using FixupMap = std::unordered_map<std::uint32_t, std::uint32_t>;

        struct ObjectInfo
        {
            std::uint32_t relativeOffset{ 0 };
            QString className;
        };

        struct Packfile
        {
            Section classNames;
            Section data;
            FixupMap localFixups;
            FixupMap globalFixups;
            QVector<ObjectInfo> objects;
        };

        [[nodiscard]] qsizetype aligned(const qsizetype value, const qsizetype alignment)
        {
            if (alignment <= 1) {
                return value;
            }
            const qsizetype remainder = value % alignment;
            return remainder == 0 ? value : value + alignment - remainder;
        }

        [[nodiscard]] Packfile parsePackfile(const QByteArray& bytes)
        {
            ByteReader reader(bytes);
            if (reader.size() < kFileHeaderSize + 3 * kSectionHeaderSize ||
                reader.u32(0) != kMagic0 || reader.u32(4) != kMagic1) {
                throw std::runtime_error("file is not a Havok packfile");
            }
            if (reader.u32(0x0C) != kPackfileVersion) {
                throw std::runtime_error("HKX packfile version is not Fallout 4 version 11");
            }
            if (reader.u8(0x10) != 8 || reader.u8(0x11) != 1) {
                throw std::runtime_error("HKX is not the little-endian 64-bit Fallout 4 layout");
            }
            const QString contentsVersion = reader.ascii(0x28, 16);
            if (contentsVersion != QStringLiteral("hk_2014.1.0-r1")) {
                throw std::runtime_error("HKX contents version is not hk_2014.1.0-r1");
            }
            const std::uint32_t sectionCount = reader.u32(0x14);
            if (sectionCount < 3 || sectionCount > 16) {
                throw std::runtime_error("HKX section count is invalid");
            }

            const std::uint16_t sectionHeaderPadding = reader.u16(0x3E);
            if (sectionHeaderPadding > 0x100 || (sectionHeaderPadding % 0x10) != 0) {
                throw std::runtime_error("HKX section-header padding is invalid");
            }
            const qsizetype sectionHeaderStart = kFileHeaderSize + sectionHeaderPadding;
            reader.require(sectionHeaderStart,
                           static_cast<qsizetype>(sectionCount) * kSectionHeaderSize);

            QVector<Section> sections;
            sections.reserve(static_cast<qsizetype>(sectionCount));
            for (std::uint32_t index = 0; index < sectionCount; ++index) {
                const qsizetype offset = sectionHeaderStart +
                                         static_cast<qsizetype>(index) * kSectionHeaderSize;
                reader.require(offset, kSectionHeaderSize);
                Section section;
                section.name = reader.ascii(offset, 16);
                section.dataStart = reader.u32(offset + 0x14);
                section.localFixups = reader.u32(offset + 0x18);
                section.globalFixups = reader.u32(offset + 0x1C);
                section.virtualFixups = reader.u32(offset + 0x20);
                section.exports = reader.u32(offset + 0x24);
                section.imports = reader.u32(offset + 0x28);
                section.end = reader.u32(offset + 0x2C);
                if (section.dataStart > static_cast<std::uint32_t>(reader.size()) ||
                    section.localFixups > section.globalFixups ||
                    section.globalFixups > section.virtualFixups ||
                    section.virtualFixups > section.exports ||
                    section.exports > section.imports || section.imports > section.end ||
                    section.end > static_cast<std::uint32_t>(reader.size()) - section.dataStart) {
                    throw std::runtime_error("HKX section offsets are invalid");
                }
                sections.append(section);
            }

            const auto findSectionIndex = [&sections](const QString& name) -> qsizetype {
                const auto iterator = std::find_if(sections.cbegin(), sections.cend(),
                    [&name](const Section& section) { return section.name == name; });
                if (iterator == sections.cend()) {
                    throw std::runtime_error("HKX required section is missing");
                }
                return std::distance(sections.cbegin(), iterator);
            };

            Packfile packfile;
            const qsizetype classNamesIndex = findSectionIndex(QStringLiteral("__classnames__"));
            const qsizetype dataIndex = findSectionIndex(QStringLiteral("__data__"));
            packfile.classNames = sections.at(classNamesIndex);
            packfile.data = sections.at(dataIndex);

            const qsizetype localStart = packfile.data.dataStart + packfile.data.localFixups;
            const qsizetype localEnd = packfile.data.dataStart + packfile.data.globalFixups;
            for (qsizetype offset = localStart; offset + 8 <= localEnd; offset += 8) {
                const std::uint32_t source = reader.u32(offset);
                const std::uint32_t destination = reader.u32(offset + 4);
                if (source == std::numeric_limits<std::uint32_t>::max()) {
                    continue;
                }
                if (source >= packfile.data.localFixups || destination >= packfile.data.localFixups) {
                    throw std::runtime_error("HKX local fixup is outside the data objects");
                }
                packfile.localFixups[source] = destination;
            }

            const qsizetype globalStart = packfile.data.dataStart + packfile.data.globalFixups;
            const qsizetype globalEnd = packfile.data.dataStart + packfile.data.virtualFixups;
            for (qsizetype offset = globalStart; offset + 12 <= globalEnd; offset += 12) {
                const std::uint32_t source = reader.u32(offset);
                const std::uint32_t destinationSection = reader.u32(offset + 4);
                const std::uint32_t destination = reader.u32(offset + 8);
                if (source == std::numeric_limits<std::uint32_t>::max()) {
                    continue;
                }
                if (source >= packfile.data.localFixups || destinationSection >= sectionCount) {
                    throw std::runtime_error("HKX global fixup is invalid");
                }
                if (destinationSection == static_cast<std::uint32_t>(dataIndex)) {
                    if (destination >= packfile.data.localFixups) {
                        throw std::runtime_error("HKX object fixup is outside the data objects");
                    }
                    packfile.globalFixups[source] = destination;
                }
            }

            const qsizetype virtualStart = packfile.data.dataStart + packfile.data.virtualFixups;
            const qsizetype virtualEnd = packfile.data.dataStart + packfile.data.exports;
            for (qsizetype offset = virtualStart; offset + 12 <= virtualEnd; offset += 12) {
                const std::uint32_t objectOffset = reader.u32(offset);
                const std::uint32_t classSection = reader.u32(offset + 4);
                const std::uint32_t classOffset = reader.u32(offset + 8);
                if (objectOffset == std::numeric_limits<std::uint32_t>::max()) {
                    continue;
                }
                if (packfile.objects.size() >= static_cast<qsizetype>(kMaximumObjects) ||
                    objectOffset >= packfile.data.localFixups ||
                    classSection != static_cast<std::uint32_t>(classNamesIndex) ||
                    classOffset >= packfile.classNames.end) {
                    throw std::runtime_error("HKX virtual fixup is invalid");
                }
                packfile.objects.append(ObjectInfo{
                    objectOffset,
                    reader.ascii(packfile.classNames.dataStart + classOffset, 256),
                });
            }
            return packfile;
        }

        [[nodiscard]] std::optional<std::uint32_t> fixedRelative(
            const FixupMap& fixups, const std::uint32_t source)
        {
            const auto iterator = fixups.find(source);
            if (iterator == fixups.end()) {
                return std::nullopt;
            }
            return iterator->second;
        }

        [[nodiscard]] std::uint32_t objectOffset(const Packfile& packfile, const QString& className)
        {
            const auto iterator = std::find_if(packfile.objects.cbegin(), packfile.objects.cend(),
                [&className](const ObjectInfo& object) { return object.className == className; });
            if (iterator == packfile.objects.cend()) {
                throw std::runtime_error("HKX object class is missing");
            }
            return iterator->relativeOffset;
        }

        [[nodiscard]] const ObjectInfo* objectAt(const Packfile& packfile,
                                                 const std::uint32_t relativeOffset)
        {
            const auto iterator = std::find_if(packfile.objects.cbegin(), packfile.objects.cend(),
                [relativeOffset](const ObjectInfo& object) {
                    return object.relativeOffset == relativeOffset;
                });
            return iterator == packfile.objects.cend() ? nullptr : &*iterator;
        }

        [[nodiscard]] QString fixedString(const ByteReader& reader, const Packfile& packfile,
                                          const std::uint32_t owner,
                                          const std::uint32_t fieldOffset)
        {
            const auto relative = fixedRelative(packfile.localFixups, owner + fieldOffset);
            return relative.has_value()
                ? reader.ascii(packfile.data.dataStart + *relative, 512)
                : QString{};
        }

        struct ArrayView
        {
            std::uint32_t relativeOffset{ 0 };
            std::uint32_t count{ 0 };
        };

        [[nodiscard]] ArrayView arrayView(const ByteReader& reader, const Packfile& packfile,
                                          const std::uint32_t owner,
                                          const std::uint32_t fieldOffset,
                                          const std::uint32_t maximum)
        {
            const std::uint32_t field = owner + fieldOffset;
            const std::uint32_t count = reader.u32(packfile.data.dataStart + field + 8);
            if (count > maximum) {
                throw std::runtime_error("HKX array count exceeds the supported limit");
            }
            if (count == 0) {
                return {};
            }
            const auto relative = fixedRelative(packfile.localFixups, field);
            if (!relative.has_value()) {
                throw std::runtime_error("HKX array has no local fixup");
            }
            return ArrayView{ *relative, count };
        }

        [[nodiscard]] QString fixedString(const ByteReader& reader, const Packfile& packfile,
                                          const std::uint32_t source)
        {
            const auto relative = fixedRelative(packfile.localFixups, source);
            if (!relative.has_value()) {
                return {};
            }
            return reader.ascii(packfile.data.dataStart + *relative, 512);
        }

        enum class ValueKind : std::uint8_t
        {
            Identity,
            Static,
            Spline,
        };

        struct ScalarCurve
        {
            ValueKind kind{ ValueKind::Identity };
            float identity{ 0.0f };
            float value{ 0.0f };
            std::uint8_t degree{ 0 };
            QVector<float> knots;
            QVector<float> controls;
        };

        struct QuaternionCurve
        {
            ValueKind kind{ ValueKind::Identity };
            Quaternionf value{};
            std::uint8_t degree{ 0 };
            QVector<float> knots;
            QVector<Quaternionf> controls;
        };

        struct TrackBlock
        {
            std::array<ScalarCurve, 3> translation;
            QuaternionCurve rotation;
            std::array<ScalarCurve, 3> scale;

            TrackBlock()
            {
                for (ScalarCurve& curve : scale) {
                    curve.identity = 1.0f;
                }
            }
        };

        struct TrackMask
        {
            std::uint8_t positionQuantization{ 0 };
            std::uint8_t rotationQuantization{ 0 };
            std::uint8_t scaleQuantization{ 0 };
            std::uint8_t positionFlags{ 0 };
            std::uint8_t rotationFlags{ 0 };
            std::uint8_t scaleFlags{ 0 };

            [[nodiscard]] ValueKind positionKind(const int axis) const noexcept
            {
                if (((positionFlags >> (axis + 4)) & 1U) != 0) {
                    return ValueKind::Spline;
                }
                return ((positionFlags >> axis) & 1U) != 0 ? ValueKind::Static
                                                            : ValueKind::Identity;
            }

            [[nodiscard]] ValueKind rotationKind() const noexcept
            {
                if ((rotationFlags & 0xF0U) != 0) {
                    return ValueKind::Spline;
                }
                return (rotationFlags & 0x0FU) != 0 ? ValueKind::Static
                                                    : ValueKind::Identity;
            }

            [[nodiscard]] ValueKind scaleKind(const int axis) const noexcept
            {
                if (((scaleFlags >> (axis + 4)) & 1U) != 0) {
                    return ValueKind::Spline;
                }
                return ((scaleFlags >> axis) & 1U) != 0 ? ValueKind::Static
                                                        : ValueKind::Identity;
            }
        };

        [[nodiscard]] bool anySpline(const std::array<ValueKind, 3>& kinds)
        {
            return std::any_of(kinds.cbegin(), kinds.cend(),
                [](const ValueKind kind) { return kind == ValueKind::Spline; });
        }

        [[nodiscard]] float readQuantizedScalar(const ByteReader& reader, qsizetype& offset,
                                                const std::uint8_t quantization,
                                                const float minimum, const float maximum)
        {
            float unit = 0.0f;
            if (quantization == 0) {
                unit = static_cast<float>(reader.u8(offset++)) / 255.0f;
            } else {
                unit = static_cast<float>(reader.u16(offset)) / 65535.0f;
                offset += 2;
            }
            return minimum + (maximum - minimum) * unit;
        }

        [[nodiscard]] Quaternionf readQuaternion(const ByteReader& reader, qsizetype& offset,
                                                 const std::uint8_t format)
        {
            Quaternionf result;
            if (format == 0) {
                const std::uint32_t packed = reader.u32(offset);
                offset += 4;
                constexpr float fraction = 1.0f / 1023.0f;
                float radius = static_cast<float>((packed >> 18U) & 0x3FFU) * fraction;
                radius = 1.0f - radius * radius;
                const float phiTheta = static_cast<float>(packed & 0x3FFFFU);
                float phi = std::floor(std::sqrt(phiTheta));
                float theta = 0.0f;
                if (phi > 0.0f) {
                    theta = (std::numbers::pi_v<float> / 4.0f) *
                            (phiTheta - phi * phi) / phi;
                }
                phi = (std::numbers::pi_v<float> / 2.0f / 511.0f) * phi;
                const float magnitude = std::sqrt((std::max)(0.0f, 1.0f - radius * radius));
                result = Quaternionf{
                    std::sin(phi) * std::cos(theta) * magnitude,
                    std::sin(phi) * std::sin(theta) * magnitude,
                    std::cos(phi) * magnitude,
                    radius,
                };
                const std::array<std::uint32_t, 4> signs{
                    0x10000000U, 0x20000000U, 0x40000000U, 0x80000000U
                };
                float* values = &result.x;
                for (int index = 0; index < 4; ++index) {
                    if ((packed & signs[static_cast<std::size_t>(index)]) != 0) {
                        values[index] = -values[index];
                    }
                }
            } else if (format == 1) {
                reader.require(offset, 5);
                std::uint64_t packed = 0;
                for (int index = 0; index < 5; ++index) {
                    packed |= static_cast<std::uint64_t>(reader.u8(offset + index)) << (index * 8U);
                }
                offset += 5;
                constexpr float fraction = 0.0003454357f;
                const std::array<float, 3> values{
                    (static_cast<float>((packed >> 0U) & 0xFFFU) - 2047.0f) * fraction,
                    (static_cast<float>((packed >> 12U) & 0xFFFU) - 2047.0f) * fraction,
                    (static_cast<float>((packed >> 24U) & 0xFFFU) - 2047.0f) * fraction,
                };
                float missing = std::sqrt((std::max)(0.0f,
                    1.0f - values[0] * values[0] - values[1] * values[1] - values[2] * values[2]));
                if (((packed >> 38U) & 1U) != 0) {
                    missing = -missing;
                }
                const int shift = static_cast<int>((packed >> 36U) & 3U);
                std::array<float, 4> output{};
                int source = 0;
                for (int component = 0; component < 4; ++component) {
                    output[static_cast<std::size_t>(component)] = component == shift
                        ? missing
                        : values[static_cast<std::size_t>(source++)];
                }
                result = Quaternionf{ output[0], output[1], output[2], output[3] };
            } else if (format == 2) {
                const std::uint16_t x = reader.u16(offset);
                const std::uint16_t y = reader.u16(offset + 2);
                const std::uint16_t z = reader.u16(offset + 4);
                offset += 6;
                constexpr std::uint16_t mask = 0x7FFFU;
                constexpr float fraction = 0.000043161f;
                const std::array<float, 3> values{
                    (static_cast<float>(x & mask) - static_cast<float>(mask >> 1U)) * fraction,
                    (static_cast<float>(y & mask) - static_cast<float>(mask >> 1U)) * fraction,
                    (static_cast<float>(z & mask) - static_cast<float>(mask >> 1U)) * fraction,
                };
                float missing = std::sqrt((std::max)(0.0f,
                    1.0f - values[0] * values[0] - values[1] * values[1] - values[2] * values[2]));
                if ((z & 0x8000U) != 0) {
                    missing = -missing;
                }
                const int shift = static_cast<int>(((y >> 14U) & 2U) | ((x >> 15U) & 1U));
                std::array<float, 4> output{};
                int source = 0;
                for (int component = 0; component < 4; ++component) {
                    output[static_cast<std::size_t>(component)] = component == shift
                        ? missing
                        : values[static_cast<std::size_t>(source++)];
                }
                result = Quaternionf{ output[0], output[1], output[2], output[3] };
            } else if (format == 5) {
                result = Quaternionf{
                    reader.f32(offset), reader.f32(offset + 4),
                    reader.f32(offset + 8), reader.f32(offset + 12)
                };
                offset += 16;
            } else {
                throw std::runtime_error("HKX uses an unsupported quaternion encoding");
            }
            return normalized(result);
        }

        [[nodiscard]] int quaternionAlignment(const std::uint8_t format)
        {
            switch (format) {
            case 0: return 4;
            case 1: return 1;
            case 2: return 2;
            case 5: return 4;
            default: return 1;
            }
        }

        struct VectorSplineHeader
        {
            std::uint16_t itemCount{ 0 };
            std::uint8_t degree{ 0 };
            QVector<float> knots;
        };

        [[nodiscard]] VectorSplineHeader readSplineHeader(const ByteReader& reader, qsizetype& offset)
        {
            VectorSplineHeader header;
            header.itemCount = reader.u16(offset);
            header.degree = reader.u8(offset + 2);
            if (header.degree > kMaximumSplineDegree) {
                throw std::runtime_error("HKX spline degree is invalid");
            }
            offset += 3;
            const std::uint32_t knotCount = static_cast<std::uint32_t>(header.itemCount) +
                                            header.degree + 2U;
            if (knotCount > 65'536U) {
                throw std::runtime_error("HKX spline knot count is invalid");
            }
            header.knots.reserve(static_cast<qsizetype>(knotCount));
            for (std::uint32_t index = 0; index < knotCount; ++index) {
                header.knots.append(static_cast<float>(reader.u8(offset++)));
            }
            return header;
        }

        void readVectorCurves(const ByteReader& reader, qsizetype& offset,
                              const std::array<ValueKind, 3>& kinds,
                              const std::uint8_t quantization,
                              std::array<ScalarCurve, 3>& curves)
        {
            if (!anySpline(kinds)) {
                for (int axis = 0; axis < 3; ++axis) {
                    ScalarCurve& curve = curves[static_cast<std::size_t>(axis)];
                    curve.kind = kinds[static_cast<std::size_t>(axis)];
                    if (curve.kind == ValueKind::Static) {
                        curve.value = reader.f32(offset);
                        offset += 4;
                    }
                }
                offset = aligned(offset, 4);
                return;
            }

            const VectorSplineHeader header = readSplineHeader(reader, offset);
            offset = aligned(offset, 4);
            std::array<float, 3> minimum{};
            std::array<float, 3> maximum{};
            for (int axis = 0; axis < 3; ++axis) {
                ScalarCurve& curve = curves[static_cast<std::size_t>(axis)];
                curve.kind = kinds[static_cast<std::size_t>(axis)];
                curve.degree = header.degree;
                curve.knots = header.knots;
                if (curve.kind == ValueKind::Spline) {
                    minimum[static_cast<std::size_t>(axis)] = reader.f32(offset);
                    maximum[static_cast<std::size_t>(axis)] = reader.f32(offset + 4);
                    offset += 8;
                } else if (curve.kind == ValueKind::Static) {
                    curve.value = reader.f32(offset);
                    offset += 4;
                }
            }
            const std::uint32_t controlCount = static_cast<std::uint32_t>(header.itemCount) + 1U;
            for (std::uint32_t control = 0; control < controlCount; ++control) {
                for (int axis = 0; axis < 3; ++axis) {
                    ScalarCurve& curve = curves[static_cast<std::size_t>(axis)];
                    if (curve.kind == ValueKind::Spline) {
                        curve.controls.append(readQuantizedScalar(
                            reader, offset, quantization,
                            minimum[static_cast<std::size_t>(axis)],
                            maximum[static_cast<std::size_t>(axis)]));
                    }
                }
            }
            offset = aligned(offset, 4);
        }

        void readRotationCurve(const ByteReader& reader, qsizetype& offset,
                               const ValueKind kind, const std::uint8_t format,
                               QuaternionCurve& curve)
        {
            curve.kind = kind;
            const int alignment = quaternionAlignment(format);
            if (kind == ValueKind::Identity) {
                offset = aligned(offset, 4);
                return;
            }
            if (kind == ValueKind::Static) {
                offset = aligned(offset, alignment);
                curve.value = readQuaternion(reader, offset, format);
                offset = aligned(offset, 4);
                return;
            }

            const VectorSplineHeader header = readSplineHeader(reader, offset);
            curve.degree = header.degree;
            curve.knots = header.knots;
            offset = aligned(offset, alignment);
            const std::uint32_t controlCount = static_cast<std::uint32_t>(header.itemCount) + 1U;
            curve.controls.reserve(static_cast<qsizetype>(controlCount));
            for (std::uint32_t control = 0; control < controlCount; ++control) {
                Quaternionf value = readQuaternion(reader, offset, format);
                if (!curve.controls.isEmpty()) {
                    const Quaternionf& previous = curve.controls.back();
                    const float dot = previous.x * value.x + previous.y * value.y +
                                      previous.z * value.z + previous.w * value.w;
                    if (dot < 0.0f) {
                        value = Quaternionf{ -value.x, -value.y, -value.z, -value.w };
                    }
                }
                curve.controls.append(value);
            }
            offset = aligned(offset, 4);
        }

        [[nodiscard]] int knotSpan(const std::uint8_t degree, const float value,
                                   const int controlCount, const QVector<float>& knots)
        {
            if (controlCount <= 1) {
                return 0;
            }
            if (knots.size() <= controlCount || value >= knots.at(controlCount)) {
                return controlCount - 1;
            }
            int low = degree;
            int high = controlCount;
            int middle = (low + high) / 2;
            for (int iteration = 0; iteration < 100; ++iteration) {
                if (middle + 1 >= knots.size()) {
                    return controlCount - 1;
                }
                if (value < knots.at(middle)) {
                    high = middle;
                } else if (value >= knots.at(middle + 1)) {
                    low = middle;
                } else {
                    break;
                }
                const int next = (low + high) / 2;
                if (next == middle) {
                    break;
                }
                middle = next;
            }
            return middle;
        }

        template <class Control, class Accumulate>
        [[nodiscard]] Control evaluateSpline(const std::uint8_t degree, const float value,
                                             const QVector<float>& knots,
                                             const QVector<Control>& controls,
                                             Control result, Accumulate accumulate)
        {
            if (controls.isEmpty()) {
                return result;
            }
            if (controls.size() == 1) {
                return controls.front();
            }
            const int span = knotSpan(degree, value, controls.size(), knots);
            std::array<float, static_cast<std::size_t>(kMaximumSplineDegree) + 1U> basis{};
            basis[0] = 1.0f;
            for (int order = 1; order <= degree; ++order) {
                for (int index = order - 1; index >= 0; --index) {
                    const int high = span + order - index;
                    const int low = span - index;
                    const float denominator = high < knots.size() && low >= 0
                        ? knots.at(high) - knots.at(low)
                        : 0.0f;
                    const float alpha = std::abs(denominator) > 1.0e-10f
                        ? (value - knots.at(low)) / denominator
                        : 0.0f;
                    const float partial = basis[static_cast<std::size_t>(index)] * alpha;
                    if (index + 1 <= degree) {
                        basis[static_cast<std::size_t>(index + 1)] +=
                            basis[static_cast<std::size_t>(index)] - partial;
                    }
                    basis[static_cast<std::size_t>(index)] = partial;
                }
            }
            for (int index = 0; index <= degree; ++index) {
                const int control = span - index;
                if (control >= 0 && control < controls.size()) {
                    accumulate(result, controls.at(control),
                               basis[static_cast<std::size_t>(index)]);
                }
            }
            return result;
        }

        [[nodiscard]] float sampleScalar(const ScalarCurve& curve, const float frame)
        {
            if (curve.kind == ValueKind::Identity) {
                return curve.identity;
            }
            if (curve.kind == ValueKind::Static) {
                return curve.value;
            }
            return evaluateSpline<float>(curve.degree, frame, curve.knots, curve.controls, 0.0f,
                [](float& result, const float control, const float weight) {
                    result += control * weight;
                });
        }

        [[nodiscard]] Quaternionf sampleQuaternion(const QuaternionCurve& curve, const float frame)
        {
            if (curve.kind == ValueKind::Identity) {
                return {};
            }
            if (curve.kind == ValueKind::Static) {
                return curve.value;
            }
            // Weighted quaternion components need a zero accumulator. The
            // default Quaternionf is identity and would bias every spline.
            const Quaternionf value = evaluateSpline<Quaternionf>(
                curve.degree, frame, curve.knots, curve.controls,
                Quaternionf{ 0.0f, 0.0f, 0.0f, 0.0f },
                [](Quaternionf& result, const Quaternionf& control, const float weight) {
                    result.x += control.x * weight;
                    result.y += control.y * weight;
                    result.z += control.z * weight;
                    result.w += control.w * weight;
                });
            return normalized(value);
        }

        [[nodiscard]] Transformf readInterleavedTransform(
            const ByteReader& reader, const qsizetype offset)
        {
            const Transformf transform{
                Vec3f{
                    reader.f32(offset),
                    reader.f32(offset + 0x04),
                    reader.f32(offset + 0x08),
                },
                normalized(Quaternionf{
                    reader.f32(offset + 0x10),
                    reader.f32(offset + 0x14),
                    reader.f32(offset + 0x18),
                    reader.f32(offset + 0x1C),
                }),
                Vec3f{
                    reader.f32(offset + 0x20),
                    reader.f32(offset + 0x24),
                    reader.f32(offset + 0x28),
                },
            };
            if (!std::isfinite(transform.translation.x) ||
                !std::isfinite(transform.translation.y) ||
                !std::isfinite(transform.translation.z) ||
                !std::isfinite(transform.scale.x) ||
                !std::isfinite(transform.scale.y) ||
                !std::isfinite(transform.scale.z)) {
                throw std::runtime_error("HKX interleaved transform is not finite");
            }
            return transform;
        }

        [[nodiscard]] Transformf interpolateInterleavedTransform(
            const Transformf& first, const Transformf& second, const float alpha)
        {
            const float inverseAlpha = 1.0f - alpha;
            const float dot = first.rotation.x * second.rotation.x +
                              first.rotation.y * second.rotation.y +
                              first.rotation.z * second.rotation.z +
                              first.rotation.w * second.rotation.w;
            const float rotationAlpha = dot < 0.0f ? -alpha : alpha;
            return Transformf{
                Vec3f{
                    first.translation.x * inverseAlpha + second.translation.x * alpha,
                    first.translation.y * inverseAlpha + second.translation.y * alpha,
                    first.translation.z * inverseAlpha + second.translation.z * alpha,
                },
                normalized(Quaternionf{
                    first.rotation.x * inverseAlpha + second.rotation.x * rotationAlpha,
                    first.rotation.y * inverseAlpha + second.rotation.y * rotationAlpha,
                    first.rotation.z * inverseAlpha + second.rotation.z * rotationAlpha,
                    first.rotation.w * inverseAlpha + second.rotation.w * rotationAlpha,
                }),
                Vec3f{
                    first.scale.x * inverseAlpha + second.scale.x * alpha,
                    first.scale.y * inverseAlpha + second.scale.y * alpha,
                    first.scale.z * inverseAlpha + second.scale.z * alpha,
                },
            };
        }
    }

    struct HkxClip::Storage
    {
        enum class Encoding
        {
            SplineCompressed,
            InterleavedUncompressed,
        };

        Encoding encoding{ Encoding::SplineCompressed };
        float blockDuration{ 0.0f };
        float frameDuration{ 0.0f };
        std::uint32_t maximumFramesPerBlock{ 0 };
        QVector<QVector<TrackBlock>> blocks;
        QVector<Transformf> interleavedTransforms;
    };

    int HkxSkeleton::boneIndex(const QString& name) const noexcept
    {
        for (qsizetype index = 0; index < bones.size(); ++index) {
            if (bones.at(index).name.compare(name, Qt::CaseInsensitive) == 0) {
                return static_cast<int>(index);
            }
        }
        return -1;
    }

    bool HkxClip::valid() const noexcept
    {
        if (!_storage || !(durationSeconds > 0.0f) || transformTrackCount == 0 ||
            frameCount < 2) {
            return false;
        }
        if (_storage->encoding == Storage::Encoding::InterleavedUncompressed) {
            const std::uint64_t expected = static_cast<std::uint64_t>(frameCount) *
                                           transformTrackCount;
            return expected == static_cast<std::uint64_t>(
                                   _storage->interleavedTransforms.size());
        }
        return !_storage->blocks.isEmpty();
    }

    void HkxClip::sample(float timeSeconds, QVector<Transformf>& result) const
    {
        result.clear();
        if (!valid()) {
            return;
        }
        timeSeconds = std::clamp(timeSeconds, 0.0f, durationSeconds);
        if (_storage->encoding == Storage::Encoding::InterleavedUncompressed) {
            const float framePosition = timeSeconds >= durationSeconds
                ? static_cast<float>(frameCount - 1U)
                : timeSeconds * static_cast<float>(frameCount - 1U) / durationSeconds;
            const std::uint32_t firstFrame = (std::min)(
                static_cast<std::uint32_t>(std::floor(framePosition)), frameCount - 1U);
            const std::uint32_t secondFrame = (std::min)(firstFrame + 1U, frameCount - 1U);
            const float alpha = secondFrame == firstFrame
                ? 0.0f
                : std::clamp(framePosition - static_cast<float>(firstFrame), 0.0f, 1.0f);
            const qsizetype firstOffset = static_cast<qsizetype>(firstFrame) *
                                          static_cast<qsizetype>(transformTrackCount);
            const qsizetype secondOffset = static_cast<qsizetype>(secondFrame) *
                                           static_cast<qsizetype>(transformTrackCount);
            result.reserve(static_cast<qsizetype>(transformTrackCount));
            for (std::uint32_t track = 0; track < transformTrackCount; ++track) {
                const Transformf& first = _storage->interleavedTransforms.at(
                    firstOffset + static_cast<qsizetype>(track));
                const Transformf& second = _storage->interleavedTransforms.at(
                    secondOffset + static_cast<qsizetype>(track));
                result.append(interpolateInterleavedTransform(first, second, alpha));
            }
            return;
        }

        int blockIndex = 0;
        if (_storage->blockDuration > 0.0f) {
            blockIndex = static_cast<int>(std::floor(timeSeconds / _storage->blockDuration));
        }
        blockIndex = std::clamp(blockIndex, 0, static_cast<int>(_storage->blocks.size()) - 1);
        const float blockStart = static_cast<float>(blockIndex) * _storage->blockDuration;
        const float localTime = (std::max)(0.0f, timeSeconds - blockStart);
        float frame = _storage->frameDuration > 0.0f ? localTime / _storage->frameDuration : 0.0f;
        const std::uint32_t blockFrameStride =
            (std::max)(2U, _storage->maximumFramesPerBlock) - 1U;
        const std::uint32_t firstFrame = static_cast<std::uint32_t>(blockIndex) *
                                         blockFrameStride;
        const std::uint32_t remaining = frameCount > firstFrame ? frameCount - firstFrame : 1U;
        const std::uint32_t framesInBlock = (std::min)(_storage->maximumFramesPerBlock, remaining);
        frame = std::clamp(frame, 0.0f, static_cast<float>((std::max)(1U, framesInBlock) - 1U));

        const QVector<TrackBlock>& tracks = _storage->blocks.at(blockIndex);
        result.reserve(tracks.size());
        for (const TrackBlock& track : tracks) {
            Transformf transform;
            transform.translation = Vec3f{
                sampleScalar(track.translation[0], frame),
                sampleScalar(track.translation[1], frame),
                sampleScalar(track.translation[2], frame),
            };
            transform.rotation = sampleQuaternion(track.rotation, frame);
            transform.scale = Vec3f{
                sampleScalar(track.scale[0], frame),
                sampleScalar(track.scale[1], frame),
                sampleScalar(track.scale[2], frame),
            };
            result.append(transform);
        }
    }

    HkxSkeleton HkxReader::readSkeleton(const QByteArray& data, QString& error)
    {
        error.clear();
        HkxSkeleton skeleton;
        try {
            const ByteReader reader(data);
            const Packfile packfile = parsePackfile(data);
            const std::uint32_t skeletonObject = objectOffset(packfile, QStringLiteral("hkaSkeleton"));
            skeleton.name = fixedString(reader, packfile, skeletonObject + 0x10);
            const ArrayView parents = arrayView(reader, packfile, skeletonObject, 0x18, kMaximumBones);
            const ArrayView bones = arrayView(reader, packfile, skeletonObject, 0x28, kMaximumBones);
            const ArrayView reference = arrayView(reader, packfile, skeletonObject, 0x38, kMaximumBones);
            if (parents.count == 0 || parents.count != bones.count || bones.count != reference.count) {
                throw std::runtime_error("HKX skeleton arrays have different sizes");
            }
            skeleton.bones.reserve(static_cast<qsizetype>(bones.count));
            for (std::uint32_t index = 0; index < bones.count; ++index) {
                const std::uint32_t boneRelative = bones.relativeOffset + index * 0x10U;
                const std::uint32_t poseRelative = reference.relativeOffset + index * 0x30U;
                HkxBone bone;
                bone.name = fixedString(reader, packfile, boneRelative);
                bone.lockTranslation = reader.u8(packfile.data.dataStart + boneRelative + 8) != 0;
                bone.parentIndex = reader.i16(packfile.data.dataStart + parents.relativeOffset + index * 2U);
                bone.referenceLocal = Transformf{
                    Vec3f{
                        reader.f32(packfile.data.dataStart + poseRelative),
                        reader.f32(packfile.data.dataStart + poseRelative + 4),
                        reader.f32(packfile.data.dataStart + poseRelative + 8),
                    },
                    normalized(Quaternionf{
                        reader.f32(packfile.data.dataStart + poseRelative + 0x10),
                        reader.f32(packfile.data.dataStart + poseRelative + 0x14),
                        reader.f32(packfile.data.dataStart + poseRelative + 0x18),
                        reader.f32(packfile.data.dataStart + poseRelative + 0x1C),
                    }),
                    Vec3f{
                        reader.f32(packfile.data.dataStart + poseRelative + 0x20),
                        reader.f32(packfile.data.dataStart + poseRelative + 0x24),
                        reader.f32(packfile.data.dataStart + poseRelative + 0x28),
                    },
                };
                if (bone.name.isEmpty() || bone.parentIndex >= static_cast<std::int16_t>(index)) {
                    throw std::runtime_error("HKX skeleton bone hierarchy is invalid");
                }
                skeleton.bones.append(std::move(bone));
            }
        } catch (const std::exception& exception) {
            error = QString::fromUtf8(exception.what());
            skeleton = {};
        }
        return skeleton;
    }

    HkxClip HkxReader::readClip(const QByteArray& data, QString& error)
    {
        error.clear();
        HkxClip clip;
        try {
            const ByteReader reader(data);
            const Packfile packfile = parsePackfile(data);
            const std::uint32_t binding = objectOffset(packfile, QStringLiteral("hkaAnimationBinding"));
            const auto animationRelative = fixedRelative(packfile.globalFixups, binding + 0x18U);
            if (!animationRelative.has_value()) {
                throw std::runtime_error("HKX animation binding has no animation object");
            }
            const ObjectInfo* animationObject = objectAt(packfile, *animationRelative);
            if (animationObject == nullptr) {
                throw std::runtime_error("HKX animation binding target is invalid");
            }
            const bool splineCompressed = animationObject->className ==
                                          QStringLiteral("hkaSplineCompressedAnimation");
            const bool interleavedUncompressed = animationObject->className ==
                                                 QStringLiteral("hkaInterleavedUncompressedAnimation");
            if (!splineCompressed && !interleavedUncompressed) {
                throw std::runtime_error("HKX animation encoding is not supported");
            }
            const std::uint32_t animation = animationObject->relativeOffset;
            const qsizetype animationAbsolute = packfile.data.dataStart + animation;

            clip.durationSeconds = reader.f32(animationAbsolute + 0x14);
            clip.transformTrackCount = reader.u32(animationAbsolute + 0x18);
            if (!(clip.durationSeconds > 0.0f) || clip.transformTrackCount == 0 ||
                clip.transformTrackCount > kMaximumTracks) {
                throw std::runtime_error("HKX animation metadata is invalid");
            }

            std::uint32_t blockCount{ 0 };
            std::uint32_t maximumFramesPerBlock{ 0 };
            std::uint32_t maskAndQuantizationSize{ 0 };
            float blockDuration{ 0.0f };
            float frameDuration{ 0.0f };
            if (splineCompressed) {
                clip.frameCount = reader.u32(animationAbsolute + 0x38);
                blockCount = reader.u32(animationAbsolute + 0x3C);
                maximumFramesPerBlock = reader.u32(animationAbsolute + 0x40);
                maskAndQuantizationSize = reader.u32(animationAbsolute + 0x44);
                blockDuration = reader.f32(animationAbsolute + 0x48);
                frameDuration = reader.f32(animationAbsolute + 0x50);
                if (clip.frameCount < 2 || blockCount == 0 || blockCount > kMaximumBlocks ||
                    maximumFramesPerBlock == 0 || !(frameDuration > 0.0f)) {
                    throw std::runtime_error("HKX spline animation metadata is invalid");
                }
            }

            clip.skeletonName = fixedString(reader, packfile, binding + 0x10);
            clip.additive = reader.i32(packfile.data.dataStart + binding + 0x50) == 2;
            const ArrayView mapping = arrayView(reader, packfile, binding, 0x20, kMaximumTracks);
            if (mapping.count != 0 && mapping.count != clip.transformTrackCount) {
                throw std::runtime_error("HKX track-to-bone mapping size is invalid");
            }
            clip.trackToBone.reserve(static_cast<qsizetype>(clip.transformTrackCount));
            if (mapping.count == 0) {
                for (std::uint32_t index = 0; index < clip.transformTrackCount; ++index) {
                    clip.trackToBone.append(static_cast<std::int16_t>(index));
                }
            } else {
                for (std::uint32_t index = 0; index < mapping.count; ++index) {
                    clip.trackToBone.append(reader.i16(
                        packfile.data.dataStart + mapping.relativeOffset + index * 2U));
                }
            }

            const ArrayView annotationTracks = arrayView(
                reader, packfile, animation, 0x28, kMaximumAnnotations);
            for (std::uint32_t trackIndex = 0; trackIndex < annotationTracks.count; ++trackIndex) {
                const std::uint32_t track = annotationTracks.relativeOffset + trackIndex * 0x18U;
                const QString trackName = fixedString(reader, packfile, track);
                const ArrayView annotations = arrayView(reader, packfile, track, 0x08, kMaximumAnnotations);
                if (clip.annotations.size() + static_cast<qsizetype>(annotations.count) >
                    static_cast<qsizetype>(kMaximumAnnotations)) {
                    throw std::runtime_error("HKX contains too many annotations");
                }
                for (std::uint32_t annotationIndex = 0; annotationIndex < annotations.count;
                     ++annotationIndex) {
                    const std::uint32_t annotation = annotations.relativeOffset + annotationIndex * 0x10U;
                    clip.annotations.append(HkxAnnotation{
                        reader.f32(packfile.data.dataStart + annotation),
                        trackName,
                        fixedString(reader, packfile, annotation + 8),
                    });
                }
            }
            std::sort(clip.annotations.begin(), clip.annotations.end(),
                [](const HkxAnnotation& left, const HkxAnnotation& right) {
                    return left.timeSeconds < right.timeSeconds;
                });

            clip._storage = QSharedPointer<HkxClip::Storage>::create();
            if (interleavedUncompressed) {
                const ArrayView transforms = arrayView(
                    reader, packfile, animation, 0x38,
                    kMaximumAnimationDataBytes / kInterleavedTransformSize);
                if (transforms.count == 0 ||
                    transforms.count % clip.transformTrackCount != 0) {
                    throw std::runtime_error("HKX interleaved transform array is invalid");
                }
                clip.frameCount = transforms.count / clip.transformTrackCount;
                if (clip.frameCount < 2) {
                    throw std::runtime_error("HKX interleaved animation has too few frames");
                }
                const qsizetype transformDataSize = static_cast<qsizetype>(transforms.count) *
                                                    kInterleavedTransformSize;
                const qsizetype transformDataStart = packfile.data.dataStart +
                                                     transforms.relativeOffset;
                reader.require(transformDataStart, transformDataSize);
                clip._storage->encoding = HkxClip::Storage::Encoding::InterleavedUncompressed;
                clip._storage->interleavedTransforms.reserve(
                    static_cast<qsizetype>(transforms.count));
                for (std::uint32_t index = 0; index < transforms.count; ++index) {
                    clip._storage->interleavedTransforms.append(readInterleavedTransform(
                        reader, transformDataStart +
                                    static_cast<qsizetype>(index) * kInterleavedTransformSize));
                }
                return clip;
            }

            const ArrayView blockOffsets = arrayView(reader, packfile, animation, 0x58, kMaximumBlocks);
            const ArrayView compressedData = arrayView(
                reader, packfile, animation, 0x98, kMaximumAnimationDataBytes);
            if (blockOffsets.count != blockCount || compressedData.count == 0) {
                throw std::runtime_error("HKX compressed animation arrays are invalid");
            }
            QVector<std::uint32_t> offsets;
            offsets.reserve(static_cast<qsizetype>(blockCount));
            for (std::uint32_t index = 0; index < blockCount; ++index) {
                offsets.append(reader.u32(
                    packfile.data.dataStart + blockOffsets.relativeOffset + index * 4U));
            }
            const QByteArray blob = reader.bytes(
                packfile.data.dataStart + compressedData.relativeOffset,
                static_cast<qsizetype>(compressedData.count));
            const ByteReader blobReader(blob);

            clip._storage->blockDuration = blockDuration;
            clip._storage->frameDuration = frameDuration;
            clip._storage->maximumFramesPerBlock = maximumFramesPerBlock;
            clip._storage->blocks.reserve(static_cast<qsizetype>(blockCount));

            for (std::uint32_t blockIndex = 0; blockIndex < blockCount; ++blockIndex) {
                const std::uint32_t blockStart = offsets.at(static_cast<qsizetype>(blockIndex));
                const std::uint32_t blockEnd = blockIndex + 1U < blockCount
                    ? offsets.at(static_cast<qsizetype>(blockIndex + 1U))
                    : compressedData.count;
                if (blockStart >= blockEnd || blockEnd > compressedData.count ||
                    maskAndQuantizationSize < clip.transformTrackCount * 4U ||
                    blockStart > blockEnd - maskAndQuantizationSize) {
                    throw std::runtime_error("HKX compressed block offsets are invalid");
                }
                QVector<TrackMask> masks;
                masks.reserve(static_cast<qsizetype>(clip.transformTrackCount));
                qsizetype maskOffset = blockStart;
                for (std::uint32_t track = 0; track < clip.transformTrackCount; ++track) {
                    const std::uint8_t first = blobReader.u8(maskOffset);
                    masks.append(TrackMask{
                        static_cast<std::uint8_t>(first & 0x03U),
                        static_cast<std::uint8_t>((first >> 2U) & 0x0FU),
                        static_cast<std::uint8_t>((first >> 6U) & 0x03U),
                        blobReader.u8(maskOffset + 1),
                        blobReader.u8(maskOffset + 2),
                        blobReader.u8(maskOffset + 3),
                    });
                    maskOffset += 4;
                }
                qsizetype offset = blockStart + maskAndQuantizationSize;
                QVector<TrackBlock> tracks;
                tracks.reserve(static_cast<qsizetype>(clip.transformTrackCount));
                for (const TrackMask& mask : masks) {
                    TrackBlock track;
                    const std::array<ValueKind, 3> positionKinds{
                        mask.positionKind(0), mask.positionKind(1), mask.positionKind(2)
                    };
                    const std::array<ValueKind, 3> scaleKinds{
                        mask.scaleKind(0), mask.scaleKind(1), mask.scaleKind(2)
                    };
                    readVectorCurves(blobReader, offset, positionKinds,
                                     mask.positionQuantization, track.translation);
                    readRotationCurve(blobReader, offset, mask.rotationKind(),
                                      mask.rotationQuantization, track.rotation);
                    readVectorCurves(blobReader, offset, scaleKinds,
                                     mask.scaleQuantization, track.scale);
                    if (offset > blockEnd) {
                        throw std::runtime_error("HKX compressed track exceeds its block");
                    }
                    tracks.append(std::move(track));
                }
                clip._storage->blocks.append(std::move(tracks));
            }
        } catch (const std::exception& exception) {
            error = QString::fromUtf8(exception.what());
            clip = {};
        }
        return clip;
    }

    HkxBehaviorGraph HkxReader::readBehaviorGraph(const QByteArray& data, QString& error)
    {
        error.clear();
        HkxBehaviorGraph graph;
        try {
            const ByteReader reader(data);
            const Packfile packfile = parsePackfile(data);
            std::unordered_map<std::uint32_t, qsizetype> clipIndices;

            for (const ObjectInfo& object : packfile.objects) {
                if (object.className == QStringLiteral("hkbClipGenerator")) {
                    HkxBehaviorClip clip{
                        fixedString(reader, packfile, object.relativeOffset, 0x38),
                        fixedString(reader, packfile, object.relativeOffset, 0x90),
                    };
                    if (!clip.nodeName.isEmpty() && !clip.animationPath.isEmpty()) {
                        clipIndices.emplace(object.relativeOffset, graph.clips.size());
                        graph.clips.append(std::move(clip));
                    }
                } else if (object.className ==
                           QStringLiteral("hkbBehaviorReferenceGenerator")) {
                    const QString reference = fixedString(
                        reader, packfile, object.relativeOffset, 0x88);
                    if (!reference.isEmpty() &&
                        !graph.behaviorReferences.contains(reference, Qt::CaseInsensitive)) {
                        graph.behaviorReferences.append(reference);
                    }
                }
            }

            const auto stateTransitions = [&reader, &packfile](
                                              const ObjectInfo& state) {
                const auto transitionArrayRelative = fixedRelative(
                    packfile.globalFixups, state.relativeOffset + 0x50U);
                if (!transitionArrayRelative.has_value()) {
                    return ArrayView{};
                }
                const ObjectInfo* transitionArray = objectAt(
                    packfile, *transitionArrayRelative);
                return transitionArray && transitionArray->className ==
                           QStringLiteral("hkbStateMachineTransitionInfoArray")
                    ? arrayView(reader, packfile,
                                transitionArray->relativeOffset, 0x10U, 256U)
                    : ArrayView{};
            };
            const auto transitionDuration = [&reader, &packfile](
                                                const std::uint32_t transition) {
                // hk_2014.1.0-r1 stores TransitionInfo entries with a 0x48
                // stride. The effect is at +0x20; a blending effect stores
                // its duration at +0xA8.
                const auto effectRelative = fixedRelative(
                    packfile.globalFixups, transition + 0x20U);
                if (!effectRelative.has_value()) {
                    return 0.0f;
                }
                const ObjectInfo* effect = objectAt(packfile, *effectRelative);
                if (!effect || effect->className !=
                        QStringLiteral("hkbBlendingTransitionEffect")) {
                    return 0.0f;
                }
                const float duration = reader.f32(
                    packfile.data.dataStart + effect->relativeOffset + 0xA8U);
                return std::isfinite(duration) && duration > 0.0f &&
                        duration <= 10.0f
                    ? duration
                    : 0.0f;
            };
            const auto clipForState = [&graph, &reader, &packfile](
                                          const ObjectInfo& state) {
                QString stateName = fixedString(
                    reader, packfile, state.relativeOffset, 0x60U);
                if (stateName.endsWith(QStringLiteral("iState"),
                                       Qt::CaseInsensitive) &&
                    stateName.size() > 6) {
                    stateName.chop(6);
                } else if (stateName.endsWith(QStringLiteral("_MG"),
                                              Qt::CaseInsensitive) &&
                           stateName.size() > 3) {
                    stateName.chop(3);
                } else {
                    return graph.clips.end();
                }
                return std::find_if(
                    graph.clips.begin(), graph.clips.end(),
                    [&stateName](const HkxBehaviorClip& candidate) {
                        return candidate.nodeName.compare(
                                   stateName, Qt::CaseInsensitive) == 0;
                    });
            };
            const auto stateBlendOutSeconds = [
                                                  &stateTransitions,
                                                  &transitionDuration](
                                                  const ObjectInfo& state) {
                const ArrayView transitions = stateTransitions(state);
                float commonDuration = 0.0f;
                for (std::uint32_t index = 0; index < transitions.count; ++index) {
                    const float duration = transitionDuration(
                        transitions.relativeOffset + index * 0x48U);
                    if (!(duration > 0.0f)) {
                        continue;
                    }
                    if (commonDuration == 0.0f) {
                        commonDuration = duration;
                    } else if (std::abs(commonDuration - duration) > 1.0e-4f) {
                        // Different conditional exits cannot be selected
                        // offline without evaluating the behavior variables.
                        return 0.0f;
                    }
                }
                return commonDuration;
            };

            for (const ObjectInfo& state : packfile.objects) {
                if (state.className != QStringLiteral("hkbStateMachineStateInfo")) {
                    continue;
                }
                const auto clip = clipForState(state);
                if (clip == graph.clips.end()) {
                    continue;
                }
                const float blendOut = stateBlendOutSeconds(state);
                if (blendOut > 0.0f) {
                    clip->blendOutSeconds = blendOut;
                }
            }

            // FO4VR's TransitionInfoArray constructor at 0x14191C6E0 proves
            // the 0x48 entry stride. Correlation against each owning machine's
            // StateInfo IDs identifies +0x34 as the destination state. Resolve
            // incoming effects only inside that machine because IDs are local.
            for (const ObjectInfo& machine : packfile.objects) {
                if (machine.className != QStringLiteral("hkbStateMachine")) {
                    continue;
                }
                const ArrayView stateLinks = arrayView(
                    reader, packfile, machine.relativeOffset, 0xD0U, 4'096U);
                QVector<const ObjectInfo*> states;
                states.reserve(static_cast<qsizetype>(stateLinks.count));
                for (std::uint32_t index = 0; index < stateLinks.count; ++index) {
                    const auto stateRelative = fixedRelative(
                        packfile.globalFixups,
                        stateLinks.relativeOffset + index * 8U);
                    const ObjectInfo* state = stateRelative.has_value()
                        ? objectAt(packfile, *stateRelative)
                        : nullptr;
                    if (state && state->className ==
                                     QStringLiteral("hkbStateMachineStateInfo")) {
                        states.append(state);
                    }
                }
                const auto registerIncomingTransitions = [
                                                             &reader,
                                                             &packfile,
                                                             &states,
                                                             &clipForState,
                                                             &transitionDuration,
                                                             &graph](
                                                             const ArrayView transitions) {
                    for (std::uint32_t index = 0; index < transitions.count; ++index) {
                        const std::uint32_t transition =
                            transitions.relativeOffset + index * 0x48U;
                        const std::int32_t destinationId = reader.i32(
                            packfile.data.dataStart + transition + 0x34U);
                        if (destinationId < 0) {
                            continue;
                        }
                        const auto destination = std::find_if(
                            states.cbegin(), states.cend(),
                            [&reader, &packfile, destinationId](
                                const ObjectInfo* candidate) {
                                return reader.i32(
                                           packfile.data.dataStart +
                                           candidate->relativeOffset + 0x68U) ==
                                       destinationId;
                            });
                        if (destination == states.cend()) {
                            continue;
                        }
                        const auto clip = clipForState(**destination);
                        const float duration = transitionDuration(transition);
                        if (clip == graph.clips.end() || !(duration > 0.0f) ||
                            clip->blendInSeconds < 0.0f) {
                            continue;
                        }
                        if (clip->blendInSeconds == 0.0f) {
                            clip->blendInSeconds = duration;
                        } else if (std::abs(
                                       clip->blendInSeconds - duration) > 1.0e-4f) {
                            // The source condition is not evaluable offline.
                            // Mark the entry as ambiguous instead of choosing
                            // a transition that may not be active.
                            clip->blendInSeconds = -1.0f;
                        }
                    }
                };
                for (const ObjectInfo* sourceState : states) {
                    registerIncomingTransitions(stateTransitions(*sourceState));
                }
                const auto wildcardRelative = fixedRelative(
                    packfile.globalFixups, machine.relativeOffset + 0xE0U);
                const ObjectInfo* wildcard = wildcardRelative.has_value()
                    ? objectAt(packfile, *wildcardRelative)
                    : nullptr;
                if (wildcard && wildcard->className ==
                                    QStringLiteral("hkbStateMachineTransitionInfoArray")) {
                    registerIncomingTransitions(arrayView(
                        reader, packfile, wildcard->relativeOffset,
                        0x10U, 256U));
                }
            }
            for (HkxBehaviorClip& clip : graph.clips) {
                if (clip.blendInSeconds < 0.0f) {
                    clip.blendInSeconds = 0.0f;
                }
            }

            qsizetype bestReadyScore = -1;
            for (const ObjectInfo& object : packfile.objects) {
                if (object.className != QStringLiteral("hkbStateMachine")) {
                    continue;
                }
                const ArrayView states = arrayView(
                    reader, packfile, object.relativeOffset, 0xD0, 4'096U);
                if (states.count == 0) {
                    continue;
                }
                const std::int32_t startStateId = reader.i32(
                    packfile.data.dataStart + object.relativeOffset + 0x80);
                qsizetype readyVariantCount = 0;
                QString startReadyAnimation;
                for (std::uint32_t index = 0; index < states.count; ++index) {
                    const auto stateRelative = fixedRelative(
                        packfile.globalFixups, states.relativeOffset + index * 8U);
                    if (!stateRelative.has_value()) {
                        continue;
                    }
                    const ObjectInfo* state = objectAt(packfile, *stateRelative);
                    if (!state || state->className !=
                                      QStringLiteral("hkbStateMachineStateInfo")) {
                        continue;
                    }
                    const QString stateName = fixedString(
                        reader, packfile, state->relativeOffset, 0x60);
                    const bool readyVariant =
                        stateName == QStringLiteral("WPNIdleReadyA") ||
                        stateName == QStringLiteral("WPNIdleReadyB") ||
                        stateName == QStringLiteral("WPNIdleReadyC") ||
                        stateName == QStringLiteral("WPNIdleReadyD");
                    if (readyVariant) {
                        ++readyVariantCount;
                    }
                    const std::int32_t stateId = reader.i32(
                        packfile.data.dataStart + state->relativeOffset + 0x68);
                    if (readyVariant && stateId == startStateId) {
                        const auto generator = fixedRelative(
                            packfile.globalFixups, state->relativeOffset + 0x58);
                        if (generator.has_value()) {
                            const auto clip = clipIndices.find(*generator);
                            if (clip != clipIndices.end()) {
                                startReadyAnimation = graph.clips.at(clip->second).animationPath;
                            }
                        }
                    }
                }
                if (!startReadyAnimation.isEmpty() && readyVariantCount > bestReadyScore) {
                    bestReadyScore = readyVariantCount;
                    graph.defaultReadyAnimationPath = startReadyAnimation;
                }
            }

            if (graph.defaultReadyAnimationPath.isEmpty()) {
                const auto findReady = [&graph](const QString& name) -> QString {
                    const auto iterator = std::find_if(
                        graph.clips.cbegin(), graph.clips.cend(),
                        [&name](const HkxBehaviorClip& clip) {
                            return clip.nodeName.compare(name, Qt::CaseInsensitive) == 0;
                        });
                    return iterator == graph.clips.cend()
                        ? QString{}
                        : iterator->animationPath;
                };
                graph.defaultReadyAnimationPath = findReady(QStringLiteral("WPNIdleReadyA"));
                if (graph.defaultReadyAnimationPath.isEmpty()) {
                    graph.defaultReadyAnimationPath = findReady(
                        QStringLiteral("WPNIdleReady"));
                }
            }
            if (!graph.valid()) {
                throw std::runtime_error("HKX contains no behavior generators");
            }
        } catch (const std::exception& exception) {
            error = QString::fromUtf8(exception.what());
            graph = {};
        }
        return graph;
    }

    Quaternionf normalized(Quaternionf value) noexcept
    {
        const float lengthSquared = value.x * value.x + value.y * value.y +
                                    value.z * value.z + value.w * value.w;
        if (!(lengthSquared > 1.0e-12f) || !std::isfinite(lengthSquared)) {
            return {};
        }
        const float inverseLength = 1.0f / std::sqrt(lengthSquared);
        value.x *= inverseLength;
        value.y *= inverseLength;
        value.z *= inverseLength;
        value.w *= inverseLength;
        return value;
    }

    Quaternionf multiplied(const Quaternionf& left, const Quaternionf& right) noexcept
    {
        return normalized(Quaternionf{
            left.w * right.x + left.x * right.w + left.y * right.z - left.z * right.y,
            left.w * right.y - left.x * right.z + left.y * right.w + left.z * right.x,
            left.w * right.z + left.x * right.y - left.y * right.x + left.z * right.w,
            left.w * right.w - left.x * right.x - left.y * right.y - left.z * right.z,
        });
    }

    Vec3f rotated(const Quaternionf& rotation, const Vec3f& value) noexcept
    {
        const Quaternionf vector{ value.x, value.y, value.z, 0.0f };
        const Quaternionf inverse{ -rotation.x, -rotation.y, -rotation.z, rotation.w };
        const Quaternionf first{
            rotation.w * vector.x + rotation.y * vector.z - rotation.z * vector.y,
            rotation.w * vector.y - rotation.x * vector.z + rotation.z * vector.x,
            rotation.w * vector.z + rotation.x * vector.y - rotation.y * vector.x,
            -rotation.x * vector.x - rotation.y * vector.y - rotation.z * vector.z,
        };
        const Quaternionf output{
            first.w * inverse.x + first.x * inverse.w + first.y * inverse.z - first.z * inverse.y,
            first.w * inverse.y - first.x * inverse.z + first.y * inverse.w + first.z * inverse.x,
            first.w * inverse.z + first.x * inverse.y - first.y * inverse.x + first.z * inverse.w,
            first.w * inverse.w - first.x * inverse.x - first.y * inverse.y - first.z * inverse.z,
        };
        return Vec3f{ output.x, output.y, output.z };
    }

    Transformf composed(const Transformf& parent, const Transformf& local) noexcept
    {
        const Vec3f scaled{
            local.translation.x * parent.scale.x,
            local.translation.y * parent.scale.y,
            local.translation.z * parent.scale.z,
        };
        const Vec3f translated = rotated(parent.rotation, scaled);
        return Transformf{
            Vec3f{
                parent.translation.x + translated.x,
                parent.translation.y + translated.y,
                parent.translation.z + translated.z,
            },
            multiplied(parent.rotation, local.rotation),
            Vec3f{
                parent.scale.x * local.scale.x,
                parent.scale.y * local.scale.y,
                parent.scale.z * local.scale.z,
            },
        };
    }
}
