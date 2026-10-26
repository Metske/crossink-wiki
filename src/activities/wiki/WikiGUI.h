#pragma once
#include <GfxRenderer.h>

namespace WikiGUI {

struct ButtonHint {
    const char* shortPress;
    const char* longPress;
};

// Height of the double-line button hints bar
constexpr int WIKI_HINTS_HEIGHT = 46;

void drawDoubleButtonHints(GfxRenderer& renderer, const ButtonHint& btn1, const ButtonHint& btn2, const ButtonHint& btn3, const ButtonHint& btn4);

}
