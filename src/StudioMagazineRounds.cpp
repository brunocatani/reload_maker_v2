// Rounds hidden when empty: the selected magazine's pieces, each with a Round
// checkbox. Opened from the checklist's "Full and empty magazines" item; the
// controller's magazine_rounds command does the work.
#include "ReloadMakerController.h"
#include "StudioWindow.h"
#include "glview.h"

#include <QDialog>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <stdexcept>

namespace reloadmaker
{
    void ReloadMakerController::showMagazineRounds()
    {
        // The pieces belong to the magazine Operate prepares; a dialog left
        // open for another magazine is replaced.
        if (auto* existing = _window->findChild<QDialog*>(QStringLiteral("MagazineRoundsDialog"))) {
            existing->close();
        }
        setWorkspace(OperateWorkspace);
        QJsonObject state;
        try {
            state = magazineRoundsState();
        } catch (const std::exception& e) {
            setStatus(QString::fromUtf8(e.what()), true);
            return;
        }
        const auto key = state["key"].toString();
        auto* dialog = new QDialog(_window);
        dialog->setObjectName(QStringLiteral("MagazineRoundsDialog"));
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(QStringLiteral("Rounds hidden when empty"));
        dialog->resize(560, 560);
        auto* layout = new QVBoxLayout(dialog);
        auto* intro = new QLabel(QStringLiteral("Pieces checked as rounds disappear when this magazine is empty: in Operate, in its empty world model and in "
                                                "game. Maker proposes them from piece names and materials. Your choice applies to every configuration "
                                                "that uses this magazine."),
                                 dialog);
        intro->setWordWrap(true);
        layout->addWidget(intro);
        auto* magazine = new QLabel(QStringLiteral("<b>%1</b> · %2").arg(state["magazine"].toString().toHtmlEscaped(), state["record"].toString().toHtmlEscaped()), dialog);
        magazine->setToolTip(state["model"].toString());
        layout->addWidget(magazine);
        auto* tree = new QTreeWidget(dialog);
        tree->setColumnCount(2);
        tree->setHeaderLabels({ QStringLiteral("Round · piece"), QStringLiteral("Maker detected") });
        tree->setRootIsDecorated(false);
        tree->setColumnWidth(0, 300);
        layout->addWidget(tree, 1);
        auto* status = new QLabel(dialog);
        status->setWordWrap(true);
        layout->addWidget(status);
        auto* actions = new QHBoxLayout;
        layout->addLayout(actions);
        auto* previewEmpty = new QPushButton(QStringLiteral("Preview empty"), dialog);
        auto* previewFull = new QPushButton(QStringLiteral("Preview full"), dialog);
        auto* detected = new QPushButton(QStringLiteral("Use detected"), dialog);
        auto* save = new QPushButton(QStringLiteral("Save"), dialog);
        save->setObjectName(QStringLiteral("PrimaryButton"));
        actions->addWidget(previewEmpty);
        actions->addWidget(previewFull);
        actions->addStretch(1);
        actions->addWidget(detected);
        actions->addWidget(save);
        const bool setupDecides = state["setupDecides"].toBool();
        if (setupDecides) {
            auto* setup = new QPushButton(QStringLiteral("Open Magazine models and payload…"), dialog);
            layout->addWidget(setup);
            connect(setup, &QPushButton::clicked, dialog, [this, dialog] {
                dialog->close();
                showMagazineSetup();
            });
        }

        const auto fill = [tree](const QJsonObject& current) {
            tree->clear();
            for (const auto& value : current["pieces"].toArray()) {
                const auto piece = value.toObject();
                const bool round = piece["round"].toBool();
                const bool proposed = piece["detected"].toBool();
                auto* item = new QTreeWidgetItem(tree, { piece["name"].toString().isEmpty() ? piece["path"].toString() : piece["name"].toString(),
                                                         QString(proposed ? QStringLiteral("round") : QStringLiteral("body")) +
                                                             (round != proposed ? QStringLiteral(" · your choice: %1").arg(round ? QStringLiteral("round") : QStringLiteral("body"))
                                                                                : QString()) });
                item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                item->setCheckState(0, round ? Qt::Checked : Qt::Unchecked);
                item->setData(0, Qt::UserRole, piece["path"].toString());
                item->setToolTip(0, piece["path"].toString());
            }
        };
        fill(state);
        tree->setEnabled(!setupDecides);
        save->setEnabled(!setupDecides);
        detected->setEnabled(!setupDecides && !state["chosen"].toObject().isEmpty());
        status->setText(setupDecides ? QStringLiteral("This configuration's Magazine models and payload settings decide its pieces. Change its rounds there.")
                                     : QStringLiteral("Select a piece to highlight it in the view."));

        // The selected piece is highlighted in the view.
        connect(tree, &QTreeWidget::currentItemChanged, dialog, [this, key](QTreeWidgetItem* item, QTreeWidgetItem*) {
            if (!item || !_magazineRail || magazineRoundsKey() != key) {
                return;
            }
            const QModelIndex carrier = _sceneNodes[_reloadTrackNodes[_magazineRail->fullTrack]];
            for (const auto& visual : _magazineVisuals) {
                if (magazineRoundPath(visual.node, carrier) == item->data(0, Qt::UserRole).toString()) {
                    _window->getGLView()->setCurrentIndex(visual.node);
                    break;
                }
            }
        });
        const auto run = [this, key, fill, status, detected](const QJsonObject& request, const QString& done) {
            try {
                if (magazineRoundsKey() != key) {
                    throw std::runtime_error("The magazine changed; reopen Rounds hidden when empty.");
                }
                const auto result = configureMagazineRounds(request);
                fill(result);
                detected->setEnabled(!result["chosen"].toObject().isEmpty());
                status->setText(done);
            } catch (const std::exception& e) {
                status->setText(QString::fromUtf8(e.what()));
            }
        };
        connect(save, &QPushButton::clicked, dialog, [tree, run] {
            QJsonObject rounds;
            for (int i = 0; i < tree->topLevelItemCount(); ++i) {
                const auto* item = tree->topLevelItem(i);
                rounds[item->data(0, Qt::UserRole).toString()] = item->checkState(0) == Qt::Checked;
            }
            run({ { "operation", "save" }, { "rounds", rounds } },
                QStringLiteral("Saved for every configuration that uses this magazine. Recapture its full and empty world models before the next pack save."));
        });
        connect(detected, &QPushButton::clicked, dialog, [run] {
            run({ { "operation", "reset" } }, QStringLiteral("Back to the detected rounds. Recapture its world models before the next pack save."));
        });
        const auto preview = [this, key, status](bool empty) {
            if (magazineRoundsKey() != key) {
                status->setText(QStringLiteral("The magazine changed; reopen Rounds hidden when empty."));
                return;
            }
            previewMagazineOutside(empty);
            status->setText(QStringLiteral("Previewing the saved rounds. Save first to preview changes."));
        };
        connect(previewEmpty, &QPushButton::clicked, dialog, [preview] { preview(true); });
        connect(previewFull, &QPushButton::clicked, dialog, [preview] { preview(false); });
        dialog->show();
    }
}
