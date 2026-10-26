#pragma once
#include "activities/Activity.h"
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include "WikiSearchEngine.h"
#include "SdRawReader.h"
#include "WikiTextFormatter.h"
#include "WikiShared.h"
#include <FreeInkUICore.h>
#include "WikiGUI.h"

class WikiInfoboxViewerActivity : public Activity {
public:
    WikiInfoboxViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                              SdRawReader& reader, uint32_t rootSector, uint32_t chunksSector,
                              std::string title, uint64_t articleOffset,
                              uint32_t infoboxStreamOffset, uint32_t infoboxStreamLength);
    ~WikiInfoboxViewerActivity() override = default;
    
    void onEnter() override;
    void onExit() override;
    void loop() override;
    void render(RenderLock&& lock) override;

private:
    WikiScratchScope scratchScope_;
    WikiSearchEngine searchEngine_;
    SdRawReader& reader_;
    uint32_t chunksSector_;
    std::string title_;
    uint64_t articleOffset_ = 0;
    uint32_t infoboxStreamOffset_ = 0;
    uint32_t infoboxStreamLength_ = 0;
    std::unique_ptr<WikiTextFormatter> formatter_;

    enum class EntryType : uint8_t {
        HEADER,     // Top title or section header
        IMAGE,      // Profile / section image with caption
        KEY_VALUE   // Stacked label on top, content indented below
    };

    struct RawLink {
        size_t startChar = 0;
        size_t endChar = 0;
        std::string target;
    };

    struct WrappedToken {
        size_t textStart = 0;
        size_t textEnd = 0;
        size_t lineOffset = 0;
    };

    struct WrappedLine {
        std::string text;
        std::vector<WrappedToken> tokens;
    };

    struct LinkRect {
        int16_t x = 0;
        int16_t y = 0; // Content-space Y (relative to content top, before scrollY_)
        int16_t w = 0;
        int16_t h = 0;
    };

    struct InfoboxLink {
        uint16_t hrefOffset = 0;
        uint16_t hrefLen = 0;
        uint16_t rectStartIdx = 0;
        uint16_t rectCount = 0;
        int minY = 0;
        int maxY = 0;
    };

    struct InfoboxEntry {
        EntryType type = EntryType::KEY_VALUE;
        bool indentValue = false;
        bool hasMoreInRow = false;
        uint16_t linkStartIdx = 0;
        uint16_t linkCount = 0;
        std::string imageKey;
        uint32_t imgSector = 0;
        uint32_t imgOffsetInSector = 0;
        int imgWidth = 0;
        int imgHeight = 0;
        int y = 0;
        int height = 0;
        std::vector<std::string> keyLines;
        std::vector<std::string> valueLines;
    };

    struct PendingImageRect {
        uint32_t imgSector = 0;
        uint32_t imgOffsetInSector = 0;
        int16_t imgDrawX = 0;
        int16_t imgDrawY = 0;
        int16_t dstW = 0;
        int16_t dstH = 0;
        int16_t clipMinX = 0;
        int16_t clipMaxX = 0;
        int16_t clipMinY = 0;
        int16_t clipMaxY = 0;
        int16_t boxX = 0;
        int16_t boxY = 0;
        int16_t boxW = 0;
        int16_t boxH = 0;
    };

    std::vector<InfoboxEntry> entries_;
    std::string hrefPool_;
    std::vector<LinkRect> linkRects_;
    std::vector<InfoboxLink> links_;
    std::vector<PendingImageRect> pendingImages_;
    std::vector<uint32_t> stageStartOffsets_;
    std::vector<uint8_t> stageStartFlags_;
    int currentStage_ = 0;
    bool hasNextStage_ = false;
    bool stageLoaded_ = false;
    bool pendingImageRender_ = false;
    bool imageStage2Triggered_ = false;
    uint32_t stage1RenderCompleteMs_ = 0;
    int selectedLinkIndex_ = -1;
    int renderedLinkIndex_ = -1;
    int renderedScrollY_ = -1;
    bool pageRedrawNeeded_ = true;
    int scrollY_ = 0;
    int maxScrollY_ = 0;
    int viewWidth_ = 0;
    int viewHeight_ = 0;
    int totalContentHeight_ = 0;

    void releaseStageMemory();
    void loadStage(int stageIdx, bool scrollToBottom = false);
    void renderPendingImages();
    void renderLinkHighlightUpdate(int oldLinkIdx, int newLinkIdx);
    int measureTextWidth(const std::string& text, bool isBold) const;
    std::vector<WrappedLine> wrapTextWithTokens(const std::string& text, int maxWidth, bool isBold);
    void appendLinksForLines(const std::vector<WrappedLine>& wrappedLines,
                             const std::vector<RawLink>& rawLinks,
                             int baseLineX, int firstLineY, int lineStepY, int rectHeight,
                             bool isBold, bool centerLines);
    std::string cleanText(const std::string& input, std::vector<RawLink>* outLinks = nullptr);
};
