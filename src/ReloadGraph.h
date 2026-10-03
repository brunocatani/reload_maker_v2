#pragma once
#include "ReloadRecipe.h"
#include <QWidget>
#include <functional>

namespace reloadmaker {
    // The learned reload over time: one row per weapon part that moves, a bar
    // for each span of its movement, and the current time. Clicking right of
    // the part names seeks there.
    class ReloadGraph final:public QWidget {
    public:
        explicit ReloadGraph(QWidget* parent=nullptr);
        // The graph reads the recipe while painting; clear it before the recipe is replaced.
        void setRecipe(const ReloadRecipe* recipe);
        void setTime(float seconds);
        [[nodiscard]] int movingParts() const noexcept { return int(_rows.size()); }
        std::function<void(float)> sought;
    protected:
        void paintEvent(QPaintEvent*)override;
        void mousePressEvent(QMouseEvent*)override;
    private:
        struct PartRow {
            int track{-1};
            QVector<ReloadSpan> spans;
        };
        [[nodiscard]] float xAt(float seconds) const;
        [[nodiscard]] float timeAt(float x) const;
        const ReloadRecipe* _recipe{nullptr}; // GUI-thread controller-owned; cleared before replacement.
        QVector<PartRow> _rows;
        float _seconds{0};
        static constexpr int labelWidth=210;
        static constexpr int rightMargin=18;
        static constexpr int rowTop=34;
        static constexpr int rowHeight=25;
    };
}
