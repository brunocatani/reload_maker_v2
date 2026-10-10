#pragma once
#include <QJsonObject>
#include <QSet>
#include <QStringList>
#include <QVector>
#include <QHash>
#include <array>
#include <cstdint>
#include <optional>

namespace reloadmaker {
struct SoundCue {
    QString id,label,trigger,record,editorId;
    QStringList files;
    QString sourceAnimation;
    float sourceSeconds{-1},progress{0},gain{1},attenuationDb{0};
    bool enabled{true};
};
struct SoundProfile {
    QString weapon,animation,magazine;
    QVector<SoundCue> cues;
};
// FO4 SNDR descriptor types (CNAM), one CTDA condition and one automatic
// rate-of-fire sample (xEdit wbDefinitionsFO4 SNDR).
enum class SoundDescriptorType { Standard, Compound, AutoWeapon };
struct SoundCondition {
    std::uint8_t type{0}; // operator in bits 5-7, flags (OR, global, ...) below
    std::uint16_t function{0};
    float value{0};
};
struct RateOfFireSample {
    int rpm{0};
    QString file;
};
// A sound descriptor in record identities (children lower case).
struct SoundDescriptor {
    QString record,editorId;
    SoundDescriptorType type{SoundDescriptorType::Standard};
    QStringList files;
    float attenuationDb{0};
    QStringList children;                 // Compound: played together
    QVector<RateOfFireSample> ratesOfFire; // AutoWeapon: loops by rate
    QVector<SoundCondition> conditions;
};
// What one shot plays: every Standard layer once, or an automatic loop.
struct FireSoundPlan {
    QVector<SoundDescriptor> layers;
    std::optional<SoundDescriptor> loop;
};
// Expands a descriptor for the preview, which stands outdoors: a Compound
// plays each child whose conditions hold. IsInInterior (function 300) is the
// only condition evaluated, as false; OR chains, globals and any other
// function make that descriptor unpreviewable rather than guessed.
bool planFireSound(const QHash<QString,SoundDescriptor>& descriptors,const QString& record,FireSoundPlan& plan,QString& error);
// The automatic sample whose rate is nearest `rpm`; null when there is none.
const RateOfFireSample* nearestRateOfFire(const SoundDescriptor& descriptor,float rpm);
// A Sound Keyword Mapping (KSSM) in record identities, lower case: the
// descriptor the engine plays for a shot whose Sound keywords hold all of
// `keywords`. Weapon fire sounds are chosen this way, not from WEAP DNAM.
struct SoundMapping {
    QString record,editorId,descriptor;
    QSet<QString> keywords;
    int loadOrder{0}; // rank of the plugin that defines it; later is higher
};
// The player's first-person context keyword, s_10_Player (Fallout4.esm KYWD
// 051747, Sound type). Every vanilla and Glock 19x player mapping is its NPC
// mapping plus this keyword (local Fallout4.esm and Glock19x.esp, 2026-10-04).
inline constexpr char kPlayerSoundKeyword[]="fallout4.esm:051747";
// The most specific mapping whose keywords all lie in `context`. Equally
// specific mappings resolve to the one defined later in load order: a weapon
// mod that carries a vanilla family keyword (MK18: s_40_RifleIstvan beside
// s_40_MK18) adds its own mapping to replace the vanilla one. That tie rule
// is read from such data, not verified in the engine. Null with `error` when
// none applies or two tie within one plugin.
const SoundMapping* chooseSoundMapping(const QVector<SoundMapping>& mappings,const QSet<QString>& context,QString& error);
QStringList soundTriggers();
void applyMissingSoundDefaults(SoundProfile&,const QHash<QString,SoundCue>& catalog);
QString soundTriggerLabel(const QString& trigger);
QJsonObject soundProfileJson(const SoundProfile& profile);
bool readSoundProfile(const QJsonObject& json,SoundProfile& profile,QString& error);

// Progress is always normalized: opening/extraction increase; closing decreases;
// insertion increases toward seated. Each cue rearms only after real retreat.
class SoundCueGate {
public:
    void reset() { fired_.fill(false); }
    QVector<int> cross(const SoundProfile&,const QString& trigger,float before,float after);
    QVector<int> event(const SoundProfile&,const QString& trigger);
private:
    std::array<bool,64> fired_{};
};
}
