#pragma once
#include <QMainWindow>
#include <functional>
class GLView;
class NifModel;
class QCloseEvent;
class QVBoxLayout;
class StudioWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit StudioWindow(QWidget* parent = nullptr);
    GLView* getGLView() const { return _view; }
    NifModel* model() const { return _model; }
    // The native GL window's container. Widgets cannot overlay it, so view
    // controls sit in a bar above it (setViewportBar).
    QWidget* viewportContainer() const { return _container; }
    void setViewportBar(QWidget* bar);
    std::function<bool()> prepareClose;
protected:
    void closeEvent(QCloseEvent* event) override;
private:
    GLView* _view = nullptr;
    NifModel* _model = nullptr;
    QWidget* _container = nullptr;
    QVBoxLayout* _viewportLayout = nullptr;
};
