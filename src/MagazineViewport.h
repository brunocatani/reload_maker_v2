#pragma once
#include "MagazineInteraction.h"
#include "GripViewportMath.h"
#include "SlotOverlay.h"
#include <QObject>
#include <functional>
#include <optional>
class GLView;
namespace reloadmaker {
class MagazineViewport final:public QObject {
public:
    MagazineViewport(GLView* view,QObject* parent);
    void setActive(bool active);
    void beginCarry(){_planeReady=false;}
    std::function<std::optional<MagazineRail>()> rail;
    std::function<MagazineMotion()> state;
    std::function<void(QVector3D)> freeMove;
    std::function<void(float)> railMove;
    std::function<void()> cancel;
protected:
    bool eventFilter(QObject*,QEvent*)override;
private:
    void paint();
    void updatePointer(QPointF point,float wheel=0);
    GLView* _view;
    bool _active{false},_planeReady{false};
    QVector3D _planePoint,_planeNormal;
    SlotPointerGesture _gesture;
};
}
