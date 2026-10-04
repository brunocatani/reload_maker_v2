#pragma once
#include <QWidget>
class QComboBox; class QListWidget; class QLabel; class QDoubleSpinBox; class QCheckBox;
namespace reloadmaker {
class CollisionWorkshop;
// Shows the collision workshop; its controls edit the workshop.
class CollisionPanel final : public QWidget {
public:
    explicit CollisionPanel(CollisionWorkshop& workshop,QWidget* parent=nullptr);
    // What the workspace collides: its heading and one-line explanation.
    void setSubject(const QString& title,const QString& intro);
private:
    void showWorkshop();
    void run(const QString& operation);
    CollisionWorkshop& _workshop;
    QComboBox* _target{};
    QListWidget* _hulls{};
    QListWidget* _pieces{};
    QLabel* _status{};
    QLabel *_title{},*_intro{};
    QDoubleSpinBox *_gap{},*_error{},*_points{},*_mass{},*_radius{};
    QCheckBox *_overlay{},*_compare{},*_isolate{};
};
}
