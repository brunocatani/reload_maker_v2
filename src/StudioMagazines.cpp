// Reload builder › Magazines panel: the sub-step bar, the Pieces page and the
// Insertion guide page. The work happens in the viewport; the panel lists the
// pieces, holds the typed guide values and the occasional advanced choices.
// Sample indexes, native approach timing and payload sampling stay MCP-only
// (configure_magazine_setup).
#include "ReloadMakerController.h"
#include "MagazineGuide.h"
#include "StudioWindow.h"
#include "design/DesignWidgets.h"
#include "glview.h"
#include "viewport/MagazineAuthoringViewport.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <stdexcept>
#include <utility>

namespace reloadmaker
{
    namespace
    {
        constexpr int kPathRole = Qt::UserRole;
        constexpr int kRoleRole = Qt::UserRole + 1;
        const QString kRoles[]{ QStringLiteral("body"), QStringLiteral("cartridge"), QStringLiteral("spare") };

        // Shows the selected piece's role; nothing is checked without one.
        void showRole(QButtonGroup* group, const QString& role)
        {
            const QSignalBlocker block(group);
            group->setExclusive(false);
            for (int i = 0; i < 3; ++i) {
                group->button(i)->setChecked(kRoles[i] == role);
                group->button(i)->setEnabled(!role.isEmpty());
            }
            group->setExclusive(true);
        }

        QDoubleSpinBox* lengthSpin(QWidget* parent)
        {
            auto* spin = new QDoubleSpinBox(parent);
            spin->setRange(.05, 999);
            spin->setDecimals(3);
            spin->setSingleStep(.1);
            spin->setSuffix(QStringLiteral(" units"));
            spin->setKeyboardTracking(false);
            return spin;
        }
    }

    QWidget* ReloadMakerController::createMagazineStepBar(QWidget* parent)
    {
        auto* bar = new design::SegmentedControl(QStringLiteral("StepSwitch"), parent);
        bar->addSegment(QStringLiteral("Pieces"), int(MagazineStep::Pieces))->setToolTip(QStringLiteral("Body, rounds and animation-only pieces; full and empty"));
        bar->addSegment(QStringLiteral("Guide"), int(MagazineStep::Guide))->setToolTip(QStringLiteral("The line the hand follows to insert and remove the magazine"));
        bar->addSegment(QStringLiteral("Hand fit"), int(MagazineStep::HandFit))->setToolTip(QStringLiteral("How the reload hand holds this magazine"));
        bar->addSegment(QStringLiteral("World models"), int(MagazineStep::WorldModels))->setToolTip(QStringLiteral("The loose full and empty magazines and their collision"));
        _magazineStepGroups.append(bar->group());
        connect(bar->group(), &QButtonGroup::idClicked, this, [this](int step) { showMagazineStep(MagazineStep(step)); });
        return bar;
    }

    void ReloadMakerController::syncMagazineStepBars()
    {
        const auto step = magazineStep();
        for (auto* group : std::as_const(_magazineStepGroups)) {
            const QSignalBlocker block(group);
            if (step) {
                group->button(int(*step))->setChecked(true);
            } else if (auto* checked = group->checkedButton()) {
                group->setExclusive(false);
                checked->setChecked(false);
                group->setExclusive(true);
            }
        }
        if (_partGripMagazineSteps) {
            _partGripMagazineSteps->setVisible(step == MagazineStep::HandFit);
        }
    }

    void ReloadMakerController::createMagazinesUi()
    {
        using namespace design;
        _magazinesDock = new QDockWidget(QStringLiteral("Magazines"), _window);
        _magazinesDock->setObjectName(QStringLiteral("MagazinesDock"));
        _magazinesDock->setFeatures(QDockWidget::NoDockWidgetFeatures);
        _magazinesDock->setMinimumWidth(360);
        _magazinesDock->setMaximumWidth(470);
        _magazinesHeader = new PanelHeader(QStringLiteral("Reload builder"), QStringLiteral("Magazines"), _magazinesDock);
        _magazinesDock->setTitleBarWidget(_magazinesHeader);

        auto* panel = new QWidget;
        auto* layout = new QVBoxLayout(panel);
        layout->setContentsMargins(16, 10, 16, 16);
        layout->setSpacing(8);
        layout->addWidget(createMagazineStepBar(panel));
        _magazinePages = new QStackedWidget(panel);
        layout->addWidget(_magazinePages);
        layout->addStretch(1);

        // ---- Pieces ----
        auto* pieces = new QWidget(_magazinePages);
        auto* piecesLayout = new QVBoxLayout(pieces);
        piecesLayout->setContentsMargins(0, 4, 0, 0);
        piecesLayout->setSpacing(8);
        piecesLayout->addWidget(hintLabel(QStringLiteral("Click a piece on the magazine, or in the list, then choose what it is. Rounds disappear "
                                                         "when the magazine is empty; animation-only pieces are never shown outside the reload."),
                                          pieces));
        piecesLayout->addWidget(sectionLabel(QStringLiteral("Show"), pieces));
        auto* presentation = new SegmentedControl(QStringLiteral("StepSwitch"), pieces);
        presentation->addSegment(QStringLiteral("Full magazine"), 0)->setChecked(true);
        presentation->addSegment(QStringLiteral("Empty magazine"), 1);
        _magazinePresentation = presentation->group();
        piecesLayout->addWidget(presentation);
        connect(_magazinePresentation, &QButtonGroup::idClicked, this, [this](int id) {
            _magazineShowsEmpty = id == 1;
            presentMagazineStep();
        });
        piecesLayout->addWidget(sectionLabel(QStringLiteral("Pieces"), pieces));
        _magazinePieceList = new QListWidget(pieces);
        _magazinePieceList->setObjectName(QStringLiteral("MagazinePieces"));
        _magazinePieceList->setMinimumHeight(180);
        _magazinePieceList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        _magazinePieceList->setTextElideMode(Qt::ElideMiddle);
        piecesLayout->addWidget(_magazinePieceList);
        connect(_magazinePieceList, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item) {
            // The list is not rebuilt here: the item emitting this would be deleted.
            _selectedMagazinePiece = item ? item->data(kPathRole).toString() : QString();
            showRole(_magazinePieceRole, item ? item->data(kRoleRole).toString() : QString());
            updateMagazineAuthoring();
        });
        auto* role = new SegmentedControl(QStringLiteral("StepSwitch"), pieces);
        role->addSegment(QStringLiteral("Body"), 0)->setToolTip(QStringLiteral("Magazine body or follower: always shown"));
        role->addSegment(QStringLiteral("Round"), 1)->setToolTip(QStringLiteral("A cartridge: hidden when the magazine is empty"));
        role->addSegment(QStringLiteral("Animation only"), 2)->setToolTip(QStringLiteral("A piece only the reload animation shows; never on the loose magazine"));
        _magazinePieceRole = role->group();
        piecesLayout->addWidget(role);
        connect(_magazinePieceRole, &QButtonGroup::idClicked, this, [this](int id) {
            try {
                setMagazinePieceRole(_selectedMagazinePiece, kRoles[id]);
            } catch (const std::exception& e) {
                setStatus(QString::fromUtf8(e.what()), true);
                refreshMagazinesUi();
            }
        });
        _magazinePiecesStatus = hintLabel({}, pieces);
        piecesLayout->addWidget(_magazinePiecesStatus);
        _magazineRestorePieces = new QPushButton(QStringLiteral("Restore detected pieces"), pieces);
        _magazineRestorePieces->setToolTip(QStringLiteral("Back to the roles read from the reload, without this configuration's setup or this magazine's round choices"));
        piecesLayout->addWidget(_magazineRestorePieces);
        connect(_magazineRestorePieces, &QPushButton::clicked, this, [this] { restoreMagazinePieces(); });

        auto* advanced = new QWidget(pieces);
        auto* advancedForm = new QFormLayout(advanced);
        advancedForm->setContentsMargins(0, 0, 0, 0);
        advancedForm->setSpacing(8);
        _magazineEmptyBody = new QComboBox(advanced);
        _magazineEmptyBody->setToolTip(QStringLiteral("The part shown as the empty magazine; the full one when both share a body"));
        advancedForm->addRow(QStringLiteral("Empty body"), _magazineEmptyBody);
        _magazineMotion = new QComboBox(advanced);
        _magazineMotion->addItem(QStringLiteral("Read from the reload"), QStringLiteral("preset"));
        _magazineMotion->addItem(QStringLiteral("Straight out"), QStringLiteral("linear"));
        _magazineMotion->addItem(QStringLiteral("As the reload inserts it, with rotation"), QStringLiteral("native"));
        _magazineMotion->addItem(QStringLiteral("A saved Move / Rotate movement"), QStringLiteral("manual"));
        _magazineMotion->setToolTip(QStringLiteral("How the magazine moves on screen between seated and the end of its guide"));
        advancedForm->addRow(QStringLiteral("Visible movement"), _magazineMotion);
        _magazineMovement = new QComboBox(advanced);
        advancedForm->addRow(QStringLiteral("Saved movement"), _magazineMovement);
        auto* editMovement = new QPushButton(QStringLiteral("Edit the magazine's movement…"), advanced);
        advancedForm->addRow(editMovement);
        auto* reference = new QPushButton(QStringLiteral("Hand reference and connection…"), advanced);
        advancedForm->addRow(reference);
        piecesLayout->addSpacing(4);
        piecesLayout->addWidget(disclosure(QStringLiteral("Advanced"), advanced, pieces));
        piecesLayout->addWidget(advanced);
        connect(_magazineEmptyBody, qOverload<int>(&QComboBox::activated), this, [this] {
            setMagazineAdvanced({ { "emptyTrack", _magazineEmptyBody->currentData().toInt() } });
        });
        const auto saveMotion = [this] {
            const auto motion = _magazineMotion->currentData().toString();
            const auto movement = _magazineMovement->currentData().toString();
            _magazineMovement->setEnabled(motion == QLatin1String("manual"));
            if (motion == QLatin1String("manual") && movement.isEmpty()) {
                _magazinePiecesStatus->setText(QStringLiteral("Choose the saved movement the magazine follows."));
                return;
            }
            setMagazineAdvanced({ { "motion", motion }, { "movementId", motion == QLatin1String("manual") ? movement : QString() } });
        };
        connect(_magazineMotion, qOverload<int>(&QComboBox::activated), this, saveMotion);
        connect(_magazineMovement, qOverload<int>(&QComboBox::activated), this, saveMotion);
        connect(editMovement, &QPushButton::clicked, this, [this] {
            if (!_reloadRecipe || !_simulationRig) {
                return;
            }
            for (int i = 1; i < _handPoseAnchors.size(); ++i) {
                if (sameMechanismPart(_handPoseAnchors[i].description, _reloadRecipe->tracks[_simulationRig->magazine].anchor)) {
                    setWorkspace(GripsWorkspace);
                    selectWorkshopPart(i);
                    return;
                }
            }
            setStatus(QStringLiteral("The magazine carrier has no editable native anchor."), true);
        });
        connect(reference, &QPushButton::clicked, this, [this] { showMagazineReference(); });
        _magazinePages->addWidget(pieces);

        // ---- Insertion guide ----
        auto* guide = new QWidget(_magazinePages);
        auto* guideLayout = new QVBoxLayout(guide);
        guideLayout->setContentsMargins(0, 4, 0, 0);
        guideLayout->setSpacing(8);
        guideLayout->addWidget(hintLabel(QStringLiteral("The straight line the hand follows into the magazine well. Drag the handles on the weapon, or "
                                                        "type the values. The magazine keeps its own visible movement, rocking included."),
                                         guide));
        guideLayout->addWidget(sectionLabel(QStringLiteral("Direction"), guide));
        guideLayout->addWidget(hintLabel(QStringLiteral("Outward from the seated magazine, in its connection's frame."), guide));
        auto* direction = new QGridLayout;
        direction->setHorizontalSpacing(6);
        direction->setVerticalSpacing(2);
        for (int axis = 0; axis < 3; ++axis) {
            direction->addWidget(hintLabel(QString(QChar('X' + axis)), guide), 0, axis);
            auto* spin = new QDoubleSpinBox(guide);
            spin->setRange(-1, 1);
            spin->setDecimals(3);
            spin->setSingleStep(.05);
            spin->setKeyboardTracking(false);
            spin->setMinimumWidth(70);
            direction->addWidget(spin, 1, axis);
            _guideDirection[axis] = spin;
            connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this] { typeMagazineGuide(); });
        }
        guideLayout->addLayout(direction);
        guideLayout->addWidget(sectionLabel(QStringLiteral("Lengths"), guide));
        auto* lengths = new QFormLayout;
        lengths->setSpacing(8);
        _guideTravel = lengthSpin(guide);
        _guideTravel->setToolTip(QStringLiteral("How far the hand moves along the line, from seated to its outer end"));
        _guidePickup = lengthSpin(guide);
        _guidePickup->setToolTip(QStringLiteral("How close to the line the magazine must come before it is guided"));
        _guideRelease = lengthSpin(guide);
        _guideRelease->setToolTip(QStringLiteral("How far past the outer end the hand pulls before the magazine leaves the guide"));
        lengths->addRow(QStringLiteral("Travel"), _guideTravel);
        lengths->addRow(QStringLiteral("Pickup distance"), _guidePickup);
        lengths->addRow(QStringLiteral("Release margin"), _guideRelease);
        guideLayout->addLayout(lengths);
        for (auto* spin : { _guideTravel, _guidePickup, _guideRelease }) {
            connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this] { typeMagazineGuide(); });
        }
        auto* actions = new QHBoxLayout;
        actions->setSpacing(6);
        _guideTest = new QPushButton(QStringLiteral("Test grab"), guide);
        _guideTest->setObjectName(QStringLiteral("PrimaryButton"));
        _guideTest->setToolTip(QStringLiteral("Carry the magazine with the mouse and insert it along the guide"));
        _guideRestore = new QPushButton(QStringLiteral("Restore source guide"), guide);
        _guideRestore->setToolTip(QStringLiteral("Back to the guide read from the reload's animation"));
        actions->addWidget(_guideTest);
        actions->addWidget(_guideRestore);
        guideLayout->addLayout(actions);
        _guideStatus = hintLabel({}, guide);
        guideLayout->addWidget(_guideStatus);
        connect(_guideTest, &QPushButton::clicked, this, [this] { testMagazineGuide(); });
        connect(_guideRestore, &QPushButton::clicked, this, [this] {
            try {
                (void)configureMagazineGuide({ { "operation", "reset" } });
            } catch (const std::exception& e) {
                _guideStatus->setText(QString::fromUtf8(e.what()));
            }
            refreshMagazinesUi();
        });
        _magazinePages->addWidget(guide);

        auto* scroll = new QScrollArea(_magazinesDock);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setWidget(panel);
        _magazinesDock->setWidget(scroll);
        _window->addDockWidget(Qt::RightDockWidgetArea, _magazinesDock);
        _magazinesDock->hide();

        _magazineAuthoring = new MagazineAuthoringViewport(_window->getGLView(), this);
        _magazineAuthoring->pieceClicked = [this](int block) {
            for (const auto& value : magazinePieceRows()) {
                const auto row = value.toObject();
                if (row["block"].toInt() == block) {
                    _selectedMagazinePiece = row["path"].toString();
                }
            }
            refreshMagazinesUi();
            updateMagazineAuthoring();
        };
        _magazineAuthoring->guideEdited = [this](const GuideShape& shape, bool finished) { editMagazineGuide(shape, finished); };
        _magazineAuthoring->blocked = [this] { return _simulationActive && _simulation.magazine.carried(); };
    }

    void ReloadMakerController::refreshMagazinesUi()
    {
        if (!_magazinesDock) {
            return;
        }
        syncMagazineStepBars();
        _magazinePages->setCurrentIndex(_magazineStep == MagazineStep::Guide ? 1 : 0);
        const bool ready = _simulationActive && _magazineRail.has_value();
        const QString waiting = !currentWeapon()                                   ? QStringLiteral("Choose a weapon with a detachable magazine.")
                                : !_magazineError.isEmpty()                        ? _magazineError
                                : !_simulationError.isEmpty()                      ? _simulationError
                                : _loadThread || _reloadCapturing || !_simulationActive ? QStringLiteral("Preparing the magazine…")
                                                                                   : QStringLiteral("This weapon has no detachable magazine to set up.");

        // Pieces.
        {
            const QSignalBlocker listSignals(_magazinePieceList);
            _magazinePieceList->clear();
            QString selectedRole;
            if (ready) {
                for (const auto& value : magazinePieceRows()) {
                    const auto row = value.toObject();
                    const auto role = row["role"].toString();
                    const auto name = row["name"].toString().isEmpty() ? row["path"].toString().section('/', -1) : row["name"].toString();
                    QString caption = role == QLatin1String("cartridge") ? QStringLiteral("Round") : role == QLatin1String("spare") ? QStringLiteral("Animation only") : QStringLiteral("Body");
                    if (row["empty"].toBool()) {
                        caption += QStringLiteral(" · empty magazine");
                    }
                    auto* item = new QListWidgetItem(QStringLiteral("%1  ·  %2").arg(name, caption), _magazinePieceList);
                    item->setData(kPathRole, row["path"].toString());
                    item->setData(kRoleRole, role);
                    item->setToolTip(row["path"].toString());
                    item->setForeground(role == QLatin1String("cartridge") ? QColor("#f0c785") : role == QLatin1String("spare") ? QColor("#8c9ca3") : QColor("#9fe3d4"));
                    if (row["path"].toString() == _selectedMagazinePiece) {
                        _magazinePieceList->setCurrentItem(item);
                        selectedRole = role;
                    }
                }
            }
            showRole(_magazinePieceRole, selectedRole);
            const QSignalBlocker presentationSignals(_magazinePresentation);
            _magazinePresentation->button(_magazineShowsEmpty ? 1 : 0)->setChecked(true);
            _magazinePieceList->setEnabled(ready);
            _magazineRestorePieces->setEnabled(ready && (_magazineSetup.has_value() || !_magazineRounds.value(magazineRoundsKey()).isEmpty()));
            _magazinePiecesStatus->setText(!ready ? waiting
                                           : _magazineSetup ? QStringLiteral("This configuration's saved magazine setup decides every piece.")
                                                            : QStringLiteral("Round marks apply to every configuration that uses this magazine."));
        }
        {
            const QSignalBlocker emptySignals(_magazineEmptyBody);
            const QSignalBlocker motionSignals(_magazineMotion);
            const QSignalBlocker movementSignals(_magazineMovement);
            _magazineEmptyBody->clear();
            _magazineMovement->clear();
            _magazineMovement->addItem(QStringLiteral("Choose a saved movement…"), QString());
            QJsonObject state;
            if (ready) {
                try {
                    state = magazineSetupState();
                } catch (const std::exception&) {
                }
            }
            const auto settings = state["settings"].toObject();
            for (const auto& value : state["tracks"].toArray()) {
                const auto track = value.toObject();
                _magazineEmptyBody->addItem(track["name"].toString() + (track["index"] == state["fullTrack"] ? QStringLiteral(" · shared with the full magazine") : QString()),
                                            track["index"].toInt());
            }
            _magazineEmptyBody->setCurrentIndex(_magazineEmptyBody->findData(settings["emptyTrack"].toInt()));
            for (const auto& value : state["movements"].toArray()) {
                const auto movement = value.toObject();
                _magazineMovement->addItem(movement["name"].toString(), movement["id"].toString());
            }
            _magazineMotion->setCurrentIndex(std::max(0, _magazineMotion->findData(settings["motion"].toString())));
            _magazineMovement->setCurrentIndex(std::max(0, _magazineMovement->findData(settings["movementId"].toString())));
            const bool editable = !state.isEmpty();
            _magazineEmptyBody->setEnabled(editable);
            _magazineMotion->setEnabled(editable);
            _magazineMovement->setEnabled(editable && settings["motion"].toString() == QLatin1String("manual"));
        }

        // Insertion guide: the prepared rail's values, the direction in the
        // connection's frame.
        {
            QVector3D local;
            if (_magazineRail) {
                Transformf inverse;
                if (invertUniformTransform(_magazineRail->connection, inverse)) {
                    const auto axis = rotated(inverse.rotation, { _magazineRail->outward.x(), _magazineRail->outward.y(), _magazineRail->outward.z() });
                    local = QVector3D(axis.x, axis.y, axis.z).normalized();
                }
            }
            for (int axis = 0; axis < 3; ++axis) {
                const QSignalBlocker block(_guideDirection[axis]);
                _guideDirection[axis]->setValue(local[axis]);
            }
            const QSignalBlocker travel(_guideTravel), pickup(_guidePickup), release(_guideRelease);
            _guideTravel->setValue(_magazineRail ? _magazineRail->travel : 0);
            _guidePickup->setValue(_magazineRail ? _magazineRail->captureRadius : 0);
            _guideRelease->setValue(_magazineRail ? _magazineRail->releaseMargin : 0);
            const bool saved = !authoringPlan().value("magazineGuide").toObject().isEmpty();
            for (auto* spin : { _guideDirection[0], _guideDirection[1], _guideDirection[2], _guideTravel, _guidePickup, _guideRelease }) {
                spin->setEnabled(ready);
            }
            _guideTest->setEnabled(ready && _simulation.action == SimulationAction::Idle);
            _guideRestore->setEnabled(_simulationActive && saved);
            _guideStatus->setText(!ready  ? waiting
                                  : saved ? QStringLiteral("Your guide. Save keeps it with the weapon; Restore source guide returns to the reload's own.")
                                          : QStringLiteral("Read from the reload's animation."));
        }
    }
}
