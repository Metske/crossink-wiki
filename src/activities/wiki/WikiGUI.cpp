#include "WikiGUI.h"
#include <EpdFont.h>
#include "fontIds.h"
#include <cstring>

namespace WikiGUI {

void drawDoubleButtonHints(GfxRenderer& renderer, const ButtonHint& btn1, const ButtonHint& btn2, const ButtonHint& btn3, const ButtonHint& btn4) {
    const GfxRenderer::Orientation orig_orientation = renderer.getOrientation();
    renderer.setOrientation(GfxRenderer::Orientation::Portrait);

    const int pageHeight = renderer.getScreenHeight();
    const int screenWidth = renderer.getScreenWidth();
    
    constexpr int buttonWidth = 118;
    constexpr int buttonHeight = WIKI_HINTS_HEIGHT;
    constexpr int buttonY = WIKI_HINTS_HEIGHT;
    constexpr int textYOffsetShort = 6;
    constexpr int textYOffsetLong = 24;
    
    constexpr int narrowButtonPositions[] = {1, 121, 241, 361};
    constexpr int wideButtonPositions[] = {8, 140, 272, 404};
    const int* buttonPositions = screenWidth >= 528 ? wideButtonPositions : narrowButtonPositions;
    
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
                    std::string holdStr = std::string("Hold: ") + labels[i].longPress;
                    int w = renderer.getTextWidth(SMALL_FONT_ID, holdStr.c_str());
                    renderer.drawText(SMALL_FONT_ID, x + (buttonWidth - w) / 2, pageHeight - buttonY + 24, holdStr.c_str(), true, EpdFontFamily::REGULAR);
                }
            }
        }
    }

    renderer.setOrientation(orig_orientation);
}

}
