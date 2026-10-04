#include "ReloadMakerController.h"
#include "StudioWindow.h"

#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDockWidget>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QTabWidget>
#include <QUrl>
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
        _handPoseName->setMaxLength(kPoseNameLength);
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
        connect(_handPoseName, &QLineEdit::textChanged, this, &ReloadMakerController::setHandCaptureName);
        connect(_handPoseNotes, &QPlainTextEdit::textChanged, this, [this]() { setHandCaptureNotes(_handPoseNotes->toPlainText()); });
        connect(_handPoseSide, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
            setEditedHand(static_cast<PoseHand>(_handPoseSide->itemData(index).toInt()));
        });
        connect(_handPoseSearch, &QLineEdit::textChanged, this, &ReloadMakerController::setHandPoseFilter);
        connect(_handPoseList, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item) {
            selectHandPose(item ? item->data(Qt::UserRole).toString() : QString());
        });
        connect(_handPoseList, &QListWidget::itemActivated, this, [this]() { previewSavedHandPose(); });
        connect(_saveHandPose, &QPushButton::clicked, this, &ReloadMakerController::saveCurrentHandPose);
        connect(_handPoseAnchor,qOverload<int>(&QComboBox::currentIndexChanged),this,&ReloadMakerController::setHandCaptureAnchor);
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

    // The saved poses matching the search text. The selection stays on the
    // selected pose while it is listed, else moves to the first listed pose.
    void ReloadMakerController::refreshHandPoseList()
    {
        const QSignalBlocker blocker(_handPoseList);
        _handPoseList->clear();
        QString first;
        int listed = 0;
        bool selectedListed = false;
        for (const auto& pose : _savedHandPoses) {
            if (!handPoseMatchesFilter(pose)) { continue; }
            const QString side = pose.hand == PoseHand::Left ? QStringLiteral("Left") : QStringLiteral("Right");
            const QString weapon = pose.source.value("weaponName").toString();
            auto* item = new QListWidgetItem(QStringLiteral("%1\n%2 hand · %3").arg(pose.name, side, weapon), _handPoseList);
            item->setData(Qt::UserRole, pose.id);
            item->setToolTip(QStringLiteral("%1\n%2\n%3").arg(pose.createdUtc, pose.notes, pose.id));
            if (listed++ == 0) { first = pose.id; }
            if (pose.id == _selectedHandPoseId) { _handPoseList->setCurrentItem(item); selectedListed = true; }
        }
        // Filtering is not a user selection; the document follows the list.
        if (!selectedListed) {
            _selectedHandPoseId = first;
            if (listed) { _handPoseList->setCurrentRow(0); }
        }
        updateHandPoseControls();
    }

    void ReloadMakerController::showHandPoseFilter()
    {
        if (_handPoseSearch->text() == _handPoseFilter) { return; }
        const QSignalBlocker blocker(_handPoseSearch);
        _handPoseSearch->setText(_handPoseFilter);
    }

    void ReloadMakerController::updateHandPoseControls()
    {
        if (!_handPoseDock) { return; }
        const bool frame = handPoseFrameReady();
        const auto* selected = selectedHandPose();
        const bool gripActive = partGripPreviewActive();
        _saveHandPose->setEnabled(canSaveHandPose());
        _previewHandPose->setEnabled(canPreviewHandPose());
        _restoreHandPose->setEnabled((_handPosePreview.has_value() || gripActive) && !_loadThread);
        const QString status = _handPosePreview
            ? QStringLiteral("PREVIEW: %1 · %2 hand\nFingers only at the current wrist. Restore animation before capturing again.")
                .arg(_handPosePreview->name, _handPosePreview->hand == PoseHand::Left ? "left" : "right")
            : (frame ? QStringLiteral("%1 saved poses · capture pauses at the current frame. Placement is stored, not applied by finger preview.").arg(_savedHandPoses.size())
                     : QStringLiteral("%1 saved poses · load a weapon and animation to capture or preview.").arg(_savedHandPoses.size()));
        _handPoseStatus->setText(gripActive?QStringLiteral("Part-grip preview is active. Edit it in Reload builder > Part grips or restore the animation here."):status);
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

    void ReloadMakerController::showHandEditorPage()
    {
        if (!_handPosePages) { return; }
        const QSignalBlocker blocker(_handPosePages);
        _handPosePages->setCurrentIndex(int(_handEditorPage));
    }

    void ReloadMakerController::showEditedHand()
    {
        if (!_handPoseSide) { return; }
        const QSignalBlocker blocker(_handPoseSide);
        _handPoseSide->setCurrentIndex(_handPoseSide->findData(int(_editedHand)));
    }

    void ReloadMakerController::showHandPoseAnchors()
    {
        const QSignalBlocker blocker(_handPoseAnchor);
        _handPoseAnchor->clear();
        for (int i = 0; i < _handPoseAnchors.size(); ++i) {
            _handPoseAnchor->addItem(_handPoseAnchors[i].label, i);
            if (!_handPoseAnchors[i].tooltip.isEmpty()) { _handPoseAnchor->setItemData(i, _handPoseAnchors[i].tooltip, Qt::ToolTipRole); }
        }
        _handPoseAnchor->setCurrentIndex(_handCaptureAnchor);
    }

    void ReloadMakerController::showHandCaptureAnchor()
    {
        const QSignalBlocker blocker(_handPoseAnchor);
        _handPoseAnchor->setCurrentIndex(_handCaptureAnchor);
    }

    void ReloadMakerController::showHandCaptureName()
    {
        if (_handPoseName->text() == _handCaptureName) { return; }
        const QSignalBlocker blocker(_handPoseName);
        _handPoseName->setText(_handCaptureName);
    }

    void ReloadMakerController::showHandCaptureNotes()
    {
        if (_handPoseNotes->toPlainText() == _handCaptureNotes) { return; }
        const QSignalBlocker blocker(_handPoseNotes);
        _handPoseNotes->setPlainText(_handCaptureNotes);
    }

    void ReloadMakerController::showSelectedHandPose()
    {
        const QSignalBlocker blocker(_handPoseList);
        for (int row = 0; row < _handPoseList->count(); ++row) {
            if (_handPoseList->item(row)->data(Qt::UserRole).toString() == _selectedHandPoseId) { _handPoseList->setCurrentRow(row); return; }
        }
        _handPoseList->setCurrentRow(-1);
    }
}
