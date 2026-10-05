// Scene cost per frame, reported by the viewport diagnostics. CPU time runs
// from the view transform through the scene draw; GPU time covers the draw.
// Together they are what one more camera on the same scene would add, for
// example a close-up inset drawn into a corner viewport as the axes are (rework
// 03 §6). The GPU result of an earlier frame is read without waiting for it.
#include "glview.h"
#include "gl/glscene.h"
#include "gl/renderer.h"

void GLView::reloadMakerBeginSceneTiming()
{
    auto* renderer = scene ? scene->renderer : nullptr;
    if (!renderer) {
        return;
    }
    auto* fn = renderer->fn;
    if (reloadMakerSceneQueryPending) {
        GLint available = 0;
        fn->glGetQueryObjectiv(reloadMakerSceneQuery, GL_QUERY_RESULT_AVAILABLE, &available);
        if (available) {
            GLuint64 nanoseconds = 0;
            fn->glGetQueryObjectui64v(reloadMakerSceneQuery, GL_QUERY_RESULT, &nanoseconds);
            reloadMakerSceneGpuMs = double(nanoseconds) / 1.0e6;
            reloadMakerSceneQueryPending = false;
        }
    }
    // One query object for the life of the GL context; it is released with it.
    if (!reloadMakerSceneQuery) {
        fn->glGenQueries(1, &reloadMakerSceneQuery);
    }
    reloadMakerSceneQueryRunning = reloadMakerSceneQuery && !reloadMakerSceneQueryPending;
    if (reloadMakerSceneQueryRunning) {
        fn->glBeginQuery(GL_TIME_ELAPSED, reloadMakerSceneQuery);
    }
}

// Ends before the part outlines start their own query: time queries of one
// target cannot nest.
void GLView::reloadMakerEndSceneTiming(std::chrono::steady_clock::time_point start)
{
    if (reloadMakerSceneQueryRunning) {
        scene->renderer->fn->glEndQuery(GL_TIME_ELAPSED);
        reloadMakerSceneQueryRunning = false;
        reloadMakerSceneQueryPending = true;
    }
    reloadMakerSceneCpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}
