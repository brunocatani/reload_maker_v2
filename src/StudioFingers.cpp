#include "ReloadMakerController.h"
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QQuaternion>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QVBoxLayout>
#include <stdexcept>

namespace reloadmaker {
void ReloadMakerController::createFingerEditor() {
    auto* page = _handPosePages->widget(0);
    auto* section = new QGroupBox("Exact finger rotation", page);
    auto* form = new QFormLayout(section);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    _fingerJoint = new QComboBox(section);
    for (const QString& finger : {"Thumb", "Index", "Middle", "Ring", "Little"})
        for (const QString& joint : {"base", "middle", "tip"}) _fingerJoint->addItem(finger+" / "+joint);
    form->addRow("Joint", _fingerJoint);
    for (int i = 0; i < 3; ++i) {
        _fingerAngles[i] = new QDoubleSpinBox(section);
        _fingerAngles[i]->setRange(-360, 360);
        _fingerAngles[i]->setDecimals(2);
        _fingerAngles[i]->setSingleStep(1);
        _fingerAngles[i]->setPrefix(QStringList{"X ","Y ","Z "}[i]);
        _fingerAngles[i]->setKeyboardTracking(false);
        form->addRow(QStringList{"X degrees","Y degrees","Z degrees"}[i],_fingerAngles[i]);
        connect(_fingerAngles[i], qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this] {
            if (_fingerRefreshing || !_clip.valid()) return;
            const auto q = QQuaternion::fromEulerAngles(float(_fingerAngles[0]->value()), float(_fingerAngles[1]->value()), float(_fingerAngles[2]->value()));
            try { editFinger(_fingerJointIndex, {q.x(),q.y(),q.z(),q.scalar()}); }
            catch (const std::exception& e) { setStatus(QString::fromUtf8(e.what()), true); refreshFingerEditor(); }
        });
    }
    auto* restore = new QPushButton("Restore this hand's animation", section);
    form->addRow(restore);
    auto* help = new QLabel("Edits preserve bone lengths and the other hand. Save the current hand above to keep this finger pose in the library.", section);
    help->setWordWrap(true); help->setObjectName("Hint"); form->addRow(help);
    auto* edit=new QPushButton("EDIT FINGER JOINTS",page);edit->setCheckable(true);
    section->setVisible(false);connect(edit,&QPushButton::toggled,section,&QWidget::setVisible);
    static_cast<QVBoxLayout*>(page->layout())->insertWidget(2,edit);
    static_cast<QVBoxLayout*>(page->layout())->insertWidget(3,section);
    connect(_fingerJoint, qOverload<int>(&QComboBox::currentIndexChanged), this, &ReloadMakerController::setFingerJoint);
    connect(restore, &QPushButton::clicked, this, [this] {
        const auto hand = _editedHand;
        try {executeCommand({{"method","restore_hand"},{"params",QJsonObject{{"hand",hand==PoseHand::Left?"left":"right"}}}});}
        catch(const std::exception& e){setStatus(QString::fromUtf8(e.what()),true);}
    });
    refreshFingerEditor();
}
void ReloadMakerController::refreshFingerEditor() {
    if (!_fingerJoint) return;
    _fingerRefreshing = true;
    const auto hand = _editedHand;
    const int bone = _skeleton.boneIndex(handPoseBoneName(hand, _fingerJointIndex));
    const bool valid = bone >= 0 && bone < _localPose.size() && !_loadThread && !_reloadCapturing;
    QVector3D angles;
    if (valid) {
        const auto& r = _localPose[bone].rotation;
        angles = QQuaternion(r.w,r.x,r.y,r.z).toEulerAngles();
    }
    for (int i = 0; i < 3; ++i) { _fingerAngles[i]->setEnabled(valid); _fingerAngles[i]->setValue(angles[i]); }
    _fingerRefreshing = false;
}
void ReloadMakerController::showFingerJoint() {
    if (!_fingerJoint) return;
    const QSignalBlocker blocker(_fingerJoint);
    _fingerJoint->setCurrentIndex(_fingerJointIndex);
}
}
