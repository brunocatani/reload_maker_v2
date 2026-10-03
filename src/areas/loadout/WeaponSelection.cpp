// Which weapon and arms body are being authored. Both are document fields:
// the source bar, the coverage dialog and MCP change the weapon through
// selectWeapon(); session restores assign the fields directly.
#include "ReloadMakerController.h"

#include <QComboBox>
#include <QSignalBlocker>

namespace reloadmaker
{
    int ReloadMakerController::validWeaponIndex(int index) const
    {
        return index >= 0 && index < _weaponSelections.size() ? index : -1;
    }

    void ReloadMakerController::selectWeapon(int index)
    {
        index = validWeaponIndex(index);
        if (index == _selectedWeapon) {
            return;
        }
        _selectedWeapon = index;
        alignPluginToWeapon();
        // Switching to a weapon with its own saved project reopens that
        // project instead of reassembling here.
        if (!beginWeaponProject(index)) {
            refreshWeaponPicker();
            refreshAttachments();
            scheduleRebuild();
        }
        noteDocumentChange("weapon selection");
    }

    bool ReloadMakerController::isArmsBody(const QString& body)
    {
        return body == QLatin1String("male") || body == QLatin1String("female");
    }

    void ReloadMakerController::showArmsBody()
    {
        if (!_bodyCombo) {
            return;
        }
        const QSignalBlocker block(_bodyCombo);
        _bodyCombo->setCurrentIndex(_bodyCombo->findData(_armsBody));
    }
}
