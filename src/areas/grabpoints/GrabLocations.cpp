// Grab locations: the points on the operating part where a hand may grab
// it in game (nearest allowed point wins). They are kept per weapon,
// operating group and part model, follow the animated part, and are
// exported with the interaction grips.
#include "ReloadMakerController.h"
#include "GripViewport.h"

#include <QJsonArray>
#include <QUuid>
#include <algorithm>
#include <stdexcept>

namespace reloadmaker
{
    namespace
    {
        [[noreturn]] void fail(const QString& message)
        {
            throw std::runtime_error(message.toStdString());
        }
    }

    // The reload is captured where it is consumed: the reload page, Operate
    // and Sounds, and the grab locations page.
    bool ReloadMakerController::reloadCaptureAllowed() const noexcept
    {
        return _reloadWorkspaceActive || simulationWorkspace(_workspace) || (_workspace == GripsWorkspace && _handEditorPage == HandEditorPage::GrabLocations);
    }

    // The operating part's saved locations, or an empty set for it.
    OperatingGrabLocations ReloadMakerController::currentGrabLocations() const
    {
        const auto a = operatingAnchor();
        if (a < 0 || !currentWeapon()) {
            return {};
        }
        const auto& source = _handPoseAnchors[a].description;
        OperatingGrabLocations g{ poseRecordKey(currentWeapon()->formId), operatingGroup(), source.ownerModel, source.anchorPath, source.anchorName, {} };
        return _grabLocations.value(grabLocationsKey(g), g);
    }

    // Selecting a location shows its fields and its marker.
    void ReloadMakerController::selectGrabLocation(int index)
    {
        if (index == _selectedGrabLocation) {
            return;
        }
        _selectedGrabLocation = index;
        showSelectedGrabLocation();
        refreshGrabLocationsUi();
        updateGripViewport();
        noteDocumentChange("selected grab location");
    }

    // The selection stays within the current locations; none without any.
    void ReloadMakerController::normalizeGrabLocationSelection()
    {
        const auto count = int(currentGrabLocations().points.size());
        _selectedGrabLocation = count == 0 ? -1 : std::clamp(_selectedGrabLocation, 0, count - 1);
    }

    bool ReloadMakerController::openGrabLocations()
    {
        if (!currentWeapon() || _loadThread) {
            setStatus("Load a weapon first.", true);
            return false;
        }
        rememberMagazineGrip();
        setWorkspace(GripsWorkspace);
        setHandEditorPage(HandEditorPage::GrabLocations);
        prepareGrabLocationFrame();
        return true;
    }

    bool ReloadMakerController::ensureOperatingRig()
    {
        if (_loadThread || _reloadCapturing || _reloadCaptureQueued) {
            setStatus("Wait for the reload to finish learning.", true);
            return false;
        }
        if (!_reloadRecipe) {
            startReloadCapture();
            setStatus("Learning the reload first; try again when it finishes.", true);
            return false;
        }
        QString error;
        if (!buildOperatingRig(error)) {
            setStatus(error, true);
            return false;
        }
        return true;
    }

    // Builds the operating rig from the learned reload when it is missing.
    bool ReloadMakerController::buildOperatingRig(QString& error)
    {
        if (_simulationRig) {
            return true;
        }
        SimulationRig rig;
        if (!buildCurrentSimulationRig(rig, error)) {
            return false;
        }
        _simulationRig = rig;
        resolveOperatingFingers();
        return true;
    }

    // The operating part needs the learned reload and its rig.
    void ReloadMakerController::prepareGrabLocationFrame()
    {
        if (!ensureOperatingRig()) {
            return;
        }
        updateInteractionGripUi();
        updateGripViewport();
    }

    bool ReloadMakerController::beginGrabLocationPick(bool replace)
    {
        if (!openGrabLocations()) {
            return false;
        }
        if (operatingAnchor() < 0) {
            setStatus("Wait for the operating part to finish loading.", true);
            return false;
        }
        if (replace && _selectedGrabLocation < 0) {
            setStatus("Select a grab location to replace.", true);
            return false;
        }
        _replaceGrabLocation = replace;
        _pickingGrabLocation = true;
        _gripViewport->setPicking(true);
        showGrabLocationStatus("Click the exact surface location on " + currentGrabLocations().sourceName + ". Esc cancels.");
        return true;
    }

    // Adds a location at the clicked point on the operating part (at most
    // 32), or moves the selected one when replacing.
    bool ReloadMakerController::pickGrabLocation(QPointF pixel)
    {
        if (!_pickingGrabLocation || _loadThread) {
            return false;
        }
        int automatic = -1;
        Vec3f hit;
        QString surface;
        QString error;
        QVector<int> candidates;
        if (!pickWeaponPart(pixel, automatic, hit, surface, candidates, error)) {
            setStatus(error, true);
            return false;
        }
        const int a = operatingAnchor();
        if (a < 0 || !candidates.contains(a)) {
            setStatus("Pick a surface on the selected operating part.", true);
            return false;
        }
        Transformf world;
        Transformf inverse;
        if (!sceneNodeWorld(_handPoseAnchors[a].node, world, error) || !invertUniformTransform(world, inverse)) {
            setStatus("Operating part frame is unavailable.", true);
            return false;
        }
        auto g = currentGrabLocations();
        const auto p = composed(inverse, Transformf{ hit }).translation;
        const int selected = _selectedGrabLocation;
        if (_replaceGrabLocation && selected >= 0 && selected < g.points.size()) {
            g.points[selected].point = p;
        } else {
            if (g.points.size() >= 32) {
                setStatus("A group supports up to 32 locations.", true);
                return false;
            }
            g.points.append({ QUuid::createUuid().toString(QUuid::WithoutBraces), QString("%1 point %2").arg(g.sourceName).arg(g.points.size() + 1), p, 3, 3 });
        }
        _grabLocations[grabLocationsKey(g)] = g;
        _pickingGrabLocation = false;
        _lastError.clear();
        _gripViewport->setPicking(false);
        noteDocumentChange(QStringLiteral("grab location picked"));
        refreshGrabLocationsUi();
        if (!_replaceGrabLocation) {
            selectGrabLocation(g.points.size() - 1);
        }
        updateGripViewport();
        return true;
    }

    // The viewport gizmo moved the selected location.
    void ReloadMakerController::editGrabLocationWorld(const Transformf& target)
    {
        const auto a = operatingAnchor();
        const auto index = _selectedGrabLocation;
        auto g = currentGrabLocations();
        if (a < 0 || index < 0 || index >= g.points.size()) {
            return;
        }
        Transformf world;
        Transformf inverse;
        QString error;
        if (!sceneNodeWorld(_handPoseAnchors[a].node, world, error) || !invertUniformTransform(world, inverse)) {
            return;
        }
        g.points[index].point = composed(inverse, target).translation;
        _grabLocations[grabLocationsKey(g)] = g;
        noteDocumentChange(QStringLiteral("grab location moved"));
        refreshGrabLocationsUi();
        updateGripViewport();
    }

    bool ReloadMakerController::saveGrabLocations()
    {
        const auto g = currentGrabLocations();
        OperatingGrabLocations checked;
        QString error;
        if (!readGrabLocations(grabLocationsJson(g), checked, error)) {
            setStatus(error, true);
            return false;
        }
        _grabLocations[grabLocationsKey(g)] = g;
        noteDocumentChange(QStringLiteral("grab locations"));
        showGrabLocationStatus("Grab locations kept. Save the project to keep them; export them for the game's point-target registration.");
        return true;
    }

    // Only saved locations are exported, with the operating input track.
    QJsonObject ReloadMakerController::exportGrabLocations() const
    {
        const auto g = currentGrabLocations();
        if (!_grabLocations.contains(grabLocationsKey(g))) {
            return {};
        }
        auto json = grabLocationsJson(g);
        if (_simulationRig) {
            json["inputTrack"] = _simulationRig->slide;
        }
        return json;
    }

    QJsonObject ReloadMakerController::grabLocationsState() const
    {
        const auto g = currentGrabLocations();
        const int selected = _grabLocationList ? _selectedGrabLocation : -1;
        return { { "configured", _grabLocations.contains(grabLocationsKey(g)) }, { "profile", grabLocationsJson(g) }, { "selected", selected } };
    }

    // open, pick, save; and on the selected location (or the one named by
    // id): select, remove, edit (name, point, radius, hands) and use_palm,
    // which adds the part grip's picked palm contact.
    QJsonObject ReloadMakerController::configureGrabLocation(const QJsonObject& p)
    {
        const auto operation = p["operation"].toString();
        if (operation == "open") {
            if (!openGrabLocations()) {
                fail(_lastError);
            }
            return grabLocationsState();
        }
        if (operation == "pick") {
            if (!beginGrabLocationPick(p["replace"].toBool()) || !pickGrabLocation({ p["x"].toDouble(-1), p["y"].toDouble(-1) })) {
                fail(_lastError);
            }
            return grabLocationsState();
        }
        if (operation == "save") {
            if (!saveGrabLocations()) {
                fail(_lastError);
            }
            return grabLocationsState();
        }
        auto g = currentGrabLocations();
        if (g.weapon.isEmpty()) {
            fail("Prepare the weapon in Operate first.");
        }
        int i = _selectedGrabLocation;
        if (p.contains("id")) {
            i = -1;
            for (int n = 0; n < g.points.size(); ++n) {
                if (g.points[n].id == p["id"].toString()) {
                    i = n;
                }
            }
        }
        if (operation == "use_palm") {
            const auto a = operatingAnchor();
            if (a < 0 || !_partGripDraft || !_partGripDraft->surfaceContact || !sameGripAnchor(_partGripDraft->pose.placement, _handPoseAnchors[a].description)) {
                fail("Pick a palm contact on this operating part in Reload builder > Part grips first.");
            }
            g.points.append({ QUuid::createUuid().toString(QUuid::WithoutBraces), "Picked palm contact", _partGripDraft->surfaceContact->pointInAnchor, 3, 3 });
            i = g.points.size() - 1;
        } else {
            if (i < 0 || i >= g.points.size()) {
                fail("Select a grab location first.");
            }
            if (operation == "select") {
                selectGrabLocation(i);
                return grabLocationsState();
            }
            if (operation == "remove") {
                g.points.removeAt(i);
            } else if (operation == "edit") {
                editGrabLocationFields(g, i, p);
            } else {
                fail("Unknown grab-location operation.");
            }
        }
        OperatingGrabLocations checked;
        QString error;
        if (!readGrabLocations(grabLocationsJson(g), checked, error)) {
            fail(error);
        }
        _grabLocations[grabLocationsKey(g)] = g;
        noteDocumentChange(QStringLiteral("grab location edited"));
        refreshGrabLocationsUi();
        if (i < g.points.size()) {
            selectGrabLocation(i);
        }
        updateGripViewport();
        return grabLocationsState();
    }

    // Applies the request's name, point, radius and hand mask to location
    // i through the saved format, so the same checks apply.
    void ReloadMakerController::editGrabLocationFields(OperatingGrabLocations& g, int i, const QJsonObject& p) const
    {
        auto json = grabLocationsJson(g);
        auto points = json["points"].toArray();
        auto point = points[i].toObject();
        for (const auto* key : { "name", "pointInSource", "radiusGame", "handMask" }) {
            if (p.contains(key)) {
                point[key] = p[key];
            }
        }
        points[i] = point;
        json["points"] = points;
        QString error;
        if (!readGrabLocations(json, g, error)) {
            fail(error);
        }
    }
}
