// The fire sound. The game, not the pack, plays it for every shot; Operate
// plays the same sound so firing is not silent. Weapons name it through a
// Sound Keyword Mapping (KSSM): the mapping whose keywords match the shot's
// Sound keywords, which are the configured weapon's own plus the player's
// first-person keyword. Parts that add or remove Sound keywords (a
// suppressor's s_20_Silenced, an automatic receiver's s_30_Auto) therefore
// choose it as the game does. Standard and compound descriptors play once per
// shot; an automatic descriptor loops its rate-of-fire sample for the burst.
#include "ReloadMakerController.h"
#include "areas/sounds/SoundFiles.h"

#include <QTimer>
#include <cmath>

namespace reloadmaker
{
    namespace
    {
        SoundCue layerCue(const SoundDescriptor& descriptor)
        {
            SoundCue cue;
            cue.id = "fire";
            cue.label = "Fire";
            cue.trigger = "fire";
            cue.record = descriptor.record;
            cue.editorId = descriptor.editorId;
            cue.attenuationDb = descriptor.attenuationDb;
            for (const auto& file : descriptor.files) {
                cue.files.append(soundResourcePath(file));
            }
            return cue;
        }
    }

    void ReloadMakerController::resolveFireSound()
    {
        endFireLoop();
        _fireSound = {};
        _fireSoundMapping.clear();
        _fireSoundError.clear();
        _fireSoundContext.clear();
        const auto* weapon = currentWeapon();
        if (!weapon || !currentPlugin() || !configuration()) {
            return;
        }
        // Package masters first; the loaded plugins replace them in load order.
        QHash<QString, SoundMapping> mappings;
        for (const auto& mapping : _referenceSoundMappings) {
            mappings.insert(mapping.record, mapping);
        }
        QSet<QString> soundKeywords = _referenceSoundKeywords;
        for (int index = 0; index < _plugins.size(); ++index) {
            const auto& plugin = _plugins[index];
            for (const auto keyword : plugin.soundKeywords) {
                soundKeywords.insert(recordIdentity(plugin, keyword).toLower());
            }
            for (const auto& mapping : plugin.soundMappings) {
                // Loaded plugins rank above every package master; a merged MO2
                // load order ranks each mapping by the plugin that defines it.
                const auto origin = plugin.origins.constFind(mapping.formId);
                const int defining = origin == plugin.origins.cend() ? 0 : int(plugin.sourcePlugins.indexOf(origin->plugin, 0, Qt::CaseInsensitive)) + 1;
                SoundMapping value{ recordIdentity(plugin, mapping.formId).toLower(), mapping.editorId,
                                    recordIdentity(plugin, mapping.descriptor).toLower(), {}, 1000 * (index + 1) + defining };
                for (const auto keyword : mapping.keywords) {
                    value.keywords.insert(recordIdentity(plugin, keyword).toLower());
                }
                mappings.insert(value.record, value);
            }
        }
        // The configured weapon's keywords: its own and what the parts add,
        // less what they remove (the order ammunition uses, too).
        QSet<std::uint32_t> keywords(weapon->keywords.cbegin(), weapon->keywords.cend());
        QSet<std::uint32_t> added;
        QSet<std::uint32_t> removed;
        for (const auto id : configuration()->activeMods()) {
            if (const auto* mod = configuration()->mod(id)) {
                added.unite(QSet<std::uint32_t>(mod->addedKeywords.cbegin(), mod->addedKeywords.cend()));
                removed.unite(QSet<std::uint32_t>(mod->removedKeywords.cbegin(), mod->removedKeywords.cend()));
            }
        }
        keywords.subtract(removed);
        keywords.unite(added);
        QSet<QString> context{ QString::fromLatin1(kPlayerSoundKeyword) };
        for (const auto keyword : keywords) {
            const auto identity = recordIdentity(*currentPlugin(), keyword).toLower();
            if (soundKeywords.contains(identity)) {
                context.insert(identity);
            }
        }
        _fireSoundContext = QStringList(context.cbegin(), context.cend());
        _fireSoundContext.sort();
        const auto all = mappings.values();
        const auto* chosen = chooseSoundMapping(all, context, _fireSoundError);
        if (!chosen) {
            return;
        }
        FireSoundPlan plan;
        if (!planFireSound(_soundDescriptors, chosen->descriptor, plan, _fireSoundError)) {
            _fireSoundError = chosen->editorId + ": " + _fireSoundError;
            return;
        }
        // Every sample is cached now so a shot never reads an archive.
        const QString previous = _soundError;
        bool cached = true;
        for (const auto& layer : plan.layers) {
            cached = cached && cacheSound(layerCue(layer));
        }
        if (plan.loop) {
            SoundCue loop = layerCue(*plan.loop);
            for (const auto& sample : plan.loop->ratesOfFire) {
                loop.files.append(soundResourcePath(sample.file));
            }
            cached = cached && cacheSound(loop);
        }
        if (!cached) {
            _fireSoundError = _soundError;
            _soundError = previous;
            return;
        }
        _fireSound = plan;
        _fireSoundMapping = chosen->editorId;
    }

    // One shot: each layer once, or the automatic loop at the simulated rate.
    void ReloadMakerController::playShotSound()
    {
        for (const auto& layer : _fireSound.layers) {
            (void)playSound(layerCue(layer));
        }
        if (!_fireSound.loop || _audio.looping()) {
            return;
        }
        const float rpm = _simulation.fireCycleSeconds > .01f ? 60.f / _simulation.fireCycleSeconds : 600.f;
        const auto* sample = nearestRateOfFire(*_fireSound.loop, rpm);
        _lastSound = _fireSound.loop->editorId;
        ++_soundEventCount;
        if (!sample || _soundsMuted) {
            return;
        }
        const auto file = soundResourcePath(sample->file);
        QString error;
        const float gain = std::pow(10.f, -_fireSound.loop->attenuationDb / 20.f);
        if (!_audio.loop(file, gain, error)) {
            _soundError = _fireSound.loop->editorId + ": " + error;
        }
    }

    void ReloadMakerController::endFireLoop()
    {
        _audio.stopLoop();
    }

    bool ReloadMakerController::playFireSound()
    {
        if (_fireSound.layers.isEmpty() && !_fireSound.loop) {
            setStatus(_fireSoundError.isEmpty() ? QStringLiteral("Prepare a weapon in Operate to find its fire sound.") : _fireSoundError, true);
            return false;
        }
        endFireLoop();
        const QString previous = _soundError;
        _soundError.clear();
        playShotSound();
        // An audition of an automatic sound plays one second of the loop.
        if (_audio.looping()) {
            QTimer::singleShot(1000, this, [this] { endFireLoop(); });
        }
        const bool ok = _soundError.isEmpty();
        if (!ok) {
            setStatus(_soundError, true);
        } else {
            _soundError = previous;
        }
        refreshSoundsUi();
        return ok;
    }
}
