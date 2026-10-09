// Reload builder › Magazines: the pieces of the magazine and its insertion
// guide, edited in the viewport on Operate's simulation without its drag
// controls (owner, 2026-10-08: an in-builder mode, not configuration
// dialogs). Hand fit and World models are the existing magazine grip editor
// and collisions page, shown as the step's other sub-steps.
//
// Piece roles go where they are owned: a round mark on the full magazine is
// the magazine's round choice (every configuration using it); spares, the
// empty body's pieces and the advanced presentation are this configuration's
// saved magazine setup, which then decides every role. The guide is applied
// to the prepared rail while a handle moves, which is exactly what preparing
// with the saved guide produces, and written to the authoring record when the
// drag ends or a value is typed.
#include "ReloadMakerController.h"
#include "MagazineGuide.h"
#include "MagazineViewport.h"
#include "ReloadDriveViewport.h"
#include "StudioWindow.h"
#include "design/Theme.h"
#include "glview.h"
#include "model/nifmodel.h"
#include "viewport/MagazineAuthoringViewport.h"

#include <QAbstractButton>
#include <QButtonGroup>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QJsonArray>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTimer>

#include <algorithm>
#include <stdexcept>

namespace reloadmaker
{
    namespace
    {
        // Lengths the guide accepts (readMagazineGuide): positive, below 1000
        // units, pickup below the travel and release at most the travel.
        constexpr float kGuideMinimum = .05f;
        constexpr float kGuideMaximumTravel = 999.f;
        constexpr float kPickupShare = .95f;

        QColor roleColor(const QString& role)
        {
            if (role == QLatin1String("cartridge")) {
                return QColor("#f0c785");
            }
            if (role == QLatin1String("spare")) {
                return QColor::fromRgba(design::color::textFaint);
            }
            return QColor("#6cc7b4");
        }

        QString roleName(const QString& role)
        {
            return role == QLatin1String("cartridge") ? QStringLiteral("Round") : role == QLatin1String("spare") ? QStringLiteral("Animation only") : QStringLiteral("Body");
        }

        [[noreturn]] void fail(const QString& message)
        {
            throw std::runtime_error(message.toStdString());
        }

        Vec3f vec(QVector3D v) { return { v.x(), v.y(), v.z() }; }
        QVector3D qvec(Vec3f v) { return { v.x, v.y, v.z }; }
    }

    std::optional<ReloadMakerController::MagazineStep> ReloadMakerController::magazineStep() const
    {
        if (_workspace == MagazinesWorkspace) {
            // Hand fit may be waiting here for the physical magazine to finish
            // preparing. Keep that requested page through rail refreshes.
            return _magazineStep;
        }
        if (_workspace == CollisionsWorkspace) {
            return MagazineStep::WorldModels;
        }
        if (_workspace == GripsWorkspace && _handEditorPage == HandEditorPage::PartGrip && _editingMagazine) {
            return MagazineStep::HandFit;
        }
        return std::nullopt;
    }

    void ReloadMakerController::showMagazineStep(MagazineStep step)
    {
        _magazineStep = step;
        switch (step) {
        case MagazineStep::Pieces:
        case MagazineStep::Guide:
            if (_workspace != MagazinesWorkspace) {
                setWorkspace(MagazinesWorkspace);
            } else {
                presentMagazineStep();
            }
            break;
        case MagazineStep::HandFit:
            // The fit is edited on the magazine this page prepares, held where
            // the hand grabs it.
            if (!_magazineFitPreview) {
                if (!simulationWorkspace(_workspace)) {
                    setWorkspace(MagazinesWorkspace);
                }
                if (_simulationActive && !_magazineFitPreview) {
                    (void)editMagazineGrip();
                }
            }
            break;
        case MagazineStep::WorldModels:
            setWorkspace(CollisionsWorkspace);
            break;
        }
        syncRail();
    }

    // Leaves the magazine's hand fit for another part's grip.
    void ReloadMakerController::leaveMagazineGripEditing()
    {
        rememberMagazineGrip();
        _editingMagazine = false;
        if (_magazineFitPreview) {
            stopSimulation();
        }
        restoreHandPosePreview();
        updatePartGripControls();
    }

    // The page's view: Operate's drag controls stay off, the magazine waits at
    // the end of its guide (empty when Pieces shows the empty magazine), and
    // the panel and viewport tools follow.
    void ReloadMakerController::presentMagazineStep()
    {
        if (_workspace != MagazinesWorkspace) {
            return;
        }
        if (_magazineStep == MagazineStep::HandFit && _simulationActive) {
            (void)editMagazineGrip();
            return;
        }
        if (_simulationDrive) {
            _simulationDrive->setActive(false, false);
        }
        if (_simulationActive && _magazineRail && !_simulation.magazine.carried()) {
            presentMagazineOutside(_magazineStep == MagazineStep::Pieces && _magazineShowsEmpty);
        }
        refreshMagazinesUi();
        updateMagazineAuthoring();
    }

    void ReloadMakerController::updateMagazineAuthoring()
    {
        if (!_magazineAuthoring) {
            return;
        }
        const bool here = _workspace == MagazinesWorkspace && _simulationActive && _magazineRail && !_loadThread;
        if (_workspace == MagazinesWorkspace && _magazineViewport) {
            // Only a test grab carries the magazine here.
            _magazineViewport->setActive(_simulationActive && _simulation.magazine.carried());
        }
        QVector<MagazinePieceMark> pieces;
        if (here) {
            auto* model = activeModel();
            for (const auto& visual : _magazineVisuals) {
                if (!model || !visual.node.isValid() || magazineVisualHidden(visual)) {
                    continue;
                }
                const auto role = visual.nativeOnly ? QStringLiteral("spare") : visual.cartridge ? QStringLiteral("cartridge") : QStringLiteral("body");
                pieces.append({ model->getBlockNumber(visual.node), roleColor(role), magazinePiecePath(visual.node) == _selectedMagazinePiece });
            }
        }
        _magazineAuthoring->setPieces(std::move(pieces));
        _magazineAuthoring->setGuide(here ? magazineGuideShape() : std::nullopt);
        _magazineAuthoring->setMode(!here ? MagazineAuthoringViewport::Mode::Off
                                    : _magazineStep == MagazineStep::Guide ? MagazineAuthoringViewport::Mode::Guide
                                                                           : MagazineAuthoringViewport::Mode::Pieces);
    }

    void ReloadMakerController::presentMagazineOutside(bool empty)
    {
        if (!_magazineRail || !_simulationActive) {
            return;
        }
        _simulationTimer->stop();
        _simulation.reset();
        _simulation.magazine.phase = MagazinePhase::OutsideEmpty;
        _simulation.magazine.distance = _magazineRail->travel;
        _simulation.magazine.offset = magazineInputOffset(*_magazineRail, _magazineRail->travel);
        _simulation.magazineRounds = empty ? 0 : _simulationAmmo.capacity;
        updateMagazinePose();
    }

    QJsonArray ReloadMakerController::magazinePieceRows() const
    {
        QJsonArray rows;
        auto* model = activeModel();
        if (!model || !_magazineRail) {
            return rows;
        }
        const QModelIndex carrier = _sceneNodes[_reloadTrackNodes[_magazineRail->fullTrack]];
        for (const auto& visual : _magazineVisuals) {
            if (!visual.node.isValid()) {
                continue;
            }
            const auto role = visual.nativeOnly ? QStringLiteral("spare") : visual.cartridge ? QStringLiteral("cartridge") : QStringLiteral("body");
            rows.append(QJsonObject{
                { "path", magazinePiecePath(visual.node) },
                { "roundPath", visual.empty || visual.nativeOnly ? QString() : magazineRoundPath(visual.node, carrier) },
                { "name", model->get<QString>(visual.node, "Name") },
                { "role", role },
                { "empty", visual.empty },
                { "block", model->getBlockNumber(visual.node) },
            });
        }
        return rows;
    }

    void ReloadMakerController::setMagazinePieceRole(const QString& path, const QString& role)
    {
        {
            if (role != QLatin1String("body") && role != QLatin1String("cartridge") && role != QLatin1String("spare")) {
                fail(QStringLiteral("Choose body, cartridge or spare"));
            }
            QJsonObject row;
            for (const auto& value : magazinePieceRows()) {
                if (value.toObject()["path"].toString() == path) {
                    row = value.toObject();
                }
            }
            if (row.isEmpty()) {
                fail(QStringLiteral("Select a piece of this magazine first"));
            }
            if (row["role"].toString() == role) {
                return;
            }
            _selectedMagazinePiece = path;
            const auto roundPath = row["roundPath"].toString();
            if (!_magazineSetup && role != QLatin1String("spare") && !roundPath.isEmpty()) {
                (void)configureMagazineRounds({ { "operation", "save" }, { "rounds", QJsonObject{ { roundPath, role == QLatin1String("cartridge") } } } });
            } else {
                const auto state = magazineSetupState();
                auto settings = state["settings"].toObject();
                QJsonArray pieces;
                for (const auto& value : state["pieces"].toArray()) {
                    const auto piece = value.toObject();
                    const auto piecePath = piece["path"].toString();
                    pieces.append(QJsonObject{ { "path", piecePath }, { "role", piecePath == path ? role : piece["role"].toString() } });
                }
                settings["pieces"] = pieces;
                (void)configureMagazineSetup({ { "operation", "save" }, { "settings", settings } });
            }
            setStatus(QStringLiteral("%1 is now %2. Recapture the magazine's world models before the next pack save.")
                          .arg(row["name"].toString(), roleName(role).toLower()));
        }
        refreshMagazinesUi();
    }

    // Empty body and visible movement: this configuration's saved setup, with
    // every piece keeping its current role.
    void ReloadMakerController::setMagazineAdvanced(const QJsonObject& changes)
    {
        try {
            const auto state = magazineSetupState();
            auto settings = state["settings"].toObject();
            for (auto it = changes.begin(); it != changes.end(); ++it) {
                settings[it.key()] = it.value();
            }
            QJsonArray pieces;
            for (const auto& value : state["pieces"].toArray()) {
                const auto piece = value.toObject();
                pieces.append(QJsonObject{ { "path", piece["path"] }, { "role", piece["role"] } });
            }
            settings["pieces"] = pieces;
            (void)configureMagazineSetup({ { "operation", "save" }, { "settings", settings } });
        } catch (const std::exception& e) {
            setStatus(QString::fromUtf8(e.what()), true);
        }
        refreshMagazinesUi();
    }

    // Back to what Maker read: the setup and the round choices are removed.
    void ReloadMakerController::restoreMagazinePieces()
    {
        try {
            if (_magazineSetup) {
                (void)configureMagazineSetup({ { "operation", "reset" } });
            }
            if (!magazineRoundsState()["chosen"].toObject().isEmpty()) {
                (void)configureMagazineRounds({ { "operation", "reset" } });
            }
            setStatus(QStringLiteral("Back to the pieces read from the reload. Recapture the magazine's world models before the next pack save."));
        } catch (const std::exception& e) {
            setStatus(QString::fromUtf8(e.what()), true);
        }
        refreshMagazinesUi();
    }

    std::optional<GuideShape> ReloadMakerController::magazineGuideShape() const
    {
        if (!_magazineRail) {
            return std::nullopt;
        }
        const auto& rail = *_magazineRail;
        return GuideShape{ rail.handle, rail.outward, rail.travel, rail.captureRadius, rail.releaseMargin };
    }

    // Applies a guide to the prepared rail, and on commit to the authoring
    // record, where Save keeps it. Lengths are held inside the guide's limits.
    void ReloadMakerController::editMagazineGuide(const GuideShape& shape, bool commit)
    {
        if (!_magazineRail || !_reloadRecipe || _simulation.magazine.carried()) {
            return;
        }
        Transformf inverse;
        if (!invertUniformTransform(_magazineRail->connection, inverse) || shape.outward.lengthSquared() < 1e-8f) {
            setStatus(QStringLiteral("The magazine connection frame is invalid; the guide cannot be edited."), true);
            return;
        }
        MagazineGuide guide;
        guide.recipeId = _reloadRecipe->id;
        guide.outwardInConnection = qvec(rotated(inverse.rotation, vec(shape.outward.normalized()))).normalized();
        guide.travel = std::clamp(shape.travel, kGuideMinimum * 2, kGuideMaximumTravel);
        guide.captureRadius = std::clamp(shape.pickup, kGuideMinimum, guide.travel * kPickupShare);
        guide.releaseMargin = std::clamp(shape.release, kGuideMinimum, guide.travel);
        const auto before = magazineGuideJson(MagazineGuide{ _reloadRecipe->id,
                                                             qvec(rotated(inverse.rotation, vec(_magazineRail->outward))).normalized(),
                                                             _magazineRail->travel, _magazineRail->captureRadius, _magazineRail->releaseMargin });
        if (_guideEditStart.isEmpty()) {
            _guideEditStart = before;
        }
        auto rail = *_magazineRail;
        QString error;
        if (!applyMagazineGuide(guide, rail, error, _magazineSetup && _magazineSetup->motion == "linear")) {
            setStatus(error, true);
            return;
        }
        _magazineRail = rail;
        presentMagazineOutside(false);
        if (commit) {
            const auto json = magazineGuideJson(guide);
            if (json != _guideEditStart && authoringPlanField(QStringLiteral("magazineGuide")).value.toObject() != json) {
                writePlanField(QStringLiteral("magazineGuide"), json);
                noteDocumentChange(QStringLiteral("magazine guide"));
            }
            _guideEditStart = {};
            refreshMagazinesUi();
        }
        updateMagazineAuthoring();
    }

    void ReloadMakerController::applyMagazineGuideEdit(const MagazineGuide& guide)
    {
        if (!_reloadRecipe || magazineSettingsBusy()) {
            fail(QStringLiteral("Wait for the magazine's current source capture"));
        }
        if (guide.outwardInConnection.lengthSquared() < 1e-8f) {
            fail(QStringLiteral("Give the direction at least one nonzero component."));
        }
        if (!_magazineRail) {
            // No rail to edit live: the guide is saved and the magazine prepared again.
            auto saved = guide;
            saved.outwardInConnection.normalize();
            (void)configureMagazineGuide({ { "operation", "save" }, { "settings", magazineGuideJson(saved) } });
            return;
        }
        if (_simulation.magazine.carried()) {
            fail(QStringLiteral("Finish the test grab first"));
        }
        const auto outward = qvec(rotated(_magazineRail->connection.rotation, vec(guide.outwardInConnection.normalized()))).normalized();
        _guideEditStart = {};
        editMagazineGuide({ _magazineRail->handle, outward, guide.travel, guide.captureRadius, guide.releaseMargin }, true);
    }

    // The typed direction is in the magazine connection's frame.
    void ReloadMakerController::typeMagazineGuide()
    {
        if (!_reloadRecipe) {
            return;
        }
        try {
            applyMagazineGuideEdit({ _reloadRecipe->id,
                                     QVector3D(float(_guideDirection[0]->value()), float(_guideDirection[1]->value()), float(_guideDirection[2]->value())),
                                     float(_guideTravel->value()), float(_guidePickup->value()), float(_guideRelease->value()) });
        } catch (const std::exception& e) {
            _guideStatus->setText(QString::fromUtf8(e.what()));
            return;
        }
        refreshMagazinesUi();
    }

    // Grabs the magazine at the end of the guide; the mouse carries it as in
    // Operate until it is seated, dropped or Esc cancels.
    void ReloadMakerController::testMagazineGuide()
    {
        if (!_simulationActive || !_magazineRail) {
            return;
        }
        presentMagazineOutside(false);
        _magazineViewport->setActive(true);
        _magazineViewport->beginCarry();
        runSimulationAction(QStringLiteral("grab_magazine"));
        updateMagazineAuthoring();
    }
}
