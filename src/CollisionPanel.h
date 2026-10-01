#pragma once
#include "CollisionAuthoring.h"
#include <QWidget>
#include <QFutureWatcher>
#include <functional>
class QComboBox; class QListWidget; class QLabel; class QDoubleSpinBox; class QCheckBox;
namespace reloadmaker {
class CollisionPanel final : public QWidget {
public:
    explicit CollisionPanel(QWidget* parent=nullptr);
    ~CollisionPanel() override;
    std::function<CollisionProfile(int,bool)> snapshot;
    std::function<void()> changed;
    std::function<void()> cook;
    void setTargets(const QStringList& names);
    void invalidate();
    void cancel();
    QJsonObject command(const QJsonObject& args);
    QJsonObject state() const;
    const CollisionProfile& profile()const{return _profile;}
    const CollisionProfile& baseline()const{return _baseline;}
    bool overlay()const;
    bool compare()const;
    bool isolated()const;
    int selected()const;
private:
    void refresh();
    void showError(const QString& error);
    CollisionProfile context(bool referenceOnly);
    QString savePath()const;
    QComboBox* _target{};
    QListWidget* _hulls{};
    QLabel* _status{};
    QDoubleSpinBox *_gap{},*_error{},*_points{},*_mass{},*_radius{};
    QCheckBox *_overlay{},*_compare{},*_isolate{};
    CollisionProfile _profile,_baseline;
    QFutureWatcher<CollisionResult> _worker;
    std::shared_ptr<std::atomic<bool>> _cancel;
    QString _lastError,_lastPath;
    bool _dirty{false},_refreshing{false};
};
}
