#include "UnattendedRun.h"

#include <QCoreApplication>
#include <QEvent>
#include <QWidget>

namespace reloadmaker
{
    namespace
    {
        // Whether the desktop sent this event: someone's pointer or keyboard,
        // or focus moving between windows. Events the editor sends itself pass.
        bool fromDesktop(const QEvent& event)
        {
            switch (event.type()) {
            case QEvent::WindowActivate:
            case QEvent::WindowDeactivate:
                // A window passes these on to its children itself, so the
                // copies are not marked as coming from the window system.
                return true;
            case QEvent::MouseMove:
            case QEvent::MouseButtonPress:
            case QEvent::MouseButtonRelease:
            case QEvent::MouseButtonDblClick:
            case QEvent::Wheel:
            case QEvent::ContextMenu:
            case QEvent::KeyPress:
            case QEvent::KeyRelease:
            case QEvent::Enter:
            case QEvent::Leave:
            case QEvent::FocusIn:
            case QEvent::FocusOut:
            case QEvent::TabletMove:
            case QEvent::TabletPress:
            case QEvent::TabletRelease:
            case QEvent::TouchBegin:
            case QEvent::TouchUpdate:
            case QEvent::TouchEnd:
            case QEvent::TouchCancel:
                return event.spontaneous();
            default:
                return false;
            }
        }

        QWidget* topLevelWidget(QObject* object)
        {
            auto* widget = qobject_cast<QWidget*>(object);
            return widget && widget->isWindow() ? widget : nullptr;
        }
    }

    UnattendedRun::UnattendedRun(QCoreApplication& application)
        : QObject(&application)
    {
        application.installEventFilter(this);
    }

    bool UnattendedRun::eventFilter(QObject* watched, QEvent* event)
    {
        if (event->type() == QEvent::Create) {
            // Sent as a widget is constructed. The attribute is read when the
            // native window is created, which is before the widget is polished.
            if (auto* window = topLevelWidget(watched)) {
                window->setAttribute(Qt::WA_ShowWithoutActivating);
            }
            return false;
        }
        if (event->type() == QEvent::Show && !event->spontaneous()) {
            if (auto* window = topLevelWidget(watched)) {
                // The native window appears after this event; then it goes behind the others.
                QMetaObject::invokeMethod(window, [window] { window->lower(); }, Qt::QueuedConnection);
            }
            return false;
        }
        return fromDesktop(*event);
    }
}
