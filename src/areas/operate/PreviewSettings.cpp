// Preview and Operate settings saved with the project: slide spring return,
// weapon-fixed motion, approximate clip transitions and arms shown. They are
// document fields; their controls only show them and report user changes.
// Each setter does what the control's change used to trigger, and only when
// the value changes.
#include "ReloadMakerController.h"

#include <QCheckBox>
#include <QComboBox>
#include <QSignalBlocker>

namespace reloadmaker
{
    namespace
    {
        void showChecked(QCheckBox* box, bool checked)
        {
            if (box) {
                const QSignalBlocker block(box);
                box->setChecked(checked);
            }
        }
    }

    QString ReloadMakerController::motionModeName() const
    {
        return _weaponFixedMotion ? QStringLiteral("Weapon fixed") : QStringLiteral("Full native motion");
    }

    void ReloadMakerController::setSlideSpringReturn(bool enabled)
    {
        if (enabled == _slideSpringReturn) {
            return;
        }
        _slideSpringReturn = enabled;
        showChecked(_simulationSpring, enabled);
        noteDocumentChange("slide spring return");
    }

    void ReloadMakerController::setWeaponFixedMotion(bool fixed)
    {
        if (fixed == _weaponFixedMotion) {
            return;
        }
        _weaponFixedMotion = fixed;
        showWeaponFixedMotion();
        refreshPoseMode();
        noteDocumentChange("motion mode");
    }

    void ReloadMakerController::showWeaponFixedMotion()
    {
        if (_motionMode) {
            const QSignalBlocker block(_motionMode);
            _motionMode->setCurrentIndex(_weaponFixedMotion ? 0 : 1);
        }
    }

    void ReloadMakerController::setApproximateTransitions(bool enabled)
    {
        if (enabled == _approximateTransitions) {
            return;
        }
        _approximateTransitions = enabled;
        showChecked(_transitionCheck, enabled);
        refreshPoseMode();
        noteDocumentChange("clip transitions");
    }

    void ReloadMakerController::setArmsShown(bool shown)
    {
        if (shown == _armsShown) {
            return;
        }
        _armsShown = shown;
        showChecked(_showArmsCheck, shown);
        setArmsVisible(shown);
        noteDocumentChange("arms shown");
    }

    // Re-poses the current clip after a motion or transition change.
    void ReloadMakerController::refreshPoseMode()
    {
        if (!_clip.valid()) {
            return;
        }
        _playbackTime = std::min(_playbackTime, playbackDuration());
        if (applyPose(_playbackTime)) {
            updateTimelineText();
            showClipDetails();
            setStatus(QStringLiteral("%1 — %2 — %3 s; offline clip preview")
                          .arg(_activeAction, motionModeName())
                          .arg(playbackDuration(), 0, 'f', 3));
        }
    }
}
