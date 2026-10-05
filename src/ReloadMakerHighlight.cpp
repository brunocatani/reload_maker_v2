// Part outlines for selection and hover (rework 03 §2, option B). GLView::paintGL
// calls reloadMakerDrawHighlights() right after the scene, on the window's own
// framebuffer (which has 8 stencil bits and is cleared every frame), so renders
// and captures show exactly what the author sees.
//
// Per highlight: the shapes' silhouette is written to the stencil buffer (1).
// The shapes are then drawn filled in the outline colour, shifted a few pixels
// in eight directions, only where the stencil is still 0; each such pixel is
// incremented so it is coloured once. A first round with the depth test colours
// the outline where it is visible; a second round without it colours, dimmed,
// what other geometry hides, so a part behind the hands still shows its shape.
//
// NifSkope's thick-line wireframe program (wireframe.prog) rasterizes nothing in
// this context (0 samples passed, measured 2026-10-05), so it is not used.
#include "glview.h"
#include "gl/glscene.h"
#include "gl/glshape.h"
#include "gl/renderer.h"

#include <QSet>

#include <algorithm>
#include <array>
#include <cmath>

namespace
{
    // Opacity of the outline where other geometry covers it.
    constexpr float kHiddenOutlineAlpha = 0.35f;
    // Directions of the shifted copies; the outline is their union.
    constexpr std::array<std::array<float, 2>, 8> kOutlineDirections{ { { 1.0f, 0.0f },
                                                                         { -1.0f, 0.0f },
                                                                         { 0.0f, 1.0f },
                                                                         { 0.0f, -1.0f },
                                                                         { 0.7071f, 0.7071f },
                                                                         { -0.7071f, 0.7071f },
                                                                         { 0.7071f, -0.7071f },
                                                                         { -0.7071f, -0.7071f } } };
}

// Draws the shape filled in one colour with the scene's current projection.
void Shape::reloadMakerDrawHighlight(const FloatVector4& color)
{
    if (isHidden() || !scene || !scene->renderer || !bindShape()) {
        return;
    }
    auto* context = scene->renderer;
    auto* program = context->useProgram("selection.prog");
    if (!program) {
        return;
    }
    setUniforms(program);
    program->uni1i("selectionFlags", 0);
    program->uni1i("selectionParam", -1);
    // Components above zero replace the vertex colour.
    program->uni4f("vertexColorOverride", FloatVector4(1.0e-15f).maxValues(color));
    const auto count = std::clamp<qsizetype>(lodTriangleCount, 0, triangles.size());
    if (count > 0) {
        context->fn->glDrawElements(GL_TRIANGLES, GLsizei(count) * 3, GL_UNSIGNED_SHORT, nullptr);
    }
}

void GLView::reloadMakerSetHighlights(QVector<ReloadMakerHighlight> highlights)
{
    const auto same = [](const ReloadMakerHighlight& a, const ReloadMakerHighlight& b) {
        return a.blocks == b.blocks && a.color == b.color && a.width == b.width;
    };
    if (std::equal(highlights.cbegin(), highlights.cend(), reloadMakerHighlights.cbegin(), reloadMakerHighlights.cend(), same)) {
        return;
    }
    reloadMakerHighlights = std::move(highlights);
    update();
}

void GLView::reloadMakerDrawHighlights()
{
    auto* renderer = scene ? scene->renderer : nullptr;
    if (!renderer || !renderer->globalUniforms) {
        return;
    }
    auto* fn = renderer->fn;
    // The GPU time of an earlier frame's outlines, read without waiting for it.
    if (reloadMakerHighlightQueryPending) {
        GLint available = 0;
        fn->glGetQueryObjectiv(reloadMakerHighlightQuery, GL_QUERY_RESULT_AVAILABLE, &available);
        if (available) {
            GLuint64 nanoseconds = 0;
            fn->glGetQueryObjectui64v(reloadMakerHighlightQuery, GL_QUERY_RESULT, &nanoseconds);
            reloadMakerHighlightMs = double(nanoseconds) / 1.0e6;
            reloadMakerHighlightQueryPending = false;
        }
    }
    QVector<QVector<Shape*>> shapes(reloadMakerHighlights.size());
    bool any = false;
    for (int i = 0; i < reloadMakerHighlights.size(); ++i) {
        const auto& blocks = reloadMakerHighlights[i].blocks;
        const QSet<int> wanted(blocks.cbegin(), blocks.cend());
        for (auto* shape : scene->shapes) {
            if (shape && !shape->isHidden() && wanted.contains(shape->id())) {
                shapes[i].append(shape);
            }
        }
        any |= !shapes[i].isEmpty();
    }
    if (!any || pixelWidth <= 0 || pixelHeight <= 0) {
        return;
    }
    // One query object for the life of the GL context; it is released with it.
    if (!reloadMakerHighlightQuery) {
        fn->glGenQueries(1, &reloadMakerHighlightQuery);
    }
    const bool timing = reloadMakerHighlightQuery && !reloadMakerHighlightQueryPending;
    if (timing) {
        fn->glBeginQuery(GL_TIME_ELAPSED, reloadMakerHighlightQuery);
    }

    GLint depthFunc = GL_LEQUAL;
    GLboolean depthMask = GL_TRUE, colorMask[4]{ GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
    glGetIntegerv(GL_DEPTH_FUNC, &depthFunc);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
    glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);
    const GLenum flags[]{ GL_DEPTH_TEST, GL_STENCIL_TEST, GL_CULL_FACE, GL_BLEND, GL_POLYGON_OFFSET_FILL };
    GLboolean enabled[std::size(flags)];
    for (size_t i = 0; i < std::size(flags); ++i) {
        enabled[i] = glIsEnabled(flags[i]);
    }
    auto& uniforms = *renderer->globalUniforms;
    std::array<FloatVector4, 4> projection;
    std::copy(std::begin(uniforms.projectionMatrix), std::end(uniforms.projectionMatrix), projection.begin());
    const auto restoreProjection = [&] {
        std::copy(projection.cbegin(), projection.cend(), std::begin(uniforms.projectionMatrix));
        renderer->setGlobalUniforms();
    };
    // Shifts the projection by a screen offset in device pixels: clip x and y
    // move by offset * w, which is the NDC offset after the divide.
    const auto shiftProjection = [&](float x, float y) {
        const float ndcX = 2.0f * x / float(pixelWidth);
        const float ndcY = 2.0f * y / float(pixelHeight);
        for (int column = 0; column < 4; ++column) {
            uniforms.projectionMatrix[column][0] = projection[column][0] + ndcX * projection[column][3];
            uniforms.projectionMatrix[column][1] = projection[column][1] + ndcY * projection[column][3];
        }
        renderer->setGlobalUniforms();
    };

    // The scene leaves the last shape's vertex attribute defaults behind (the
    // skinned hands); rigid parts must not inherit them.
    renderer->setGlobalUniforms();
    renderer->setDefaultVertexAttribs(Scene::defaultAttrMask, Scene::defaultVertexAttrs);
    glDisable(GL_CULL_FACE);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0xFF);
    glClearStencil(0);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    fn->glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    for (int i = 0; i < reloadMakerHighlights.size(); ++i) {
        if (shapes[i].isEmpty()) {
            continue;
        }
        const auto& highlight = reloadMakerHighlights[i];
        glClear(GL_STENCIL_BUFFER_BIT);
        // Silhouette into the stencil buffer only.
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glDisable(GL_DEPTH_TEST);
        glStencilFunc(GL_ALWAYS, 1, 0xFF);
        glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
        for (auto* shape : shapes[i]) {
            shape->reloadMakerDrawHighlight(FloatVector4(0.0f));
        }
        // Shifted copies outside the silhouette, each pixel once: a coloured
        // pixel's stencil leaves 0, a depth-failed one stays 0 for the dim round.
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glStencilFunc(GL_EQUAL, 0, 0xFF);
        glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
        const float radius = std::max(1.0f, 0.5f * highlight.width * float(devicePixelRatioF()));
        for (const bool visible : { true, false }) {
            if (visible) {
                glEnable(GL_DEPTH_TEST);
                glDepthFunc(GL_LEQUAL);
            } else {
                glDisable(GL_DEPTH_TEST);
            }
            const float alpha = float(highlight.color.alphaF()) * (visible ? 1.0f : kHiddenOutlineAlpha);
            const FloatVector4 color(float(highlight.color.redF()), float(highlight.color.greenF()), float(highlight.color.blueF()), alpha);
            for (const auto& direction : kOutlineDirections) {
                shiftProjection(std::round(direction[0] * radius), std::round(direction[1] * radius));
                for (auto* shape : shapes[i]) {
                    shape->reloadMakerDrawHighlight(color);
                }
            }
        }
        restoreProjection();
    }

    glStencilFunc(GL_ALWAYS, 0, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glDepthFunc(GLenum(depthFunc));
    glDepthMask(depthMask);
    glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
    for (size_t i = 0; i < std::size(flags); ++i) {
        if (enabled[i]) {
            glEnable(flags[i]);
        } else {
            glDisable(flags[i]);
        }
    }
    if (timing) {
        fn->glEndQuery(GL_TIME_ELAPSED);
        reloadMakerHighlightQueryPending = true;
    }
}
