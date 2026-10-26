#include <FreeInkUICore.h>
#pragma once

#include "activities/UiListActivity.h"
#include "WikiHtmlToPlainText.h"
#include <string>
#include <vector>

class WikiTocActivity : public UiListActivity {
 public:
  WikiTocActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                  const std::vector<TOCEntry>& currentToc);
  ~WikiTocActivity() override = default;
  void onEnter() override;

 protected:
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;
  void onBackButton() override;
  void drawFooter() override;

 private:
  std::vector<TOCEntry> items_;
  std::vector<freeink::ui::ListItem> rowItems_;
  std::vector<std::string> displayLabels_;
};
