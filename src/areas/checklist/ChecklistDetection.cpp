// Checklist detection and confirmation (rework 02 §4, P2-03). The items come
// from the weapon's preset model; each one is observed from what Maker
// already knows: the learned reload, the operating rig, the magazine, the
// hands and the sounds (P2-04 turns these readings into standalone
// detectors).
//
// An observation's inputs become its fingerprint, so they use only data that
// is the same on every page: preset values, the rig, the learned reload and
// authored data. Evidence that exists only while Operate runs (the magazine
// rail) or after Operate prepared it once (the sound profile) reports that it
// cannot run here, and the item shows its saved confirmation instead of
// flipping to stale when the author moves between pages.
#include "ReloadMakerController.h"
#include "AuthoringProgress.h"
#include "ReloadBundle.h"
#include "ReloadTemplates.h"

#include <QDateTime>
#include <QScopeGuard>
#include <QTimer>
#include <stdexcept>

namespace reloadmaker
{
    namespace
    {
        ChecklistObservation found(const QString& value, const QString& evidence, const QJsonObject& inputs)
        {
            ChecklistObservation observation;
            observation.detected = true;
            observation.value = value;
            observation.evidence = evidence;
            observation.inputs = inputs;
            return observation;
        }

        ChecklistObservation missing(const QString& problem)
        {
            ChecklistObservation observation;
            observation.problem = problem;
            return observation;
        }

        ChecklistObservation unavailable(const QString& why)
        {
            ChecklistObservation observation;
            observation.available = false;
            observation.problem = why;
            return observation;
        }

        ChecklistObservation notApplicable(const QString& why)
        {
            ChecklistObservation observation;
            observation.applicable = false;
            observation.problem = why;
            return observation;
        }

        QString afterShotText(const WeaponModel& model)
        {
            switch (model.fire.afterShot) {
            case AfterShot::AutomaticCycle: return QStringLiteral("The action cycles itself after every shot");
            case AfterShot::ManualCycle: return QStringLiteral("You cycle the ") + model.fire.manualCycleMechanism + QStringLiteral(" by hand between shots");
            case AfterShot::OpenBolt: return QStringLiteral("The bolt stays open between shots");
            }
            return {};
        }

        QString ejectionText(CaseEjection ejection)
        {
            switch (ejection) {
            case CaseEjection::OnFire: return QStringLiteral("Ejected by every shot");
            case CaseEjection::OnManualCycle: return QStringLiteral("Ejected when you pull the action back");
            case CaseEjection::OnOpening: return QStringLiteral("Ejected when the weapon opens");
            case CaseEjection::None: return QStringLiteral("No case to eject");
            }
            return {};
        }

        QString motionText(FireMotion motion)
        {
            switch (motion) {
            case FireMotion::NativeClip: return QStringLiteral("The weapon's own fire animation");
            case FireMotion::ReplayActionStroke: return QStringLiteral("The action stroke, replayed");
            case FireMotion::Nothing: return QStringLiteral("Nothing moves when it fires");
            }
            return {};
        }

        const ChecklistItemStatus* findItem(const QVector<ChecklistItemStatus>& items, const QString& id)
        {
            for (const auto& item : items) {
                if (item.item.id == id) {
                    return &item;
                }
            }
            return nullptr;
        }
    }

    // The assigned preset, else the one Maker detects from the mechanism.
    const WeaponPreset* ReloadMakerController::checklistPreset() const
    {
        if (!currentWeapon()) {
            return nullptr;
        }
        const QString assigned = assignedTemplate();
        const QString id = assigned.isEmpty() ? templateIdentity(simulationMechanism()) : assigned;
        for (const auto& preset : reloadPresets()) {
            if (preset.id == id) {
                return &preset;
            }
        }
        return nullptr;
    }

    // Builds the operating rig from the learned reload when it is missing,
    // as Grab points does, without starting a capture or reporting status.
    void ReloadMakerController::prepareChecklistEvidence()
    {
        if (!currentWeapon() || !configuration() || _loadThread || _reloadCapturing || _reloadCaptureQueued || !_reloadRecipe || _simulationRig ||
            _checklistRigRecipe == _reloadRecipe->id) {
            return;
        }
        QString error;
        if (!buildOperatingRig(error)) {
            _checklistRigRecipe = _reloadRecipe->id;
            _checklistRigError = error;
        }
    }

    QHash<QString, ChecklistObservation> ReloadMakerController::checklistObservations(const WeaponPreset& preset) const
    {
        QHash<QString, ChecklistObservation> o;
        const auto& model = preset.model;
        const bool busy = _loadThread || _reloadCapturing || _reloadCaptureQueued || (_rebuildTimer && _rebuildTimer->isActive());
        const bool loaded = currentWeapon() && configuration() && currentPlugin() && !busy;
        const bool captured = loaded && _reloadRecipe.has_value();
        const bool rig = captured && _simulationRig && _simulationRig->recipeId == _reloadRecipe->id;
        const QString learn = QStringLiteral("Learn the reload in Learned reload first.");
        const QString why = busy ? QStringLiteral("Wait for the weapon and its reload to finish loading.")
                            : !loaded ? QStringLiteral("Load a weapon first.")
                            : !captured ? learn
                            : _checklistRigRecipe == _reloadRecipe->id && !_checklistRigError.isEmpty()
                                ? QStringLiteral("The operating parts could not be prepared: ") + _checklistRigError
                                : QStringLiteral("The operating parts could not be prepared from the learned reload.");
        for (const auto& item : checklistItems(model)) {
            o.insert(item.id, unavailable(why));
        }
        if (!loaded) {
            return o;
        }
        const auto saved = _authoringRoadmaps.value(roadmapKey()).toObject();

        // Learned reload.
        o["weapon.preset"] = found(preset.label,
                                   assignedTemplate().isEmpty() ? QStringLiteral("Maker matched this weapon's mechanism. Confirming assigns the preset to the weapon.")
                                                                : QStringLiteral("Assigned to this weapon."),
                                   { { "template", preset.id } });
        QJsonObject sources;
        if (!captured) {
            o["weapon.reload"] = missing(learn);
        } else {
            const auto animation = _reloadRecipe->source["animation"].toString();
            o["weapon.reload"] = found(QStringLiteral("%1 s · %2 tracks").arg(_reloadRecipe->duration, 0, 'f', 2).arg(_reloadRecipe->tracks.size()),
                                       animation, { { "recipe", _reloadRecipe->id } });
            QString error;
            try {
                sources = sourceEvidence();
            } catch (const std::exception& e) {
                error = QString::fromUtf8(e.what());
            }
            if (sources.isEmpty()) {
                o["weapon.sources"] = missing(error);
            } else {
                QStringList paths;
                for (const auto& value : sources["resources"].toArray()) {
                    paths << value.toObject()["path"].toString();
                }
                o["weapon.sources"] = found(QStringLiteral("%1 source files").arg(paths.size()),
                                            QStringLiteral("Confirming records their bytes, so a replaced model or animation shows as changed.\n") + paths.join('\n'),
                                            sources);
            }
        }

        // Fire behavior comes from the preset; fire modes from the record.
        o["fire.afterShot"] = found(afterShotText(model), QStringLiteral("From the ") + preset.label + QStringLiteral(" preset."), { { "template", preset.id } });
        o["fire.ejection"] = found(ejectionText(model.fire.ejection), QStringLiteral("From the preset."), { { "template", preset.id } });
        o["fire.motion"] = found(motionText(model.fire.motion),
                                 _rifleBoltSource.isEmpty() ? QStringLiteral("From the preset.") : QStringLiteral("Fire clip: ") + _rifleBoltSource,
                                 { { "template", preset.id } });
        for (const auto& m : model.mechanisms) {
            if (model.fire.lockOpenMechanism == m.id) {
                o["action." + m.id + ".catch"] = found(QStringLiteral("Held open after the last shot"),
                                                       QStringLiteral("From the preset; Operate's release action closes it."),
                                                       { { "template", preset.id }, { "mechanism", m.id } });
            }
        }
        const auto ammunition = resolveWeaponAmmunition(*currentPlugin(), *currentWeapon(), *configuration(), _referenceNames);
        o["fire.modes"] = ammunition.valid() ? found(ammunition.automatic ? QStringLiteral("Automatic") : QStringLiteral("Semi-automatic"),
                                                     QStringLiteral("From the weapon record and its receiver."), { { "automatic", ammunition.automatic } })
                                             : missing(ammunition.error);

        // Collision drafts are saved per configuration and need no rig.
        if (captured) {
            const auto drafts = saved.value("collisionDrafts").toObject();
            const auto policy = collisionDraftPolicy(_reloadRecipe->id, sources, saved.value("magazineSetup").toObject());
            const auto full = drafts["full"].toObject();
            const auto empty = drafts["empty"].toObject();
            const bool ready = full["usable"].toBool() && empty["usable"].toBool() && full["policy"] == policy && empty["policy"] == policy;
            if (o.contains("feed.world")) {
                o["feed.world"] = ready ? found(QStringLiteral("Full and empty hulls"), QStringLiteral("Cooked models are checked again when the pack is saved."),
                                                { { "full", full["edit"] }, { "empty", empty["edit"] }, { "policy", policy } })
                                        : missing(QStringLiteral("Capture the full and empty magazines in Magazines, then generate their hulls."));
            }
        }
        if (!rig) {
            return o;
        }

        const auto& r = *_simulationRig;
        const auto& tracks = _reloadRecipe->tracks;
        const auto label = [&](int track) { return track >= 0 && track < tracks.size() ? tracks[track].label : QString(); };
        const bool pistol = simulationMechanism() == WeaponMechanism::PistolSlide;
        QStringList followers;
        for (const int track : r.slideTracks) {
            if (track != r.slide) {
                followers << label(track);
            }
        }
        const QJsonObject roles{ { "operating", label(r.slide) }, { "bolt", label(r.bolt) }, { "magazine", label(r.magazine) },
                                 { "followers", QJsonArray::fromStringList(followers) } };

        // Mechanisms and grab points. The rig carries one hand-operated part
        // and, for the AR-15, the separate bolt it drives.
        const auto grabs = currentGrabLocations();
        for (const auto& m : model.mechanisms) {
            const QString prefix = "action." + m.id;
            if (m.driver == MechanismDriver::Hand) {
                o[prefix + ".part"] = found(label(r.slide), followers.isEmpty() ? QStringLiteral("No followers.") : QStringLiteral("Followers: ") + followers.join(", "), roles);
                o[prefix + ".motion"] = found(QStringLiteral("Open at frame %1 · closed at frame %2").arg(r.openFrame).arg(r.closedFrame),
                                              r.mechanicalSource["animation"].toString(),
                                              { { "setup", r.authoring }, { "source", r.mechanicalSource }, { "open", r.openFrame }, { "closed", r.closedFrame } });
                QStringList names;
                for (const auto& point : grabs.points) {
                    names << point.name;
                }
                o[prefix + ".grab"] = !names.isEmpty() ? found(QStringLiteral("%1 point(s)").arg(names.size()), names.join(", "), { { "points", names.size() } })
                                      : pistol ? found(QStringLiteral("Whole slide"), QStringLiteral("Pistol slides keep the default grab when no point is picked."), {})
                                               : missing(QStringLiteral("Pick where a hand may grab it in Grab points."));
            } else {
                o[prefix + ".follows"] = r.bolt >= 0 ? found(label(r.bolt), QStringLiteral("Moves with ") + label(r.slide) + QStringLiteral(" and holds open on its own catch."),
                                                             { { "bolt", label(r.bolt) }, { "driver", label(r.slide) } })
                                                     : missing(QStringLiteral("No separate bolt was found in the reload."));
            }
            if (model.fire.afterShot == AfterShot::ManualCycle && model.fire.manualCycleMechanism == m.id) {
                const bool cycle = !_boltCycle.opening.isEmpty() && _boltCycle.inputTravel > 0;
                o[prefix + ".cycle"] = cycle ? found(QStringLiteral("Lift, pull, push, lock · ejects at %1%").arg(qRound(_boltCycle.ejectProgress * 100)),
                                                     _boltCycleSource["animation"].toString(),
                                                     { { "source", _boltCycleSource }, { "travel", double(_boltCycle.inputTravel) }, { "eject", double(_boltCycle.ejectProgress) } })
                                       : _simulationActive ? missing(QStringLiteral("No bolt cycle animation was found for this weapon."))
                                                           : unavailable(QStringLiteral("Open Operate once to read the bolt cycle animation."));
            }
        }
        if (o.contains("action.fingers")) {
            const auto fingers = exportInteractionGrips()["operatingFingers"].toArray().size();
            o["action.fingers"] = !_operatingFingerError.isEmpty() ? missing(_operatingFingerError)
                                  : fingers == 2 ? found(QStringLiteral("Both hands"), QStringLiteral("Finger shapes only; the wrist follows your hand."), {})
                                  : pistol ? notApplicable(QStringLiteral("Pistol slides use the default grip."))
                                           : missing(QStringLiteral("Choose operating fingers in Grab points."));
        }

        // Magazine hand fits: both hands, except the pistol's reload hand.
        if (o.contains("feed.fits")) {
            QStringList fitted;
            QString problem;
            for (const auto hand : { PoseHand::Left, PoseHand::Right }) {
                if (hand == PoseHand::Right && pistol) {
                    continue;
                }
                QString error;
                const auto fit = magazineGripFor(poseRecordKey(currentWeapon()->formId), simulationMagazineAnchor(), _partGrips, _magazineGrabGrips,
                                                 _magazineGrabDefaults, error, hand);
                const QString side = hand == PoseHand::Left ? QStringLiteral("Left") : QStringLiteral("Right");
                if (fit) {
                    fitted << side + " · " + fit->pose.name;
                } else if (problem.isEmpty()) {
                    problem = error.isEmpty() ? QStringLiteral("Fit the %1 hand on the magazine in Part grips.").arg(side.toLower()) : error;
                }
            }
            o["feed.fits"] = problem.isEmpty() ? found(fitted.join(", "), QStringLiteral("Saved hand fits for the selected magazine."), {}) : missing(problem);
        }

        // The magazine rail exists while Operate runs.
        if (o.contains("feed.presentation")) {
            if (_simulationActive && _magazineRail && _magazineError.isEmpty()) {
                auto presentation = saved.value("magazineSetup").toObject();
                presentation["fullTrack"] = label(_magazineRail->fullTrack);
                presentation["emptyTrack"] = label(_magazineRail->emptyTrack);
                o["feed.presentation"] = found(QStringLiteral("Full: %1 · empty: %2").arg(label(_magazineRail->fullTrack), label(_magazineRail->emptyTrack)),
                                               saved.contains("magazineSetup") ? QStringLiteral("Your models and payload settings.")
                                                                               : QStringLiteral("Detected from the reload."),
                                               presentation);
                o["feed.guide"] = found(QStringLiteral("%1 units outward").arg(_magazineRail->travel, 0, 'f', 1),
                                        saved.contains("magazineGuide") ? QStringLiteral("Your insertion guide.") : QStringLiteral("Detected from the reload."),
                                        { { "guide", saved.value("magazineGuide") }, { "travel", qRound(_magazineRail->travel * 100) / 100.0 } });
            } else if (_simulationActive) {
                o["feed.presentation"] = missing(_magazineError);
                o["feed.guide"] = missing(_magazineError);
            } else {
                const auto away = QStringLiteral("Checked in Operate, where the magazine moves.");
                o["feed.presentation"] = unavailable(away);
                o["feed.guide"] = unavailable(away);
            }
        }

        // Sounds are prepared in Operate for this weapon and reload.
        const bool sounds = _soundProfile.weapon.compare(poseRecordKey(currentWeapon()->formId), Qt::CaseInsensitive) == 0 &&
                            _soundProfile.animation == _reloadRecipe->source["animation"].toString();
        if (!sounds) {
            const auto away = QStringLiteral("Read when Operate prepares the weapon's sounds.");
            o["fire.dry"] = unavailable(away);
            o["fire.sound"] = unavailable(away);
            o["sounds.reload"] = unavailable(away);
            return o;
        }
        const SoundCue* dry = nullptr;
        int enabled = 0;
        for (const auto& cue : _soundProfile.cues) {
            enabled += cue.enabled;
            if (cue.id == QLatin1String("dry")) {
                dry = &cue;
            }
        }
        o["fire.dry"] = dry && dry->enabled ? found(dry->editorId, dry->record, { { "record", dry->record } })
                                            : missing(QStringLiteral("The weapon record names no dry-fire sound."));
        QStringList fire;
        QJsonArray descriptors;
        for (const auto& layer : _fireSound.layers) {
            fire << layer.editorId;
            descriptors.append(layer.record);
        }
        if (_fireSound.loop) {
            fire << _fireSound.loop->editorId + QStringLiteral(" (automatic loop)");
            descriptors.append(_fireSound.loop->record);
        }
        o["fire.sound"] = !fire.isEmpty() ? found(fire.join(" + "), QStringLiteral("From ") + _fireSoundMapping + QStringLiteral(". The game plays it; packs never include it."),
                                                  { { "mapping", _fireSoundMapping }, { "descriptors", descriptors } })
                                          : missing(_fireSoundError.isEmpty() ? QStringLiteral("No fire sound was found.") : _fireSoundError);
        const auto profile = exportSounds();
        o["sounds.reload"] = !profile.isEmpty() && _soundError.isEmpty()
                                 ? found(QStringLiteral("%1 of %2 cues play").arg(enabled).arg(_soundProfile.cues.size()),
                                         _soundsDirty ? QStringLiteral("Unsaved changes on the Sounds page.") : QStringLiteral("Saved on the Sounds page."), profile)
                                 : missing(_soundError.isEmpty() ? QStringLiteral("Open Sounds and save the reload's cues.") : _soundError);
        return o;
    }

    // Saved confirmations of this configuration. The preset's confirmation is
    // its assignment to the weapon, which is saved on its own.
    QJsonObject ReloadMakerController::checklistConfirmations() const
    {
        auto confirmations = _authoringRoadmaps.value(roadmapKey()).toObject().value("checklist").toObject();
        const QString assigned = assignedTemplate();
        if (!assigned.isEmpty()) {
            QString label = assigned;
            for (const auto& preset : reloadPresets()) {
                if (preset.id == assigned) {
                    label = preset.label;
                }
            }
            confirmations["weapon.preset"] = checklistConfirmation(checklistFingerprint("weapon.preset", { { "template", assigned } }), label, {});
        }
        return confirmations;
    }

    ReloadMakerController::ChecklistView ReloadMakerController::checklistView() const
    {
        ChecklistView view;
        view.preset = checklistPreset();
        if (!view.preset) {
            view.problem = !currentWeapon() ? QStringLiteral("Load a weapon to see its checklist.")
                           : !reloadPresetsError().isEmpty() ? QStringLiteral("Behavior presets are unavailable: ") + reloadPresetsError()
                                                             : QStringLiteral("No behavior preset matches this weapon yet.");
            return view;
        }
        view.items = evaluateChecklist(checklistItems(view.preset->model), checklistObservations(*view.preset), checklistConfirmations());
        view.summary = summarizeChecklist(view.items);
        const bool busy = _loadThread || _reloadCapturing || _reloadCaptureQueued || (_rebuildTimer && _rebuildTimer->isActive());
        view.evaluated = configuration() && !busy && _reloadRecipe && _simulationRig && _simulationRig->recipeId == _reloadRecipe->id;
        return view;
    }

    QJsonObject ReloadMakerController::checklistState() const
    {
        const auto view = checklistView();
        static const char* sections[]{ "learned-reload", "part-grips", "mechanisms", "grab-points", "magazines", "fire" };
        static const char* subsystems[]{ "weapon", "feed", "action", "fire" };
        static const char* owners[]{ "auto", "preset", "you" };
        static const char* states[]{ "needs-you", "detected", "confirmed", "not-applicable" };
        QJsonArray items;
        for (const auto& status : view.items) {
            items.append(QJsonObject{ { "id", status.item.id }, { "section", sections[int(status.item.section)] }, { "subsystem", subsystems[int(status.item.subsystem)] },
                                      { "owner", owners[int(status.item.owner)] }, { "mandatory", status.item.mandatory }, { "title", status.item.title },
                                      { "state", states[int(status.state)] }, { "value", status.value }, { "evidence", status.evidence },
                                      { "message", status.message }, { "stale", status.stale }, { "rechecked", status.rechecked },
                                      { "canConfirm", status.canConfirm }, { "canSkip", status.canSkip }, { "byYou", status.byYou },
                                      { "confirmedAt", status.confirmedAt } });
        }
        const auto& s = view.summary;
        return { { "preset", view.preset ? view.preset->id : QString() }, { "presetLabel", view.preset ? view.preset->label : QString() },
                 { "evaluated", view.evaluated }, { "problem", view.problem }, { "items", items },
                 { "summary", QJsonObject{ { "total", s.total }, { "done", s.done }, { "mandatory", s.mandatory }, { "mandatoryDone", s.mandatoryDone },
                                           { "needsYou", s.needsYou }, { "toConfirm", s.toConfirm }, { "started", s.started }, { "complete", s.complete },
                                           { "progress", weaponProgressName(weaponProgress(s)) } } } };
    }

    // confirm, unconfirm, skip and unskip one item by id; preset assigns a
    // preset by id. Confirming an item whose detector runs only in Operate
    // checks it there and returns to the current builder step.
    QJsonObject ReloadMakerController::checklistCommand(const QJsonObject& request)
    {
        const auto operation = request["operation"].toString("state");
        if (operation == QLatin1String("state")) {
            return checklistState();
        }
        const auto key = roadmapKey();
        if (key.isEmpty() || _loadThread || _reloadCapturing || _reloadCaptureQueued || (_rebuildTimer && _rebuildTimer->isActive())) {
            throw std::runtime_error("Wait for a loaded weapon and its learned reload");
        }
        const auto id = request["id"].toString();
        if (operation == QLatin1String("preset")) {
            (void)roadmapCommand({ { "operation", "template" }, { "id", id } });
            return checklistState();
        }
        auto view = checklistView();
        const auto* status = findItem(view.items, id);
        if (!status) {
            throw std::runtime_error(("Unknown checklist item: " + id).toStdString());
        }
        std::optional<BuilderStep> returnTo;
        const auto restore = qScopeGuard([&] {
            if (returnTo) {
                showBuilderStep(*returnTo);
            }
        });
        if (operation == QLatin1String("confirm") && !status->rechecked && _workspace != OperateWorkspace) {
            returnTo = builderStep();
            setWorkspace(OperateWorkspace);
            view = checklistView();
            status = findItem(view.items, id);
            if (!status) {
                throw std::runtime_error("The checklist changed while checking in Operate");
            }
            if (!status->rechecked) {
                throw std::runtime_error(("Operate could not check it: " + status->message).toStdString());
            }
        }
        auto saved = _authoringRoadmaps.value(key).toObject();
        auto confirmations = saved.value("checklist").toObject();
        const auto now = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        if (operation == QLatin1String("confirm")) {
            if (!status->canConfirm) {
                throw std::runtime_error((status->message.isEmpty() ? QStringLiteral("Nothing to confirm for ") + status->item.title : status->message).toStdString());
            }
            if (id == QLatin1String("weapon.preset")) {
                (void)roadmapCommand({ { "operation", "template" }, { "id", view.preset->id } });
                return checklistState();
            }
            if (id == QLatin1String("weapon.sources")) {
                // Records the bytes the source audit compares later.
                (void)roadmapCommand({ { "operation", "record-sources" } });
                saved = _authoringRoadmaps.value(key).toObject();
                confirmations = saved.value("checklist").toObject();
            }
            confirmations[id] = checklistConfirmation(status->fingerprint, status->value, now);
        } else if (operation == QLatin1String("skip")) {
            if (!status->canSkip) {
                throw std::runtime_error("Only optional items can be marked not applicable");
            }
            confirmations[id] = checklistConfirmation(kChecklistNotApplicable, {}, now);
        } else if (operation == QLatin1String("unconfirm") || operation == QLatin1String("unskip")) {
            if (id == QLatin1String("weapon.preset")) {
                throw std::runtime_error("Choose another preset instead");
            }
            confirmations.remove(id);
        } else {
            throw std::runtime_error("Unknown checklist operation");
        }
        saved["checklist"] = confirmations;
        _authoringRoadmaps[key] = saved;
        ++_revision;
        if (!saveSession()) {
            throw std::runtime_error("Could not save the checklist");
        }
        return checklistState();
    }
}
