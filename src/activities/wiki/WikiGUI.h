#pragma once
#include <GfxRenderer.h>

#include "MappedInputManager.h"

namespace WikiGUI {

struct ButtonHint {
    const char* shortPress;
    const char* longPress;
};

// Height of the double-line button hints bar on non-touch boards (0 on touch boards).
constexpr int WIKI_HINTS_HEIGHT = 46;

inline int hintsHeight(const MappedInputManager& input) {
    return input.hasTouchHardware() ? 0 : WIKI_HINTS_HEIGHT;
}

void drawDoubleButtonHints(GfxRenderer& renderer, const ButtonHint& btn1, const ButtonHint& btn2, const ButtonHint& btn3, const ButtonHint& btn4);

void drawThreeDotsButton(GfxRenderer& renderer, int btnX, int btnY, int btnW, int btnH, bool selected = false);

}
