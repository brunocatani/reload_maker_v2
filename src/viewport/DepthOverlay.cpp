#include "viewport/DepthOverlay.h"

#include <QObject>
#include <QOpenGLBuffer>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>

#include <QVector2D>
#include <QVector4D>

#include <cmath>
#include <cstddef>
#include <iterator>

namespace reloadmaker
{
    namespace
    {
        // Vertices arrive in clip space; the fragment keeps their colour.
        constexpr const char* kVertexShader = R"(#version 410 core
layout(location = 0) in vec4 clipPosition;
layout(location = 1) in vec4 color;
uniform float alphaScale;
out vec4 strokeColor;
void main()
{
    gl_Position = clipPosition;
    strokeColor = vec4(color.rgb, color.a * alphaScale);
})";
        constexpr const char* kFragmentShader = R"(#version 410 core
in vec4 strokeColor;
out vec4 fragColor;
void main()
{
    fragColor = strokeColor;
})";

        // Points closer than this to the camera plane are not drawn.
        constexpr float kMinimumW = 1.0e-4f;
    }

    DepthOverlay::DepthOverlay() = default;

    DepthOverlay::~DepthOverlay()
    {
        QObject::disconnect(_contextDestroyed);
        // Without its context current the objects cannot be deleted; the
        // context releases them itself when it is destroyed.
        if (_context && QOpenGLContext::currentContext() == _context) {
            release();
        } else {
            (void)_arrays.release();
            (void)_buffer.release();
            (void)_program.release();
        }
    }

    void DepthOverlay::begin(const QMatrix4x4& worldToClip, QSizeF pixels)
    {
        _worldToClip = worldToClip;
        _pixels = pixels;
        _vertices.clear();
    }

    bool DepthOverlay::toClip(const QVector3D& world, QVector4D& clip) const
    {
        clip = _worldToClip * QVector4D(world, 1.0f);
        return std::isfinite(clip.x()) && std::isfinite(clip.y()) && std::isfinite(clip.z()) && clip.w() > kMinimumW;
    }

    void DepthOverlay::addQuad(const QVector4D corners[4], const QColor& color)
    {
        const float rgba[4]{ float(color.redF()), float(color.greenF()), float(color.blueF()), float(color.alphaF()) };
        for (const int index : { 0, 1, 2, 0, 2, 3 }) {
            Vertex vertex{};
            for (int i = 0; i < 4; ++i) {
                vertex.clip[i] = corners[index][i];
                vertex.color[i] = rgba[i];
            }
            _vertices.append(vertex);
        }
    }

    void DepthOverlay::addPolyline(const QVector<QVector3D>& points, float widthPixels, const QColor& color, bool closed)
    {
        if (points.size() < 2 || _pixels.width() <= 0 || _pixels.height() <= 0) {
            return;
        }
        const int segments = closed ? int(points.size()) : int(points.size()) - 1;
        for (int i = 0; i < segments; ++i) {
            QVector4D a, b;
            if (!toClip(points[i], a) || !toClip(points[(i + 1) % points.size()], b)) {
                continue;
            }
            // Screen direction of the segment, then a perpendicular offset of
            // half the width, converted to NDC and scaled by each end's w.
            const QVector2D screenA(a.x() / a.w() * float(_pixels.width()) * 0.5f, a.y() / a.w() * float(_pixels.height()) * 0.5f);
            const QVector2D screenB(b.x() / b.w() * float(_pixels.width()) * 0.5f, b.y() / b.w() * float(_pixels.height()) * 0.5f);
            QVector2D direction = screenB - screenA;
            if (direction.lengthSquared() < 1.0e-6f) {
                continue;
            }
            direction.normalize();
            // Extend each end by half the width so consecutive segments join.
            const QVector2D along = direction * (0.5f * widthPixels);
            const QVector2D side(-along.y(), along.x());
            const auto ndc = [&](const QVector2D& pixels) {
                return QVector2D(pixels.x() / (float(_pixels.width()) * 0.5f), pixels.y() / (float(_pixels.height()) * 0.5f));
            };
            const auto corner = [&](const QVector4D& clip, const QVector2D& pixels) {
                const auto offset = ndc(pixels);
                return QVector4D(clip.x() + offset.x() * clip.w(), clip.y() + offset.y() * clip.w(), clip.z(), clip.w());
            };
            const QVector4D quad[4]{ corner(a, side - along), corner(b, side + along), corner(b, -side + along), corner(a, -side - along) };
            addQuad(quad, color);
        }
    }

    void DepthOverlay::addBox(const QVector3D& center, float halfPixels, const QColor& fill)
    {
        QVector4D clip;
        if (!toClip(center, clip) || _pixels.width() <= 0 || _pixels.height() <= 0) {
            return;
        }
        const float x = halfPixels / (float(_pixels.width()) * 0.5f) * clip.w();
        const float y = halfPixels / (float(_pixels.height()) * 0.5f) * clip.w();
        const QVector4D quad[4]{ { clip.x() - x, clip.y() - y, clip.z(), clip.w() },
                                 { clip.x() + x, clip.y() - y, clip.z(), clip.w() },
                                 { clip.x() + x, clip.y() + y, clip.z(), clip.w() },
                                 { clip.x() - x, clip.y() + y, clip.z(), clip.w() } };
        addQuad(quad, fill);
    }

    void DepthOverlay::addArrowHead(const QVector3D& base, const QVector3D& tip, float lengthPixels, float halfWidthPixels, const QColor& color)
    {
        QVector4D a, b;
        if (!toClip(base, a) || !toClip(tip, b) || _pixels.width() <= 0 || _pixels.height() <= 0) {
            return;
        }
        const QVector2D halfSize(float(_pixels.width()) * 0.5f, float(_pixels.height()) * 0.5f);
        QVector2D direction(b.x() / b.w() * halfSize.x() - a.x() / a.w() * halfSize.x(), b.y() / b.w() * halfSize.y() - a.y() / a.w() * halfSize.y());
        if (direction.lengthSquared() < 1.0e-6f) {
            return;
        }
        direction.normalize();
        const QVector2D side(-direction.y(), direction.x());
        const auto corner = [&](const QVector2D& pixels) {
            return QVector4D(b.x() + pixels.x() / halfSize.x() * b.w(), b.y() + pixels.y() / halfSize.y() * b.w(), b.z(), b.w());
        };
        const QVector4D back = corner(-direction * lengthPixels);
        const QVector4D quad[4]{ corner(QVector2D()), corner(-direction * lengthPixels + side * halfWidthPixels), back,
                                 corner(-direction * lengthPixels - side * halfWidthPixels) };
        addQuad(quad, color);
    }

    bool DepthOverlay::ensureResources()
    {
        auto* current = QOpenGLContext::currentContext();
        if (!current) {
            return false;
        }
        if (_context == current && _program) {
            return true;
        }
        if (_context && _context != current) {
            // A new context; the old one owns and releases the old objects.
            (void)_arrays.release();
            (void)_buffer.release();
            (void)_program.release();
        }
        _context = current;
        QObject::disconnect(_contextDestroyed);
        // The context need not be current while it is destroyed; it frees its
        // own objects, so the wrappers are dropped without GL calls.
        _contextDestroyed = QObject::connect(current, &QOpenGLContext::aboutToBeDestroyed, current, [this] {
            (void)_arrays.release();
            (void)_buffer.release();
            (void)_program.release();
            _context = nullptr;
        });
        _program = std::make_unique<QOpenGLShaderProgram>();
        if (!_program->addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShader) ||
            !_program->addShaderFromSourceCode(QOpenGLShader::Fragment, kFragmentShader) || !_program->link()) {
            qWarning("DepthOverlay shader failed: %s", qPrintable(_program->log()));
            _program.reset();
            return false;
        }
        _arrays = std::make_unique<QOpenGLVertexArrayObject>();
        _arrays->create();
        _buffer = std::make_unique<QOpenGLBuffer>(QOpenGLBuffer::VertexBuffer);
        _buffer->create();
        _buffer->setUsagePattern(QOpenGLBuffer::StreamDraw);
        _arrays->bind();
        _buffer->bind();
        auto* functions = current->extraFunctions();
        functions->glEnableVertexAttribArray(0);
        functions->glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void*>(offsetof(Vertex, clip)));
        functions->glEnableVertexAttribArray(1);
        functions->glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void*>(offsetof(Vertex, color)));
        _arrays->release();
        _buffer->release();
        return true;
    }

    void DepthOverlay::release()
    {
        if (_arrays) {
            _arrays->destroy();
        }
        if (_buffer) {
            _buffer->destroy();
        }
        _arrays.reset();
        _buffer.reset();
        _program.reset();
        _context = nullptr;
    }

    void DepthOverlay::draw(float hiddenAlpha)
    {
        if (_vertices.isEmpty() || !ensureResources()) {
            return;
        }
        auto* functions = _context->extraFunctions();
        GLint depthFunc = GL_LEQUAL, program = 0, arrays = 0, buffer = 0;
        functions->glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &arrays);
        functions->glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &buffer);
        GLboolean depthMask = GL_TRUE;
        functions->glGetIntegerv(GL_DEPTH_FUNC, &depthFunc);
        functions->glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        functions->glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
        const GLenum flags[]{ GL_DEPTH_TEST, GL_BLEND, GL_CULL_FACE, GL_STENCIL_TEST };
        GLboolean enabled[std::size(flags)];
        for (size_t i = 0; i < std::size(flags); ++i) {
            enabled[i] = functions->glIsEnabled(flags[i]);
        }

        _program->bind();
        _arrays->bind();
        _buffer->bind();
        _buffer->allocate(_vertices.constData(), int(_vertices.size() * sizeof(Vertex)));
        functions->glDisable(GL_CULL_FACE);
        functions->glDisable(GL_STENCIL_TEST);
        functions->glEnable(GL_BLEND);
        functions->glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        functions->glEnable(GL_DEPTH_TEST);
        functions->glDepthMask(GL_FALSE);
        // Covered parts first, dimmed, then the visible parts over them.
        functions->glDepthFunc(GL_GREATER);
        _program->setUniformValue("alphaScale", hiddenAlpha);
        functions->glDrawArrays(GL_TRIANGLES, 0, GLsizei(_vertices.size()));
        functions->glDepthFunc(GL_LEQUAL);
        _program->setUniformValue("alphaScale", 1.0f);
        functions->glDrawArrays(GL_TRIANGLES, 0, GLsizei(_vertices.size()));
        _buffer->release();
        _arrays->release();

        functions->glUseProgram(GLuint(program));
        functions->glBindVertexArray(GLuint(arrays));
        functions->glBindBuffer(GL_ARRAY_BUFFER, GLuint(buffer));
        functions->glDepthFunc(GLenum(depthFunc));
        functions->glDepthMask(depthMask);
        for (size_t i = 0; i < std::size(flags); ++i) {
            if (enabled[i]) {
                functions->glEnable(flags[i]);
            } else {
                functions->glDisable(flags[i]);
            }
        }
    }
}
