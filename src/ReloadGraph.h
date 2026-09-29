#pragma once
#include "ReloadRecipe.h"
#include <QWidget>
#include <functional>

namespace reloadmaker {
    class ReloadGraph final:public QWidget {
    public:
        explicit ReloadGraph(QWidget* parent=nullptr);
        void setRecipe(const ReloadRecipe* recipe);
        void setSelection(int window,float seconds);
        std::function<void(int,int)> selected;
        std::function<void(float)> sought;
    protected:
        void paintEvent(QPaintEvent*)override;
        void mousePressEvent(QMouseEvent*)override;
    private:
        struct Hit {QRectF rect;int window,track;};
        const ReloadRecipe* _recipe{nullptr}; // GUI-thread controller-owned; cleared before replacement.
        QVector<Hit> _hits;
        int _selected{-1};float _seconds{0};
        static constexpr int labelWidth=210;
    };
}
