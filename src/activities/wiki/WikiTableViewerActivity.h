#pragma once
#include "activities/Activity.h"
#include <string>
#include <vector>
#include <memory>
#include "WikiSearchEngine.h"
#include "SdRawReader.h"
#include "WikiTextFormatter.h"
#include "WikiShared.h"
#include <FreeInkUICore.h>
#include "WikiGUI.h"

class WikiTableViewerActivity : public Activity {
public:
    struct TableLayoutConfig {
        int minColWidth = 80;
        int maxColWidth = 240;
        int minFirstColWidth = 80;
        int maxFirstColWidth = 175;
        int maxUnbreakableWordPx = 150;
        int stickyLeftMaxRatioPct = 36;
        int stickyTopMaxRatioPct = 28;
        int cellPadX = 6;
        int cellPadY = 1;
        int lineSpacing = 0;
        bool snapSectors = true;
        bool ignorePresetCols = true;
        int stickyMode = 0; // 0 = Auto, 1 = Force On, 2 = Force Off
    };

    struct CellData {
        std::string displayText;
        std::string linkTarget;
        bool isHeader = false;
        bool hasImage = false;
        bool isRowSpanCont = false;
        bool isColSpanCont = false;
        bool isFullRowBanner = false;
        uint16_t logicalCol = 0;
        uint16_t colSpan = 1;
        uint16_t rowSpan = 1;
        int16_t rowSpanRootRow = -1;
        int16_t colSpanRootCol = -1;
        uint32_t imgSector = 0;
        uint32_t imgOffsetInSector = 0;
        int16_t imgWidth = 0;
        int16_t imgHeight = 0;
    };

    struct CellLayout {
        int16_t x = 0;
        int16_t y = 0;
        int16_t width = 0;
        int16_t height = 0;
        std::vector<std::string> displayLines;
    };

    struct ActiveColSpan {
        uint16_t col = 0;
        int16_t remainingRows = 0;
        uint16_t rootColSpan = 1;
        int16_t rootRow = -1;
        int16_t rootCol = -1;
        bool isHeader = false;
        bool isColSpanCont = false;
        std::string displayText;
        std::string linkTarget;
    };

    struct RowWindowInfo {
        uint32_t streamByteOffset = 0;
        uint32_t streamByteLength = 0;
        uint16_t startDataRow = 0;
        uint16_t rowCount = 0;
        uint16_t startGlobalSecY = 0;
        uint16_t numSectorsY = 1;
        std::vector<ActiveColSpan> entrySpans;
    };

    WikiTableViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, SdRawReader& reader,
                            uint32_t rootSector, uint32_t chunksSector, std::string title, std::string payload);
    WikiTableViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, SdRawReader& reader,
                            uint32_t rootSector, uint32_t chunksSector, std::string title,
                            uint64_t articleOffset, uint32_t tableStreamOffset, uint32_t tableStreamLength,
                            bool isCladePayload = false);
    ~WikiTableViewerActivity() override = default;
    
    void onEnter() override;
    void loop() override;
    void render(RenderLock&& lock) override;

    void setLayoutConfig(const TableLayoutConfig& cfg) {
        layoutCfg_ = cfg;
        rebuildWindowsAndLayout();
        updateScrollForFocusedCell();
        pageRedrawNeeded_ = true;
    }
    const TableLayoutConfig& getLayoutConfig() const { return layoutCfg_; }
    const std::vector<std::vector<CellData>>& getRows() const { return rows_; }
    const std::vector<std::vector<CellLayout>>& getCellLayouts() const { return cellLayouts_; }
    const std::vector<int>& getColWidths() const { return colWidths_; }
    const std::vector<int>& getSectorOffsetsX() const { return sectorOffsetsX_; }
    const std::vector<int>& getSectorOffsetsY() const { return sectorOffsetsY_; }
    const std::vector<int>& getSectorClipWidth() const { return sectorClipWidth_; }
    const std::vector<int>& getSectorClipHeight() const { return sectorClipHeight_; }
    int getNumSectorsX() const { return numSectorsX_; }
    int getNumSectorsY() const { return totalSectorsY_ > 0 ? totalSectorsY_ : numSectorsY_; }
    int getLocalNumSectorsY() const { return numSectorsY_; }
    int getCurrentSecX() const { return currentSecX_; }
    int getCurrentSecY() const {
        if (currentWindowIdx_ < windows_.size()) {
            return static_cast<int>(windows_[currentWindowIdx_].startGlobalSecY) + currentSecY_;
        }
        return currentSecY_;
    }
    int getLocalSecY() const { return currentSecY_; }
    int getScrollX() const { return scrollX_; }
    int getScrollY() const { return scrollY_; }
    int getFocusedRow() const { return getGlobalRowForLocalRow(focusedRow_); }
    int getLocalFocusedRow() const { return focusedRow_; }
    int getFocusedCol() const { return focusedCol_; }
    int getTotalRows() const { return totalLogicalRows_ > 0 ? totalLogicalRows_ : static_cast<int>(rows_.size()); }
    int getCurrentWindowIndex() const { return static_cast<int>(currentWindowIdx_); }
    int getWindowCount() const { return static_cast<int>(windows_.size()); }
    int getGlobalRowForLocalRow(int localRow) const {
        if (localRow < stickyTopRowCount_ || currentWindowIdx_ >= windows_.size()) {
            return localRow;
        }
        return stickyTopRowCount_ + static_cast<int>(windows_[currentWindowIdx_].startDataRow) + (localRow - stickyTopRowCount_);
    }
    bool isClade() const { return isClade_; }
    bool hasStickyTopRow() const { return hasStickyTopRow_; }
    int getStickyTopRowCount() const { return stickyTopRowCount_; }
    bool hasStickyLeftCol() const { return hasStickyLeftCol_; }
    int getStickyTopHeight() const { return stickyTopHeight_; }
    int getStickyLeftWidth() const { return stickyLeftWidth_; }
    int getViewWidth() const { return viewWidth_; }
    int getViewHeight() const { return viewHeight_; }
    void setFocusAndSector(int row, int col, int secX = -1, int secY = -1);
    size_t estimateHeapBytes() const;

private:
    WikiScratchScope scratchScope_;
    WikiSearchEngine searchEngine_;
    SdRawReader& reader_;
    uint32_t chunksSector_;
    std::string title_;
    std::string payload_;
    uint64_t articleOffset_ = 0;
    uint32_t tableStreamOffset_ = 0;
    uint32_t tableStreamLength_ = 0;
    uint32_t bodyStreamStartOffset_ = 0;
    bool isClade_ = false;
    TableLayoutConfig layoutCfg_;
    std::vector<int> presetColWidths_;
    std::vector<int> colWidths_;
    std::unique_ptr<WikiTextFormatter> formatter_;
    
    // Streamed Row-Window State
    size_t maxLogicalCols_ = 1;
    bool alreadyExpanded_ = false;
    int stickyTopRowCount_ = 0;
    int totalLogicalRows_ = 0;
    int totalSectorsY_ = 1;
    size_t currentWindowIdx_ = 0;
    std::vector<std::vector<CellData>> stickyHeaderRows_;
    std::vector<RowWindowInfo> windows_;

    std::vector<std::vector<CellData>> rows_;
    
    int scrollX_ = 0;
    int scrollY_ = 0;
    int maxScrollX_ = 0;
    int maxScrollY_ = 0;
    int focusedRow_ = 0;
    int focusedCol_ = 0;
    int renderedScrollX_ = -1;
    int renderedScrollY_ = -1;
    int renderedFocusedRow_ = -1;
    int renderedFocusedCol_ = -1;
    bool pageRedrawNeeded_ = true;
    bool pendingImageRender_ = false;
    bool imageStage2Triggered_ = false;
    uint32_t stage1RenderCompleteMs_ = 0;
    
    // Map & discrete sector layout states
    bool hasStickyTopRow_ = false;
    bool hasStickyLeftCol_ = false;
    int stickyTopHeight_ = 0;
    int stickyLeftWidth_ = 0;
    int viewWidth_ = 0;
    int viewHeight_ = 0;
    
    int numSectorsX_ = 1;
    int numSectorsY_ = 1;
    int currentSecX_ = 0;
    int currentSecY_ = 0;
    std::vector<int> sectorOffsetsX_;
    std::vector<int> sectorOffsetsY_;
    std::vector<int> sectorClipWidth_;
    std::vector<int> sectorClipHeight_;
    
    std::vector<std::vector<CellLayout>> cellLayouts_;

    struct PendingCellImage {
        uint16_t row = 0;
        uint16_t col = 0;
        uint32_t imgSector = 0;
        uint32_t imgOffsetInSector = 0;
        int16_t screenX = 0;
        int16_t screenY = 0;
        int16_t cellW = 0;
        int16_t cellH = 0;
        int16_t clipMinX = 0;
        int16_t clipMaxX = 0;
        int16_t clipMinY = 0;
        int16_t clipMaxY = 0;
        int16_t placeholderX = 0;
        int16_t placeholderY = 0;
        int16_t placeholderW = 0;
        int16_t placeholderH = 0;
    };
    std::vector<PendingCellImage> pendingImages_;
    
    CellData parseRawCell(const std::string& rawStr, bool resolveImgDimensions = true);
    bool parseRawRowLine(const std::string& line, std::vector<CellData>& outRow, bool resolveImgDimensions = true);
    void expandRawRowIntoGrid(std::vector<CellData>& srcRow, int rowIndex,
                              std::vector<ActiveColSpan>& activeCols,
                              std::vector<CellData>& outExpandedRow) const;
    void readStreamSlice(uint32_t relOffset, uint32_t length, std::string& outSlice) const;
    void parsePayload();
    void rebuildWindowsAndLayout();
    void loadRowWindow(size_t windowIdx);
    void computeLayout();
    void updateScrollForFocusedCell();
    int getEffectiveSectorClipW(int secX) const;
    int getEffectiveSectorClipH(int secY) const;
    bool getCellScreenGeometry(size_t r, size_t c, int& screenX, int& screenY,
                               int& clipMinX, int& clipMaxX, int& clipMinY, int& clipMaxY) const;
    void renderPendingImages();
    void renderFocusUpdate(int oldRow, int oldCol, int newRow, int newCol);
    std::vector<std::string> wrapText(const std::string& text, int maxWidth, bool isBold = false);
};
