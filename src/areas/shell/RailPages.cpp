// Rail pages (owner, 2026-10-04). The rail groups the persisted workspaces:
// Operate only works the parts the reload already has; Reload builder holds
// every step that builds the reload except its sounds; Poses library is where
// finger poses are harvested from any reload for later use. Weapon collision
// is a separate menu tool, not a rail page. The workspace id and the hand
// editor page stay the saved and MCP identity, so the rail is always derived
// from them and never stored.
#include "ReloadMakerController.h"

#include <QAbstractButton>
#include <QAction>
#include <QButtonGroup>
#include <QDockWidget>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTabBar>

#include "design/DesignWidgets.h"

namespace reloadmaker
{
    std::optional<ReloadMakerController::RailPage> ReloadMakerController::railPage() const
    {
        switch (_workspace) {
        case OperateWorkspace:
            return RailPage::Operate;
        case SoundsWorkspace:
            return RailPage::Sounds;
        case ScopesWorkspace:
            return RailPage::Scopes;
        case GripsWorkspace:
            return _handEditorPage == HandEditorPage::FingerPoses ? RailPage::Poses : RailPage::Builder;
        case PhasesWorkspace:
        case CollisionsWorkspace:
            return RailPage::Builder;
        default:
            return std::nullopt;
        }
    }

    std::optional<ReloadMakerController::BuilderStep> ReloadMakerController::builderStep() const
    {
        if (_workspace == PhasesWorkspace) {
            return BuilderStep::Reload;
        }
        if (_workspace == CollisionsWorkspace) {
            return BuilderStep::Magazines;
        }
        if (_workspace != GripsWorkspace) {
            return std::nullopt;
        }
        switch (_handEditorPage) {
        case HandEditorPage::PartGrip:
            return BuilderStep::PartGrips;
        case HandEditorPage::Mechanism:
            return BuilderStep::Mechanisms;
        case HandEditorPage::GrabLocations:
            return BuilderStep::GrabPoints;
        default:
            return std::nullopt;
        }
    }

    // Each page opens the way its own entry points always have: the
    // workspace first, then the hand editor page.
    void ReloadMakerController::showRailPage(RailPage page)
    {
        switch (page) {
        case RailPage::Operate:
            setWorkspace(OperateWorkspace);
            break;
        case RailPage::Builder:
            showBuilderStep(_lastBuilderStep);
            break;
        case RailPage::Poses: {
            // Entering the hand editor briefly shows its previous builder
            // page; that is not a step the author chose.
            const auto builder = _lastBuilderStep;
            setWorkspace(GripsWorkspace);
            if (_workspace == GripsWorkspace) {
                setHandEditorPage(HandEditorPage::FingerPoses);
            }
            _lastBuilderStep = builder;
            break;
        }
        case RailPage::Sounds:
            setWorkspace(SoundsWorkspace);
            break;
        case RailPage::Scopes:
            setWorkspace(ScopesWorkspace);
            break;
        }
        syncRail();
    }

    void ReloadMakerController::showChecklist()
    {
        showBuilderStep(builderStep().value_or(_lastBuilderStep));
        if (_leftTabs) {
            _leftTabs->setCurrentIndex(0);
        }
    }

    void ReloadMakerController::showBuilderStep(BuilderStep step)
    {
        const auto handStep = [this](HandEditorPage page) {
            setWorkspace(GripsWorkspace);
            if (_workspace == GripsWorkspace) {
                setHandEditorPage(page);
            }
        };
        switch (step) {
        case BuilderStep::Reload:
            setWorkspace(PhasesWorkspace);
            break;
        case BuilderStep::PartGrips:
            handStep(HandEditorPage::PartGrip);
            break;
        case BuilderStep::Mechanisms:
            handStep(HandEditorPage::Mechanism);
            break;
        case BuilderStep::GrabPoints:
            (void)openGrabLocations();
            break;
        case BuilderStep::Magazines:
            setWorkspace(CollisionsWorkspace);
            break;
        }
        syncRail();
    }

    // Shows the page and step the current workspace belongs to; a refused
    // change (for example while a source loads) shows the real page again.
    // The rail opens pages on any check (mouse, keyboard or accessibility),
    // so reflecting the state here must not emit.
    void ReloadMakerController::syncRail()
    {
        if (!_railButtons) {
            return;
        }
        const QSignalBlocker railSignals(_railButtons);
        const QSignalBlocker stepSignals(_builderStepButtons);
        const auto page = railPage();
        if (page) {
            _railButtons->button(int(*page))->setChecked(true);
        } else if (auto* checked = _railButtons->checkedButton()) {
            _railButtons->setExclusive(false);
            checked->setChecked(false);
            _railButtons->setExclusive(true);
        }
        if (const auto step = builderStep()) {
            _lastBuilderStep = *step;
            _builderStepButtons->button(int(*step))->setChecked(true);
        }
        // The builder shows its checklist beside the loadout; it opens on the
        // checklist whenever the builder is entered.
        const bool builder = page == RailPage::Builder;
        if (_leftTabs) {
            const bool wasBuilder = !_leftTabs->isHidden();
            const QSignalBlocker tabSignals(_leftTabs);
            if (builder && !wasBuilder) {
                _leftTabs->setCurrentIndex(0);
            }
            _leftTabs->setVisible(builder);
            _leftStack->setCurrentIndex(builder ? _leftTabs->currentIndex() : 1);
        }
        if (_handPoseHeader) {
            const QString title = page == RailPage::Poses                          ? QStringLiteral("Capture and reuse hand poses")
                                  : _handEditorPage == HandEditorPage::PartGrip    ? QStringLiteral("Part grips")
                                  : _handEditorPage == HandEditorPage::Mechanism   ? QStringLiteral("Mechanisms")
                                                                                   : QStringLiteral("Grab points");
            _handPoseHeader->setSection(page == RailPage::Poses ? QStringLiteral("Poses library") : QStringLiteral("Reload builder"));
            _handPoseHeader->setTitle(title);
            _handPoseDock->setWindowTitle(title);
        }
        // The open checklist section follows the builder step.
        refreshChecklistUi();
    }
}
