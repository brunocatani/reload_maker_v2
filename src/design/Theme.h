#pragma once

#include <QColor>
#include <QFont>
#include <QRectF>
#include <QString>

class QPainter;

// Design tokens of the approved canvas (rework/13_UI_REBUILD.md). Widgets get
// them through the stylesheet; painted chrome (viewport overlays, list rows)
// reads them here, so every surface uses the same palette.
namespace reloadmaker::design
{
    namespace color
    {
        inline constexpr QRgb ground = 0xff14191e;
        inline constexpr QRgb header = 0xff10151a;
        inline constexpr QRgb panel = 0xff1a2026;
        inline constexpr QRgb card = 0xff151b20;
        inline constexpr QRgb row = 0xff1f272e;
        inline constexpr QRgb rowHover = 0xff243039;
        inline constexpr QRgb border = 0xff2a353d;
        inline constexpr QRgb borderStrong = 0xff2f3b43;
        inline constexpr QRgb divider = 0xff253038;
        inline constexpr QRgb input = 0xff10171d;
        inline constexpr QRgb inputBorder = 0xff35434c;
        inline constexpr QRgb menu = 0xff1d252c;
        inline constexpr QRgb menuBorder = 0xff45535c;
        inline constexpr QRgb text = 0xffe3ecea;
        inline constexpr QRgb textSecondary = 0xffb9c8cd;
        inline constexpr QRgb textMuted = 0xff9aabb2;
        inline constexpr QRgb textFaint = 0xff8c9ca3;
        inline constexpr QRgb textDisabled = 0xff5d6b72;
        inline constexpr QRgb section = 0xff8fb9b3;
        inline constexpr QRgb primary = 0xff1f5c55;
        inline constexpr QRgb primaryHover = 0xff26706a;
        inline constexpr QRgb onPrimary = 0xfff2fffc;
        inline constexpr QRgb selected = 0xff26423f;
        inline constexpr QRgb button = 0xff222b32;
        inline constexpr QRgb buttonHover = 0xff2a3640;
        inline constexpr QRgb buttonBorder = 0xff34424b;
        inline constexpr QRgb accent = 0xff6cc7b4;
        inline constexpr QRgb done = 0xff7fd6ae;
        inline constexpr QRgb doneFill = 0xff1d3a31;
        inline constexpr QRgb detected = 0xff8bb8ff;
        inline constexpr QRgb detectedFill = 0xff1d2b44;
        inline constexpr QRgb needsYou = 0xffffb562;
        inline constexpr QRgb needsYouFill = 0xff43311a;
        inline constexpr QRgb selection = 0xfff0c785;
        inline constexpr QRgb selectionFill = 0xff3a3022;
        inline constexpr QRgb error = 0xffff9a8f;
        inline constexpr QRgb errorFill = 0xff40221f;
        inline constexpr QRgb ai = 0xffc7a6ff;
        inline constexpr QRgb viewport = 0xff2e2e2e;
        inline constexpr QRgb playhead = 0xffffb562;
        inline constexpr QRgb chipFill = 0xff1d3a35;
        inline constexpr QRgb chipText = 0xff8fe0cf;
    }

    // Body text is 13 px; the canvas falls back to Segoe UI, which Windows has.
    inline constexpr int kBodyPx = 13;
    inline constexpr int kSmallPx = 12;
    inline constexpr int kLabelPx = 11;

    [[nodiscard]] QColor qcolor(QRgb rgb, int alpha = 255);
    [[nodiscard]] QString hex(QRgb rgb);
    [[nodiscard]] QFont uiFont(int px, QFont::Weight weight = QFont::Normal);
    [[nodiscard]] QFont monoFont(int px);
    // Fusion with the token palette, application font and the stylesheet.
    // Call once, after the QApplication exists.
    void applyTheme();

    // The viewport's instruction pill, painted over the GL frame (widgets
    // cannot overlay the native GL window). Returns the pill's rectangle.
    QRectF paintViewportHint(QPainter& painter, QPointF topLeft, qreal maxWidth, const QString& text, bool emphasis = false);
}
