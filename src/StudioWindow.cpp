#include "StudioWindow.h"
#include "glview.h"
#include "model/nifmodel.h"
#include <QCloseEvent>
#include <QJsonArray>
#include <QPainter>
#include <QScreen>
#include <algorithm>
#include "gl/glshape.h"

StudioWindow::StudioWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("Reload Maker V2");
    // Fit the available desktop instead of a fixed size that can exceed it.
    const QRect available = screen() ? screen()->availableGeometry() : QRect(0, 0, 1600, 980);
    resize(std::min(1600, available.width() - 40), std::min(980, available.height() - 60));
    setMinimumSize(std::min(1120, available.width() - 40), std::min(700, available.height() - 60));
    _model = new NifModel(this);
    _view = new GLView(nullptr);
    setCentralWidget(_view->createWindowContainer(this));
    _view->setNif(_model);
    _view->getScene()->options &= ~(Scene::ShowGrid | Scene::ShowAxes);
    _view->setRotation(-65,0,135);
    QPixmap icon(64,64);icon.fill(Qt::transparent);
    QPainter painter(&icon);painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(QColor("#235854"));painter.setPen(Qt::NoPen);painter.drawRoundedRect(1,1,62,62,12,12);
    painter.setPen(QColor("#d8fff5"));painter.setFont(QFont("Segoe UI",32,QFont::Bold));painter.drawText(icon.rect(),Qt::AlignCenter,"R");painter.end();
    setWindowIcon(QIcon(icon));
}
void StudioWindow::closeEvent(QCloseEvent* event) {
    if (prepareClose && !prepareClose()) { event->ignore(); return; }
    QMainWindow::closeEvent(event);
}
QImage GLView::studioCaptureFrame() {
    if(!isValid()||isDisabled)return {};
    makeCurrent();
    paintGL();
    for(int pass=0;pass<2&&doCompile;++pass)paintGL();
    if(doCompile||isDisabled){doneCurrent();return {};}
    QImage result = grabFramebuffer();
    doneCurrent();
    return result;
}
QJsonObject GLView::studioDiagnostics() const {
    QJsonArray meshes;
    if (scene&&!doCompile&&!isDisabled) for (const auto* shape : scene->shapes) {
        if (shape) meshes.append(QJsonObject{{"block",shape->id()},{"hidden",shape->isHidden()},{"triangles",shape->triangles.size()}});
    }
    return {{"renderer",bool(scene&&scene->haveRenderer())},{"validContext",isValid()},{"sceneReady",bool(scene&&!doCompile&&!isDisabled)},
        {"disabled",isDisabled},{"compilePending",int(doCompile)},{"modelBlocks",model?model->getBlockCount():0},
        {"meshes",meshes},{"fieldOfView",cfg.fov},{"navigationScale",axis},{"distance",Dist},{"zoom",Zoom},{"position",QJsonArray{Pos[0],Pos[1],Pos[2]}},
        {"rotation",QJsonArray{Rot[0],Rot[1],Rot[2]}},{"width",width()},{"height",height()}};
}
