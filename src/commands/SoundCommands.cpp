#include "ReloadMakerController.h"
#include "commands/CommandSupport.h"

namespace reloadmaker
{
    using commands::require;

    void ReloadMakerController::registerSoundCommands()
    {
        _commands.add({ "sound_state", CommandAccess::Read, CommandGate::Always,
                        [this](const QJsonObject&) -> std::optional<QJsonObject> { return soundState(); } });

        _commands.add({ "sound", CommandAccess::Edit, CommandGate::WhenIdle,
                        [this](const QJsonObject& params) -> std::optional<QJsonObject> {
                            soundOperation(params);
                            ++_revision;
                            return soundState();
                        } });
    }

    void ReloadMakerController::soundOperation(const QJsonObject& params)
    {
        const auto operation = params["operation"].toString();
        const auto id = params["id"].toString();
        int index = -1;
        for (int i = 0; i < _soundProfile.cues.size(); ++i) {
            if (_soundProfile.cues[i].id == id) {
                index = i;
            }
        }
        if (operation == "play" && id == "fire") {
            // The game's fire sound is auditioned by id; it is not a cue.
            require(playFireSound(), _fireSoundError.isEmpty() ? _soundError : _fireSoundError);
        } else if (operation == "play") {
            require(playSoundCue(index), _soundError);
        } else if (operation == "stop") {
            _audio.stop();
        } else if (operation == "save") {
            require(saveSounds(), _lastError);
        } else if (operation == "mute") {
            require(params["enabled"].isBool(), "Specify enabled");
            setSoundsMuted(params["enabled"].toBool());
            refreshSoundsUi();
        } else if (operation == "edit") {
            require(index >= 0, "Unknown sound cue");
            editSoundCueFromCommand(index, params);
        } else {
            require(false, "Unknown sound operation");
        }
    }

    // The edited profile goes through the same validation as a loaded one before
    // it replaces the current profile.
    void ReloadMakerController::editSoundCueFromCommand(int index, const QJsonObject& params)
    {
        auto json = soundProfileJson(_soundProfile);
        auto cues = json["cues"].toArray();
        auto cue = cues[index].toObject();
        for (const auto* field : { "trigger", "progress", "gain", "enabled" }) {
            if (params.contains(field)) {
                cue[field] = params[field];
            }
        }
        if (params.contains("record")) {
            const auto key = params["record"].toString().toLower();
            require(key.isEmpty() || _soundCatalog.contains(key), "Sound descriptor is not in the loaded source");
            const auto source = _soundCatalog.value(key);
            cue["record"] = source.record;
            cue["editorId"] = source.editorId;
            cue["files"] = QJsonArray::fromStringList(source.files);
            cue["attenuationDb"] = source.attenuationDb;
            // A replaced descriptor no longer comes from the source animation.
            cue["sourceSeconds"] = -1;
            cue["sourceAnimation"] = "";
        }
        cues[index] = cue;
        json["cues"] = cues;
        SoundProfile next;
        QString error;
        require(readSoundProfile(json, next, error), error);
        _soundProfile = std::move(next);
        _savedSounds[soundProfileKey()] = _soundProfile;
        _soundsDirty = true;
        _soundError.clear();
        if (_soundProfile.cues[index].enabled) {
            (void)cacheSound(_soundProfile.cues[index]);
        }
        resetSoundTracking();
        refreshSoundsUi();
    }
}
