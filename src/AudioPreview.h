#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <memory>

namespace reloadmaker {
// UI-thread owner. XAudio owns its device threads; no application callbacks retain
// controller or scene pointers. Voices keep their PCM storage until destroyed.
class AudioPreview {
public:
    AudioPreview();
    ~AudioPreview();
    AudioPreview(const AudioPreview&)=delete;
    AudioPreview& operator=(const AudioPreview&)=delete;
    bool cache(const QString& key,const QByteArray& wave,QString& error);
    bool play(const QString& key,float gain,QString& error);
    bool contains(const QString& key) const;
    void stop();
    void clear();
    QJsonObject state() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
