#pragma once

#include <QObject>

class QCoreApplication;

namespace reloadmaker
{
    // An automated run is driven only by commands, but the editor still needs a
    // real window to render into. This keeps the desktop out of that run: its
    // windows open behind the others without taking focus, and the desktop's
    // pointer, keyboard and focus changes never reach the editor.
    //
    // Without it, someone working at the same desktop changes the run: the
    // pointer crossing the viewport moves a carried magazine, and a focus
    // change cancels its placement. Editors running side by side would do the
    // same to each other.
    class UnattendedRun final : public QObject
    {
    public:
        explicit UnattendedRun(QCoreApplication& application);

    protected:
        bool eventFilter(QObject* watched, QEvent* event) override;
    };
}
