#include "ReloadMakerController.h"
#include "commands/CommandSupport.h"

#include <QComboBox>
#include <QPushButton>
#include <QTabWidget>

namespace reloadmaker
{
    using commands::require;

    void ReloadMakerController::registerMagazineCommands()
    {
        _commands.add({ "magazine_guide_state", CommandAccess::Read, CommandGate::Always,
                        [this](const QJsonObject&) -> std::optional<QJsonObject> { return magazineGuideState(); } });

        _commands.add({ "magazine_guide", CommandAccess::Edit, CommandGate::Always,
                        [this](const QJsonObject& params) -> std::optional<QJsonObject> {
                            return configureMagazineGuide(params);
                        } });

        _commands.add({ "magazine_reference_state", CommandAccess::Read, CommandGate::Always,
                        [this](const QJsonObject&) -> std::optional<QJsonObject> { return magazineReferenceState(); } });

        _commands.add({ "magazine_reference", CommandAccess::Edit, CommandGate::Always,
                        [this](const QJsonObject& params) -> std::optional<QJsonObject> {
                            return configureMagazineReference(params);
                        } });

        _commands.add({ "magazine_setup_state", CommandAccess::Read, CommandGate::Always,
                        [this](const QJsonObject&) -> std::optional<QJsonObject> { return magazineSetupState(); } });

        _commands.add({ "magazine_setup", CommandAccess::Edit, CommandGate::Always,
                        [this](const QJsonObject& params) -> std::optional<QJsonObject> {
                            return configureMagazineSetup(params);
                        } });

        _commands.add({ "capture_magazine_world", CommandAccess::Edit, CommandGate::Always,
                        [this](const QJsonObject& params) -> std::optional<QJsonObject> {
                            require(params["empty"].isBool(), "Choose the full or empty presentation");
                            return captureMagazineWorld(params["empty"].toBool());
                        } });

        _commands.add({ "use_magazine_grip", CommandAccess::Edit, CommandGate::WhenIdle,
                        [this](const QJsonObject& params) -> std::optional<QJsonObject> {
                            const auto scope = params["scope"].toString("magazine");
                            require(scope == "magazine" || scope == "weapon_default",
                                    "Scope must be magazine or weapon_default");
                            require(useMagazineGrabGrip(scope == "weapon_default"), _lastError);
                            return std::nullopt;
                        } });

        _commands.add({ "configure_magazine", CommandAccess::Edit, CommandGate::WhenIdle,
                        [this](const QJsonObject& params) -> std::optional<QJsonObject> {
                            require(editMagazineGrip(params["gripId"].toString()), _lastError);
                            return std::nullopt;
                        } });

        _commands.add({ "use_default_magazine_grip", CommandAccess::Edit, CommandGate::WhenIdle,
                        [this](const QJsonObject&) -> std::optional<QJsonObject> {
                            require(useDefaultMagazineGrip(), _lastError);
                            return std::nullopt;
                        } });

        // Applies a named finger pose through the magazine editor's own control,
        // so the result matches choosing it in the interface.
        _commands.add({ "apply_magazine_pose", CommandAccess::Edit, CommandGate::WhenIdle,
                        [this](const QJsonObject& params) -> std::optional<QJsonObject> {
                            const int index = _gripPoseCombo->findData(params["poseId"].toString());
                            require(index >= 0, "Unknown named finger pose");
                            require(_workspace == GripsWorkspace && _handEditorPage == HandEditorPage::PartGrip,
                                    "Configure the magazine first");
                            _gripPoseCombo->setCurrentIndex(index);
                            require(_gripCurrentStart->isEnabled(), "Wait for the magazine editor");
                            _gripCurrentStart->click();
                            require(_partGripDraft.has_value(), _lastError);
                            return std::nullopt;
                        } });

        _commands.add({ "mirror_magazine_grip", CommandAccess::Edit, CommandGate::WhenIdle,
                        [this](const QJsonObject&) -> std::optional<QJsonObject> {
                            require(mirrorMagazineGrip(), _lastError);
                            return std::nullopt;
                        } });
    }
}
