#include "ReloadMakerController.h"
#include "GripViewport.h"
#include "StudioWindow.h"
#include "model/nifmodel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDockWidget>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QTabWidget>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>

namespace reloadmaker {
    void ReloadMakerController::createHandPoseUi()
    {
        _handPoseDock = new QDockWidget(QStringLiteral("HAND POSE LIBRARY"), _window);
        _handPoseDock->setObjectName(QStringLiteral("ReloadMakerHandPoseDock"));
        _handPoseDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
        _handPoseDock->setMinimumWidth(390);
        _handPosePages = new QTabWidget(_handPoseDock);
        auto* panel = new QWidget(_handPosePages);
        auto* layout = new QVBoxLayout(panel);
        layout->setContentsMargins(14,14,14,14);
        layout->setSpacing(10);
        auto* intro = new QLabel(QStringLiteral("Capture either hand at the current frame. Exact finger joints and wrist placement are saved separately."), panel);
        intro->setWordWrap(true);
        layout->addWidget(intro);
        auto* capture = new QGroupBox(QStringLiteral("CAPTURE CURRENT HAND"), panel);
        auto* form = new QFormLayout(capture);
        form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        _handPoseName = new QLineEdit(capture);
        _handPoseName->setMaxLength(128);
        _handPoseName->setPlaceholderText(QStringLiteral("e.g. Bolt pinch · relaxed"));
        _handPoseSide = new QComboBox(capture);
        _handPoseSide->addItem(QStringLiteral("Right hand"), static_cast<int>(PoseHand::Right));
        _handPoseSide->addItem(QStringLiteral("Left hand"), static_cast<int>(PoseHand::Left));
        _handPoseAnchor = new QComboBox(capture);
        _handPoseAnchor->setMinimumContentsLength(12);
        _handPoseAnchor->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        _handPoseAnchor->setToolTip(QStringLiteral("Save the wrist relative to this actual assembled frame, plus a separate weapon-relative reference. Does not move the hand."));
        _handPoseNotes = new QPlainTextEdit(capture);
        _handPoseNotes->setPlaceholderText(QStringLiteral("Optional notes / intended use"));
        _handPoseNotes->setMaximumHeight(65);
        form->addRow(QStringLiteral("Name"), _handPoseName);
        form->addRow(QStringLiteral("Hand"), _handPoseSide);
        form->addRow(QStringLiteral("Reference part"), _handPoseAnchor);
        form->addRow(QStringLiteral("Notes"), _handPoseNotes);
        _saveHandPose = new QPushButton(QStringLiteral("SAVE CURRENT HAND"), capture);
        _saveHandPose->setObjectName(QStringLiteral("PrimaryButton"));
        _saveHandPose->setToolTip(QStringLiteral("Pauses playback and saves a new snapshot. Existing poses are never overwritten."));
        form->addRow(_saveHandPose);
        layout->addWidget(capture);
        _handPoseSearch = new QLineEdit(panel);
        _handPoseSearch->setPlaceholderText(QStringLiteral("Search saved poses, hand, weapon or notes…"));
        _handPoseSearch->setClearButtonEnabled(true);
        layout->addWidget(_handPoseSearch);
        _handPoseList = new QListWidget(panel);
        _handPoseList->setAlternatingRowColors(true);
        layout->addWidget(_handPoseList,1);
        _handPoseDetails = new QPlainTextEdit(panel);
        _handPoseDetails->setReadOnly(true);
        _handPoseDetails->setMaximumHeight(150);
        layout->addWidget(_handPoseDetails);
        auto* preview = new QHBoxLayout();
        _previewHandPose = new QPushButton(QStringLiteral("PREVIEW FINGERS"), panel);
        _restoreHandPose = new QPushButton(QStringLiteral("RESTORE ANIMATION"), panel);
        _previewHandPose->setToolTip(QStringLiteral("Pauses and applies this pose to its original left/right hand at the current wrist. Does not apply saved placement, mirror hands or move weapon parts."));
        preview->addWidget(_previewHandPose); preview->addWidget(_restoreHandPose);
        layout->addLayout(preview);
        auto* partGrip = new QPushButton(QStringLiteral("USE POSE ON A WEAPON PART…"), panel);
        partGrip->setToolTip(QStringLiteral("Choose a saved pose, then click the magazine or other part where the palm should hold. Adjust the placement and save the grip."));
        layout->addWidget(partGrip);
        connect(partGrip, &QPushButton::clicked, this, &ReloadMakerController::applySavedPoseToPart);
        _handPoseStatus = new QLabel(panel);
        _handPoseStatus->setTextFormat(Qt::PlainText);
        _handPoseStatus->setWordWrap(true);
        layout->addWidget(_handPoseStatus);
        auto* footer = new QHBoxLayout();
        auto* refresh = new QPushButton(QStringLiteral("REFRESH LIST"), panel);
        auto* folder = new QPushButton(QStringLiteral("LIBRARY FOLDER"), panel);
        folder->setToolTip(_handPoseLibrary.directory());
        footer->addWidget(refresh); footer->addWidget(folder);
        layout->addLayout(footer);
        _handPosePages->addTab(panel, QStringLiteral("FINGER POSES"));
        _handPoseDock->setWidget(_handPosePages);
        createPartGripUi();
        createMechanismUi();
        _window->addDockWidget(Qt::RightDockWidgetArea, _handPoseDock);

        _handPoseDock->show(); _handPoseDock->raise();
        connect(_handPoseName, &QLineEdit::textChanged, this, &ReloadMakerController::updateHandPoseControls);
        connect(_handPoseSearch, &QLineEdit::textChanged, this, &ReloadMakerController::refreshHandPoseList);
        connect(_handPoseList, &QListWidget::currentItemChanged, this, &ReloadMakerController::updateHandPoseControls);
        connect(_handPoseList, &QListWidget::itemActivated, this, [this]() { previewSavedHandPose(); });
        connect(_saveHandPose, &QPushButton::clicked, this, &ReloadMakerController::saveCurrentHandPose);
        connect(_handPoseAnchor,qOverload<int>(&QComboBox::currentIndexChanged),this,&ReloadMakerController::updateHandPoseControls);
        connect(_previewHandPose, &QPushButton::clicked, this, &ReloadMakerController::previewSavedHandPose);
        connect(_restoreHandPose, &QPushButton::clicked, this, &ReloadMakerController::restoreHandPosePreview);
        connect(refresh, &QPushButton::clicked, this, &ReloadMakerController::refreshHandPoseLibrary);
        connect(folder, &QPushButton::clicked, this, [this]() {
            if (!QDir().mkpath(_handPoseLibrary.directory()) ||
                !QDesktopServices::openUrl(QUrl::fromLocalFile(_handPoseLibrary.directory()))) {
                setStatus(QStringLiteral("Cannot open the hand-pose library folder."), true);
            }
        });
        updateHandPoseControls();
        QTimer::singleShot(0, this, &ReloadMakerController::refreshHandPoseLibrary);
    }

    const HandPose* ReloadMakerController::selectedHandPose() const
    {
        const auto* item = _handPoseList->currentItem();
        if (!item) { return nullptr; }
        const QString id = item->data(Qt::UserRole).toString();
        for (const auto& pose : _savedHandPoses) { if (pose.id == id) { return &pose; } }
        return nullptr;
    }

    void ReloadMakerController::refreshHandPoseLibrary()
    {
        QStringList diagnostics;
        _savedHandPoses = _handPoseLibrary.load(diagnostics);
        refreshHandPoseList();
        refreshPartGripLibrary();
        refreshMechanisms();
        for (const auto& message : diagnostics) { appendDiagnostic(QStringLiteral("POSE FILE %1").arg(message)); }
        if (!diagnostics.isEmpty()) {
            setStatus(QStringLiteral("Loaded %1 poses; %2 invalid pose files were left untouched. See Diagnostics.")
                .arg(_savedHandPoses.size()).arg(diagnostics.size()), true);
        }
    }

    void ReloadMakerController::refreshHandPoseList()
    {
        const auto* current = _handPoseList->currentItem();
        const QString selected = current ? current->data(Qt::UserRole).toString() : QString();
        const QSignalBlocker blocker(_handPoseList);
        _handPoseList->clear();
        const QString query = _handPoseSearch->text().trimmed();
        for (const auto& pose : _savedHandPoses) {
            const QString side = pose.hand == PoseHand::Left ? QStringLiteral("Left") : QStringLiteral("Right");
            const QString weapon = pose.source.value("weaponName").toString();
            if (!(pose.name + side + weapon + pose.notes).contains(query, Qt::CaseInsensitive)) { continue; }
            auto* item = new QListWidgetItem(QStringLiteral("%1\n%2 hand · %3").arg(pose.name, side, weapon), _handPoseList);
            item->setData(Qt::UserRole, pose.id);
            item->setToolTip(QStringLiteral("%1\n%2\n%3").arg(pose.createdUtc, pose.notes, pose.id));
            if (pose.id == selected) { _handPoseList->setCurrentItem(item); }
        }
        if (!_handPoseList->currentItem() && _handPoseList->count()) { _handPoseList->setCurrentRow(0); }
        updateHandPoseControls();
    }

    void ReloadMakerController::updateHandPoseControls()
    {
        if (!_handPoseDock) { return; }
        const bool frame = !_loadThread && !_rebuildTimer->isActive() && _clip.valid() &&
            _renderedWeapon == _weaponCombo->currentIndex() && _renderedWeapon >= 0 &&
            !_geometrySignature.isEmpty() &&
            _globalPose.size() == _skeleton.bones.size();
        const auto* selected = selectedHandPose();
        const bool gripActive = _partGripDraft && _handPosePages->currentIndex() != 0;
        _saveHandPose->setEnabled(frame && !_handPosePreview && !gripActive && !_handPoseName->text().trimmed().isEmpty() && !_handPoseAnchors.isEmpty());
        _previewHandPose->setEnabled(frame && selected);
        _restoreHandPose->setEnabled((_handPosePreview.has_value() || gripActive) && !_loadThread);
        const QString status = _handPosePreview
            ? QStringLiteral("PREVIEW: %1 · %2 hand\nFingers only at the current wrist. Restore animation before capturing again.")
                .arg(_handPosePreview->name, _handPosePreview->hand == PoseHand::Left ? "left" : "right")
            : (frame ? QStringLiteral("%1 saved poses · capture pauses at the current frame. Placement is stored, not applied by finger preview.").arg(_savedHandPoses.size())
                     : QStringLiteral("%1 saved poses · load a weapon and animation to capture or preview.").arg(_savedHandPoses.size()));
        _handPoseStatus->setText(gripActive?QStringLiteral("Part-grip preview is active. Edit it in Part grip or restore the animation here."):status);
        updatePartGripControls();
        if (!selected) { _handPoseDetails->setPlainText(QStringLiteral("Saved poses appear here and remain available after restarting Reload Maker.")); return; }
        const auto& p = selected->placement.handInAnchor;
        _handPoseDetails->setPlainText(QStringLiteral("%1 hand · 15 exact parent-local joints\nCaptured: %2\nSource: %3 · %4 at %5 s\nPlacement: %6\nHand in anchor: (%7, %8, %9) game units\nRotation quaternion (x,y,z,w): (%10, %11, %12, %13)\n%14\n%15")
            .arg(selected->hand == PoseHand::Left ? "Left" : "Right", selected->createdUtc,
                 selected->source.value("weaponName").toString(), selected->source.value("action").toString())
            .arg(selected->source.value("timeSeconds").toDouble(),0,'f',4).arg(selected->placement.anchorName)
            .arg(p.translation.x,0,'f',4).arg(p.translation.y,0,'f',4).arg(p.translation.z,0,'f',4)
            .arg(p.rotation.x,0,'f',5).arg(p.rotation.y,0,'f',5).arg(p.rotation.z,0,'f',5).arg(p.rotation.w,0,'f',5)
            .arg(selected->placement.ownerRecord, selected->notes));
    }

    QString ReloadMakerController::poseRecordKey(std::uint32_t formId) const
    {
        const auto* plugin = currentPlugin();
        if (!plugin || !formId) { return {}; }
        const auto found = plugin->origins.constFind(formId);
        if (found != plugin->origins.cend()) {
            return QStringLiteral("%1:%2").arg(found->plugin).arg(found->objectId,6,16,QLatin1Char('0'));
        }
        const auto index = formId >> 24;
        const QString origin = index < static_cast<std::uint32_t>(plugin->masters.size()) ? plugin->masters[index] : plugin->fileName;
        return QStringLiteral("%1:%2").arg(origin).arg(formId & 0xffffffU,6,16,QLatin1Char('0'));
    }

    void ReloadMakerController::refreshHandPoseAnchors()
    {
        const QString previous = _handPoseAnchor->currentText();
        const QSignalBlocker blocker(_handPoseAnchor);
        _handPoseAnchor->clear(); _handPoseAnchors.clear();
        if (!_skeleton.valid() || _renderedWeapon < 0) { updateHandPoseControls(); return; }
        _handPoseAnchors.append(HandPoseAnchor{{"weapon", "Weapon", "Weapon", {}, {}, {}, {}}, {}});
        _handPoseAnchor->addItem(QStringLiteral("Weapon · whole weapon"), 0);
        const auto* model = activeModel();
        const auto* config = configuration();
        if (!model || !config) { updateHandPoseControls(); return; }
        const auto add = [this,model](const QModelIndex& node, const QString& kind, const QString& label,
                                     const QString& modelPath, const QString& record, const QModelIndex& owner) {
            if (!node.isValid()) { return; }
            QStringList path;
            auto ancestor = node;
            for (int depth=0; ancestor.isValid() && depth<model->getBlockCount(); ++depth) {
                path.prepend(model->get<QString>(ancestor,"Name"));
                if (ancestor == owner) { break; }
                ancestor = model->getBlockIndex(model->getParent(ancestor));
            }
            _handPoseAnchors.append(HandPoseAnchor{{kind,model->get<QString>(node,"Name"),path.join('/'),modelPath,record,{},{}}, node});
            _handPoseAnchor->addItem(label, _handPoseAnchors.size()-1);
            _handPoseAnchor->setItemData(_handPoseAnchor->count()-1,
                QStringLiteral("%1\n%2\n%3").arg(record,modelPath,path.join('/')),Qt::ToolTipRole);
        };
        if (_weaponRoot.isValid() && currentWeapon()) {
            add(_weaponRoot,"installed-part",QStringLiteral("Base weapon model"),currentWeapon()->baseModelPath,poseRecordKey(currentWeapon()->formId),_weaponRoot);
        }
        for (const auto& slot : config->entries()) {
            const auto root = _partRoots.value(slot.key);
            const auto* mod = config->mod(config->selected(slot.key));
            if (root.isValid() && mod) {
                add(root,"installed-part",QStringLiteral("%1 · %2").arg(slot.label,mod->name),mod->modelPath,poseRecordKey(mod->formId),root);
            }
        }
        for (const auto& binding : _nodeBindings) {
            if (!binding.weaponPart) { continue; }
            const auto node = _sceneNodes[binding.nodeIndex];
            auto ancestor = QModelIndex(node);
            bool added = false;
            for (int depth=0; ancestor.isValid() && depth<model->getBlockCount() && !added; ++depth) {
                for (const auto& slot : config->entries()) {
                    const auto root = _partRoots.value(slot.key);
                    if (!root.isValid() || root != ancestor) { continue; }
                    const auto* mod = config->mod(config->selected(slot.key));
                    if (mod) {
                        add(node,"animated-part",QStringLiteral("%1 · %2").arg(_skeleton.bones[binding.boneIndex].name,slot.label),
                            mod->modelPath,poseRecordKey(mod->formId),root);
                        added = true;
                    }
                    break;
                }
                if (!added && ancestor == _weaponRoot && currentWeapon()) {
                    add(node,"animated-part",QStringLiteral("%1 · base weapon").arg(_skeleton.bones[binding.boneIndex].name),
                        currentWeapon()->baseModelPath,poseRecordKey(currentWeapon()->formId),_weaponRoot);
                    added = true;
                }
                ancestor = model->getBlockIndex(model->getParent(ancestor));
            }
            if (!added && currentWeapon()) {
                add(node,"animated-part",QStringLiteral("%1 · native rig").arg(_skeleton.bones[binding.boneIndex].name),
                    currentWeapon()->baseModelPath,poseRecordKey(currentWeapon()->formId),node);
            }
        }
        const int restore = _handPoseAnchor->findText(previous);
        if (restore >= 0) { _handPoseAnchor->setCurrentIndex(restore); }
        refreshPartGripChoices();
        refreshPartGripList();
        refreshMechanismChoices();
        // A rebuilt assembly is re-learned wherever the reload is consumed
        // (Reload phases or Operate), so part swaps take effect in place.
        // Queued, not immediate: the rebuild must finish first. The queued
        // capture counts as busy so MCP clients do not race its revision bump.
        if(reloadCaptureAllowed()){_reloadCaptureQueued=true;QTimer::singleShot(0,this,&ReloadMakerController::startReloadCapture);}
        updateHandPoseControls();
    }

    bool ReloadMakerController::sceneNodeWorld(const QModelIndex& node, Transformf& world, QString& error) const
    {
        const auto* model = activeModel();
        if (!model || !node.isValid()) { error = QStringLiteral("The placement part is no longer available"); return false; }
        auto ancestor = node;
        world = {};
        for (int depth=0; ancestor.isValid() && depth<model->getBlockCount(); ++depth) {
            Transformf local;
            if (!readNodeTransform(ancestor,local)) { error = QStringLiteral("The selected part has an invalid or zero-scaled frame"); return false; }
            world = composed(local,world);
            const int parent = model->getParent(ancestor);
            if (parent < 0) { return true; }
            ancestor = model->getBlockIndex(parent);
        }
        error = QStringLiteral("Cannot resolve the selected part's complete parent chain");
        return false;
    }

    bool ReloadMakerController::saveCurrentHandPose()
    {
        updateHandPoseControls();
        if (!_saveHandPose->isEnabled()) { return false; }
        if (_handPoseNotes->toPlainText().size() > 4096) { setStatus(QStringLiteral("Keep pose notes below 4097 characters."),true); return false; }
        _playing = false; _playbackTimer->stop(); _playButton->setText(QStringLiteral("PLAY"));
        if (!applyPose(_playbackTime)) { updateHandPoseControls(); return false; }
        const int selection = _handPoseAnchor->currentData().toInt();
        if (selection < 0 || selection >= _handPoseAnchors.size()) { return false; }
        const auto& anchor = _handPoseAnchors[selection];
        Transformf frame = _globalPose[_weaponBoneIndex];
        QString error;
        if (anchor.description.anchorKind != "weapon" && !sceneNodeWorld(anchor.node,frame,error)) { setStatus(error,true); return false; }
        HandPose pose;
        pose.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        pose.name = _handPoseName->text().trimmed(); pose.notes = _handPoseNotes->toPlainText();
        pose.createdUtc = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        pose.hand = static_cast<PoseHand>(_handPoseSide->currentData().toInt());
        pose.placement = anchor.description;
        pose.source = currentPoseSource();
        if (!captureHandPose(_skeleton,_localPose,_globalPose,frame,pose,error) || !_handPoseLibrary.save(pose,error)) {
            setStatus(QStringLiteral("Hand pose was not saved: %1").arg(error),true); return false;
        }
        _handPoseSearch->clear();
        refreshHandPoseLibrary();
        for (int row=0;row<_handPoseList->count();++row) {
            if (_handPoseList->item(row)->data(Qt::UserRole).toString()==pose.id) { _handPoseList->setCurrentRow(row); break; }
        }
        appendDiagnostic(QStringLiteral("SAVED HAND POSE %1 · %2 · %3").arg(pose.name,pose.id,_handPoseLibrary.directory()));
        _handPoseName->clear();
        setStatus(QStringLiteral("Saved %1 hand pose “%2”. It will remain available after restart.")
            .arg(pose.hand==PoseHand::Left ? "left" : "right",pose.name));
        return true;
    }

    QJsonObject ReloadMakerController::currentPoseSource() const
    {
        QJsonArray mods;
        for (const auto* mod : configuredObjectMods()) {
            mods.append(QJsonObject{{"record",poseRecordKey(mod->formId)},{"editorId",mod->editorId},{"name",mod->name},{"model",mod->modelPath}});
        }
        const auto* weapon = currentWeapon();
        if (!weapon) { return {}; }
        return {{"weaponName",weapon->name},{"weaponEditorId",weapon->editorId},{"weaponRecord",poseRecordKey(weapon->formId)},
            {"modifications",mods},{"action",_activeAction},{"animation",_activeAnimationPath},{"baselineAnimation",_stateBaselineAnimationPath},
            {"timeSeconds",_playbackTime},{"clipTimeSeconds",std::min(_playbackTime,_clip.durationSeconds)},
            {"body",_bodyCombo->currentData().toString()},{"viewMode",_motionMode->currentText()},
            {"approximateTransitions",_transitionCheck->isChecked()},
            {"resourceProvider",_libraryMode ? _packageResources.provider(_activeAnimationPath) : _package.sourcePath}};
    }

    void ReloadMakerController::previewSavedHandPose()
    {
        updateHandPoseControls();
        const auto* selected = selectedHandPose();
        if (!_previewHandPose->isEnabled() || !selected) { return; }
        restoreHandPosePreview();
        auto check = _localPose;
        QString error;
        if (!applyHandPoseFingers(_skeleton,*selected,check,error)) { setStatus(error,true); return; }
        _playing = false; _playbackTimer->stop(); _playButton->setText(QStringLiteral("PLAY"));
        _handPosePreview = *selected;
        _showArmsCheck->setChecked(true);
        if (!applyPose(_playbackTime)) { _handPosePreview.reset(); updateHandPoseControls(); return; }
        updateHandPoseControls();
        setStatus(QStringLiteral("Previewing “%1”: saved fingers, current wrist. Placement metadata has not been applied.").arg(selected->name));
    }

    void ReloadMakerController::restoreHandPosePreview()
    {
        rememberMagazineGrip();
        _workshopHandEditing=false;_workshopPicking=false;
        if(_gripViewport)_gripViewport->setPicking(false);
        if (!_handPosePreview && !_partGripDraft && !_mechanismPreview) { return; }
        stopMechanismPreview();
        _handPosePreview.reset();
        _partGripDraft.reset();
        _gripPreviewValid = false;
        _gripError.clear();
        if (_clip.valid() && !_loadThread) { (void)applyPose(_playbackTime); }
        updateHandPoseControls();
    }
}
