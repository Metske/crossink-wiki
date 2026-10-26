#include "WikiDecompressor.h"
#include "WikiHtmlToPlainText.h"
#include <vector>
#include "WikiSearchActivity.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <FontCacheManager.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <cstring>

#include <HalClock.h>
#include "WikiReaderActivity.h"
#include "WikiTextFormatter.h"
#include "activities/ActivityManager.h"
#include "activities/settings/ClockSyncActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "WikiGUI.h"
#include "WikiLogoLarge.h"
#include "WikiShared.h"

namespace fui = freeink::ui;

static const char* TAG = "WikiSearchActivity";

WikiSearchActivity::WikiSearchActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                       std::string initialQuery, bool allowFastInitialRefresh)
    : Activity("WikiSearch", renderer, mappedInput),
      scratchScope_(renderer),
      allowFastInitialRefresh_(allowFastInitialRefresh),
      searchEngine_(reader_, 0),
      text(std::move(initialQuery)) {
  cursorPos = text.length();
  reader_.mount(-1, -1, -1, -1);
}

WikiSearchActivity::~WikiSearchActivity() {
  reader_.unmount();
}

void WikiSearchActivity::onEnter() {
  Activity::onEnter();
  interactionsReady = false;
  if (auto fcm = renderer.getFontCacheManager()) {
    fcm->clearCache();
  }
  WikiScratchBuffer::registerRenderer(&renderer);
  WikiScratchBuffer::get();
  if (!reader_.isMounted()) {
    reader_.mount(-1, -1, -1, -1);
  }

  auto& cfg = WikiConfig::get();
  if (reader_.isMounted() && reader_.getSectorCount() > 1) {
    alignas(4) uint8_t footer[512];
    if (reader_.readSectors(reader_.getSectorCount() - 1, 1, footer)) {
      if (footer[0] == 'C' && footer[1] == 'P' && footer[2] == 'R' && footer[3] == '1') {
        uint32_t idxSec = footer[4] | (footer[5] << 8) | (footer[6] << 16) | (footer[7] << 24);
        uint32_t chunkSec = footer[8] | (footer[9] << 8) | (footer[10] << 16) | (footer[11] << 24);
        uint32_t fontSec = footer[12] | (footer[13] << 8) | (footer[14] << 16) | (footer[15] << 24);
        uint8_t hasImg = (footer[16] & 1) ? 1 : 0;
        if (idxSec > 0 && (cfg.indexSector != idxSec || cfg.chunksSector != chunkSec ||
                           cfg.fontSector != fontSec || cfg.hasImages != hasImg)) {
          cfg.indexSector = idxSec;
          cfg.chunksSector = chunkSec;
          cfg.fontSector = fontSec;
          cfg.hasImages = hasImg;
          WikiConfig::save();
        }
      }
    }
  }
  if (cfg.indexSector > 0 && reader_.isMounted()) {
    rootSector_ = cfg.indexSector;
    searchEngine_.setRootSector(rootSector_);
    viewState_ = ViewState::SearchReady;
    if (pendingOnThisDayAfterDateSync_) {
      pendingOnThisDayAfterDateSync_ = false;
      if (openOnThisDayFromTimestamp(0)) {
        return;
      }
    }
    if (!text.empty()) {
      doSearch();
    } else if (cfg.resumeTitle[0] != '\0') {
      std::string resTitle = cfg.resumeTitle;
      uint64_t resOffset = cfg.resumeOffset;
      int resPage = cfg.resumePage;
      int resLink = cfg.resumeLink;
      std::vector<uint32_t> resOffsets = cfg.resumePageOffsets;
      bool fastRefresh = allowFastInitialRefresh_;
      allowFastInitialRefresh_ = false;
      cfg.clearResume();

      touchRouter.reset();
      auto reader = std::make_unique<WikiReaderActivity>(
          renderer, mappedInput, std::move(resTitle), resOffset, rootSector_, cfg.chunksSector);
      reader->restorePageState(resPage, resLink, resOffsets, fastRefresh);
      activityManager.pushActivity(std::move(reader));
      return;
    }
  } else {
    viewState_ = ViewState::NeedsScan;
  }

  touchRouter.reset();
  requestUpdate();
}

void WikiSearchActivity::onExit() {
  Activity::onExit();
  cancelPreviewTask();
  reader_.unmount();
}

const fui::KeyboardLayout& WikiSearchActivity::currentLayout() const {
  if (symbols) return fui::builtinKeyboardLayout(layoutId, shifted, true);
  return fui::builtinKeyboardLayout(layoutId, shifted, false, /*numberRow=*/true);
}

const fui::KeyboardKey* WikiSearchActivity::selectedKey() const {
  if (menuButtonSelected_ || selectedResultIndex >= 0 || selRow < 0) return nullptr;
  const fui::KeyboardLayout& layout = currentLayout();
  if (selRow >= layout.rowCount) return nullptr;
  const fui::KeyboardRow& row = layout.rows[selRow];
  if (selCol < 0 || selCol >= row.count) return nullptr;
  return &row.keys[selCol];
}

int WikiSearchActivity::selectedLogicalIndex() const {
  if (menuButtonSelected_ || selectedResultIndex >= 0 || selRow < 0) return -1;
  const fui::KeyboardLayout& layout = currentLayout();
  int index = 0;
  for (int r = 0; r < selRow && r < layout.rowCount; r++) {
    index += layout.rows[r].count;
  }
  return index + selCol;
}

void WikiSearchActivity::clampSelection() {
  const fui::KeyboardLayout& layout = currentLayout();
  if (layout.rowCount == 0) {
    selRow = 0;
    selCol = 0;
    return;
  }
  if (selRow < 0) selRow = 0;
  if (selRow >= layout.rowCount) selRow = layout.rowCount - 1;
  const int cols = layout.rows[selRow].count;
  if (selCol < 0) selCol = 0;
  if (selCol >= cols) selCol = cols > 0 ? cols - 1 : 0;
}

void WikiSearchActivity::moveSelectionRow(int delta) {
  if (delta < 0) {
    if (menuButtonSelected_) {
      return;
    }
    if (selectedResultIndex >= 0) {
      int baseIdx = currentPageIndex * 10;
      if (selectedResultIndex + 1 < resultCount && selectedResultIndex + 1 < baseIdx + 10) {
        selectedResultIndex++;
      }
    } else {
      if (selRow == 0) {
        if (text.empty()) {
          menuButtonSelected_ = true;
        } else if (resultCount > 0) {
          if (resultCount == 10) fetchMoreResults();
          selectedResultIndex = currentPageIndex * 10;
        }
      } else if (selRow > 0) {
        selRow--;
        clampSelection();
      }
    }
  } else if (delta > 0) {
    if (menuButtonSelected_) {
      menuButtonSelected_ = false;
      selRow = 0;
      clampSelection();
    } else if (selectedResultIndex >= 0) {
      int baseIdx = currentPageIndex * 10;
      if (selectedResultIndex > baseIdx) {
        selectedResultIndex--;
      } else {
        selectedResultIndex = -1;
        selRow = 0;
        clampSelection();
      }
    } else {
      const fui::KeyboardLayout& layout = currentLayout();
      if (selRow + 1 < layout.rowCount) {
        selRow++;
        clampSelection();
      }
    }
  }
}

void WikiSearchActivity::turnResultsPage(int delta) {
  if (resultCount == 10) fetchMoreResults();
  int maxPages = (resultCount + 9) / 10;
  if (maxPages > 1) {
    if (delta < 0 && currentPageIndex > 0) {
      currentPageIndex--;
      if (selectedResultIndex >= 0) {
          selectedResultIndex -= 10;
          if (selectedResultIndex < 0) selectedResultIndex = 0;
      }
    } else if (delta > 0 && currentPageIndex < maxPages - 1) {
      currentPageIndex++;
      if (selectedResultIndex >= 0) {
          selectedResultIndex += 10;
          if (selectedResultIndex >= resultCount) selectedResultIndex = resultCount - 1;
      }
    }
  }
}

void WikiSearchActivity::moveSelectionCol(int delta) {
  if (menuButtonSelected_) {
    return;
  }
  if (selectedResultIndex >= 0) {
    turnResultsPage(delta);
    return;
  }
  const fui::KeyboardLayout& layout = currentLayout();
  if (selRow < 0 || selRow >= layout.rowCount) return;
  const int cols = layout.rows[selRow].count;
  if (cols <= 0) return;
  selCol = (selCol + delta + cols) % cols;
}

bool WikiSearchActivity::syncSelectionToValue(int16_t value) {
  const fui::KeyboardLayout& layout = currentLayout();
  for (int r = 0; r < layout.rowCount; r++) {
    for (int c = 0; c < layout.rows[r].count; c++) {
      if (layout.rows[r].keys[c].value == value) {
        selRow = r;
        selCol = c;
        selectedResultIndex = -1;
        menuButtonSelected_ = false;
        return true;
      }
    }
  }
  return false;
}

size_t WikiSearchActivity::utf8Prev(const std::string& s, size_t pos) {
  if (pos == 0) return 0;
  pos--;
  while (pos > 0 && (static_cast<uint8_t>(s[pos]) & 0xC0) == 0x80) pos--;
  return pos;
}

size_t WikiSearchActivity::utf8Next(const std::string& s, size_t pos) {
  if (pos >= s.length()) return s.length();
  pos++;
  while (pos < s.length() && (static_cast<uint8_t>(s[pos]) & 0xC0) == 0x80) pos++;
  return pos;
}

void WikiSearchActivity::insertUtf8(const char* out) {
  if (!out || !*out) return;
  const size_t n = std::strlen(out);
  if (cursorPos > text.length()) cursorPos = text.length();
  text.insert(cursorPos, out, n);
  cursorPos += n;
  selectedResultIndex = -1;
  menuButtonSelected_ = false;
  doSearch();
}

bool WikiSearchActivity::backspaceUtf8() {
  if (text.empty() || cursorPos == 0) return false;
  if (cursorPos > text.length()) cursorPos = text.length();
  const size_t prev = utf8Prev(text, cursorPos);
  text.erase(prev, cursorPos - prev);
  cursorPos = prev;
  selectedResultIndex = -1;
  doSearch();
  return true;
}

bool WikiSearchActivity::activateValue(int16_t value, bool longPress) {
  switch (value) {
    case fui::QWERTY_KEY_SHIFT:
      shifted = !shifted;
      clampSelection();
      return true;
    case fui::QWERTY_KEY_MODE:
      symbols = !symbols;
      shifted = false;
      clampSelection();
      return true;
    case fui::QWERTY_KEY_ENTER:
      if (resultCount > 0) {
        int idx = (selectedResultIndex >= 0) ? selectedResultIndex : 0;
        openArticle(searchResults[idx].title, searchResults[idx].offset, true);
      }
      return true;
    case fui::QWERTY_KEY_BACKSPACE:
      if (longPress) {
        text.clear();
        cursorPos = 0;
        doSearch();
        return true;
      }
      return backspaceUtf8();
    default: {
      const fui::KeyboardLayout& layer = currentLayout();
      const char* out = longPress ? fui::keyboardAltOutputFor(layer, value) : nullptr;
      if (!out) out = fui::keyboardOutputFor(layer, value);
      if (!out) return false;
      insertUtf8(out);
      if (shifted && !symbols) {
        shifted = false;
        clampSelection();
      }
      return true;
    }
  }
}

void WikiSearchActivity::doSearch() {
  cancelPreviewTask();
  previewResultIndex = -1;
  previewLoading = false;
  previewLoaded = false;
  previewText.clear();
  previewText.shrink_to_fit();
  if (text.empty()) {
    resultCount = 0;
    selectedResultIndex = -1;
    return;
  }
  menuButtonSelected_ = false;
  if (!reader_.isMounted()) {
    reader_.mount(-1, -1, -1, -1);
  }

  std::string searchQuery = text;
  if (!searchQuery.empty() && std::islower((unsigned char)searchQuery[0])) {
    searchQuery[0] = std::toupper((unsigned char)searchQuery[0]);
  }

  int count = searchEngine_.searchPrefix(searchQuery.c_str(), searchResults, 10);
  if (count == 0 && searchQuery != text) {
    count = searchEngine_.searchPrefix(text.c_str(), searchResults, 10);
  }
  if (count == 0) {
    std::string lowerQuery = text;
    for (char& c : lowerQuery) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (lowerQuery != searchQuery && lowerQuery != text) {
      count = searchEngine_.searchPrefix(lowerQuery.c_str(), searchResults, 10);
    }
  }
  resultCount = std::min(count, 10);
  currentPageIndex = 0;
  selectedResultIndex = -1;
}

void WikiSearchActivity::fetchMoreResults() {
  if (text.empty()) return;
  if (!reader_.isMounted()) {
    reader_.mount(-1, -1, -1, -1);
  }
  std::string searchQuery = text;
  if (!searchQuery.empty() && std::islower((unsigned char)searchQuery[0])) {
    searchQuery[0] = std::toupper((unsigned char)searchQuery[0]);
  }
  int count = searchEngine_.searchPrefix(searchQuery.c_str(), searchResults, 60);
  if (count == 0 && searchQuery != text) {
    count = searchEngine_.searchPrefix(text.c_str(), searchResults, 60);
  }
  if (count == 0) {
    std::string lowerQuery = text;
    for (char& c : lowerQuery) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (lowerQuery != searchQuery && lowerQuery != text) {
      count = searchEngine_.searchPrefix(lowerQuery.c_str(), searchResults, 60);
    }
  }
  resultCount = std::min(count, 60);
}

void WikiSearchActivity::openArticle(const char* title, uint64_t offset, bool fromSearchResults) {
  LOG_INF(TAG, "Opening article '%s' at offset %llu", title, offset);
  if (fromSearchResults) {
    WikiConfig::get().addRecentSearch(title, offset);
    WikiConfig::save();
  }
  cancelPreviewTask();
  previewResultIndex = -1;
  previewLoading = false;
  previewLoaded = false;
  previewText.clear();
  previewText.shrink_to_fit();
  reader_.unmount();
  activityManager.pushActivity(
      std::make_unique<WikiReaderActivity>(renderer, mappedInput, title, offset, rootSector_, WikiConfig::get().chunksSector));
}

void WikiSearchActivity::openRecentActivity() {
  cancelPreviewTask();
  startActivityForResult(std::make_unique<WikiRecentActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (!result.isCancelled) {
                             if (auto* fileRes = std::get_if<FilePathResult>(&result.data)) {
                               const std::string& s = fileRes->path;
                               size_t bar = s.find('|');
                               if (bar != std::string::npos && bar + 1 < s.size()) {
                                 uint64_t offset = std::strtoull(s.c_str(), nullptr, 10);
                                 std::string title = s.substr(bar + 1);
                                 if (offset != 0 && !title.empty()) {
                                   openArticle(title.c_str(), offset, false);
                                   return;
                                 }
                               }
                             }
                           }
                           requestUpdate();
                         });
}

void WikiSearchActivity::openRandomArticle() {
  if (!reader_.isMounted()) {
    reader_.mount(-1, -1, -1, -1);
  }
  WikiSearchResult res{};
  if (searchEngine_.getRandomArticle(res)) {
    openArticle(res.title, res.offset);
  }
}

bool WikiSearchActivity::openOnThisDayFromTimestamp(uint32_t) {
  uint16_t year = 0;
  uint8_t month = 0;
  uint8_t day = 0;
  uint8_t hour = 0;
  uint8_t minute = 0;
  int mon = -1;
  int mday = -1;
  if (halClock.isAvailable() && SETTINGS.clockDateHasBeenSynced &&
      halClock.getDateTime(year, month, day, hour, minute) && year >= 2024 && month >= 1 && month <= 12 &&
      day >= 1 && day <= 31) {
    mon = static_cast<int>(month) - 1;
    mday = static_cast<int>(day);
  } else {
    time_t currentTime = time(nullptr);
    tm localTime{};
    if (currentTime > 1704067200 && localtime_r(&currentTime, &localTime) != nullptr) {
      mon = std::clamp(localTime.tm_mon, 0, 11);
      mday = std::clamp(localTime.tm_mday, 1, 31);
    } else {
      return false;
    }
  }

  static constexpr const char* kMonths[12] = {
      "January", "February", "March",     "April",   "May",      "June",
      "July",    "August",   "September", "October", "November", "December",
  };
  static constexpr const char* kMonthsLower[12] = {
      "january", "february", "march",     "april",   "may",      "june",
      "july",    "august",   "september", "october", "november", "december",
  };

  const int dayNum = std::clamp(mday, 1, 31);

  const char* suffix = "th";
  if (dayNum % 100 < 11 || dayNum % 100 > 13) {
    if (dayNum % 10 == 1) suffix = "st";
    else if (dayNum % 10 == 2) suffix = "nd";
    else if (dayNum % 10 == 3) suffix = "rd";
  }

  char canonicalTitle[32];
  std::snprintf(canonicalTitle, sizeof(canonicalTitle), "%s %d", kMonths[mon], dayNum);

  char candidates[4][32];
  std::snprintf(candidates[0], sizeof(candidates[0]), "%s %d", kMonths[mon], dayNum);
  std::snprintf(candidates[1], sizeof(candidates[1]), "%s %d", kMonthsLower[mon], dayNum);
  std::snprintf(candidates[2], sizeof(candidates[2]), "%d %s", dayNum, kMonthsLower[mon]);
  std::snprintf(candidates[3], sizeof(candidates[3]), "%d%s %s", dayNum, suffix, kMonthsLower[mon]);

  if (!reader_.isMounted()) {
    reader_.mount(-1, -1, -1, -1);
  }

  uint64_t offset = 0;
  for (int i = 0; i < 4; ++i) {
    if (searchEngine_.search(candidates[i], offset)) {
      openArticle(canonicalTitle, offset);
      return true;
    }
  }
  return false;
}

void WikiSearchActivity::openOnThisDayArticle() {
  if (openOnThisDayFromTimestamp(0)) {
    return;
  }

  pendingOnThisDayAfterDateSync_ = true;
  startActivityForResult(std::make_unique<ClockSyncActivity>(renderer, mappedInput),
                         [this](const ActivityResult&) { requestUpdate(); });
}

void WikiSearchActivity::openSearchMenuPopup() {
  static constexpr const char* kOptions[] = {"Recent", "Random article", "On this day"};
  optionPopup_.show("Wikipedia Menu", kOptions, 3, 0, [this](int selected) {
    if (selected == 0) {
      openRecentActivity();
    } else if (selected == 1) {
      openRandomArticle();
    } else if (selected == 2) {
      openOnThisDayArticle();
    }
  });
  requestUpdate();
}

void WikiSearchActivity::onCancel() {
  cancelPreviewTask();
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

fui::Rect WikiSearchActivity::keyboardRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int rows = currentLayout().rowCount;
  const int gap = metrics.keyboardKeySpacing;
  const int height = rows * metrics.keyboardKeyHeight + (rows > 1 ? (rows - 1) * gap : 0);
  const int width = pageWidth * metrics.keyboardWidthPercent / 100;
  const int x = (pageWidth - width) / 2;
  const int y = pageHeight - WikiGUI::WIKI_HINTS_HEIGHT - metrics.verticalSpacing - height +
                metrics.keyboardVerticalOffset;
  return fui::Rect{static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<int16_t>(width),
                   static_cast<int16_t>(height)};
}

void WikiSearchActivity::loop() {
  if (optionPopup_.isActive()) {
    if (optionPopup_.handleInput(mappedInput, [this]() { requestUpdate(); })) {
      return;
    }
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  int tx = 0;
  int ty = 0;
  if (mappedInput.wasScreenTapped(tx, ty) && ty < metrics.topPadding + metrics.headerHeight) {
    onCancel();
    return;
  }

  if (viewState_ == ViewState::NeedsScan || viewState_ == ViewState::ScanFailed) {
    int sx = 0;
    int sy = 0;
    if (mappedInput.wasScreenTapped(sx, sy) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      startActivityForResult(std::make_unique<WikiFileSelectActivity>(renderer, mappedInput, "/"),
                             [this](const ActivityResult& result) {
                               if (auto* fileRes = std::get_if<FilePathResult>(&result.data)) {
                                 if (!fileRes->path.empty()) {
                                   if (WikiConfig::configureFromImageFile(fileRes->path)) {
                                     reader_.unmount();
                                     reader_.mount(-1, -1, -1, -1);
                                     rootSector_ = WikiConfig::get().indexSector;
                                     searchEngine_.setRootSector(rootSector_);
                                     viewState_ = ViewState::SearchReady;
                                     requestUpdate();
                                     return;
                                   }
                                   viewState_ = ViewState::ScanFailed;
                                   requestUpdate();
                                 }
                               }
                             });
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasBackGesture() || mappedInput.wasHomeGesture()) {
      onCancel();
      return;
    }
    return;
  }

  // --- SearchReady ViewState handling ---
  const int headerEndY = metrics.topPadding + metrics.headerHeight;
  const int inputY = headerEndY + 6;
  const int inputHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int listStartY = inputY + inputHeight + 4;
  constexpr int rowHeight = 26;

  if (mappedInput.wasScreenTapped(tx, ty)) {
    if (text.empty()) {
      const int pageWidth = renderer.getScreenWidth();
      if (tx >= pageWidth - 56 && ty >= inputY - 4 && ty <= inputY + inputHeight + 4) {
        menuButtonSelected_ = true;
        openSearchMenuPopup();
        return;
      }
    }
    // Check if user tapped a search result
    int baseIdx = currentPageIndex * 10;
    for (int i = 0; i < 10; ++i) {
      int displayIdx = baseIdx + (9 - i);
      if (displayIdx < resultCount) {
        int ry = listStartY + i * rowHeight;
        if (ty >= ry - 2 && ty < ry + rowHeight - 2) {
          openArticle(searchResults[displayIdx].title, searchResults[displayIdx].offset, true);
          return;
        }
      }
    }
  }

  if (previewTaskHandle && previewTaskFinished.load()) {
    cancelPreviewTask();
  }

  if (selectedResultIndex >= 0 && !previewLoaded && !previewLoading) {
    previewResultIndex = -1;
  }

  if (previewResultIndex != selectedResultIndex) {
      previewResultIndex = selectedResultIndex;
      previewLoading = selectedResultIndex >= 0;
      previewLoaded = false;
      previewText.clear();
      previewLoadingStartTime = millis();
      cancelPreviewTask();
      requestUpdate();
  }

  if (previewLoading && millis() - previewLoadingStartTime > 150 && !RenderLock::peek()) {
      {
        RenderLock lock(*this);
        if (auto fcm = renderer.getFontCacheManager()) {
          fcm->clearCache();
        }
        if (!reader_.isMounted()) {
          reader_.mount(-1, -1, -1, -1);
        }
        cancelPreview = false;
        int resultIndex = previewResultIndex;
        std::string resultText;
        if (resultIndex >= 0 && resultIndex < resultCount) {
          WikiDecompressor decompressor(reader_);
          if (decompressor.beginCluster(WikiConfig::get().chunksSector, searchResults[resultIndex].offset)) {
            bool isEof = false;
            resultText = streamHtmlToPlainTextChunk(decompressor, 500 * 1024, 0, 300, nullptr, nullptr, isEof, &cancelPreview);
          } else {
            resultText = "Failed to load preview.";
          }
        }
        extern std::string wikiSanitizePreviewText(const std::string& raw);
        previewText = wikiSanitizePreviewText(resultText);
        previewLoaded = true;
        previewLoading = false;
      }
      requestUpdate();
  }

  if (interactionsReady) {
    if (viewState_ == ViewState::SearchReady) {
        if (selectedResultIndex >= 0 || menuButtonSelected_) {
            if (mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasBackGesture()) {
                selectedResultIndex = -1;
                menuButtonSelected_ = false;
                requestUpdate();
                return;
            }
        }
    }
    int tapCandidateX = 0, tapCandidateY = 0;
    const bool tapCandidate = mappedInput.wasScreenTouchDown(tapCandidateX, tapCandidateY);
    int tapX = 0, tapY = 0;
    const bool tapped = mappedInput.wasScreenTapped(tapX, tapY);
    int hx = 0, hy = 0;
    const bool inContact = mappedInput.isScreenTouchHeld(hx, hy);

    const fui::TouchHoldRouter::Result result = touchRouter.update(
        interactions, tapCandidate, static_cast<int16_t>(tapCandidateX),
        static_cast<int16_t>(tapCandidateY), tapped, static_cast<int16_t>(tapX),
        static_cast<int16_t>(tapY), inContact, millis());
    if (result.event) {
      syncSelectionToValue(result.event.value);
      if (activateValue(result.event.value, result.event.longPress)) {
        requestUpdate();
      }
      return;
    }
  }

  // Hardware button navigation
  buttonNavigator.onRelease({MappedInputManager::Button::Up}, [this] {
    moveSelectionRow(-1);
    requestUpdate();
  });
  buttonNavigator.onLongPressOnce({MappedInputManager::Button::Up}, [this] {
    if (text.empty()) {
      menuButtonSelected_ = true;
      selectedResultIndex = -1;
      requestUpdate();
    } else if (selectedResultIndex < 0 && resultCount > 0) {
      if (resultCount == 10) fetchMoreResults();
      selectedResultIndex = currentPageIndex * 10;
      requestUpdate();
    }
  });

  buttonNavigator.onRelease({MappedInputManager::Button::Down}, [this] {
    moveSelectionRow(1);
    requestUpdate();
  });
  buttonNavigator.onLongPressOnce({MappedInputManager::Button::Down}, [this] {
    if (menuButtonSelected_ || selectedResultIndex >= 0) {
      menuButtonSelected_ = false;
      selectedResultIndex = -1;
      selRow = 0;
      clampSelection();
      requestUpdate();
    } else if (syncSelectionToValue(fui::QWERTY_KEY_BACKSPACE)) {
      requestUpdate();
    }
  });

  buttonNavigator.onRelease({MappedInputManager::Button::Left}, [this] {
    moveSelectionCol(-1);
    requestUpdate();
  });
  buttonNavigator.onLongPressOnce({MappedInputManager::Button::Left}, [this] {
    turnResultsPage(-1);
    requestUpdate();
  });

  buttonNavigator.onRelease({MappedInputManager::Button::Right}, [this] {
    moveSelectionCol(1);
    requestUpdate();
  });
  buttonNavigator.onLongPressOnce({MappedInputManager::Button::Right}, [this] {
    turnResultsPage(1);
    requestUpdate();
  });

  if (mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, 600)) {
    if (selectedResultIndex < 0) {
      const fui::KeyboardKey* selKey = selectedKey();
      if (selKey) {
        if (activateValue(selKey->value, true)) {
          requestUpdate();
        }
      }
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    LOG_INF(TAG, "Confirm button released, selectedResultIndex=%d, selRow=%d, selCol=%d", selectedResultIndex, selRow, selCol);
    if (menuButtonSelected_ && text.empty()) {
      openSearchMenuPopup();
      return;
    }
    if (selectedResultIndex >= 0 && selectedResultIndex < resultCount && selectedResultIndex < 60) {
      openArticle(searchResults[selectedResultIndex].title,
                  searchResults[selectedResultIndex].offset, true);
      return;
    }
    const fui::KeyboardKey* selKey = selectedKey();
    if (selKey) {
        LOG_INF(TAG, "selKey is valid: value=%d", selKey->value);
        if (activateValue(selKey->value, false)) {
            LOG_INF(TAG, "activateValue succeeded");
            requestUpdate();
        } else {
            LOG_INF(TAG, "activateValue returned false");
        }
    } else {
        LOG_INF(TAG, "selKey is null");
    }
  }

  if (mappedInput.wasLongPressed(MappedInputManager::Button::Back, 600)) {
    mappedInput.consumeSuppressedRelease();
    cancelPreviewTask();
    reader_.unmount();
    WikiConfig::clear();
    rootSector_ = 0;
    resultCount = 0;
    selectedResultIndex = -1;
    menuButtonSelected_ = false;
    viewState_ = ViewState::NeedsScan;
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasBackGesture() || mappedInput.wasHomeGesture()) {
    onCancel();
    return;
  }
}

void WikiSearchActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const int pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();

  const Rect header{0, metrics.topPadding, pageWidth, metrics.headerHeight};
  GUI.drawHeader(renderer, header, "Wikipedia");

  if (viewState_ == ViewState::NeedsScan) {
    renderScanPromptUi("No Wikipedia .img configured.");
  } else if (viewState_ == ViewState::ScanFailed) {
    renderScanPromptUi("Invalid Wikipedia .img file.");
  } else {
    renderSearchUi();
    if (optionPopup_.isActive()) {
      optionPopup_.render(renderer);
    }
  }

  renderer.displayBuffer();
}

void WikiSearchActivity::renderScanPromptUi(const char* statusText) {
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();

  const int fontId = UI_12_FONT_ID;
  int textWidth = renderer.getTextWidth(fontId, statusText);
  renderer.drawText(fontId, (screenWidth - textWidth) / 2, screenHeight / 2 - 50, statusText, true,
                    EpdFontFamily::REGULAR);

  const char* btn1Text = "Choose .img file";
  int btn1Width = renderer.getTextWidth(fontId, btn1Text) + 20;
  int btn1X = (screenWidth - btn1Width) / 2;
  int btn1Y = screenHeight / 2;
  renderer.fillRect(btn1X, btn1Y, btn1Width, 32, true);
  renderer.drawText(fontId, btn1X + 10, btn1Y + 6, btn1Text, false, EpdFontFamily::BOLD);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", "");
  WikiGUI::drawDoubleButtonHints(renderer, 
      {labels.btn1, ""}, 
      {labels.btn2, ""}, 
      {labels.btn3, ""}, 
      {labels.btn4, ""});
}

void WikiSearchActivity::renderSearchUi() {
  const int pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();

  const int headerEndY = metrics.topPadding + metrics.headerHeight;

  // Search input field placed safely below the header band
  const int inputY = headerEndY + 6;
  const int inputHeight = renderer.getLineHeight(UI_12_FONT_ID);
  std::string displayQuery = "Search: " + text.substr(0, cursorPos) + "_" + text.substr(cursorPos);
  renderer.drawText(UI_12_FONT_ID, 16, inputY, displayQuery.c_str(), true, EpdFontFamily::BOLD);

  if (text.empty()) {
    constexpr int btnW = 30;
    const int btnH = inputHeight;
    const int btnX = pageWidth - 16 - btnW;
    const int btnY = inputY;
    const int dotY = btnY + (btnH - 3) / 2;
    const int dotStartX = btnX + (btnW - 15) / 2;
    if (menuButtonSelected_) {
      renderer.fillRect(btnX, btnY, btnW, btnH, true);
      renderer.fillRect(dotStartX, dotY, 3, 3, false);
      renderer.fillRect(dotStartX + 6, dotY, 3, 3, false);
      renderer.fillRect(dotStartX + 12, dotY, 3, 3, false);
    } else {
      renderer.drawRect(btnX, btnY, btnW, btnH, true);
      renderer.fillRect(dotStartX, dotY, 3, 3, true);
      renderer.fillRect(dotStartX + 6, dotY, 3, 3, true);
      renderer.fillRect(dotStartX + 12, dotY, 3, 3, true);
    }
  }

  // Search results list (up to 10 results)
  const int listStartY = inputY + inputHeight + 4;
  constexpr int rowHeight = 26;

  if (text.empty()) {
      int logoX = (pageWidth - WIKILOGOLARGE_WIDTH) / 2;
      const fui::Rect kbRect = keyboardRect();
      int availableHeight = kbRect.y - listStartY;
      int logoY = listStartY + (availableHeight - WIKILOGOLARGE_HEIGHT) / 2;
      if (logoY < listStartY) logoY = listStartY;
      renderer.drawImage(WikiLogoLarge, logoX, logoY, WIKILOGOLARGE_WIDTH, WIKILOGOLARGE_HEIGHT);
  } else {
      int baseIdx = currentPageIndex * 10;
      for (int i = 0; i < 10; ++i) {
        // Intentional UX decision: reverse order so the most relevant result (baseIdx + 0) 
        // is rendered at the bottom, closest to the keyboard.
        int displayIdx = baseIdx + (9 - i); 
        int y = listStartY + i * rowHeight;
        if (displayIdx < resultCount) {
      char displayTitle[65];
      std::snprintf(displayTitle, sizeof(displayTitle), "%s", searchResults[displayIdx].title);
      bool newWord = true;
      for (int k = 0; displayTitle[k] != '\0'; ++k) {
          if (displayTitle[k] == '_') displayTitle[k] = ' ';

          if (newWord && displayTitle[k] >= 'a' && displayTitle[k] <= 'z') {
              displayTitle[k] = static_cast<char>(displayTitle[k] - 'a' + 'A');
              newWord = false;
          } else if (displayTitle[k] == ' ' || displayTitle[k] == '-') {
              newWord = true;
          } else {
              newWord = false;
          }
      }
      char itemLabel[80];
      std::snprintf(itemLabel, sizeof(itemLabel), "%d. %s", displayIdx + 1, displayTitle);
      std::string safeLabel = renderer.truncatedText(UI_10_FONT_ID, itemLabel, pageWidth - 36, EpdFontFamily::REGULAR);
      if (displayIdx == selectedResultIndex) {
        renderer.fillRect(14, y - 2, pageWidth - 28, rowHeight - 1, true);
        renderer.drawText(UI_10_FONT_ID, 18, y, safeLabel.c_str(), false, EpdFontFamily::BOLD);
      } else {
        renderer.drawText(UI_10_FONT_ID, 18, y, safeLabel.c_str(), true, EpdFontFamily::REGULAR);
      }
    }
  }
  } // close else block for !text.empty()

  if (selectedResultIndex >= 0) {
      const int hintsTop = renderer.getScreenHeight() - WikiGUI::WIKI_HINTS_HEIGHT;
      int previewBoxY = listStartY + 10 * rowHeight + 10;
      int previewBoxHeight = hintsTop - previewBoxY - 10;
      renderer.drawRect(10, previewBoxY, pageWidth - 20, previewBoxHeight, true);
      
      const char* textToDraw = previewLoaded ? previewText.c_str() : "Loading preview...";
      int lh = renderer.getLineHeight(UI_12_FONT_ID);
      int maxLines = (previewBoxHeight - 10) / lh;
      WikiTextFormatter previewFormatter(renderer, reader_, UI_12_FONT_ID, UI_12_FONT_ID, WikiConfig::get().fontSector);
      
      std::vector<std::string> lines = previewFormatter.wrapLines(textToDraw, pageWidth - 30, maxLines);
      for (size_t j = 0; j < lines.size(); ++j) {
          previewFormatter.drawText(15, previewBoxY + 5 + j * lh, lines[j].c_str(), true, false);
      }
      interactionsReady = true;
  } else {
      // FreeInkUI Keyboard
      const fui::Rect kbRect = keyboardRect();
      fui::GfxRendererTarget target(renderer);
      target.setFont(fui::GfxRendererTarget::FONT_SMALL, SMALL_FONT_ID);
      target.setFont(fui::GfxRendererTarget::FONT_BODY, UI_12_FONT_ID);
      const fui::DeviceContext device = target.deviceContext();
      const fui::InputSnapshot noInput{};
      fui::Frame<56> frame(target, device, noInput, interactions);

      fui::KeyboardProps props;
      const fui::KeyboardLayout& layout = currentLayout();
      props.layout = &layout;
      props.okLabel = tr(STR_OK_BUTTON);
      props.shiftLabel = tr(STR_KEY_SHIFT);
      props.modeLabel = symbols ? tr(STR_KEY_MODE_ABC) : tr(STR_KEY_MODE_SYMBOLS);
      props.inputMask = static_cast<uint16_t>(fui::InputTouch | fui::InputLongPress);
      props.selectedIndex = static_cast<int16_t>(selectedLogicalIndex());
      props.labelText.font = fui::GfxRendererTarget::FONT_BODY;
      props.altText.font = fui::GfxRendererTarget::FONT_SMALL;
      props.gap = static_cast<int16_t>(metrics.keyboardKeySpacing);
      props.padding = fui::Insets{0, 0, 0, 0};
      const int hintsTop = renderer.getScreenHeight() - WikiGUI::WIKI_HINTS_HEIGHT;
      props.bottomHitOverflow = static_cast<int16_t>(std::max(0, hintsTop - (kbRect.y + kbRect.height)));

      fui::keyboard(frame, kbRect, props);
      interactions.publish();
      interactionsReady = true;
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  const char* holdHint = resultCount > 0 ? "Prev Pg" : "";
  const char* holdHintRight = resultCount > 0 ? "Next Pg" : "";
  WikiGUI::drawDoubleButtonHints(renderer, 
      {labels.btn1, "Setup"}, 
      {labels.btn2, ""}, 
      {labels.btn3, holdHint}, 
      {labels.btn4, holdHintRight});
}

void WikiSearchActivity::cancelPreviewTask() {
    if (previewTaskHandle) {
        cancelPreview = true;
        while (!previewTaskFinished) {
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
        previewTaskHandle = nullptr;
        // Give the idle task a moment to free the deleted task's stack memory
        vTaskDelay(20 / portTICK_PERIOD_MS);
    }
}

std::string wikiSanitizePreviewText(const std::string& raw) {
  std::string clean;
  clean.reserve(raw.length());
  
  size_t i = 0;
  while (i < raw.length()) {
    char ch = raw[i];
    
    // Skip formatting flags
    if (ch == '\x02' || ch == '\x03') {
      i++;
      continue;
    }
    // Skip list indent marker + depth byte
    if (ch == '\x04') {
      i++;
      if (i < raw.length()) i++;
      continue;
    }
    
    // Skip placeholder tags: [IMG:...], [TABLE:...], [CLADE:...], [INFOBOX]
    if (ch == '[') {
      if (raw.compare(i, 5, "[IMG:") == 0 || raw.compare(i, 7, "[TABLE:") == 0 || 
          raw.compare(i, 7, "[CLADE:") == 0 || raw.compare(i, 9, "[INFOBOX]") == 0) {
        size_t closeBracket = raw.find(']', i);
        if (closeBracket != std::string::npos) {
          i = closeBracket + 1;
          continue;
        }
      }
    }
    
    // Replace newlines, tabs, and carriage returns with a single space
    if (ch == '\n' || ch == '\r' || ch == '\t' || ch == ' ') {
      if (!clean.empty() && clean.back() != ' ') {
        clean.push_back(' ');
      }
      i++;
      continue;
    }
    
    // Non-printable control characters below ASCII 32
    if (static_cast<unsigned char>(ch) < 32) {
      i++;
      continue;
    }
    
    clean.push_back(ch);
    i++;
  }
  
  while (!clean.empty() && clean.front() == ' ') {
    clean.erase(clean.begin());
  }
  while (!clean.empty() && clean.back() == ' ') {
    clean.pop_back();
  }
  
  return clean;
}

void WikiSearchActivity::previewTaskTrampoline(void* arg) {
  static_cast<WikiSearchActivity*>(arg)->previewTask();
}

void WikiSearchActivity::previewTask() {
  {
      if (!reader_.isMounted()) {
        reader_.mount(-1, -1, -1, -1);
      }
      int resultIndex = previewResultIndex;
      std::string resultText;
      
      if (resultIndex >= 0 && resultIndex < resultCount) {
        WikiDecompressor decompressor(reader_);
        if (decompressor.beginCluster(WikiConfig::get().chunksSector, searchResults[resultIndex].offset)) {
           bool isEof = false;
           resultText = streamHtmlToPlainTextChunk(decompressor, 500 * 1024, 0, 300, nullptr, nullptr, isEof, &cancelPreview);
        } else {
           resultText = "Failed to load preview.";
        }
      }

      if (!cancelPreview.load()) {
        previewText = wikiSanitizePreviewText(resultText);
        previewLoaded = true;
        previewLoading = false;
        requestUpdate();
      }
  } // All local variables are destroyed here
  
  previewTaskFinished = true;
  vTaskDelete(NULL);
}
