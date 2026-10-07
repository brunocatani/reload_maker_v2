// The ESP-owned working project. Every authored weapon belongs to the plugin
// that defines its record; where the source was loaded from (a testbed, a
// development package, MO2) is metadata. Entering one of a plugin's weapons
// loads that ESP's latest committed version once. Switching between its
// weapons keeps each weapon's unsaved edits in memory; nothing is written.
// A deliberate Save captures the edited weapons' configurations, then the
// bundled tool commits every weapon of the ESP as one new dated version
// (tools/product_library.py owns the storage format).
#include "ReloadMakerController.h"
#include "areas/project/EspLibrary.h"
#include "commands/CommandSupport.h"
#include "domain/checklist/Checklist.h"
#include "domain/presets/WeaponPresets.h"
#include "StudioPaths.h"
#include "StudioWindow.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMessageBox>
#include <QScopeGuard>
#include <QSettings>
#include <QTimer>
#include <QUuid>

#include <utility>

namespace reloadmaker
{
    using commands::require;

    namespace
    {
        constexpr qint64 kMaximumProjectBytes = 64 * 1024 * 1024;
        constexpr int kPollMilliseconds = 15;

        QString displayName(const QJsonObject& record, const QString& fallback)
        {
            const auto name = record["weaponName"].toString();
            return name.isEmpty() ? fallback : name;
        }

        WeaponProgress statusProgress(const QJsonObject& status)
        {
            QVector<WeaponProgress> configurations;
            for (const auto& value : status["configurations"].toObject()) {
                WeaponProgress progress{};
                if (parseWeaponProgress(value.toObject()["progress"].toString(), progress)) {
                    configurations.append(progress);
                }
            }
            return combineWeaponProgress(configurations);
        }
    }

    QString ReloadMakerController::weaponRecordKey() const
    {
        return currentWeapon() ? poseRecordKey(currentWeapon()->formId).toLower() : QString();
    }

    // A save, or a deletion of a saved version, is still running.
    bool ReloadMakerController::espBusy() const
    {
        return _espSave.has_value() || (_packProcess && _packJob["operation"] == "library-delete");
    }

    QJsonObject ReloadMakerController::espIndex(const QString& plugin) const
    {
        const auto key = espProjectKey(plugin);
        if (key.isEmpty()) {
            return {};
        }
        const auto cached = _espIndexCache.constFind(key);
        if (cached != _espIndexCache.cend()) {
            return *cached;
        }
        // A damaged index reads as no saved versions here; opening the ESP
        // reports the damage (openWeaponAuthoring).
        QString error;
        const auto index = readEspProject(plugin, error);
        _espIndexCache.insert(key, index);
        return index;
    }

    // ---- Entering and switching weapons ----

    // The user's weapon change. A weapon of the same ESP keeps the current
    // weapon's edits in memory; another ESP asks about unsaved edits first.
    bool ReloadMakerController::changeWeapon(int index)
    {
        if (_loadThread || _reloadCapturing || _packProcess || _saveAll || espBusy()) {
            setStatus("Finish the current load, capture, save or pack job before changing weapons.", true);
            return false;
        }
        const auto next = selectionRecordKey(index);
        if (currentWeapon()) {
            if (_esp && espProjectKey(next.section(':', 0, 0)) == espProjectKey(_esp->plugin)) {
                stashDisplayedWeapon();
            } else if (!leaveDocument(QStringLiteral("changing weapons"), [this, index] { selectWeapon(index); })) {
                return false;
            }
        }
        _selectedWeapon = validWeaponIndex(index);
        alignPluginToWeapon();
        refreshWeaponPicker();
        openWeaponAuthoring();
        return true;
    }

    bool ReloadMakerController::showWorkingWeapon(const QString& record, bool stash)
    {
        for (int i = 0; i < _weaponSelections.size(); ++i) {
            if (selectionRecordKey(i) != record) {
                continue;
            }
            if (stash) {
                stashDisplayedWeapon();
            }
            _selectedWeapon = i;
            alignPluginToWeapon();
            refreshWeaponPicker();
            openWeaponAuthoring();
            return true;
        }
        return false;
    }

    // Shows the selected weapon's authoring: its unsaved edits when it was
    // already visited, else the committed version's record, else a new weapon.
    void ReloadMakerController::openWeaponAuthoring()
    {
        const auto* weapon = currentWeapon();
        if (!weapon) {
            return;
        }
        const QString spelled = poseRecordKey(weapon->formId);
        const QString record = spelled.toLower();
        const QString plugin = spelled.section(':', 0, 0);
        if (espProjectKey(plugin).isEmpty()) {
            setStatus("This weapon's originating plugin cannot own an ESP project.", true);
            return;
        }
        if (!_esp || espProjectKey(_esp->plugin) != espProjectKey(plugin)) {
            EspWork work;
            work.plugin = plugin;
            QString damaged;
            const auto index = readEspProject(plugin, damaged);
            _espIndexCache.insert(espProjectKey(plugin), index);
            if (!damaged.isEmpty()) {
                setStatus("The saved versions of " + plugin + " cannot be read (" + damaged + "). They are left unchanged.", true);
            }
            if (!index.isEmpty()) {
                work.plugin = index["plugin"].toString(plugin);
            }
            work.base = index["latest"].toString();
            if (!work.base.isEmpty()) {
                QString error;
                work.snapshot = readEspSnapshot(plugin, work.base, error);
                if (!error.isEmpty()) {
                    setStatus("Cannot read the latest saved version of " + plugin + ": " + error, true);
                    work.base.clear();
                    work.snapshot = {};
                }
            }
            _esp = std::move(work);
        }
        auto& work = _esp->weapons[record];
        if (!work.record.isEmpty()) {
            restoreWeaponRecord(work.record, work.committed);
            setStatus(QStringLiteral("%1 · unsaved edits kept").arg(weapon->name.isEmpty() ? weapon->editorId : weapon->name));
            return;
        }
        const auto entry = _esp->snapshot["weapons"].toObject()[record].toObject();
        work.status = entry["status"].toObject();
        QString error;
        QJsonObject saved;
        if (entry.contains("authoring")) {
            saved = readEspRecord(_esp->plugin, entry["authoring"].toString(), error);
            if (error.isEmpty() && !validWeaponRecord(saved, error)) {
                saved = {};
            }
        }
        if (!saved.isEmpty()) {
            restoreWeaponRecord(saved, {});
            setStatus(QStringLiteral("%1 · saved %2").arg(weapon->name.isEmpty() ? weapon->editorId : weapon->name,
                                                          QDateTime::fromString(_esp->snapshot["savedAt"].toString(), Qt::ISODate).toLocalTime().toString("yyyy-MM-dd HH:mm:ss")));
        } else {
            // A new weapon starts without another weapon's authoring.
            restoreProjectAuthoring({});
            refreshAttachments();
            refreshHeader();
            scheduleRebuild();
            markDocumentClean();
            if (!error.isEmpty()) {
                work.unreadable = true;
                setStatus("The saved authoring of this weapon could not be read (" + error +
                              "). The library keeps it unchanged and Save will not replace it.",
                          true);
            }
        }
        work.committed = _savedAuthoring;
        rememberLastSession();
    }

    // The record restores everything; `baseline` is what counts as unsaved,
    // or the restored state itself for a committed record.
    void ReloadMakerController::restoreWeaponRecord(const QJsonObject& record, const QJsonObject& baseline)
    {
        const auto clean = qScopeGuard([this, &baseline] {
            _savedAuthoring = baseline.isEmpty() ? authoredRecord() : baseline;
            refreshDocumentModified();
        });
        restoreProjectLoadout(record);
        restoreProjectAuthoring(record);
        // Loadout restoration paints the header before the saved preset exists.
        refreshHeader();
        rebuildWeapon();
        if (!_clip.valid()) {
            return;
        }
        QString error;
        restoreProjectPose(record, error);
        restoreProjectView(record);
        if (!error.isEmpty()) {
            setStatus("Weapon restored with a grip warning: " + error, true);
        }
    }

    // Keeps the weapon on screen in memory before another one is shown.
    void ReloadMakerController::stashDisplayedWeapon()
    {
        if (!_esp || !currentWeapon() || _loadThread) {
            return;
        }
        const auto record = weaponRecordKey();
        if (espProjectKey(record.section(':', 0, 0)) != espProjectKey(_esp->plugin)) {
            return;
        }
        auto& work = _esp->weapons[record];
        rememberMagazineGrip();
        auto value = projectRecord();
        QString message;
        if (!appendProjectDrafts(value, message)) {
            appendDiagnostic("PROJECT: an unfinished draft could not be kept in memory: " + message);
        }
        work.record = value;
        work.committed = _savedAuthoring;
        work.dirty = authoredPart(value) != _savedAuthoring;
        work.status = mergedWeaponStatus(work.status);
    }

    QStringList ReloadMakerController::espDirtyWeapons() const
    {
        QStringList names;
        if (!_esp) {
            return names;
        }
        const auto displayed = weaponRecordKey();
        if (currentWeapon() && !_loadThread && espProjectKey(displayed.section(':', 0, 0)) == espProjectKey(_esp->plugin) && documentDirty()) {
            names.append(currentWeapon()->name.isEmpty() ? currentWeapon()->editorId : currentWeapon()->name);
        }
        for (auto it = _esp->weapons.cbegin(); it != _esp->weapons.cend(); ++it) {
            if (it.key() != displayed && it->dirty) {
                names.append(displayName(it->record, it.key()));
            }
        }
        return names;
    }

    bool ReloadMakerController::espDirty() const
    {
        return !espDirtyWeapons().isEmpty();
    }

    // The checklist evaluation of the configuration on screen joins the
    // weapon's per-configuration progress.
    QJsonObject ReloadMakerController::mergedWeaponStatus(QJsonObject status) const
    {
        const auto view = checklistView();
        if (!view.preset || !currentWeapon()) {
            return status;
        }
        auto configurations = status["configurations"].toObject();
        const QString configuration = authoringRecordKey().section('/', 1);
        if (!view.evaluated && configurations[configuration].toObject()["evaluated"].toBool()) {
            return status;
        }
        const auto& s = view.summary;
        configurations[configuration] = QJsonObject{ { "progress", weaponProgressName(weaponProgress(s)) },
                                                     { "evaluated", view.evaluated },
                                                     { "done", s.done },
                                                     { "total", s.total },
                                                     { "mandatory", s.mandatory },
                                                     { "mandatoryDone", s.mandatoryDone },
                                                     { "preset", view.preset->id },
                                                     { "at", QDateTime::currentDateTimeUtc().toString(Qt::ISODate) } };
        status = QJsonObject{ { "format", "reload-maker-weapon-status" },
                              { "version", 1 },
                              { "weapon", poseRecordKey(currentWeapon()->formId) },
                              { "configurations", configurations } };
        status["progress"] = weaponProgressName(statusProgress(status));
        return status;
    }

    QHash<QString, WeaponProgress> ReloadMakerController::weaponProgressIndex() const
    {
        QHash<QString, WeaponProgress> index;
        QSet<QString> plugins;
        for (int i = 0; i < _weaponSelections.size(); ++i) {
            plugins.insert(selectionRecordKey(i).section(':', 0, 0));
        }
        for (const auto& plugin : plugins) {
            const auto project = espIndex(plugin);
            const auto latest = project["latest"].toString();
            for (const auto& value : project["saves"].toArray()) {
                const auto save = value.toObject();
                if (save["id"].toString() != latest) {
                    continue;
                }
                for (const auto& row : save["weapons"].toArray()) {
                    const auto weapon = row.toObject();
                    WeaponProgress progress = WeaponProgress::Draft;
                    if (!weapon.contains("progress") || !parseWeaponProgress(weapon["progress"].toString(), progress)) {
                        progress = WeaponProgress::Draft;
                    }
                    index.insert(weapon["record"].toString().toLower(), progress);
                }
            }
        }
        if (_esp) {
            for (auto it = _esp->weapons.cbegin(); it != _esp->weapons.cend(); ++it) {
                if (!it->status.isEmpty()) {
                    index.insert(it.key(), statusProgress(it->status));
                }
            }
        }
        return index;
    }

    // ---- Leaving the working project ----

    bool ReloadMakerController::leaveDocument(const QString& action, std::function<void()> retry)
    {
        rememberMagazineGrip();
        if (!espDirty() || qApp->property("unattended").toBool()) {
            return true;
        }
        if (espBusy()) {
            setStatus("Wait for the current save to finish.", true);
            return false;
        }
        const auto names = espDirtyWeapons();
        QMessageBox question(QMessageBox::Question, QStringLiteral("Unsaved changes"),
                             QStringLiteral("Save the changes to %1 (%2) before %3?").arg(_esp->plugin, names.join(QStringLiteral(", ")), action),
                             QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, _window);
        question.setDefaultButton(QMessageBox::Save);
        switch (question.exec()) {
        case QMessageBox::Save:
            try {
                startEspSave({});
                _afterSave = std::move(retry);
            } catch (const std::exception& error) {
                setStatus(QString::fromUtf8(error.what()), true);
            }
            return false;
        case QMessageBox::Discard:
            return true;
        default:
            return false;
        }
    }

    // ---- Saving ----

    QJsonObject ReloadMakerController::startEspSave(const QJsonObject& request)
    {
        require(!_loadThread && !_reloadCapturing && !_reloadCaptureQueued, "Wait for the source to finish loading before saving");
        require(!_packProcess && !_saveAll && !espBusy(), "Wait for the current save, capture or pack job");
        require(_esp.has_value() && currentWeapon(), "Select a weapon before saving");
        stashDisplayedWeapon();
        const auto index = espIndex(_esp->plugin);
        bool changed = !_esp->base.isEmpty() && _esp->base != index["latest"].toString();
        EspSaveJob job;
        job.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        job.folder = studioDataPath("local/tmp/library-save/" + job.id);
        job.note = request["note"].toString().left(512);
        job.returnWeapon = weaponRecordKey();
        const auto committed = _esp->snapshot["weapons"].toObject();
        for (auto it = _esp->weapons.cbegin(); it != _esp->weapons.cend(); ++it) {
            if (it->record.isEmpty() || it->unreadable) {
                continue;
            }
            const bool templated = !it->record["reloadTemplates"].toObject()[it.key()].toString().isEmpty();
            const bool packless = !committed[it.key()].toObject().contains("pack");
            changed |= it->dirty;
            // Edited weapons are captured so the version exports what was
            // authored; so is a configured weapon that has no behavior yet.
            if (templated && (it->dirty || packless)) {
                job.queue.append(it.key());
            }
        }
        require(changed || !job.queue.isEmpty(), _esp->base.isEmpty() ? QStringLiteral("Nothing to save: no weapon of this ESP has been authored yet")
                                                                     : QStringLiteral("Nothing to save: the latest version already holds this authoring"));
        require(QDir().mkpath(job.folder), "Cannot create the save folder");
        _packJob = { { "id", job.id }, { "phase", "running" }, { "operation", "library-save" }, { "stage", "capturing" },
                     { "plugin", _esp->plugin }, { "total", job.queue.size() }, { "done", 0 } };
        _espSave = std::move(job);
        setStatus("Saving " + _esp->plugin + "…");
        emit packJobChanged();
        QTimer::singleShot(0, this, [this] { continueEspSave(); });
        return _packJob;
    }

    // One step: show the next edited weapon, capture all of its
    // configurations, then return to the weapon the author was on.
    void ReloadMakerController::continueEspSave()
    {
        if (!_espSave || _espSave->committing) {
            return;
        }
        auto& job = *_espSave;
        if (assemblyBusy() || _saveAll) {
            QTimer::singleShot(kPollMilliseconds, this, [this] { continueEspSave(); });
            return;
        }
        if (job.queue.isEmpty()) {
            if (!job.returned) {
                job.returned = true;
                if (!job.captured.isEmpty() || !job.exceptions.isEmpty() || weaponRecordKey() != job.returnWeapon) {
                    // Captures change parts and Operate; the author's stashed
                    // record brings their weapon back exactly as it was.
                    showWorkingWeapon(job.returnWeapon, false);
                    QTimer::singleShot(0, this, [this] { continueEspSave(); });
                    return;
                }
            }
            job.committing = true;
            commitEspSave();
            return;
        }
        const auto weapon = job.queue.front();
        if (weaponRecordKey() != weapon) {
            // The weapon on screen was captured or stashed already; its
            // captured state is not the author's and is never kept.
            if (!showWorkingWeapon(weapon, false)) {
                job.exceptions[weapon].append(QJsonObject{ { "status", "failed" }, { "error", "This weapon is not in the loaded source" } });
                job.queue.removeFirst();
            }
            QTimer::singleShot(0, this, [this] { continueEspSave(); });
            return;
        }
        SaveAllJob capture;
        capture.folder = job.folder + QStringLiteral("/captures/%1").arg(++job.started, 4, 10, QChar('0'));
        capture.plan = saveAllPlan();
        if (capture.plan.isEmpty()) {
            // Fixed parts: the weapon's one configuration as assembled.
            capture.plan.append(QVector<ConfigurationChoice>{});
        }
        for (const auto& slot : configuration()->entries()) {
            capture.original[slot.key] = configuration()->selected(slot.key);
        }
        if (!QDir().mkpath(capture.folder)) {
            job.exceptions[weapon].append(QJsonObject{ { "status", "failed" }, { "error", "Cannot create the capture folder" } });
            job.queue.removeFirst();
            QTimer::singleShot(0, this, [this] { continueEspSave(); });
            return;
        }
        capture.finished = [this, weapon](SaveAllJob& done) {
            if (!_espSave) {
                return;
            }
            if (done.cancelled) {
                finishEspSave(false, QJsonObject{ { "error", "Save cancelled; nothing was written." }, { "cancelled", true } });
                return;
            }
            auto& job = *_espSave;
            if (!done.exceptions.isEmpty()) {
                job.exceptions[weapon] = done.exceptions;
            }
            if (done.captures > 0) {
                job.captured.insert(weapon);
                job.captureFolders.insert(weapon, done.folder);
            }
            job.queue.removeFirst();
            _packJob["done"] = _packJob["done"].toInt() + 1;
            emit packJobChanged();
            QTimer::singleShot(0, this, [this] { continueEspSave(); });
        };
        setWorkspace(OperateWorkspace);
        _saveAll = std::move(capture);
        QTimer::singleShot(0, this, [this] { continueSaveAll(); });
    }

    // Hands the working project to the bundled tool, which writes the new
    // version and makes it the latest only when everything is written.
    void ReloadMakerController::commitEspSave()
    {
        if (!_espSave) {
            return;
        }
        auto& job = *_espSave;
        try {
            // Every weapon's record was kept when the save started; captures
            // never change what is committed.
            QJsonObject weapons;
            QJsonObject sent;
            int number = 0;
            for (auto it = _esp->weapons.cbegin(); it != _esp->weapons.cend(); ++it) {
                const bool captured = job.captured.contains(it.key());
                if (it->record.isEmpty() || it->unreadable || (!it->dirty && !captured && !job.exceptions.contains(it.key()))) {
                    continue;
                }
                const auto path = job.folder + QStringLiteral("/weapons/%1.json").arg(++number, 4, 10, QChar('0'));
                commands::writeFileAtomically(path, QJsonDocument(it->record).toJson(QJsonDocument::Compact));
                QJsonObject models;
                const auto assigned = it->record["magazineModels"].toObject();
                for (auto model = assigned.begin(); model != assigned.end(); ++model) {
                    if (model.key().section('|', 0, 0).compare(it.key(), Qt::CaseInsensitive) == 0) {
                        models[model.key().section('|', 1)] = model.value();
                    }
                }
                QJsonObject change{ { "name", displayName(it->record, it.key()) },
                                    { "editorId", it->record["weaponEditorId"].toString() },
                                    { "authoring", path },
                                    { "status", it->status },
                                    { "models", models },
                                    { "captureExceptions", job.exceptions.value(it.key()) } };
                if (captured) {
                    change["captures"] = job.captureFolders.value(it.key());
                }
                weapons[it.key()] = change;
                sent[it.key()] = authoredPart(it->record);
            }
            job.sent = sent;
            QJsonObject source{ { "mode", _libraryMode ? "mo2-profile" : "package" }, { "gameData", _gameData } };
            if (_libraryMode) {
                source["mo2Instance"] = _launchSnapshot.instancePath;
                source["profile"] = _launchSnapshot.profileName;
            } else {
                source["package"] = _package.sourcePath;
            }
            const QJsonObject request{ { "format", "reload-maker-library-save-request" },
                                       { "version", 1 },
                                       { "plugin", _esp->plugin },
                                       { "base", _esp->base },
                                       { "weapons", weapons },
                                       { "source", source },
                                       { "note", job.note } };
            const auto path = job.folder + "/request.json";
            commands::writeFileAtomically(path, QJsonDocument(request).toJson());
            QString python;
            QString id = job.id;
            auto arguments = packToolArguments(python, id);
            arguments << "library" << "save" << "--request" << path;
            auto state = _packJob;
            state["stage"] = "committing";
            startPackProcess(python, arguments, state);
        } catch (const std::exception& error) {
            finishEspSave(false, QJsonObject{ { "error", QString::fromUtf8(error.what()) } });
        }
    }

    void ReloadMakerController::finishEspSave(bool success, const QJsonObject& result)
    {
        if (!_espSave) {
            return;
        }
        auto job = std::move(*_espSave);
        _espSave.reset();
        QDir(job.folder).removeRecursively();
        if (!success) {
            _afterSave = {};
            const bool cancelled = result["cancelled"].toBool();
            _packJob["phase"] = cancelled ? "cancelled" : "failed";
            _packJob["error"] = result["error"].toString("The save failed; the previous version is still the latest.");
            setStatus((cancelled ? QStringLiteral("Save cancelled. ") : "Save failed: " + _packJob["error"].toString() + " ") + "Unsaved edits are kept.", !cancelled);
            // A stop during another weapon's captures returns to the author's
            // weapon; the captured weapon's kept record is unchanged.
            if (weaponRecordKey() != job.returnWeapon) {
                QTimer::singleShot(0, this, [this, weapon = job.returnWeapon] { showWorkingWeapon(weapon, false); });
            }
            emit packJobChanged();
            return;
        }
        _espIndexCache.remove(espProjectKey(_esp->plugin));
        const auto save = result["save"].toObject();
        _esp->base = save["id"].toString();
        QString error;
        _esp->snapshot = readEspSnapshot(_esp->plugin, _esp->base, error);
        const auto displayed = weaponRecordKey();
        for (auto it = job.sent.begin(); it != job.sent.end(); ++it) {
            auto& work = _esp->weapons[it.key()];
            work.committed = it.value().toObject();
            work.dirty = !work.record.isEmpty() && authoredPart(work.record) != work.committed;
            work.imported = false;
            if (it.key() == displayed) {
                _savedAuthoring = work.committed;
            }
        }
        refreshDocumentModified();
        int notUpdated = 0;
        for (const auto& value : result["packReports"].toObject()) {
            notUpdated += value.toObject()["failed"].toInt();
        }
        const auto when = QDateTime::fromString(save["savedAt"].toString(), Qt::ISODate).toLocalTime().toString("yyyy-MM-dd HH:mm:ss");
        setStatus(notUpdated ? QStringLiteral("Saved %1 · %2 · %3 configuration(s) kept their earlier behavior; see the save report.").arg(_esp->plugin, when).arg(notUpdated)
                             : QStringLiteral("Saved %1 · %2").arg(_esp->plugin, when),
                  false);
        rememberLastSession();
        refreshWeaponPicker();
        ++_revision;
        if (_afterSave) {
            QTimer::singleShot(0, this, std::exchange(_afterSave, {}));
        }
    }

    // ---- Versions ----

    QJsonObject ReloadMakerController::espState() const
    {
        if (!_esp) {
            return { { "plugin", QJsonValue() }, { "dirty", false }, { "saving", espBusy() } };
        }
        const auto index = espIndex(_esp->plugin);
        QJsonArray weapons;
        const auto displayed = weaponRecordKey();
        const auto committed = _esp->snapshot["weapons"].toObject();
        QSet<QString> records;
        for (auto it = committed.begin(); it != committed.end(); ++it) {
            records.insert(it.key());
        }
        for (auto it = _esp->weapons.cbegin(); it != _esp->weapons.cend(); ++it) {
            records.insert(it.key());
        }
        for (const auto& record : records) {
            const auto work = _esp->weapons.value(record);
            const bool dirty = record == displayed && currentWeapon() && !_loadThread ? documentDirty() : work.dirty;
            const auto entry = committed[record].toObject();
            weapons.append(QJsonObject{ { "record", record },
                                        { "name", entry["name"].toString(displayName(work.record, record)) },
                                        { "committed", committed.contains(record) },
                                        { "exportable", entry.contains("pack") },
                                        { "visited", !work.record.isEmpty() || record == displayed },
                                        { "unsaved", dirty },
                                        { "unreadable", work.unreadable } });
        }
        const auto published = [&] {
            QFile file(QDir(espProjectFolder(_esp->plugin)).filePath("export/published.json"));
            return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object()["publications"].toArray() : QJsonArray{};
        }();
        return { { "plugin", _esp->plugin },
                 { "key", espProjectKey(_esp->plugin) },
                 { "base", _esp->base },
                 { "latest", index["latest"] },
                 { "saves", index["saves"] },
                 { "weapons", weapons },
                 { "dirty", espDirty() },
                 { "unsavedWeapons", QJsonArray::fromStringList(espDirtyWeapons()) },
                 { "saving", espBusy() },
                 { "published", published } };
    }

    // An explicitly chosen version replaces the working project for this
    // editing session; the automatic latest load does not override it.
    QJsonObject ReloadMakerController::loadEspVersion(const QString& save)
    {
        require(_esp.has_value() && currentWeapon(), "Select a weapon of the ESP first");
        require(!_loadThread && !_packProcess && !_saveAll && !espBusy(), "Wait for the current job");
        const auto index = espIndex(_esp->plugin);
        bool known = false;
        for (const auto& value : index["saves"].toArray()) {
            known |= value.toObject()["id"].toString() == save;
        }
        require(known, "Unknown saved version: " + save);
        if (!leaveDocument(QStringLiteral("loading another saved version"), [this, save] { loadEspVersion(save); })) {
            return espState();
        }
        QString error;
        auto snapshot = readEspSnapshot(_esp->plugin, save, error);
        require(error.isEmpty(), error);
        _esp->base = save;
        _esp->snapshot = snapshot;
        _esp->weapons.clear();
        openWeaponAuthoring();
        ++_revision;
        return espState();
    }

    QJsonObject ReloadMakerController::deleteEspVersion(const QString& save)
    {
        require(_esp.has_value(), "Select a weapon of the ESP first");
        require(!_loadThread && !_packProcess && !_saveAll && !espBusy(), "Wait for the current job");
        if (save == _esp->base && !leaveDocument(QStringLiteral("deleting the loaded version"), [this, save] { deleteEspVersion(save); })) {
            return espState();
        }
        QString python;
        QString id;
        auto arguments = packToolArguments(python, id);
        arguments << "library" << "delete" << "--plugin" << _esp->plugin << "--save" << save;
        startPackProcess(python, arguments, { { "id", id }, { "phase", "running" }, { "operation", "library-delete" }, { "save", save }, { "plugin", _esp->plugin } });
        return _packJob;
    }

    void ReloadMakerController::finishEspDelete(bool success, const QJsonObject& result)
    {
        if (!success) {
            setStatus("Saved version not deleted: " + result["error"].toString(), true);
            return;
        }
        if (!_esp) {
            return;
        }
        _espIndexCache.remove(espProjectKey(_esp->plugin));
        const auto deleted = result["deleted"].toString();
        setStatus("Deleted saved version " + deleted + " of " + _esp->plugin + ". Export IDs are unchanged.");
        if (deleted != _esp->base) {
            ++_revision;
            return;
        }
        // The loaded version is gone: continue from the next newest one.
        _esp->base = result["latest"].toString();
        _esp->snapshot = {};
        if (!_esp->base.isEmpty()) {
            QString error;
            _esp->snapshot = readEspSnapshot(_esp->plugin, _esp->base, error);
        }
        _esp->weapons.clear();
        openWeaponAuthoring();
        ++_revision;
    }

    // ---- Imports, magazine models and export ----

    // The open weapon's working authoring as one file, for handing it to
    // another workspace or importing it again. It is a copy, not a save: the
    // ESP project and its unsaved state are unchanged.
    QJsonObject ReloadMakerController::exportWeaponProject(const QString& requested)
    {
        require(currentWeapon() && !_loadThread, "Select a weapon first");
        auto record = projectRecord();
        QString message;
        require(appendProjectDrafts(record, message), message);
        const auto weapon = weaponRecordKey();
        const auto path = requested.isEmpty() ? studioDataPath("exports/weapon-" + QString(weapon).replace(':', '-') + ".project.json")
                                              : QFileInfo(requested).absoluteFilePath();
        require(path.endsWith(".json", Qt::CaseInsensitive), "Choose a .json file");
        commands::writeFileAtomically(path, QJsonDocument(record).toJson());
        return { { "path", path }, { "kind", "project" }, { "weapon", weapon }, { "deployed", false } };
    }

    // An earlier per-weapon project, opened deliberately. Its authoring
    // becomes unsaved edits of its weapon in that weapon's ESP project.
    QJsonObject ReloadMakerController::importLegacyProject(const QString& path)
    {
        require(!_loadThread && !_reloadCapturing && !_packProcess && !_saveAll && !espBusy(), "Finish the current load, capture or job first");
        QFile file(path);
        require(file.open(QIODevice::ReadOnly) && file.size() <= kMaximumProjectBytes, "Cannot read this project");
        QJsonParseError parse;
        const auto record = QJsonDocument::fromJson(file.readAll(), &parse).object();
        require(parse.error == QJsonParseError::NoError && record["format"] == 1 && record["kind"] == "reload-maker-v2-project",
                "Unsupported or invalid Reload Maker project");
        QString message;
        require(validWeaponRecord(record, message), message);
        const auto weapon = record["weapon"].toString().toLower();
        QJsonObject request{ { "weapon", record["weapon"] }, { "browserPlugin", record["browserPlugin"] }, { "import", record } };
        for (int i = 0; i < _weaponSelections.size(); ++i) {
            if (selectionRecordKey(i) != weapon) {
                continue;
            }
            if (i != _selectedWeapon && !changeWeapon(i)) {
                return { { "imported", false } };
            }
            _pendingSession = request;
            QTimer::singleShot(0, this, [this] { restoreSessionSelection(); });
            return { { "imported", true }, { "weapon", weapon }, { "source", "current" } };
        }
        // The weapon is not in this source: open the project's own source.
        require(QFileInfo(record["gameData"].toString()).isDir() && (record["library"].toBool() || QFileInfo(record["package"].toString()).exists()),
                "The project's source is missing. Connect a source that contains this weapon, then import it again.");
        if (!leaveDocument(QStringLiteral("opening the imported project's source"))) {
            return { { "imported", false } };
        }
        _pendingSession = request;
        _gameDataText = record["gameData"].toString();
        if (record["library"].toBool()) {
            if (!record["mo2Instance"].toString().isEmpty()) {
                _mo2Instance = record["mo2Instance"].toString();
            }
            _requestedMo2Profile = record["mo2Profile"].toString();
            startLibraryLoad(true);
        } else {
            startPackageLoad(record["package"].toString(), record["gameData"].toString(), true);
        }
        if (!_loadThread) {
            _pendingSession = {};
            _gameDataText = _gameData;
            require(false, _lastError.isEmpty() ? QStringLiteral("The project's source could not be opened") : _lastError);
        }
        return { { "imported", true }, { "weapon", weapon }, { "source", "loading" } };
    }

    // Finished full and empty world models for a magazine of this weapon.
    // The assignment is authoring: Save copies the models into the ESP project.
    QJsonObject ReloadMakerController::assignMagazineModels(const QJsonObject& request)
    {
        require(currentWeapon() && configuration(), "Select a weapon first");
        QString magazine = request["magazine"].toString().toLower();
        if (magazine.isEmpty()) {
            magazine = simulationMagazineAnchor().ownerRecord.toLower();
        }
        require(!magazine.isEmpty(), "Choose the magazine: prepare it in Operate or pass its record");
        const auto key = weaponRecordKey() + "|" + magazine;
        if (request["operation"].toString() == "clear") {
            _magazineModels.remove(key);
            noteDocumentChange(QStringLiteral("magazine models"));
            return { { "magazine", magazine }, { "assigned", false } };
        }
        QJsonObject models;
        for (const auto* presentation : { "full", "empty" }) {
            const auto path = QFileInfo(request[QLatin1String(presentation)].toString()).absoluteFilePath();
            require(QFileInfo(path).isFile() && path.endsWith(".nif", Qt::CaseInsensitive), QStringLiteral("Choose the finished %1 model").arg(presentation));
            QFile receipt(QFileInfo(path).dir().filePath(QFileInfo(path).completeBaseName() + ".receipt.json"));
            require(receipt.open(QIODevice::ReadOnly) && receipt.size() < 4 * 1024 * 1024,
                    QStringLiteral("The %1 model has no cook receipt beside it; cook it in Maker first").arg(presentation));
            const auto meta = QJsonDocument::fromJson(receipt.readAll()).object();
            const auto capture = meta["capture"].toObject();
            const auto reference = capture["referencePose"].toObject();
            require(meta["engineCooked"].toBool() && capture["weapon"].toString().compare(weaponRecordKey(), Qt::CaseInsensitive) == 0 &&
                        reference["magazineRecord"].toString().compare(magazine, Qt::CaseInsensitive) == 0 &&
                        reference["presentation"].toString() == QLatin1String(presentation),
                    QStringLiteral("The %1 model was cooked for another weapon, magazine or presentation").arg(presentation));
            models[QLatin1String(presentation)] = path;
        }
        _magazineModels[key] = models;
        noteDocumentChange(QStringLiteral("magazine models"));
        return { { "magazine", magazine }, { "assigned", true }, { "models", models } };
    }

    // Exports committed versions only; unsaved edits are listed, not saved.
    QJsonObject ReloadMakerController::espExport(const QJsonObject& request)
    {
        require(!_packProcess && !_saveAll && !espBusy(), "Wait for the current save or pack job");
        const auto archive = request["archive"].toString(), mo2 = request["mo2"].toString();
        require(archive.isEmpty() != mo2.isEmpty(), "Choose Export to MO2 or Export ZIP / 7z");
        auto selections = request["selections"].toArray();
        if (selections.isEmpty() && _esp) {
            selections.append(QJsonObject{ { "plugin", _esp->plugin } });
        }
        require(!selections.isEmpty() && selections.size() <= 256, "Choose between one and 256 ESP projects to export");
        QString python;
        QString id;
        auto arguments = packToolArguments(python, id);
        arguments << "library" << "export";
        QJsonArray unsaved;
        for (const auto& value : selections) {
            const auto selection = value.toObject();
            const auto plugin = selection["plugin"].toString();
            require(!espProjectKey(plugin).isEmpty(), "Choose a plugin filename");
            const auto save = selection["save"].toString();
            arguments << "--select" << (save.isEmpty() ? plugin : plugin + "@" + save);
            if (_esp && espProjectKey(plugin) == espProjectKey(_esp->plugin) && espDirty()) {
                unsaved.append(plugin);
            }
        }
        if (!archive.isEmpty()) {
            arguments << "--archive" << archive;
        } else {
            arguments << "--mo2" << mo2;
        }
        QJsonObject job{ { "id", id }, { "phase", "running" }, { "operation", "library-export" }, { "selections", selections } };
        if (!unsaved.isEmpty()) {
            job["unsavedNotIncluded"] = unsaved;
        }
        startPackProcess(python, arguments, job);
        return _packJob;
    }

    // ---- Resuming the last session ----

    // The source and weapon last worked on, for the next start. Authoring
    // always comes from the library; this names only where to look.
    void ReloadMakerController::rememberLastSession()
    {
        if (!currentWeapon()) {
            return;
        }
        QJsonObject last{ { "weapon", poseRecordKey(currentWeapon()->formId) }, { "browserPlugin", _browserPlugin }, { "gameData", _gameData },
                          { "library", _libraryMode } };
        if (_libraryMode) {
            last["mo2Instance"] = _launchSnapshot.instancePath;
            last["profile"] = _launchSnapshot.profileName;
        } else {
            last["package"] = _package.sourcePath;
        }
        QSettings().setValue("session/last", QString::fromUtf8(QJsonDocument(last).toJson(QJsonDocument::Compact)));
    }

    void ReloadMakerController::resumeLastSession()
    {
        const auto last = QJsonDocument::fromJson(QSettings().value("session/last").toString().toUtf8()).object();
        if (last.isEmpty() || !QFileInfo(last["gameData"].toString()).isDir()) {
            setStatus(QStringLiteral("Select a mod folder or ZIP file."));
            return;
        }
        _pendingSession = { { "weapon", last["weapon"] }, { "browserPlugin", last["browserPlugin"] } };
        _gameDataText = last["gameData"].toString();
        if (last["library"].toBool()) {
            _mo2Instance = last["mo2Instance"].toString(_mo2Instance);
            _requestedMo2Profile = last["profile"].toString();
            startLibraryLoad(true);
        } else if (QFileInfo::exists(last["package"].toString())) {
            startPackageLoad(last["package"].toString(), last["gameData"].toString(), true);
        }
        if (!_loadThread) {
            _pendingSession = {};
            _gameDataText = _gameData;
        }
    }
}
