#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "activities/Activity.h"
#include "SdRawReader.h"
#include "WikiHtmlToPlainText.h"
#include "WikiSearchEngine.h"
#include "WikiShared.h"
#include "components/OptionPopup.h"
#include <FreeInkUICore.h>

class WikiReaderActivity final : public Activity {
 public:
  WikiReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                     std::string articleTitle, uint64_t articleOffset, uint32_t rootSector, uint32_t chunksSector);
  ~WikiReaderActivity() override;
  void restorePageState(int pageIndex, int linkIndex, const std::vector<uint32_t>& offsets,
                        bool allowFastInitialRefresh = false);
  void onEnter() override;
  void onExit() override;
  void loop() override;
    void render(RenderLock&&) override;
  [[nodiscard]] bool isReaderActivity() const override { return true; }
 private:
  WikiScratchScope scratchScope_;
  OptionPopup optionPopup_;
    struct HistoryEntry {
      std::string title;
      uint64_t offset;
      int pageIndex = 0;
      int linkIndex = -1;
  };
  std::vector<HistoryEntry> history_;
  void loadArticle(const std::string& title, uint64_t offset);
  std::string title_;
  uint64_t articleOffset_;
  uint32_t rootSector_;
  uint32_t chunksSector_;
  SdRawReader reader_;
  WikiSearchEngine searchEngine_;
  std::vector<std::string> currentLines_;
  std::vector<uint8_t> currentLineFlags_;
  std::vector<uint32_t> pageStartOffsets_;
  struct PageLink {
      uint32_t hrefOffset;
      uint16_t hrefLen;
      uint16_t rectStartIdx;
      uint16_t rectCount;
  };
  std::string hrefPool_;
  std::vector<PageLink> pageLinks_;
  struct PageImage { int x; int y; int width; int height; uint32_t sector; uint16_t offsetInSector; };
  bool needsFullRefresh_ = false;
  bool needsWakeRefresh_ = false;
  bool allowFastInitialRefresh_ = false;
  bool pageRedrawNeeded_ = true;
  bool popupWasActive_ = false;
  bool pendingImageRender_ = false;
  bool imageStage2Triggered_ = false;
  bool imagesDecodedOnPage_ = false;
  uint32_t imageDecodeReadyAtMs_ = 0;
  std::vector<freeink::ui::Rect> prevPageImageRects_;
  int renderedLinkIndex_ = -1;
  bool closedByUser_ = false;
  int pendingRestoreLinkIndex_ = -1;
  void syncResumeStateToConfig();
  std::vector<PageImage> pageImages_;
  std::vector<freeink::ui::Rect> linkRects_;
  int currentPage_ = 0;
  int linesPerPage_ = 1;
  int viewportWidth_ = 0;
  bool loaded_ = false;
  bool isEof_ = false;
  bool hasInfobox_ = false;
  void showTableViewer(const std::string& payload);
  void showInfobox();
  int selectedLinkIndex_ = -1;
  std::vector<TOCEntry> currentToc_;
  void loadTOC();
  void jumpTo(uint32_t charIdx);
  void releasePageMemory();
  void formatCurrentPage();
  void onNextPage();
  void onPrevPage();
  void onBack();
  void openArticleMenuPopup();
  void selectPrevLink();
  void selectNextLink();
  bool followSelectedLink();
  void closeToSearch();
  void followLink(const std::string& href);
};
