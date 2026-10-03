// The sound profile of the weapon and reload: the sound catalog (reference
// sounds plus every plugin's sound descriptors), the default cues with the
// source's sounds and timings, the saved profile when there is one, and
// each cue's current descriptor files.
#include "ReloadMakerController.h"
#include "areas/sounds/SoundFiles.h"

#include <algorithm>

namespace reloadmaker
{
    QString ReloadMakerController::soundProfileKey() const
    {
        return (_soundProfile.weapon + "|" + _soundProfile.animation).toLower();
    }

    // Prepared with the operating rig. The previous profile is kept with the
    // saved profiles before switching.
    void ReloadMakerController::prepareSounds()
    {
        if (!currentWeapon() || !currentPlugin() || !_reloadRecipe || !_simulationRig) {
            return;
        }
        if (!_soundProfile.weapon.isEmpty()) {
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
        addDefaultSoundCues(sources);
        if (_savedSounds.contains(key)) {
            _soundProfile = _savedSounds.value(key);
            _soundProfile.magazine = magazine;
        }
        const bool boltRelease = std::any_of(_soundProfile.cues.cbegin(), _soundProfile.cues.cend(), [](const auto& c) { return c.id == "bolt-release"; });
        if ((isAr15Configuration() || isSrepConfiguration()) && !boltRelease) {
            QString edid = "WPNRU556BoltRelease";
            if (isSrepConfiguration()) {
                edid = "WPNRU556MagCatch";
            } else if (isMk18Configuration()) {
                edid = mk18SoundVariant("WPNMK18BoltCatchPress", "WPNMK18BoltCatchSlap");
            }
            addSoundCue(sources, "bolt-release", "Bolt catch release", "bolt_release", 0, edid);
        }
        resolveSoundDescriptors();
        cacheSourceSounds(_reloadClip);
        _soundsDirty = false;
        resetSoundTracking();
        refreshSoundsUi();
    }

    // The reference sounds plus every plugin's sound descriptors, by record.
    void ReloadMakerController::buildSoundCatalog()
    {
        _soundCatalog = _referenceSounds;
        for (const auto& plugin : _plugins) {
            for (const auto& s : plugin.sounds) {
                SoundCue c;
                c.record = recordIdentity(plugin, s.formId);
                c.editorId = s.editorId;
                c.files = s.files;
                c.attenuationDb = s.attenuationDb;
                _soundCatalog[c.record.toLower()] = c;
            }
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

    // MK18 reload clips may play the alternate sound instead.
    QString ReloadMakerController::mk18SoundVariant(const QString& original, const QString& alternate) const
    {
        if (isMk18Configuration()) {
            for (const auto& a : _reloadClip.annotations) {
                if (a.text.compare("SoundPlay." + alternate, Qt::CaseInsensitive) == 0) {
                    return alternate;
                }
            }
        }
        return original;
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

    // The default cues in display order, with the source sound each testbed
    // plays for them. P2-08 replaces these weapon checks with detection.
    void ReloadMakerController::addDefaultSoundCues(const SoundCueSources& sources)
    {
        const bool glock = isGlockConfiguration();
        const bool ru = isAr15Configuration();
        const bool m1a = isM1AConfiguration();
        const bool bolt = isBoltActionConfiguration();
        const bool mk18 = isMk18Configuration();
        const bool srep = isSrepConfiguration();
        const bool thanatos = isThanatosConfiguration();
        const bool l118 = isL118Configuration();
        QString prefix;
        if (glock) {
            prefix = "WPNGlock19x";
        } else if (mk18) {
            prefix = "WPNMK18";
        } else if (ru || srep) {
            prefix = "WPNRU556";
        } else if (m1a) {
            prefix = "WPNM1A";
        } else if (thanatos) {
            prefix = "WPNM107";
        } else if (l118) {
            prefix = "L115A3_";
        } else if (bolt) {
            prefix = "C14_";
        }
        const auto pick = [](bool condition, const QString& yes, const QString& no) { return condition ? yes : no; };

        addSoundCue(sources, "unlock", "Bolt unlock", "opening", .01f, pick(bolt, prefix + "BoltUnlatch", ""), bolt);
        QString pull = prefix + (glock ? "SlideBack" : ru || srep ? "Boltopen" : "BoltBack");
        if (mk18) {
            pull = "WPNRifleIstvanReloadBoltOpen";
        }
        addSoundCue(sources, "pull", "Slide / handle pull", "opening", .035f, pull, bolt);
        addSoundCue(sources, "rear", "Rear stop", "opening", 1, "", bolt);
        QString forward = prefix + (glock ? "SlideForward" : ru || srep ? "Boltclose" : thanatos ? "Boltrelease" : "BoltForward");
        if (mk18) {
            forward = "WPNRifleIstvanReloadBoltClose";
        }
        addSoundCue(sources, "forward", "Forward return", "closing", .975f, forward, bolt);
        addSoundCue(sources, "lock", "Bolt lock", "closing", 0, pick(bolt, prefix + "BoltLatch", ""), bolt);
        addSoundCue(sources, "mag-release", "Magazine release", "magazine_release", 0, pick(bolt || thanatos, prefix + "MagRelease", ""));
        addSoundCue(sources, "mag-out", "Magazine withdrawal", "extraction", .035f, mk18SoundVariant(prefix + "MagOut", "WPNMK18MagOut141"));
        QString grab;
        if (srep) {
            grab = "WPNRU556ClothMagIn";
        } else if (mk18) {
            grab = "WPNMK18ClothGrabMag";
        } else if (m1a) {
            grab = prefix + "Grab";
        }
        addSoundCue(sources, "mag-grab", "Magazine grabbed", "magazine_grab", 0, grab);
        QString entry;
        if (srep) {
            entry = "WPNRU556Magin";
        } else if (glock) {
            entry = prefix + "MagIns1";
        } else if (l118) {
            entry = prefix + "ReloadMagIn";
        } else if (bolt || mk18) {
            entry = prefix + "MagInserting";
        } else if (thanatos) {
            entry = prefix + "Magin";
        }
        addSoundCue(sources, "mag-entry", "Magazine entering", "insertion", .02f, entry);
        addSoundCue(sources, "mag-insert", "Magazine insertion", "insertion", .65f, pick(glock, prefix + "MagIns2", ""));
        const QString seat = srep ? QString("WPNRU556MagSlap") : mk18SoundVariant(prefix + (thanatos ? "Magslam" : "MagIn"), "WPNMK18MagIn141");
        addSoundCue(sources, "mag-seat", "Magazine seated", "magazine_seated", 1, seat);
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
