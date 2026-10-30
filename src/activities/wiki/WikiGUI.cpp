#include "WikiGUI.h"
#include <EpdFont.h>
#include <HalGPIO.h>
#include "fontIds.h"
#include <cstring>

namespace WikiGUI {

namespace {
constexpr int kButtonWidth = 118;
constexpr int kNarrowButtonPositions[4] = {1, 121, 241, 361};
constexpr int kWideButtonPositions[4] = {8, 140, 272, 404};
}  // namespace

void drawDoubleButtonHints(GfxRenderer& renderer, const ButtonHint& btn1, const ButtonHint& btn2, const ButtonHint& btn3, const ButtonHint& btn4) {
    if (gpio.hasTouch()) {
        return;
    }

    const GfxRenderer::Orientation orig_orientation = renderer.getOrientation();
    renderer.setOrientation(GfxRenderer::Orientation::Portrait);

    const int pageHeight = renderer.getScreenHeight();
    const int screenWidth = renderer.getScreenWidth();

    constexpr int buttonWidth = kButtonWidth;
    constexpr int buttonHeight = WIKI_HINTS_HEIGHT;
    constexpr int buttonY = WIKI_HINTS_HEIGHT;

    const int* buttonPositions = screenWidth >= 528 ? kWideButtonPositions : kNarrowButtonPositions;

    const ButtonHint labels[] = {btn1, btn2, btn3, btn4};

    for (int i = 0; i < 4; i++) {
        const int x = buttonPositions[i];

        bool hasShort = labels[i].shortPress && labels[i].shortPress[0] != '\0';
        bool hasLong = labels[i].longPress && labels[i].longPress[0] != '\0';

        if (hasShort || hasLong) {
            int y = pageHeight - buttonY;
            const int cornerRadius = 8;
            renderer.fillRoundedRect(x, y, buttonWidth, buttonHeight, cornerRadius, Color::White);
            renderer.fillRect(x, y + buttonHeight - cornerRadius, buttonWidth, cornerRadius, false);
            renderer.drawRoundedRect(x, y, buttonWidth, buttonHeight + 5, 1, cornerRadius, true, true, false, false, true);

            if (hasShort && !hasLong) {
                // Center single line vertically
                int w = renderer.getTextWidth(SMALL_FONT_ID, labels[i].shortPress);
                renderer.drawText(SMALL_FONT_ID, x + (buttonWidth - w) / 2, pageHeight - buttonY + 11, labels[i].shortPress, true, EpdFontFamily::REGULAR);
            } else {
                if (hasShort) {
                    int w = renderer.getTextWidth(SMALL_FONT_ID, labels[i].shortPress);
                    renderer.drawText(SMALL_FONT_ID, x + (buttonWidth - w) / 2, pageHeight - buttonY + 6, labels[i].shortPress, true, EpdFontFamily::REGULAR);
                }
                if (hasLong) {
                    char holdStr[48];
                    std::snprintf(holdStr, sizeof(holdStr), "Hold: %s", labels[i].longPress);
                    int w = renderer.getTextWidth(SMALL_FONT_ID, holdStr);
                    renderer.drawText(SMALL_FONT_ID, x + (buttonWidth - w) / 2, pageHeight - buttonY + 24, holdStr, true, EpdFontFamily::REGULAR);
                }
            }
        }
    }

    renderer.setOrientation(orig_orientation);
}

void drawThreeDotsButton(GfxRenderer& renderer, const int btnX, const int btnY, const int btnW, const int btnH,
                         const bool selected) {
    const int dotY = btnY + (btnH - 3) / 2;
    const int dotStartX = btnX + (btnW - 15) / 2;
    if (selected) {
        renderer.fillRect(btnX, btnY, btnW, btnH, true);
        renderer.fillRect(dotStartX, dotY, 3, 3, false);
        renderer.fillRect(dotStartX + 6, dotY, 3, 3, false);
        renderer.fillRect(dotStartX + 12, dotY, 3, 3, false);
    } else {
        renderer.fillRect(btnX, btnY, btnW, btnH, false);
        renderer.drawRect(btnX, btnY, btnW, btnH, true);
        renderer.fillRect(dotStartX, dotY, 3, 3, true);
        renderer.fillRect(dotStartX + 6, dotY, 3, 3, true);
        renderer.fillRect(dotStartX + 12, dotY, 3, 3, true);
    }
}

}
