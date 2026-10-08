#include "ReloadMakerController.h"
#include "commands/CommandSupport.h"
#include "MagazineGuide.h"

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
                            // edit: the panel's typed guide on Reload builder > Magazines,
                            // applied to the prepared rail without preparing it again.
                            if (params["operation"] == "edit") {
                                require(_workspace == MagazinesWorkspace, "Open Reload builder > Magazines (workspace 10) first");
                                MagazineGuide guide;
                                QString error;
                                auto settings = params["settings"].toObject();
                                settings["version"] = 2;
                                require(readMagazineGuide(settings, guide, error), error);
                                applyMagazineGuideEdit(guide);
                                return magazineGuideState();
                            }
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
                            // role: one piece's role as the Pieces step sets it (a round
                            // choice where the magazine owns it, else the setup).
                            if (params["operation"] == "role") {
                                require(_workspace == MagazinesWorkspace, "Open Reload builder > Magazines (workspace 10) first");
                                setMagazinePieceRole(params["path"].toString(), params["role"].toString());
                                return magazineSetupState();
                            }
                            return configureMagazineSetup(params);
                        } });

        _commands.add({ "magazine_rounds_state", CommandAccess::Read, CommandGate::Always,
                        [this](const QJsonObject&) -> std::optional<QJsonObject> { return magazineRoundsState(); } });

        _commands.add({ "magazine_rounds", CommandAccess::Edit, CommandGate::Always,
                        [this](const QJsonObject& params) -> std::optional<QJsonObject> {
                            return configureMagazineRounds(params);
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

        // Applies a named finger pose the way the magazine editor's "Apply pose"
        // button does.
        _commands.add({ "apply_magazine_pose", CommandAccess::Edit, CommandGate::WhenIdle,
                        [this](const QJsonObject& params) -> std::optional<QJsonObject> {
                            const auto pose = knownHandPoseId(params["poseId"].toString());
                            require(!pose.isEmpty(), "Unknown named finger pose");
                            require(_workspace == GripsWorkspace && _handEditorPage == HandEditorPage::PartGrip,
                                    "Configure the magazine first");
                            setGripPose(pose);
                            require(canStartPartGrip(), "Wait for the magazine editor");
                            applyGripPoseToDraft();
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
