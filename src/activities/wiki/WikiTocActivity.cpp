#include "WikiTocActivity.h"
#include "WikiReaderActivity.h"
#include "activities/ActivityManager.h"
#include "components/UITheme.h"
#include "WikiDecompressor.h"
#include "WikiHtmlToPlainText.h"
#include <Logging.h>
#include <cstring>
#include <I18n.h>
#include "WikiGUI.h"

static const char* TAG = "WikiTocActivity";

WikiTocActivity::WikiTocActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                 const std::vector<TOCEntry>& currentToc)
    : UiListActivity("WikiToc", renderer, mappedInput),
      items_(currentToc) {
    for (const auto& item : items_) {
        std::string label = item.title;
        if (label.length() > 60) label = label.substr(0, 60) + "...";
        if (item.level == 3) label = "  - " + label;
        displayLabels_.push_back(label);
    }
    for (const auto& label : displayLabels_) {
        freeink::ui::ListItem li{};
        li.label = label.c_str();
        rowItems_.push_back(li);
    }
}

void WikiTocActivity::onEnter() {
  UiListActivity::onEnter();
}

int WikiTocActivity::listCount() const {
  return items_.size();
}

const char* WikiTocActivity::headerTitle() const {
  return "Table of Contents";
}

void WikiTocActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(freeink::ui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight + 12), 0,
                                                static_cast<int16_t>(WikiGUI::WIKI_HINTS_HEIGHT), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  freeink::ui::ListProps props;
  props.items = rowItems_.data();
  props.count = static_cast<uint16_t>(rowItems_.size());
  props.action = ACTION_ROW;
  props.inputMask = freeink::ui::InputTouch;
  syncListViewport(screen, props);
  screen.list(props);
}

void WikiTocActivity::activateIndex(int index) {
  if (index >= 0 && static_cast<size_t>(index) < items_.size()) {
      ActivityResult result = IntervalResult{items_[index].charIdx};
      setResult(std::move(result));
      finish();
  }
}

void WikiTocActivity::onBackButton() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

void WikiTocActivity::drawFooter() {
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  WikiGUI::drawDoubleButtonHints(renderer, 
      WikiGUI::ButtonHint{labels.btn1, ""}, 
      WikiGUI::ButtonHint{labels.btn2, ""}, 
      WikiGUI::ButtonHint{labels.btn3, ""}, 
      WikiGUI::ButtonHint{labels.btn4, ""});
}
