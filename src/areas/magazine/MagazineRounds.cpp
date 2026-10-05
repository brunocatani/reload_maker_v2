// Rounds hidden when the magazine is empty. Maker proposes a magazine's
// round pieces from their names and materials (the MagazinePieces detector);
// the author can add or remove pieces. The choice belongs to the magazine
// (its record and model) and applies to every configuration that uses it:
// Operate hides those pieces on an empty magazine, the empty world model
// leaves them out and the pack hides their groups (emptyHideNodes). A saved
// Magazine models and payload setup assigns every piece's role itself, so
// it decides instead (owner request, 2026-10-05).
#include "ReloadMakerController.h"
#include "areas/loadout/SceneTree.h"
#include "model/nifmodel.h"

#include <QJsonArray>
#include <stdexcept>

namespace reloadmaker
{
    namespace
    {
        constexpr qsizetype kRoundChoiceLimit = 256;

        [[noreturn]] void fail(const QString& message)
        {
            throw std::runtime_error(message.toStdString());
        }
    }

    // The magazine the open configuration uses, as record|model.
    QString ReloadMakerController::magazineRoundsKey() const
    {
        const auto anchor = simulationMagazineAnchor();
        if (anchor.ownerRecord.isEmpty() || anchor.ownerModel.isEmpty()) {
            return {};
        }
        return anchor.ownerRecord.toLower() + '|' + normalizeNifPath(anchor.ownerModel);
    }

    // Block names from below the carrier down to `node`. An unnamed block is
    // #<its place among its parent's child links>, which its model fixes,
    // unlike scene block numbers. Empty when `node` is not inside `carrier`.
    QString ReloadMakerController::magazineRoundPath(const QModelIndex& node, const QModelIndex& carrier) const
    {
        auto* model = activeModel();
        if (!model || !node.isValid() || !carrier.isValid() || node == carrier || !isWithinBlock(*model, node, carrier)) {
            return {};
        }
        QStringList path;
        QModelIndex current = node;
        for (int depth = 0; current.isValid() && current != carrier && depth < model->getBlockCount(); ++depth) {
            const auto parent = model->getBlockIndex(model->getParent(current));
            auto name = model->get<QString>(current, "Name");
            if (name.isEmpty()) {
                const int place = parent.isValid() ? model->getChildLinks(model->getBlockNumber(parent)).indexOf(model->getBlockNumber(current)) : -1;
                name = '#' + QString::number(place);
            }
            path.prepend(name);
            current = parent;
        }
        return path.join('/');
    }

    // The full magazine's own pieces (not a separate empty body or native-only
    // spares), with their current and detected round role.
    QJsonArray ReloadMakerController::magazineRoundPieces() const
    {
        QJsonArray pieces;
        auto* model = activeModel();
        if (!model || !_magazineRail) {
            return pieces;
        }
        const QModelIndex carrier = _sceneNodes[_reloadTrackNodes[_magazineRail->fullTrack]];
        for (const auto& visual : _magazineVisuals) {
            if (visual.nativeOnly || visual.empty || !visual.node.isValid()) {
                continue;
            }
            const auto path = magazineRoundPath(visual.node, carrier);
            if (path.isEmpty()) {
                continue;
            }
            pieces.append(QJsonObject{ { "path", path },
                                       { "name", model->get<QString>(visual.node, "Name") },
                                       { "round", visual.cartridge },
                                       { "detected", magazinePieceProposal(visual.node).cartridge } });
        }
        return pieces;
    }

    // Applies the author's choices over the detected roles while the
    // magazine is prepared; a saved setup then assigns every role itself.
    void ReloadMakerController::applyMagazineRounds(const MagazineRail& rail)
    {
        const auto chosen = _magazineRounds.value(magazineRoundsKey());
        if (chosen.isEmpty() || !activeModel()) {
            return;
        }
        const QModelIndex carrier = _sceneNodes[_reloadTrackNodes[rail.fullTrack]];
        for (auto& visual : _magazineVisuals) {
            if (visual.nativeOnly || visual.empty) {
                continue;
            }
            const auto choice = chosen.value(magazineRoundPath(visual.node, carrier));
            if (choice.isBool()) {
                visual.cartridge = choice.toBool();
            }
        }
    }

    // What the magazine's presentation depends on beyond its source: the
    // saved setup, else the author's round choices. World models record it
    // and pack saves compare it, so changed rounds need recaptured models.
    // Empty when neither exists.
    QJsonObject ReloadMakerController::magazinePresentationJson(const QJsonObject& setup) const
    {
        if (!setup.isEmpty()) {
            return setup;
        }
        const auto chosen = _magazineRounds.value(magazineRoundsKey());
        return chosen.isEmpty() ? QJsonObject{} : QJsonObject{ { "rounds", chosen } };
    }

    QJsonObject ReloadMakerController::magazineRoundsState() const
    {
        if (!_reloadRecipe || !_simulationRig || magazineSettingsBusy()) {
            fail("Prepare the operating roles and native magazine first");
        }
        if (!_magazineRail) {
            fail(_magazineError.isEmpty() ? QStringLiteral("The magazine source is not ready") : _magazineError);
        }
        const auto anchor = simulationMagazineAnchor();
        const auto key = magazineRoundsKey();
        return {
            { "key", key },
            { "magazine", anchor.anchorName },
            { "record", anchor.ownerRecord },
            { "model", anchor.ownerModel },
            { "setupDecides", _magazineSetup.has_value() },
            { "chosen", _magazineRounds.value(key) },
            { "pieces", magazineRoundPieces() },
        };
    }

    // save: `rounds` maps piece paths to round (true) or body (false); only
    // the choices that differ from the detected role are kept. reset: back
    // to the detected roles. Both restart the simulation and save the project;
    // a choice the magazine cannot use is rolled back.
    QJsonObject ReloadMakerController::configureMagazineRounds(const QJsonObject& request)
    {
        const auto state = magazineRoundsState();
        if (state["setupDecides"].toBool()) {
            fail("This configuration's Magazine models and payload settings decide its pieces; change its rounds there.");
        }
        const auto key = state["key"].toString();
        if (key.isEmpty()) {
            fail("This magazine has no source record");
        }
        QJsonObject chosen;
        if (request["operation"] == "save") {
            // Per piece path: the detected role and the current one.
            QHash<QString, std::pair<bool, bool>> roles;
            for (const auto& value : state["pieces"].toArray()) {
                const auto piece = value.toObject();
                roles.insert(piece["path"].toString(), { piece["detected"].toBool(), piece["round"].toBool() });
            }
            const auto requested = request["rounds"].toObject();
            for (auto it = requested.begin(); it != requested.end(); ++it) {
                if (!roles.contains(it.key()) || !it.value().isBool()) {
                    fail("Choose rounds among this magazine's pieces");
                }
            }
            int bodies = 0;
            for (auto it = roles.cbegin(); it != roles.cend(); ++it) {
                const bool round = requested.value(it.key()).toBool(it->second);
                if (round != it->first) {
                    chosen[it.key()] = round;
                }
                bodies += round ? 0 : 1;
            }
            if (bodies == 0) {
                fail("Keep at least one piece as the magazine body");
            }
            if (chosen.size() > kRoundChoiceLimit) {
                fail("Too many round choices for one magazine");
            }
        } else if (request["operation"] != "reset") {
            fail("Choose save or reset for the magazine rounds");
        }
        const auto previous = _magazineRounds.value(key);
        if (chosen == previous) {
            return state;
        }
        const auto apply = [&](const QJsonObject& value) {
            stopSimulation();
            if (value.isEmpty()) {
                _magazineRounds.remove(key);
            } else {
                _magazineRounds[key] = value;
            }
            prepareSimulation();
        };
        apply(chosen);
        QString failure = _magazineError.isEmpty() ? _simulationError : _magazineError;
        if (failure.isEmpty()) {
            // The pack hides each group of rounds by its node name.
            try {
                QJsonObject probe;
                addRuntimeMagazineHiding(probe);
            } catch (const std::exception& error) {
                failure = QStringLiteral("These rounds cannot be hidden on their own in game: ") + QString::fromUtf8(error.what());
            }
        }
        if (!failure.isEmpty()) {
            apply(previous);
            fail(failure);
        }
        ++_revision;
        if (!saveSession()) {
            fail("Could not save the magazine rounds");
        }
        return magazineRoundsState();
    }
}
