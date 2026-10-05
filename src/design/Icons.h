#pragma once

#include <QIcon>
#include <QRgb>
#include <QString>

// Stroke icons drawn from paths in a 24-unit box, so they stay sharp at every
// scale without an SVG module or icon files to ship.
namespace reloadmaker::design
{
    enum class Icon
    {
        ChevronDown,
        ChevronUp,
        ChevronRight,
        Check,
        Play,
        Pause,
        Restart,
        Loop,
        Menu,
        Search,
        Fit,
        Grid,
        Tag,
        Close,
        Circle,
        Diamond,
        DotCircle,
        Minus,
        // Operate actions and options.
        Fire,           // a muzzle burst
        ReleaseForward, // a stop with an arrow running forward (slide or bolt release)
        MagazineOut,
        MagazineIn,
        MagazineGrab,
        MagazineDrop,
        Reload,         // a clockwise arrow
        Rewind,         // back to the start (reset)
        Spring,
        Ghost,          // a part and its second position
        MagazineShort,
        HandLeft,
        HandRight,
        Mirror,         // two halves facing a centre line
    };

    // Follows the button state: disabled icons are drawn in the disabled colour.
    [[nodiscard]] QIcon icon(Icon name, QRgb color);
    // A PNG of the icon for stylesheet image urls (combo arrows, check marks),
    // written once to the local cache with an @2x variant; forward slashes.
    [[nodiscard]] QString iconFile(Icon name, QRgb color, int px);
}
