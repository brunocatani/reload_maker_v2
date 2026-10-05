#pragma once

#include <QColor>
#include <QMatrix4x4>
#include <QMetaObject>
#include <QSizeF>
#include <QVector>
#include <QVector3D>

#include <memory>

class QOpenGLContext;
class QOpenGLShaderProgram;
class QOpenGLBuffer;
class QOpenGLVertexArrayObject;

namespace reloadmaker
{
    // Viewport handles (gizmo arrows, rings, boxes) drawn into the GL frame
    // after the scene and depth-tested against it: full colour where visible,
    // dimmed where weapon or hand geometry covers them (rework 03 §3).
    //
    // Strokes are built on the CPU in clip space, so there is no geometry
    // shader: NifSkope's thick-line programs rasterize nothing in Maker's
    // viewport. Draw from a GLView::paintUpdate handler, while the context is
    // current and the frame's depth buffer is intact, before any QPainter
    // overlay. The GL objects belong to the context that first drew; they are
    // released when that context is destroyed.
    class DepthOverlay final
    {
    public:
        DepthOverlay();
        ~DepthOverlay();
        DepthOverlay(const DepthOverlay&) = delete;
        DepthOverlay& operator=(const DepthOverlay&) = delete;

        // Starts a frame. worldToClip and the viewport size in device pixels
        // turn world points and pixel widths into clip-space strokes.
        void begin(const QMatrix4x4& worldToClip, QSizeF pixels);
        // A stroke through world points, widthPixels wide on screen.
        void addPolyline(const QVector<QVector3D>& points, float widthPixels, const QColor& color, bool closed = false);
        // A screen-aligned square of halfPixels around a world point.
        void addBox(const QVector3D& center, float halfPixels, const QColor& fill);
        // A screen-space arrow head at tip, pointing away from base.
        void addArrowHead(const QVector3D& base, const QVector3D& tip, float lengthPixels, float halfWidthPixels, const QColor& color);
        // Draws everything added since begin(); hiddenAlpha scales the opacity
        // of whatever lies behind the scene's depth.
        void draw(float hiddenAlpha);

    private:
        struct Vertex
        {
            float clip[4];
            float color[4];
        };
        bool toClip(const QVector3D& world, QVector4D& clip) const;
        void addQuad(const QVector4D corners[4], const QColor& color);
        bool ensureResources();
        void release();

        QMatrix4x4 _worldToClip;
        QSizeF _pixels;
        QVector<Vertex> _vertices;
        QOpenGLContext* _context{ nullptr };
        QMetaObject::Connection _contextDestroyed;
        std::unique_ptr<QOpenGLShaderProgram> _program;
        std::unique_ptr<QOpenGLBuffer> _buffer;
        std::unique_ptr<QOpenGLVertexArrayObject> _arrays;
    };
}
