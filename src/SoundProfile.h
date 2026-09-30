#pragma once
#include <QJsonObject>
#include <QStringList>
#include <QVector>
#include <array>

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
QStringList soundTriggers();
QString soundTriggerLabel(const QString& trigger);
QJsonObject soundProfileJson(const SoundProfile& profile);
bool readSoundProfile(const QJsonObject& json,SoundProfile& profile,QString& error);

// Progress is always normalized: opening/extraction increase; closing decreases;
// insertion increases toward seated. Each cue rearms only after real retreat.
class SoundCueGate {
public:
    void reset() { fired_.fill(false); }
    QVector<int> cross(const SoundProfile&,const QString& trigger,float before,float after);
    QVector<int> event(const SoundProfile&,const QString& trigger) const;
private:
    std::array<bool,64> fired_{};
};
}
