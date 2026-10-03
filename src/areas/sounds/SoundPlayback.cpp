// Playing the sound cues: caching their samples from the package or game
// archives, auditioning a cue, the source animation's own sounds, and the
// simulation's movements and events crossing each cue's trigger.
#include "ReloadMakerController.h"
#include "areas/sounds/SoundFiles.h"

#include <QRandomGenerator>
#include <algorithm>
#include <cmath>

namespace reloadmaker
{
    // Package resources override the game's sound archives.
    bool ReloadMakerController::cacheSound(const SoundCue& cue)
    {
        for (const auto& file : cue.files) {
            const auto path = soundResourcePath(file);
            if (_audio.contains(path)) {
                continue;
            }
            QString error;
            const auto& archive = _packageResources.contains(path) ? _packageResources : _gameSounds;
            const auto bytes = archive.read(path, error);
            if (bytes.isEmpty() || !_audio.cache(path, bytes, error)) {
                _soundError = cue.editorId + ": " + error;
                return false;
            }
        }
        if (cue.files.isEmpty()) {
            _soundError = "This descriptor has no directly previewable samples: " + cue.editorId;
            return false;
        }
        return true;
    }

    // A random sample of the cue at its volume less the descriptor's
    // attenuation. Muted cues still count as played.
    bool ReloadMakerController::playSound(const SoundCue& cue)
    {
        _lastSound = cue.editorId;
        ++_soundEventCount;
        if (_soundsMuted) {
            return true;
        }
        if (cue.files.isEmpty()) {
            _soundError = "No sample for " + cue.editorId;
            return false;
        }
        const auto file = soundResourcePath(cue.files[QRandomGenerator::global()->bounded(int(cue.files.size()))]);
        QString error;
        const float gain = cue.gain * std::pow(10.f, -cue.attenuationDb / 20.f);
        if (!_audio.play(file, gain, error)) {
            _soundError = cue.editorId + ": " + error;
            return false;
        }
        return true;
    }

    bool ReloadMakerController::playSoundCue(int index)
    {
        if (index < 0 || index >= _soundProfile.cues.size()) {
            return false;
        }
        const auto& cue = _soundProfile.cues[index];
        const bool ok = cacheSound(cue) && playSound(cue);
        if (!ok) {
            setStatus(_soundError, true);
        }
        refreshSoundsUi();
        return ok;
    }

    void ReloadMakerController::emitSoundEvent(const QString& event)
    {
        for (const int i : _soundGate.event(_soundProfile, event)) {
            (void)playSound(_soundProfile.cues[i]);
        }
    }

    void ReloadMakerController::resetSoundTracking(bool stop)
    {
        if (stop) {
            _audio.stop();
        }
        _soundGate.reset();
        _soundPrevious = _simulation;
        _soundTracking = true;
    }

    // The source clip's SoundPlay annotations in (from, to].
    void ReloadMakerController::playSourceSounds(const HkxClip& clip, float from, float to)
    {
        if (to < from) {
            return;
        }
        for (const auto& a : clip.annotations) {
            if (!(a.timeSeconds > from && a.timeSeconds <= to && a.text.startsWith("SoundPlay.", Qt::CaseInsensitive))) {
                continue;
            }
            for (const auto& cue : _soundCatalog) {
                if (cue.editorId.compare(a.text.mid(10), Qt::CaseInsensitive) == 0) {
                    (void)playSound(cue);
                    break;
                }
            }
        }
    }

    void ReloadMakerController::cacheSourceSounds(const HkxClip& clip)
    {
        for (const auto& a : clip.annotations) {
            if (!a.text.startsWith("SoundPlay.", Qt::CaseInsensitive)) {
                continue;
            }
            for (const auto& cue : _soundCatalog) {
                if (cue.editorId.compare(a.text.mid(10), Qt::CaseInsensitive) == 0) {
                    (void)cacheSound(cue);
                    break;
                }
            }
        }
    }

    // Compares the simulation with its previous step: the slide movement
    // crosses opening and closing cues, the magazine's rail travel crosses
    // extraction and insertion cues, and state changes emit the event cues.
    // No cue fires here while the fire or source reload clip plays.
    void ReloadMakerController::updateSimulationSounds()
    {
        if (!_simulationActive) {
            return;
        }
        if (!_soundTracking) {
            resetSoundTracking(false);
            return;
        }
        const auto& old = _soundPrevious;
        const auto cross = [&](const QString& event, float a, float b) {
            for (const int i : _soundGate.cross(_soundProfile, event, a, b)) {
                (void)playSound(_soundProfile.cues[i]);
            }
        };
        const auto animated = [](SimulationAction action) { return action == SimulationAction::Fire || action == SimulationAction::Reload; };
        if (!animated(old.action) && !animated(_simulation.action)) {
            if (_simulation.mechanism == WeaponMechanism::Ar15 && old.slideLocked && !_simulation.slideLocked && _simulation.inputProgress() <= .001f) {
                emitSoundEvent("bolt_release");
            }
            cross("opening", old.inputProgress(), _simulation.inputProgress());
            cross("closing", old.inputProgress(), _simulation.inputProgress());
            if (old.magazine.seated() && !_simulation.magazine.seated()) {
                emitSoundEvent("magazine_release");
                if (_simulation.magazine.phase == MagazinePhase::Absent) {
                    cross("extraction", 0, 1);
                }
            }
            if (_magazineRail) {
                const float a = std::clamp(old.magazine.distance / _magazineRail->travel, 0.f, 1.f);
                const float b = std::clamp(_simulation.magazine.distance / _magazineRail->travel, 0.f, 1.f);
                if (_simulation.magazine.phase == MagazinePhase::Outgoing || old.magazine.phase == MagazinePhase::Outgoing) {
                    cross("extraction", a, b);
                }
                if (_simulation.magazine.phase == MagazinePhase::Rail || _simulation.magazine.phase == MagazinePhase::Incoming || _simulation.magazine.seated()) {
                    cross("insertion", 1 - a, 1 - b);
                }
            }
            if (!old.magazine.seated() && _simulation.magazine.seated()) {
                emitSoundEvent("magazine_seated");
            }
            if (!old.magazine.carried() && _simulation.magazine.carried()) {
                emitSoundEvent("magazine_grab");
                _soundGate.reset();
            }
        }
        _soundPrevious = _simulation;
    }
}
