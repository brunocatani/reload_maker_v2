#pragma once
#include "GripViewportMath.h"
#include "SlotOverlay.h"
#include <functional>

namespace reloadmaker {
    struct ReloadDriveCurve {QVector<QVector3D> points;QVector<float> times;float current{0};};
    class ReloadDriveViewport final:public QObject {
    public:
        explicit ReloadDriveViewport(GLView* view,QObject* parent);
        void setActive(bool active,bool canDrive);
        void clearCurve();
        void setTime(float time);
        void setInstructions(const QString& text) { _instructions=text; }
        std::function<bool(QPointF,ReloadDriveCurve&)> begin;
        std::function<void(float)> drive;
        std::function<void(bool)> release; // true cancels to press-time pose.
    protected:
        bool eventFilter(QObject*,QEvent*)override;
    private:
        void paint();
        GLView* _view;
        bool _active{false},_canDrive{false},_dragging{false};
        SlotPointerGesture _gesture;
        ReloadDriveCurve _curve;
        QString _instructions;
        QVector<QPointF> _projected;
    };
}
