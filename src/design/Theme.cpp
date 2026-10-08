#include "design/Theme.h"
#include "design/Icons.h"

#include <QApplication>
#include <QDir>
#include <QFontDatabase>
#include <QPainter>
#include <QPalette>

#include <algorithm>
#include <QStyleFactory>

namespace reloadmaker::design
{
    QColor qcolor(QRgb rgb, int alpha)
    {
        QColor result = QColor::fromRgb(rgb);
        result.setAlpha(alpha);
        return result;
    }

    QString hex(QRgb rgb)
    {
        return QStringLiteral("#%1").arg(rgb & 0xffffff, 6, 16, QLatin1Char('0'));
    }

    // The canvas typefaces ship in assets/fonts (SIL OFL, IBM-Plex-OFL.txt);
    // the system fonts follow so a missing file never leaves text unstyled.
    QFont uiFont(int px, QFont::Weight weight)
    {
        QFont font;
        font.setFamilies({ QStringLiteral("IBM Plex Sans"), QStringLiteral("Segoe UI") });
        font.setPixelSize(px);
        font.setWeight(weight);
        return font;
    }

    QFont monoFont(int px)
    {
        QFont font;
        font.setFamilies({ QStringLiteral("IBM Plex Mono"), QStringLiteral("Cascadia Mono"), QStringLiteral("Consolas") });
        font.setPixelSize(px);
        return font;
    }

    namespace
    {
        QPalette palette()
        {
            QPalette p;
            const auto set = [&p](QPalette::ColorRole role, QRgb rgb) { p.setColor(role, qcolor(rgb)); };
            set(QPalette::Window, color::panel);
            set(QPalette::WindowText, color::text);
            set(QPalette::Base, color::input);
            set(QPalette::AlternateBase, color::row);
            set(QPalette::Text, color::text);
            set(QPalette::Button, color::button);
            set(QPalette::ButtonText, color::text);
            set(QPalette::BrightText, color::onPrimary);
            set(QPalette::Highlight, color::primary);
            set(QPalette::HighlightedText, color::onPrimary);
            set(QPalette::ToolTipBase, color::menu);
            set(QPalette::ToolTipText, color::text);
            set(QPalette::PlaceholderText, color::textFaint);
            set(QPalette::Link, color::chipText);
            set(QPalette::Light, color::buttonBorder);
            set(QPalette::Midlight, color::borderStrong);
            set(QPalette::Mid, color::border);
            set(QPalette::Dark, color::card);
            set(QPalette::Shadow, color::header);
            for (const auto role : { QPalette::WindowText, QPalette::Text, QPalette::ButtonText }) {
                p.setColor(QPalette::Disabled, role, qcolor(color::textDisabled));
            }
            return p;
        }

        // One stylesheet for the whole application, dialogs and popups
        // included. Object names select the canvas roles.
        QString styleSheet()
        {
            using namespace color;
            const auto arrowDown = iconFile(Icon::ChevronDown, textMuted, 14);
            const auto arrowUp = iconFile(Icon::ChevronUp, textMuted, 14);
            const auto check = iconFile(Icon::Check, onPrimary, 12);
            QString css = QStringLiteral(R"(
QWidget { color:@text; font-size:13px; }
QMainWindow, QDialog { background:@ground; }
QDialog { background:@panel; }
QMainWindow::separator { background:@border; width:1px; height:1px; }

QWidget#AppHeader { background:@header; border:0; border-bottom:1px solid @border; min-height:52px; max-height:52px; }
QFrame#HeaderDivider { background:@border; border:0; }
QLabel#Wordmark { font-size:16px; font-weight:600; padding-right:6px; }
QPushButton#WeaponPill { background:@menu; border:1px solid @borderStrong; border-radius:6px; padding:0 12px; min-height:34px; text-align:left; }
QPushButton#WeaponPill:hover { border-color:@inputBorder; background:@rowHover; }
QLabel#HeaderSummary { color:@textMuted; }
QLabel#PresetChip { background:@chipFill; color:@chipText; border-radius:11px; padding:3px 10px; font-size:12px; font-weight:500; }
QLabel#PresetChip[tone="warning"] { background:@needsYouFill; color:@needsYou; }
QLabel#PresetChip[tone="detected"] { background:@detectedFill; color:@detected; }
QFrame#ModeSwitch { background:@panel; border:1px solid @borderStrong; border-radius:8px; }
QFrame#ModeSwitch QPushButton { background:transparent; border:0; border-radius:6px; padding:0 14px; min-height:30px; color:@textSecondary; }
QFrame#ModeSwitch QPushButton:hover { color:@text; background:@rowHover; }
QFrame#ModeSwitch QPushButton:checked { background:@primary; color:@onPrimary; font-weight:600; }
QFrame#StepSwitch { background:@panel; border:1px solid @borderStrong; border-radius:8px; }
QFrame#StepSwitch QPushButton { background:transparent; border:0; border-radius:6px; padding:0 8px; min-height:28px; color:@textSecondary; }
QFrame#StepSwitch QPushButton:hover { color:@text; background:@rowHover; }
QFrame#StepSwitch QPushButton:checked { background:@selected; color:@text; font-weight:600; }
QFrame#StepSwitch QPushButton:disabled { color:@textDisabled; }
QToolButton#MenuButton { background:transparent; border:1px solid @borderStrong; border-radius:6px; min-width:34px; min-height:34px; }
QToolButton#MenuButton:hover { background:@rowHover; }
QToolButton#MenuButton::menu-indicator { image:none; width:0; }

QFrame#Popover { background:@menu; border:1px solid @menuBorder; border-radius:8px; }
QMenu { background:@menu; border:1px solid @menuBorder; border-radius:8px; padding:6px; }
QMenu::item { padding:8px 28px 8px 12px; border-radius:5px; }
QMenu::item:selected { background:@selected; color:@onPrimary; }
QMenu::item:disabled { color:@textDisabled; }
QMenu::separator { height:1px; background:@borderStrong; margin:4px 6px; }
QToolTip { color:@text; background:@menu; border:1px solid @menuBorder; border-radius:6px; padding:6px 8px; }

QDockWidget > QWidget { background:@panel; }
QWidget#DockHeader { background:@panel; border-bottom:1px solid @divider; }
QScrollArea { background:transparent; border:0; }
QScrollArea > QWidget > QWidget { background:transparent; }
QWidget#Timeline { background:@card; }
QWidget#ViewBar { background:@card; border-bottom:1px solid @border; }

QLabel#Section { color:@section; font-size:11px; font-weight:600; letter-spacing:1.2px; padding-top:6px; }
QLabel#PanelTitle { font-size:18px; font-weight:600; }
QLabel#LoadoutTitle { font-size:18px; font-weight:600; }
QLabel#Hint { color:@textMuted; font-size:12px; }
QLabel#Callout, QLabel#StatusCard { background:@card; border:1px solid @border; border-radius:8px; padding:10px 12px; color:@textSecondary; }
QProgressBar#ChecklistProgress { background:@divider; border:0; border-radius:3px; }
QProgressBar#ChecklistProgress::chunk { background:@done; border-radius:3px; }
QFrame#ChecklistCard { background:@card; border:1px solid @border; border-radius:8px; }
QLabel#ChecklistCardTitle { font-size:14px; font-weight:600; }
QLabel#StateChip { border-radius:10px; padding:2px 9px; font-size:12px; font-weight:500; background:@needsYouFill; color:@needsYou; }
QLabel#StateChip[tone="confirmed"] { background:@doneFill; color:@done; }
QLabel#StateChip[tone="detected"] { background:@detectedFill; color:@detected; }
QLabel#StateChip[tone="na"] { background:@row; color:@textMuted; }
QLabel#ChecklistBanner { border-radius:8px; padding:8px 10px; font-size:12px; background:@doneFill; color:@done; }
QLabel#ChecklistBanner[tone="attention"] { background:@card; color:@textSecondary; border:1px solid @border; }
QLabel#ViewHint { color:@textFaint; font-size:12px; }
QLabel#ChecklistFooter { color:@textMuted; font-size:12px; padding-right:14px; }
QLabel#ClipPath { color:@textMuted; font-size:12px; }
QLabel#TimeLabel { color:@text; }
QLabel#SourceLabel { color:@textMuted; }

QPushButton, QToolButton { background:@button; border:1px solid @buttonBorder; border-radius:6px; padding:0 14px; min-height:32px; }
QToolButton { padding:0 8px; }
QPushButton:hover, QToolButton:hover { background:@buttonHover; }
QPushButton:pressed, QToolButton:pressed { background:@card; }
QPushButton:disabled, QToolButton:disabled { color:@textDisabled; background:transparent; border-color:@border; }
QPushButton:checked, QToolButton:checked { background:@selected; border-color:@accent; color:@onPrimary; }
QPushButton#PrimaryButton { background:@primary; border:0; color:@onPrimary; font-weight:600; }
QPushButton#PrimaryButton:hover { background:@primaryHover; }
QPushButton#PrimaryButton:disabled { background:@button; color:@textDisabled; }
QPushButton#ViewChip, QToolButton#ViewChip { background:@panel; border:1px solid @borderStrong; padding:0 10px; min-height:28px; color:@textSecondary; font-size:12px; }
QPushButton#ViewChip:hover, QToolButton#ViewChip:hover { color:@text; }
QPushButton#ViewChip:checked, QToolButton#ViewChip:checked { background:@selected; border-color:@accent; color:@onPrimary; }
QPushButton#PlayButton, QToolButton#PlayButton { background:@primary; border:0; min-width:36px; max-width:36px; min-height:32px; padding:0; }
QPushButton#PlayButton:hover, QToolButton#PlayButton:hover { background:@primaryHover; }
QPushButton#IconButton, QToolButton#IconButton { background:transparent; border:1px solid @borderStrong; min-width:32px; max-width:32px; min-height:32px; padding:0; }
QPushButton#IconButton:hover, QToolButton#IconButton:hover { background:@rowHover; }
QPushButton#Disclosure { background:transparent; border:0; padding:0 2px; text-align:left; color:@textSecondary; min-height:28px; }
QPushButton#Disclosure:hover { color:@text; background:transparent; }
QPushButton#Disclosure:checked { background:transparent; color:@text; border:0; }

QLineEdit, QComboBox, QAbstractSpinBox { background:@input; border:1px solid @inputBorder; border-radius:6px; padding:0 10px; min-height:32px; selection-background-color:@primary; }
QLineEdit:focus, QComboBox:focus, QAbstractSpinBox:focus { border-color:@accent; }
QLineEdit:disabled, QComboBox:disabled, QAbstractSpinBox:disabled { color:@textDisabled; background:@card; border-color:@border; }
QComboBox#ViewChip { background:@panel; border:1px solid @borderStrong; min-height:28px; font-size:12px; color:@textSecondary; }
QComboBox::drop-down { border:0; width:26px; }
QComboBox::down-arrow { image:url(@arrowDown); width:14px; height:14px; }
QComboBox QAbstractItemView { background:@menu; border:1px solid @menuBorder; padding:4px; outline:0; selection-background-color:@selected; selection-color:@onPrimary; }
QAbstractSpinBox { padding:0 2px 0 8px; }
QAbstractSpinBox::up-button, QAbstractSpinBox::down-button { border:0; width:16px; }
QAbstractSpinBox::up-arrow { image:url(@arrowUp); width:12px; height:12px; }
QAbstractSpinBox::down-arrow { image:url(@arrowDown); width:12px; height:12px; }

QListView, QListWidget, QTreeView, QTreeWidget, QTableView, QPlainTextEdit, QTextEdit { background:@card; border:1px solid @border; border-radius:8px; outline:0; }
QListView::item, QTreeView::item { padding:7px 8px; border-radius:5px; }
QListView::item:hover, QTreeView::item:hover { background:@rowHover; }
QListView::item:selected, QTreeView::item:selected { background:@selected; color:@onPrimary; }
QListView::indicator { width:16px; height:16px; border:1px solid @inputBorder; border-radius:4px; background:@input; }
QListView::indicator:checked { background:@primary; border-color:@accent; image:url(@check); }
QListView::indicator:disabled { background:@card; border-color:@border; }
QListWidget#Loadout { background:transparent; border:0; }
QListWidget#Loadout::item { padding:0; }
QListWidget#Loadout::item:hover, QListWidget#Loadout::item:selected { background:transparent; }
QHeaderView::section { background:@panel; color:@textMuted; border:0; border-bottom:1px solid @divider; padding:6px 8px; }

QCheckBox { spacing:8px; background:transparent; }
QCheckBox::indicator { width:16px; height:16px; border:1px solid @inputBorder; border-radius:4px; background:@input; }
QCheckBox::indicator:hover { border-color:@accent; }
QCheckBox::indicator:checked { background:@primary; border-color:@accent; image:url(@check); }
QCheckBox::indicator:disabled { background:@card; border-color:@border; }
QCheckBox:disabled { color:@textDisabled; }

QTabWidget::pane { border:0; }
QTabBar { background:transparent; }
QTabBar::tab { background:transparent; color:@textMuted; padding:9px 14px; border:0; border-bottom:2px solid transparent; }
QTabBar::tab:hover { color:@text; }
QTabBar::tab:selected { color:@text; border-bottom:2px solid @accent; font-weight:600; }

QGroupBox { border:1px solid @border; border-radius:8px; margin-top:18px; padding:12px 10px 10px; }
QGroupBox::title { subcontrol-origin:margin; left:10px; padding:0 4px; color:@section; }
QSlider::groove:horizontal { height:4px; background:@border; border-radius:2px; }
QSlider::sub-page:horizontal { background:@accent; border-radius:2px; }
QSlider::handle:horizontal { width:14px; height:14px; margin:-5px 0; background:@accent; border-radius:7px; }
QSlider::handle:horizontal:disabled { background:@textDisabled; }
QProgressBar { background:@border; border:0; border-radius:3px; text-align:center; color:@text; max-height:8px; }
QProgressBar::chunk { background:@done; border-radius:3px; }
QScrollBar:vertical { width:10px; background:transparent; margin:0; }
QScrollBar:horizontal { height:10px; background:transparent; margin:0; }
QScrollBar::handle { background:@buttonBorder; border-radius:5px; min-height:32px; min-width:32px; }
QScrollBar::handle:hover { background:@menuBorder; }
QScrollBar::add-line, QScrollBar::sub-line { width:0; height:0; }
QScrollBar::add-page, QScrollBar::sub-page { background:transparent; }
QSplitter::handle { background:@border; }
QStatusBar { background:@header; color:@textMuted; border-top:1px solid @border; font-size:12px; min-height:30px; }
QStatusBar::item { border:0; }
)");
            const std::pair<const char*, QRgb> tokens[]{
                { "@textSecondary", textSecondary }, { "@textDisabled", textDisabled }, { "@textMuted", textMuted }, { "@textFaint", textFaint },
                { "@text", text }, { "@ground", ground }, { "@header", header }, { "@panel", panel }, { "@card", card }, { "@rowHover", rowHover }, { "@row", row },
                { "@borderStrong", borderStrong }, { "@border", border }, { "@divider", divider }, { "@inputBorder", inputBorder }, { "@input", input },
                { "@menuBorder", menuBorder }, { "@menu", menu }, { "@section", section }, { "@primaryHover", primaryHover }, { "@primary", primary },
                { "@onPrimary", onPrimary }, { "@selected", selected }, { "@buttonHover", buttonHover }, { "@buttonBorder", buttonBorder },
                { "@button", button }, { "@accent", accent }, { "@doneFill", doneFill }, { "@done", done }, { "@needsYouFill", needsYouFill }, { "@needsYou", needsYou },
                { "@chipFill", chipFill }, { "@chipText", chipText }, { "@detectedFill", detectedFill }, { "@detected", detected },
            };
            // Longer names first, so @textSecondary is not read as @text.
            for (const auto& [token, value] : tokens) {
                css.replace(QLatin1String(token), hex(value));
            }
            css.replace(QLatin1String("@arrowDown"), arrowDown);
            css.replace(QLatin1String("@arrowUp"), arrowUp);
            css.replace(QLatin1String("@check"), check);
            return css;
        }
    }

    QRectF paintViewportHint(QPainter& painter, QPointF topLeft, qreal maxWidth, const QString& text, bool emphasis)
    {
        if (text.isEmpty()) {
            return {};
        }
        painter.save();
        painter.setFont(uiFont(kSmallPx, emphasis ? QFont::DemiBold : QFont::Normal));
        const QRectF bounds = painter.fontMetrics().boundingRect(QRect(0, 0, int(std::max<qreal>(80, maxWidth - 16)), 400), Qt::TextWordWrap, text);
        const QRectF pill(topLeft, QSizeF(bounds.width() + 16, bounds.height() + 8));
        painter.setPen(emphasis ? QPen(qcolor(color::accent), 1) : Qt::NoPen);
        painter.setBrush(qcolor(color::header, 214));
        painter.drawRoundedRect(pill, 4, 4);
        painter.setPen(qcolor(emphasis ? color::text : color::textSecondary));
        painter.drawText(pill.adjusted(8, 4, -8, -4), Qt::AlignLeft | Qt::TextWordWrap, text);
        painter.restore();
        return pill;
    }

    void applyTheme()
    {
        // Registered before any widget exists, so every font lookup sees them.
        const QDir fonts(QCoreApplication::applicationDirPath() + QStringLiteral("/assets/fonts"));
        for (const auto& file : fonts.entryList({ QStringLiteral("*.ttf") }, QDir::Files)) {
            if (QFontDatabase::addApplicationFont(fonts.filePath(file)) < 0) {
                qWarning("Cannot load the bundled font %s; using the system fallback.", qPrintable(file));
            }
        }
        QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
        QApplication::setPalette(palette());
        QApplication::setFont(uiFont(kBodyPx));
        qApp->setStyleSheet(styleSheet());
    }
}
