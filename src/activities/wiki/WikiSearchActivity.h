#pragma once

#include <FreeInkUIGfxRenderer.h>
#include <GfxRenderer.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <utility>

#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "util/ButtonNavigator.h"
#include "SdRawReader.h"
#include "WikiSearchEngine.h"
#include "WikiShared.h"

class WikiSearchActivity : public Activity {
 public:
  explicit WikiSearchActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                              std::string initialQuery = "", bool allowFastInitialRefresh = false);
  ~WikiSearchActivity() override;

  void onEnter() override;
  void onExit() override;
  void onActivityReturn(const ActivityResult& result);
  void loop() override;
  void render(RenderLock&&) override;

 private:
  WikiScratchScope scratchScope_;
  enum class ViewState { NeedsScan, ScanFailed, SearchReady };
  ViewState viewState_ = ViewState::NeedsScan;

  uint32_t rootSector_ = 0;
  bool allowFastInitialRefresh_ = false;
  SdRawReader reader_;
  WikiSearchEngine searchEngine_;

  std::string text;
  WikiSearchResult searchResults[60];
  int resultCount = 0;
  int currentPageIndex = 0;
  int selectedResultIndex = -1;
  int setupMenuSelection = 0;
  bool menuButtonSelected_ = false;
  bool pendingOnThisDayAfterDateSync_ = false;
  OptionPopup optionPopup_;

  int previewResultIndex = -1;
  bool previewLoading = false;
  bool previewLoaded = false;
  std::string previewText;
  unsigned long previewLoadingStartTime = 0;

  void* previewTaskHandle = nullptr;
  std::atomic<bool> cancelPreview{false};
  std::atomic<bool> previewTaskFinished{false};
  static void previewTaskTrampoline(void* arg);
  void previewTask();
  void cancelPreviewTask();


  void pickWikiImage();
  void pickWikiConfig();
  void doSearch();
  void fetchMoreResults();
  void openArticle(const char* title, uint64_t offset, bool fromSearchResults = false);
  void openSearchMenuPopup();
  void openRecentActivity();
  void openRandomArticle();
  void openOnThisDayArticle();
  bool openOnThisDayFromTimestamp(uint32_t timestamp);
  void onCancel();

  ButtonNavigator buttonNavigator;

  freeink::ui::KeyboardLayoutId layoutId = freeink::ui::KeyboardLayoutId::QwertyEn;
  bool shifted = false;
  bool symbols = false;

  freeink::ui::InteractionBuffer<56> interactions;
  int selRow = 0;
  int selCol = 0;

  bool confirmHeld = false;
  bool confirmLongHandled = false;
  bool upHeld = false;
  bool upLongHandled = false;
  bool downHeld = false;
  bool downLongHandled = false;

  bool cursorMode = false;
  size_t cursorPos = 0;

  freeink::ui::TouchHoldRouter touchRouter;
  std::atomic<bool> interactionsReady{false};

  const freeink::ui::KeyboardLayout& currentLayout() const;
  const freeink::ui::KeyboardKey* selectedKey() const;
  int selectedLogicalIndex() const;
  void clampSelection();
  void moveSelectionRow(int delta);
  void moveSelectionCol(int delta);
  void turnResultsPage(int delta);
  bool syncSelectionToValue(int16_t value);
  bool activateValue(int16_t value, bool longPress);

  void insertUtf8(const char* out);
  bool backspaceUtf8();
  static size_t utf8Prev(const std::string& s, size_t pos);
  static size_t utf8Next(const std::string& s, size_t pos);

  freeink::ui::Rect keyboardRect() const;
  void renderSearchUi();
  void renderScanPromptUi(const char* statusText);
};
