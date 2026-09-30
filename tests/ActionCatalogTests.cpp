#include "ActionCatalog.h"
#include <QTextStream>
#include <stdexcept>

using namespace reloadmaker;

namespace {
void require(bool value, const QString& message) {
    if (!value) throw std::runtime_error(message.toStdString());
}
void expect(const QString& clip, ActionFamily family, const QString& label, bool automatic = false, bool baseReload = false) {
    const auto result = classifyWeaponClip(clip);
    require(result.has_value(), clip + " was not classified");
    require(result->family == family && result->label == label && result->automatic == automatic && result->baseReload == baseReload,
            clip + " classified as " + result->label);
}
void reject(const QString& clip) {
    require(!classifyWeaponClip(clip).has_value(), clip + " must not be an authoring action");
}
}

int main() {
    try {
        expect("WPNReload", ActionFamily::Reload, "Reload", false, true);
        expect("wpnreload", ActionFamily::Reload, "Reload", false, true);
        expect("WPNReloadNoExtender", ActionFamily::Reload, "Reload — no extender");
        expect("WPNReloadStart", ActionFamily::Reload, "Reload — start");
        expect("WPNFireSingleReady", ActionFamily::Fire, "Fire — single");
        expect("WPNFireSingleReadyA", ActionFamily::Fire, "Fire — single · alternate A");
        expect("WPNFireAutoReadyForward", ActionFamily::Fire, "Fire — automatic", true);
        expect("WPNBoltChargeReady", ActionFamily::Cycle, "Cycle — bolt / charge");
        expect("WPNEquip", ActionFamily::Equip, "Equip");
        expect("WPNEquipFast", ActionFamily::Equip, "Equip — fast");
        for (const char* clip : {"WPNUnEquip", "WPNIdleSighted", "WPNBoltChargeSighted", "WPNFireSingleSighted", "WPNFireAutoReadyBack",
                                 "WPNWalkForwardReady", "WPNRunForwardReady", "WPNSprint", "WPNMelee", "WPNMeleeBayonet",
                                 "WPNGrenadeThrow", "WPNMineThrow", "WPNReloadSighted", "WPNAfterJiggleAdd", "WPNIdleReadyA"})
            reject(clip);
        QTextStream(stdout) << "ActionCatalog tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        QTextStream(stderr) << "FAILED: " << e.what() << '\n';
        return 1;
    }
}
