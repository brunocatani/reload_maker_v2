// The sound profile of the weapon and reload: the sound catalog (reference
// sounds plus every plugin's sound descriptors), the default cues with the
// source's sounds and timings, the saved profile when there is one, and
// each cue's current descriptor files.
#include "ReloadMakerController.h"
#include "areas/properties/PropertyInputs.h"
#include "areas/sounds/SoundFiles.h"

#include <algorithm>

namespace reloadmaker
{
    QString ReloadMakerController::soundProfileKey() const
    {
        return (_soundProfile.weapon + "|" + _soundProfile.animation).toLower();
    }

    // Prepared with the operating rig. Unapplied edits to the previous profile
    // are kept with the saved profiles before switching; a profile only read
    // from the animation is not kept, so preparing sounds changes nothing.
    void ReloadMakerController::prepareSounds()
    {
        if (!currentWeapon() || !currentPlugin() || !_reloadRecipe || !_simulationRig) {
            // No stale fire sound from the previous weapon.
            endFireLoop();
            _fireSound = {};
            _fireSoundMapping.clear();
            _fireSoundError.clear();
            _fireSoundContext.clear();
            return;
        }
        if (!_soundProfile.weapon.isEmpty() && _soundsDirty) {
            _savedSounds[soundProfileKey()] = _soundProfile;
        }
        _audio.clear();
        _soundError.clear();
        buildSoundCatalog();
        const QString weapon = poseRecordKey(currentWeapon()->formId);
        const QString animation = _reloadRecipe->source["animation"].toString();
        const QString magazine = simulationMagazineAnchor().ownerRecord;
        const QString key = (weapon + "|" + animation).toLower();
        _soundProfile = { weapon, animation, magazine, {} };
        SoundCueSources sources;
        sources.animation = animation;
        if (isBoltActionConfiguration()) {
            QString error;
            sources.cyclePath = _boltCycleSource["animation"].toString();
            sources.cycle = HkxReader::readClip(readAnimationResource(sources.cyclePath, error), error);
        }
        const auto defaults = defaultSoundCues(sources);
        addDefaultSoundCues(sources, defaults);
        if (_savedSounds.contains(key)) {
            _soundProfile = _savedSounds.value(key);
        }
        // Cues are shared by animation, but the exported profile belongs to
        // the currently selected magazine, including anchor fallbacks.
        _soundProfile.weapon = weapon;
        _soundProfile.magazine = magazine;
        // A bolt catch released by hand: profiles saved before this cue existed get it too.
        const bool boltRelease = std::any_of(_soundProfile.cues.cbegin(), _soundProfile.cues.cend(), [](const auto& c) { return c.id == "bolt-release"; });
        if (const auto release = defaults.sounds.value("bolt-release"); !release.isEmpty() && !boltRelease) {
            addSoundCue(sources, "bolt-release", "Bolt catch release", "bolt_release", 0, release);
        }
        resolveSoundDescriptors();
        resolveFireSound();
        cacheSourceSounds(_reloadClip);
        _soundsDirty = false;
        resetSoundTracking();
        refreshSoundsUi();
    }

    // The reference sounds plus every plugin's sound descriptors, by record.
    void ReloadMakerController::buildSoundCatalog()
    {
        _soundDescriptors = _referenceSounds;
        for (const auto& plugin : _plugins) {
            for (const auto& s : plugin.sounds) {
                const auto descriptor = soundDescriptor(plugin, s);
                _soundDescriptors[descriptor.record.toLower()] = descriptor;
            }
        }
        _soundCatalog.clear();
        for (auto it = _soundDescriptors.cbegin(); it != _soundDescriptors.cend(); ++it) {
            SoundCue c;
            c.record = it->record;
            c.editorId = it->editorId;
            c.files = it->files;
            c.attenuationDb = it->attenuationDb;
            _soundCatalog.insert(it.key(), c);
        }
    }

    // The one catalog sound with this editor ID; an ambiguous name is an
    // error and finds nothing.
    SoundCue ReloadMakerController::findSoundByEditorId(const QString& edid)
    {
        SoundCue found;
        if (edid.isEmpty()) {
            return found;
        }
        for (const auto& c : _soundCatalog) {
            if (c.editorId.compare(edid, Qt::CaseInsensitive) != 0) {
                continue;
            }
            if (!found.record.isEmpty()) {
                _soundError = "Ambiguous sound name: " + edid;
                return SoundCue{};
            }
            found = c;
        }
        return found;
    }

    // One cue, enabled when its sound has samples. Its source time comes from
    // the reload clip (or the bolt cycle clip); bolt cycle cues take their
    // movement progress from the cycle's opening or closing curve.
    void ReloadMakerController::addSoundCue(const SoundCueSources& sources, const char* id, const char* label, const char* trigger, float progress,
                                            const QString& edid, bool bolt)
    {
        SoundCue c = findSoundByEditorId(edid);
        c.id = id;
        c.label = label;
        c.trigger = trigger;
        c.progress = progress;
        c.enabled = !c.record.isEmpty() && !c.files.isEmpty();
        const auto& clip = bolt ? sources.cycle : _reloadClip;
        c.sourceAnimation = bolt ? sources.cyclePath : sources.animation;
        for (const auto& a : clip.annotations) {
            if (a.text.compare("SoundPlay." + edid, Qt::CaseInsensitive) == 0) {
                c.sourceSeconds = a.timeSeconds;
                break;
            }
        }
        if (bolt && c.sourceSeconds >= 0 && !_boltCycle.opening.isEmpty()) {
            const bool closing = c.trigger == "closing";
            const auto& curve = closing ? _boltCycle.closing : _boltCycle.opening;
            const float frame = c.sourceSeconds / _boltCycle.duration * (_boltCycle.poses.front().size() - 1) - (closing ? _boltCycle.rear : _boltCycle.first);
            const float f = std::clamp(frame, 0.f, float(curve.size() - 1));
            const int a = int(f);
            const int b = std::min(a + 1, int(curve.size() - 1));
            const float value = curve[a] + (curve[b] - curve[a]) * (f - a);
            c.progress = closing ? 1 - value : value;
        }
        for (auto& path : c.files) {
            path = soundResourcePath(path);
        }
        _soundProfile.cues.append(c);
    }

    // The default sound of each cue, read from the clips' sounds and the
    // catalog's conventional names (rework 14, item 22).
    DefaultSoundCues ReloadMakerController::defaultSoundCues(const SoundCueSources& sources) const
    {
        CueAction action = CueAction::Slide;
        switch (simulationMechanism()) {
        case WeaponMechanism::PistolSlide: action = CueAction::Slide; break;
        case WeaponMechanism::Ar15: action = CueAction::ChargingHandle; break;
        case WeaponMechanism::ReciprocatingBolt: action = CueAction::ReciprocatingBolt; break;
        case WeaponMechanism::M1A: action = CueAction::OperatingHandle; break;
        case WeaponMechanism::BoltAction: action = CueAction::BoltAction; break;
        }
        const auto catalogHas = [this](const QString& editorId) {
            return std::count_if(_soundCatalog.cbegin(), _soundCatalog.cend(), [&](const SoundCue& c) {
                       return c.editorId.compare(editorId, Qt::CaseInsensitive) == 0;
                   }) == 1;
        };
        return detectSoundCues(clipEvents(_reloadClip), clipEvents(sources.cycle), action, catalogHas);
    }

    // The default cues in display order. Bolt actions time their operating
    // cues in the bolt cycle clip.
    void ReloadMakerController::addDefaultSoundCues(const SoundCueSources& sources, const DefaultSoundCues& defaults)
    {
        const bool bolt = isBoltActionConfiguration();
        const auto sound = [&](const char* id) { return defaults.sounds.value(QLatin1String(id)); };
        addSoundCue(sources, "unlock", "Bolt unlock", "opening", .01f, sound("unlock"), bolt);
        addSoundCue(sources, "pull", "Slide / handle pull", "opening", .035f, sound("pull"), bolt);
        addSoundCue(sources, "rear", "Rear stop", "opening", 1, "", bolt);
        addSoundCue(sources, "forward", "Forward return", "closing", .975f, sound("forward"), bolt);
        addSoundCue(sources, "lock", "Bolt lock", "closing", 0, sound("lock"), bolt);
        addSoundCue(sources, "mag-release", "Magazine release", "magazine_release", 0, sound("mag-release"));
        addSoundCue(sources, "mag-out", "Magazine withdrawal", "extraction", .035f, sound("mag-out"));
        addSoundCue(sources, "mag-grab", "Magazine grabbed", "magazine_grab", 0, sound("mag-grab"));
        addSoundCue(sources, "mag-entry", "Magazine entering", "insertion", .02f, sound("mag-entry"));
        addSoundCue(sources, "mag-insert", "Magazine insertion", "insertion", .65f, sound("mag-insert"));
        addSoundCue(sources, "mag-seat", "Magazine seated", "magazine_seated", 1, sound("mag-seat"));
        const auto dryKey = recordIdentity(*currentPlugin(), currentWeapon()->attackFailSound).toLower();
        const auto dry = _soundCatalog.value(dryKey);
        addSoundCue(sources, "dry", "Dry fire", "dry_fire", 0, dry.editorId);
    }

    // Saved cues keep their descriptor identity, not file paths: each cue
    // takes the current winning descriptor's files, so overrides and package
    // versions apply. Enabled cues are cached for playback.
    void ReloadMakerController::resolveSoundDescriptors()
    {
        for (auto& c : _soundProfile.cues) {
            if (c.record.isEmpty()) {
                continue;
            }
            const auto it = _soundCatalog.constFind(c.record.toLower());
            if (it == _soundCatalog.cend()) {
                if (c.enabled) {
                    _soundError = "Missing sound descriptor: " + c.record;
                }
                continue;
            }
            c.files = it->files;
            for (auto& p : c.files) {
                p = soundResourcePath(p);
            }
            c.editorId = it->editorId;
            c.attenuationDb = it->attenuationDb;
            if (c.enabled) {
                (void)cacheSound(c);
            }
        }
    }
}
