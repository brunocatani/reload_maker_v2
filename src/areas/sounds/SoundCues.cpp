// Editing the sound cues: the selected cue, its form (original sound,
// trigger, movement progress, volume, enabled), the preview mute, and
// saving and exporting the profile.
#include "ReloadMakerController.h"

#include <QJsonArray>
#include <algorithm>

namespace reloadmaker
{
    // Selecting a cue shows its form.
    void ReloadMakerController::selectSoundCue(int index)
    {
        if (index == _selectedSoundCue) {
            return;
        }
        _selectedSoundCue = index;
        refreshSoundsUi();
        noteDocumentChange("selected sound cue");
    }

    // The selection stays within the cues; none without any.
    void ReloadMakerController::normalizeSoundCueSelection()
    {
        const auto count = int(_soundProfile.cues.size());
        _selectedSoundCue = count == 0 ? -1 : std::clamp(_selectedSoundCue, 0, count - 1);
    }

    // A different original sound replaces the cue's descriptor; it no longer
    // comes from the source animation. A cue without a sound stays disabled.
    void ReloadMakerController::applySoundCueForm(const SoundCueForm& form)
    {
        const int index = _selectedSoundCue;
        if (index < 0 || index >= _soundProfile.cues.size()) {
            return;
        }
        auto& cue = _soundProfile.cues[index];
        if (cue.record.compare(form.record, Qt::CaseInsensitive) != 0) {
            const auto source = _soundCatalog.value(form.record);
            cue.record = source.record;
            cue.editorId = source.editorId;
            cue.files = source.files;
            cue.attenuationDb = source.attenuationDb;
            cue.sourceSeconds = -1;
            cue.sourceAnimation.clear();
        }
        cue.trigger = form.trigger;
        cue.progress = form.progress;
        cue.gain = form.gain;
        cue.enabled = form.enabled && !cue.record.isEmpty();
        _soundError.clear();
        if (cue.enabled) {
            (void)cacheSound(cue);
        }
        _savedSounds[soundProfileKey()] = _soundProfile;
        _soundsDirty = true;
        resetSoundTracking();
        noteDocumentChange("edited sound cue");
        refreshSoundsUi();
    }

    QJsonObject ReloadMakerController::fireSoundState() const
    {
        QJsonArray layers;
        for (const auto& layer : _fireSound.layers) {
            layers.append(QJsonObject{ { "record", layer.record }, { "editorId", layer.editorId }, { "samples", int(layer.files.size()) } });
        }
        QJsonObject loop;
        if (_fireSound.loop) {
            QJsonArray rates;
            for (const auto& sample : _fireSound.loop->ratesOfFire) {
                rates.append(sample.rpm);
            }
            loop = { { "record", _fireSound.loop->record }, { "editorId", _fireSound.loop->editorId }, { "ratesOfFire", rates } };
        }
        return { { "mapping", _fireSoundMapping }, { "layers", layers }, { "loop", loop }, { "looping", _audio.looping() },
                 { "context", QJsonArray::fromStringList(_fireSoundContext) }, { "error", _fireSoundError } };
    }

    // Muting stops what is playing.
    void ReloadMakerController::setSoundsMuted(bool muted)
    {
        if (muted == _soundsMuted) {
            return;
        }
        _soundsMuted = muted;
        showSoundsMuted();
        if (muted) {
            _audio.stop();
        }
    }

    bool ReloadMakerController::saveSounds()
    {
        if (_soundProfile.weapon.isEmpty()) {
            return false;
        }
        SoundProfile checked;
        QString error;
        if (!readSoundProfile(soundProfileJson(_soundProfile), checked, error)) {
            setStatus(error, true);
            return false;
        }
        _savedSounds[soundProfileKey()] = _soundProfile;
        noteDocumentChange(QStringLiteral("sound profile"));
        _soundsDirty = false;
        refreshSoundsUi();
        setStatus("Sound cues applied to this weapon and reload set.");
        return true;
    }

    QJsonObject ReloadMakerController::soundState() const
    {
        return { { "profile", soundProfileJson(_soundProfile) }, { "audio", _audio.state() },  { "muted", _soundsMuted },
                 { "dirty", _soundsDirty },                      { "error", _soundError },     { "lastCue", _lastSound },
                 { "eventsTriggered", double(_soundEventCount) },
                 { "fire", fireSoundState() } };
    }

    // Only a profile that passes the saved-format checks is exported.
    QJsonObject ReloadMakerController::exportSounds() const
    {
        if (_soundProfile.weapon.isEmpty()) {
            return {};
        }
        SoundProfile checked;
        QString error;
        const auto json = soundProfileJson(_soundProfile);
        return readSoundProfile(json, checked, error) ? json : QJsonObject{};
    }
}
