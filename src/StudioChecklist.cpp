// The Reload builder's left Checklist (canvas Author board). One section per
// builder step plus Fire and sounds; each section shows its done count, and
// the open section lists its items with their state. The card under the list
// shows the selected item's value and evidence with its actions. A complete
// checklist says so and stays editable.
#include "ReloadMakerController.h"
#include "ReloadTemplates.h"
#include "design/DesignWidgets.h"
#include "design/Theme.h"
#include "StudioWindow.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>
#include <stdexcept>

namespace reloadmaker
{
    using namespace design;

    namespace
    {
        struct SectionInfo
        {
            const char* title;
            const char* subtitle;
        };
        // In ChecklistSection order; the first five are the builder steps.
        constexpr SectionInfo kSections[]{
            { "Learned reload", "Source reload and part timing" },
            { "Part grips", "Hands on the magazine and parts" },
            { "Mechanisms", "Moving parts, pulls, linked parts" },
            { "Grab points", "Where hands may grab a part" },
            { "Magazines", "Full and empty world models" },
            { "Fire and sounds", "What a shot does and sounds like" },
        };
        constexpr int kFireSection = int(ChecklistSection::Fire);

        ChecklistRow::Marker markerFor(ChecklistState state)
        {
            switch (state) {
            case ChecklistState::Confirmed: return ChecklistRow::Marker::Confirmed;
            case ChecklistState::Detected: return ChecklistRow::Marker::Detected;
            case ChecklistState::NotApplicable: return ChecklistRow::Marker::NotApplicable;
            case ChecklistState::NeedsYou: break;
            }
            return ChecklistRow::Marker::NeedsYou;
        }

        void setTone(QWidget* widget, const char* tone)
        {
            widget->setProperty("tone", QString::fromLatin1(tone));
            widget->style()->unpolish(widget);
            widget->style()->polish(widget);
        }
    }

    void ReloadMakerController::createChecklistPanel()
    {
        _checklistPanel = new QWidget(_leftStack);
        // The footer repeats what is left, as on the builder board.
        _checklistFooter = new QLabel(_window);
        _checklistFooter->setObjectName(QStringLiteral("ChecklistFooter"));
        _checklistFooter->setTextFormat(Qt::RichText);
        _checklistFooter->hide();
        _window->statusBar()->insertPermanentWidget(0, _checklistFooter);
        auto* layout = new QVBoxLayout(_checklistPanel);
        layout->setContentsMargins(10, 14, 10, 10);
        layout->setSpacing(6);

        auto* head = new QHBoxLayout;
        head->setContentsMargins(6, 0, 6, 0);
        head->addWidget(sectionLabel(QStringLiteral("Checklist"), _checklistPanel));
        head->addStretch(1);
        _checklistCount = hintLabel({}, _checklistPanel);
        head->addWidget(_checklistCount);
        layout->addLayout(head);
        _checklistProgress = new QProgressBar(_checklistPanel);
        _checklistProgress->setObjectName(QStringLiteral("ChecklistProgress"));
        _checklistProgress->setTextVisible(false);
        _checklistProgress->setFixedHeight(6);
        auto* progressRow = new QHBoxLayout;
        progressRow->setContentsMargins(6, 0, 6, 0);
        progressRow->addWidget(_checklistProgress);
        layout->addLayout(progressRow);
        _checklistBanner = new QLabel(_checklistPanel);
        _checklistBanner->setObjectName(QStringLiteral("ChecklistBanner"));
        _checklistBanner->setWordWrap(true);
        _checklistBanner->hide();
        layout->addWidget(_checklistBanner);

        auto* scroll = new QScrollArea(_checklistPanel);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        _checklistList = new QWidget;
        auto* list = new QVBoxLayout(_checklistList);
        list->setContentsMargins(0, 4, 0, 4);
        list->setSpacing(2);
        _builderStepButtons = new QButtonGroup(this);
        _checklistItemButtons = new QButtonGroup(this);
        for (int i = 0; i < int(std::size(kSections)); ++i) {
            auto* row = new ChecklistRow(QString::fromLatin1(kSections[i].title), _checklistList);
            row->setSubtitle(QString::fromLatin1(kSections[i].subtitle));
            if (i == kFireSection) {
                // Not a builder step: it opens and closes its items in place.
                row->setMarker(ChecklistRow::Marker::Open);
                row->setCheckable(false);
                connect(row, &QAbstractButton::clicked, this, [this] {
                    _checklistFireOpen = !_checklistFireOpen;
                    refreshChecklistUi();
                });
            } else {
                row->setMarker(ChecklistRow::Marker::Number, i + 1);
                _builderStepButtons->addButton(row, i);
            }
            _checklistSectionRows[i] = row;
            list->addWidget(row);
            auto* box = new QWidget(_checklistList);
            auto* boxLayout = new QVBoxLayout(box);
            boxLayout->setContentsMargins(0, 0, 0, 4);
            boxLayout->setSpacing(1);
            _checklistSectionBoxes[i] = box;
            list->addWidget(box);
        }
        list->addStretch(1);
        scroll->setWidget(_checklistList);
        layout->addWidget(scroll, 1);

        // The selected item.
        _checklistCard = new QFrame(_checklistPanel);
        _checklistCard->setObjectName(QStringLiteral("ChecklistCard"));
        auto* card = new QVBoxLayout(_checklistCard);
        card->setContentsMargins(12, 8, 12, 12);
        card->setSpacing(6);
        _checklistCardSection = sectionLabel({}, _checklistCard);
        card->addWidget(_checklistCardSection);
        _checklistCardTitle = new QLabel(_checklistCard);
        _checklistCardTitle->setObjectName(QStringLiteral("ChecklistCardTitle"));
        _checklistCardTitle->setWordWrap(true);
        card->addWidget(_checklistCardTitle);
        auto* chips = new QHBoxLayout;
        chips->setSpacing(8);
        _checklistCardState = new QLabel(_checklistCard);
        _checklistCardState->setObjectName(QStringLiteral("StateChip"));
        chips->addWidget(_checklistCardState);
        _checklistCardOwner = hintLabel({}, _checklistCard);
        chips->addWidget(_checklistCardOwner, 1);
        card->addLayout(chips);
        _checklistCardValue = new QLabel(_checklistCard);
        _checklistCardValue->setWordWrap(true);
        _checklistCardValue->setTextInteractionFlags(Qt::TextSelectableByMouse);
        card->addWidget(_checklistCardValue);
        _checklistCardEvidence = hintLabel({}, _checklistCard);
        _checklistCardEvidence->setTextInteractionFlags(Qt::TextSelectableByMouse);
        card->addWidget(_checklistCardEvidence);
        _checklistCardMessage = hintLabel({}, _checklistCard);
        card->addWidget(_checklistCardMessage);
        _checklistPresetChoice = new QComboBox(_checklistCard);
        _checklistPresetChoice->setToolTip(QStringLiteral("Behavior presets describe how the weapon feeds, cycles and fires"));
        card->addWidget(_checklistPresetChoice);
        auto* actions = new QHBoxLayout;
        actions->setSpacing(6);
        _checklistConfirm = new QPushButton(QStringLiteral("Confirm"), _checklistCard);
        _checklistConfirm->setObjectName(QStringLiteral("PrimaryButton"));
        _checklistSkip = new QPushButton(QStringLiteral("Not applicable"), _checklistCard);
        _checklistUndo = new QPushButton(QStringLiteral("Undo"), _checklistCard);
        for (auto* button : { _checklistConfirm, _checklistSkip, _checklistUndo }) {
            actions->addWidget(button);
        }
        card->addLayout(actions);
        _checklistOpen = new QPushButton(_checklistCard);
        card->addWidget(_checklistOpen);
        _checklistCard->hide();
        layout->addWidget(_checklistCard);

        // Selected on any check (mouse, keyboard or accessibility), as the rail.
        connect(_checklistItemButtons, &QButtonGroup::idToggled, this, [this](int, bool checked) {
            auto* row = _checklistItemButtons->checkedButton();
            if (!checked || !row) {
                return;
            }
            _selectedChecklistItem = row->property("item").toString();
            const int section = row->property("section").toInt();
            if (section != kFireSection && builderStep() != BuilderStep(section)) {
                showBuilderStep(BuilderStep(section));
            }
            refreshChecklistUi();
        });
        connect(_checklistConfirm, &QPushButton::clicked, this, [this] {
            if (_selectedChecklistItem == QLatin1String("weapon.preset") && _checklistPresetChoice->isVisible()) {
                runChecklistCommand({ { "operation", "preset" }, { "id", _checklistPresetChoice->currentData().toString() } });
            } else {
                runChecklistCommand({ { "operation", "confirm" }, { "id", _selectedChecklistItem } });
            }
        });
        connect(_checklistSkip, &QPushButton::clicked, this, [this] { runChecklistCommand({ { "operation", "skip" }, { "id", _selectedChecklistItem } }); });
        connect(_checklistUndo, &QPushButton::clicked, this, [this] {
            runChecklistCommand({ { "operation", _checklistUndo->property("skip").toBool() ? "unskip" : "unconfirm" }, { "id", _selectedChecklistItem } });
        });
        connect(_checklistOpen, &QPushButton::clicked, this, [this] { setWorkspace(_checklistOpen->property("workspace").toInt()); });
        connect(_checklistPresetChoice, qOverload<int>(&QComboBox::activated), this, [this] { refreshChecklistUi(); });

        // Re-evaluated while shown, only when something it reads changed.
        _checklistTimer = new QTimer(this);
        _checklistTimer->setInterval(400);
        connect(_checklistTimer, &QTimer::timeout, this, [this] {
            if (_checklistPanel->isVisible() && checklistRefreshKey() != _checklistShownKey) {
                refreshChecklistUi();
            }
        });
        _checklistTimer->start();
    }

    QString ReloadMakerController::checklistRefreshKey() const
    {
        return QStringLiteral("%1|%2|%3|%4|%5|%6|%7|%8")
            .arg(_revision)
            .arg(_loadThread || _reloadCapturing || _reloadCaptureQueued)
            .arg(_reloadRecipe ? _reloadRecipe->id : QString())
            .arg(_simulationRig ? _simulationRig->recipeId : QString())
            .arg(_simulationActive)
            .arg(currentWeapon() ? currentWeapon()->formId : 0)
            .arg(_soundProfile.weapon)
            .arg(_workspace);
    }

    void ReloadMakerController::runChecklistCommand(const QJsonObject& request)
    {
        try {
            (void)checklistCommand(request);
        } catch (const std::exception& e) {
            setStatus(QString::fromUtf8(e.what()), true);
        }
        refreshChecklistUi();
    }

    void ReloadMakerController::refreshChecklistUi()
    {
        if (!_checklistPanel || !_checklistPanel->isVisible()) {
            if (_checklistFooter) {
                _checklistFooter->hide();
            }
            return;
        }
        prepareChecklistEvidence();
        _checklistShownKey = checklistRefreshKey();
        const auto view = checklistView();

        // Rows follow the weapon model: rebuilt when its items change.
        QStringList ids;
        for (const auto& status : view.items) {
            ids << status.item.id;
        }
        if (ids != _checklistItemIds) {
            for (auto* row : std::as_const(_checklistItemRows)) {
                _checklistItemButtons->removeButton(row);
                row->deleteLater();
            }
            _checklistItemRows.clear();
            for (const auto& status : view.items) {
                const int section = int(status.item.section);
                auto* row = new ChecklistRow(status.item.title, _checklistSectionBoxes[section]);
                row->setIndent(18);
                row->setProperty("item", status.item.id);
                row->setProperty("section", section);
                _checklistSectionBoxes[section]->layout()->addWidget(row);
                _checklistItemButtons->addButton(row);
                _checklistItemRows.insert(status.item.id, row);
            }
            _checklistItemIds = ids;
            if (!ids.contains(_selectedChecklistItem)) {
                _selectedChecklistItem.clear();
            }
        }

        const QSignalBlocker itemSignals(_checklistItemButtons);
        std::array<int, 6> done{};
        std::array<int, 6> total{};
        std::array<bool, 6> holdsSelection{};
        const ChecklistItemStatus* selected = nullptr;
        for (const auto& status : view.items) {
            const int section = int(status.item.section);
            ++total[section];
            done[section] += status.state == ChecklistState::Confirmed || status.state == ChecklistState::NotApplicable;
            auto* row = _checklistItemRows.value(status.item.id);
            row->setMarker(markerFor(status.state));
            // The tag says who acts next or why the state may be old.
            const QString tag = status.stale ? QStringLiteral("changed")
                                : !status.rechecked && status.state == ChecklistState::Confirmed ? QStringLiteral("saved")
                                : status.item.owner == ChecklistOwner::You && status.state == ChecklistState::NeedsYou ? QStringLiteral("you")
                                : !status.item.mandatory && status.state != ChecklistState::Confirmed ? QStringLiteral("optional")
                                                                                                        : QString();
            row->setTrailing(tag);
            row->setTrailingColor(tag == QLatin1String("you") || tag == QLatin1String("changed") ? color::needsYou : color::textFaint);
            row->setToolTip(status.value.isEmpty() ? status.message : status.value);
            const bool isSelected = status.item.id == _selectedChecklistItem;
            {
                const QSignalBlocker block(row);
                row->setChecked(isSelected);
            }
            if (isSelected) {
                selected = &status;
                holdsSelection[section] = true;
            }
        }
        if (!selected) {
            // No visible selection: the group must allow none checked.
            _checklistItemButtons->setExclusive(false);
            for (auto* button : _checklistItemButtons->buttons()) {
                button->setChecked(false);
            }
            _checklistItemButtons->setExclusive(true);
        }
        const auto step = builderStep();
        for (int i = 0; i < int(std::size(kSections)); ++i) {
            auto* row = _checklistSectionRows[i];
            row->setTrailing(total[i] ? QStringLiteral("%1 / %2").arg(done[i]).arg(total[i]) : QString());
            row->setTrailingColor(total[i] && done[i] == total[i] ? color::done : color::textMuted);
            const bool open = i == kFireSection ? _checklistFireOpen || holdsSelection[i] : (step && int(*step) == i) || holdsSelection[i];
            _checklistSectionBoxes[i]->setVisible(open && total[i] > 0);
            if (i == kFireSection) {
                row->setMarker(total[i] && done[i] == total[i] ? ChecklistRow::Marker::Confirmed : ChecklistRow::Marker::Open);
            }
        }

        const auto& s = view.summary;
        _checklistCount->setText(view.preset ? QStringLiteral("%1 of %2 done").arg(s.done).arg(s.total) : QString());
        _checklistProgress->setRange(0, std::max(1, s.total));
        _checklistProgress->setValue(s.done);
        if (!view.preset) {
            _checklistBanner->setText(view.problem);
            setTone(_checklistBanner, "attention");
        } else if (s.complete) {
            _checklistBanner->setText(QStringLiteral("Complete. Every required item is confirmed for this configuration. Anything can still change; "
                                                     "changed items come back for review."));
            setTone(_checklistBanner, "complete");
        } else {
            QStringList parts;
            const int required = s.mandatory - s.mandatoryDone;
            parts << QStringLiteral("%1 required item(s) left").arg(required);
            if (s.needsYou) {
                parts << QStringLiteral("%1 need you").arg(s.needsYou);
            }
            if (s.toConfirm) {
                parts << QStringLiteral("%1 to confirm").arg(s.toConfirm);
            }
            _checklistBanner->setText(parts.join(QStringLiteral(" · ")) + (view.preset ? QStringLiteral("\nPreset: ") + view.preset->label : QString()));
            setTone(_checklistBanner, "attention");
        }
        _checklistBanner->setVisible(true);
        QStringList footer;
        if (view.preset && s.complete) {
            footer << QStringLiteral("Checklist complete");
        } else if (view.preset) {
            if (s.needsYou) {
                footer << QStringLiteral("<span style=\"color:%1\">%2</span>")
                              .arg(hex(color::needsYou), s.needsYou == 1 ? QStringLiteral("1 item needs you") : QStringLiteral("%1 items need you").arg(s.needsYou));
            }
            if (s.toConfirm) {
                footer << (s.toConfirm == 1 ? QStringLiteral("1 detected item to confirm") : QStringLiteral("%1 detected items to confirm").arg(s.toConfirm));
            }
        }
        _checklistFooter->setText(footer.join(QStringLiteral(" &nbsp;·&nbsp; ")));
        _checklistFooter->setVisible(!footer.isEmpty());

        _checklistCard->setVisible(selected != nullptr);
        if (!selected) {
            return;
        }
        const auto& item = *selected;
        static const char* subsystems[]{ "Weapon", "Feed", "Action", "Fire" };
        _checklistCardSection->setText(QString::fromLatin1(subsystems[int(item.item.subsystem)]).toUpper() + QStringLiteral(" · ") +
                                       QString::fromLatin1(kSections[int(item.item.section)].title).toUpper());
        _checklistCardTitle->setText(item.item.title);
        static const char* stateNames[]{ "Needs you", "Detected", "Confirmed", "Not applicable" };
        static const char* tones[]{ "needs", "detected", "confirmed", "na" };
        _checklistCardState->setText(QString::fromLatin1(stateNames[int(item.state)]));
        setTone(_checklistCardState, tones[int(item.state)]);
        QString owner = item.item.owner == ChecklistOwner::You      ? QStringLiteral("You author it")
                        : item.item.owner == ChecklistOwner::Preset ? QStringLiteral("From the preset; you confirm")
                                                                    : QStringLiteral("Maker detects; you confirm");
        if (!item.item.mandatory) {
            owner += QStringLiteral(" · optional");
        }
        _checklistCardOwner->setText(owner);
        _checklistCardValue->setText(item.value);
        _checklistCardValue->setVisible(!item.value.isEmpty());
        _checklistCardEvidence->setText(item.evidence);
        _checklistCardEvidence->setVisible(!item.evidence.isEmpty() && item.rechecked);
        QString message = item.message;
        if (item.state == ChecklistState::Confirmed && item.item.owner != ChecklistOwner::You && !item.confirmedAt.isEmpty()) {
            const auto at = QDateTime::fromString(item.confirmedAt, Qt::ISODate).toLocalTime();
            message = QStringLiteral("Confirmed by you · %1").arg(at.toString(QStringLiteral("d MMM HH:mm"))) + (message.isEmpty() ? QString() : "\n" + message);
        }
        _checklistCardMessage->setText(message);
        _checklistCardMessage->setVisible(!message.isEmpty());

        // The preset is confirmed by choosing it; without a proposal the
        // author picks one from the list.
        const bool preset = item.item.id == QLatin1String("weapon.preset");
        _checklistPresetChoice->setVisible(preset);
        if (preset) {
            const QSignalBlocker block(_checklistPresetChoice);
            const QString chosen = _checklistPresetChoice->currentData().toString();
            _checklistPresetChoice->clear();
            for (const auto& entry : reloadPresets()) {
                _checklistPresetChoice->addItem(entry.label, entry.id);
            }
            const int keep = _checklistPresetChoice->findData(!chosen.isEmpty() ? chosen : view.preset ? view.preset->id : QString());
            _checklistPresetChoice->setCurrentIndex(std::max(0, keep));
        }
        const bool otherPreset = preset && (!view.preset || _checklistPresetChoice->currentData().toString() != view.preset->id);
        _checklistConfirm->setVisible(item.canConfirm || otherPreset || (!item.rechecked && item.state != ChecklistState::NotApplicable));
        _checklistConfirm->setText(otherPreset ? QStringLiteral("Use this preset")
                                   : !item.rechecked ? QStringLiteral("Check in Operate")
                                   : item.stale ? QStringLiteral("Confirm again")
                                                : QStringLiteral("Confirm"));
        _checklistSkip->setVisible(item.canSkip);
        const bool skippedByYou = item.state == ChecklistState::NotApplicable && item.byYou;
        const bool undoable = !preset && item.item.owner != ChecklistOwner::You && item.byYou &&
                              (item.state == ChecklistState::Confirmed || skippedByYou) && item.rechecked;
        _checklistUndo->setVisible(undoable);
        _checklistUndo->setProperty("skip", skippedByYou);
        // Fire behavior is tried in Operate; sounds are tuned on their page.
        const bool sound = item.item.id == QLatin1String("fire.sound") || item.item.id == QLatin1String("fire.dry") || item.item.id == QLatin1String("sounds.reload");
        const bool fire = item.item.section == ChecklistSection::Fire && !sound;
        _checklistOpen->setVisible(sound || fire);
        _checklistOpen->setText(sound ? QStringLiteral("Open Sounds") : QStringLiteral("Try it in Operate"));
        _checklistOpen->setProperty("workspace", sound ? int(SoundsWorkspace) : int(OperateWorkspace));
    }
}
