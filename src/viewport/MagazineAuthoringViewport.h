#pragma once

#include "GripViewportMath.h"
#include "SlotOverlay.h"
#include "viewport/DepthOverlay.h"

#include <QColor>
#include <QObject>
#include <QPointF>
#include <QPolygonF>
#include <QVector>
#include <QVector3D>

#include <functional>
#include <optional>

class GLView;
class QPainter;

namespace reloadmaker
{
    // The insertion guide as the viewport edits it, in world space: the seated
    // point, the outward direction, how far the hand travels along it, the
    // pickup distance from the line and the release margin past its end.
    struct GuideShape
    {
        QVector3D seated, outward;
        float travel{ 0 }, pickup{ 0 }, release{ 0 };
    };

    // One magazine piece in the Pieces step: its rendered shape and colour.
    struct MagazinePieceMark
    {
        int block{ -1 };
        QColor color;
        bool selected{ false };
    };

    // The viewport tools of Reload builder › Magazines (StudioMagazines.cpp).
    //
    // Pieces: the magazine's pieces are outlined by role, and a click on one
    // selects it; a drag still orbits. Guide: the guide line, its pickup
    // cylinder and release point are drawn on the weapon with handles. The
    // outer end moves freely or along its arrows (aiming and sizing the line),
    // the arrow along the line changes only the travel, the ring knob sets the
    // pickup distance and the far knob the release margin. Shift drags with
    // 1/10 precision; Ctrl snaps lengths to 0.5 units.
    //
    // While `blocked` returns true (the magazine is carried for a test grab)
    // the guide tools let every event through to the magazine carry.
    class MagazineAuthoringViewport final : public QObject
    {
    public:
        enum class Mode { Off, Pieces, Guide };

        MagazineAuthoringViewport(GLView* view, QObject* parent);
        void setMode(Mode mode);
        [[nodiscard]] Mode mode() const { return _mode; }
        void setPieces(QVector<MagazinePieceMark> pieces);
        // The guide drawn and edited; nullopt hides it (no magazine rail).
        void setGuide(std::optional<GuideShape> guide);

        std::function<void(int block)> pieceClicked;
        std::function<void(const GuideShape&, bool finished)> guideEdited;
        std::function<bool()> blocked;

    protected:
        bool eventFilter(QObject* watched, QEvent* event) override;

    private:
        enum class Handle { Travel, SwingA, SwingB, End, Pickup, Release };
        struct HandleShape
        {
            Handle handle;
            QVector3D direction;
            QPolygonF line;
        };
        void paint();
        void paintGuide(QPainter& painter);
        void refreshHighlights();
        [[nodiscard]] bool beginDrag(const HandleShape& handle, QPointF point);
        void drag(QPointF point, bool snap);
        void finishDrag(bool cancel);
        void basis(QVector3D& a, QVector3D& b) const;

        GLView* _view;
        Mode _mode{ Mode::Off };
        QVector<MagazinePieceMark> _pieces;
        bool _highlighted{ false };
        std::optional<GuideShape> _guide;
        QVector<HandleShape> _handles;
        DepthOverlay _overlay;
        SlotPointerGesture _gesture;
        // A press that has not moved yet may become a piece click.
        std::optional<QPointF> _press;
        // The drag in progress: its handle, the shape it started from, the
        // ray parameter or plane point where it was grabbed and the camera.
        std::optional<Handle> _drag;
        GuideShape _dragStart;
        QVector3D _dragAxis, _dragPivot, _dragPlane;
        float _dragParameter{ 0 }, _dragPrecision{ 1 };
        QMatrix4x4 _dragCamera;
        QSizeF _dragSize;
    };
}
