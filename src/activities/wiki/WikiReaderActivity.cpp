#include <new>
#include "WikiTocActivity.h"
#include "WikiSearchActivity.h"
#include "activities/ActivityManager.h"
#include "WikiReaderActivity.h"
#include "WikiGUI.h"
#include <HalDisplay.h>
#include <I18n.h>
#include <Logging.h>
#include <algorithm>
#include <sstream>
#include "components/UITheme.h"
#include "fontIds.h"
#include "WikiHtmlToPlainText.h"
#include "WikiDecompressor.h"
#include "WikiSearchEngine.h"
#include "WikiTextFormatter.h"
#include "WikiPayloadParser.h"
#include "WikiTableViewerActivity.h"
#include "WikiInfoboxViewerActivity.h"
#include "WikiShared.h"
#include "CrossPointState.h"
#include <FontCacheManager.h>
#include <cstddef>

static const char* TAG = "WikiReaderActivity";
namespace {
inline char hexToChar(char first, char second) {
    int digit = (first >= '0' && first <= '9') ? (first - '0') :
                (first >= 'A' && first <= 'F') ? (first - 'A' + 10) :
                (first >= 'a' && first <= 'f') ? (first - 'a' + 10) : 0;
    int digit2 = (second >= '0' && second <= '9') ? (second - '0') :
                 (second >= 'A' && second <= 'F') ? (second - 'A' + 10) :
                 (second >= 'a' && second <= 'f') ? (second - 'a' + 10) : 0;
    return (digit << 4) | digit2;
}

constexpr int SIDE_MARGIN = 16;
constexpr int BOTTOM_MARGIN = WikiGUI::WIKI_HINTS_HEIGHT + 8;
constexpr int LINE_SPACING = 3;
constexpr size_t MAX_DECOMPRESS_BYTES = 1000 * 1024;
constexpr uint32_t IMAGE_DECODE_DEBOUNCE_MS = 65;
}


void WikiReaderActivity::releasePageMemory() {
    pendingRestoreLinkIndex_ = selectedLinkIndex_;
    pendingImageRender_ = false;
    imageStage2Triggered_ = false;
    imagesDecodedOnPage_ = false;
    prevPageImageRects_.clear();
    prevPageImageRects_.shrink_to_fit();
    currentLines_.clear();
    currentLines_.shrink_to_fit();
    currentLineFlags_.clear();
    currentLineFlags_.shrink_to_fit();
    hrefPool_.clear();
    hrefPool_.shrink_to_fit();
    pageLinks_.clear();
    pageLinks_.shrink_to_fit();
    linkRects_.clear();
    linkRects_.shrink_to_fit();
    pageImages_.clear();
    pageImages_.shrink_to_fit();
    currentToc_.clear();
    currentToc_.shrink_to_fit();
}

void WikiReaderActivity::showTableViewer(const std::string& payload) {
    std::string payloadTag = payload;
    LOG_INF(TAG, "showTableViewer requested for payload: %s", payloadTag.c_str());
    releasePageMemory();
    if (auto fcm = renderer.getFontCacheManager()) {
        fcm->clearCache();
    }
    uint32_t tableStreamOffset = 0;
    uint32_t tableStreamLength = 0;
    bool found = false;
    {
        WikiDecompressor decompressor(reader_);
        if (!decompressor.beginCluster(chunksSector_, articleOffset_)) {
            LOG_ERR(TAG, "Failed to begin cluster for table payload!");
            formatCurrentPage();
            return;
        }
        found = WikiPayloadParser::findPayloadRange(decompressor, payloadTag, tableStreamOffset, tableStreamLength);
    }
    if (found && tableStreamLength > 0) {
        const bool isCladePayload = (payloadTag.rfind("CLADE:", 0) == 0);
        LOG_INF(TAG, "Located table payload at stream offset %u, length: %u", tableStreamOffset, tableStreamLength);
        startActivityForResult(std::make_unique<WikiTableViewerActivity>(
                                   renderer, mappedInput, reader_, rootSector_, chunksSector_,
                                   title_, articleOffset_, tableStreamOffset, tableStreamLength, isCladePayload),
                               [this](const ActivityResult& result) {
            if (!result.isCancelled) {
                if (auto* p = std::get_if<FilePathResult>(&result.data)) {
                    history_.push_back({title_, articleOffset_, currentPage_, selectedLinkIndex_});
                    if (history_.size() > 20) history_.erase(history_.begin());
                    followLink(p->path);
                    return;
                }
            }
            formatCurrentPage();
            pageRedrawNeeded_ = true;
        });
    } else {
        LOG_ERR(TAG, "Failed to locate table payload for %s!", payloadTag.c_str());
        formatCurrentPage();
    }
}

void WikiReaderActivity::showInfobox() {
    LOG_INF(TAG, "showInfobox requested");
    releasePageMemory();
    if (auto fcm = renderer.getFontCacheManager()) {
        fcm->clearCache();
    }
    uint32_t infoboxStreamOffset = 0;
    uint32_t infoboxStreamLength = 0;
    bool found = false;
    {
        WikiDecompressor decompressor(reader_);
        if (!decompressor.beginCluster(chunksSector_, articleOffset_)) {
            LOG_ERR(TAG, "Failed to begin cluster for infobox payload!");
            formatCurrentPage();
            return;
        }
        found = WikiPayloadParser::findPayloadRange(decompressor, "INFOBOX", infoboxStreamOffset, infoboxStreamLength);
    }
    if (found && infoboxStreamLength > 0) {
        LOG_INF(TAG, "Located infobox payload at stream offset %u, length: %u", infoboxStreamOffset, infoboxStreamLength);
        startActivityForResult(std::make_unique<WikiInfoboxViewerActivity>(
                                   renderer, mappedInput, reader_, rootSector_, chunksSector_,
                                   title_, articleOffset_, infoboxStreamOffset, infoboxStreamLength),
                               [this](const ActivityResult& result) {
            if (!result.isCancelled) {
                if (auto* p = std::get_if<FilePathResult>(&result.data)) {
                    history_.push_back({title_, articleOffset_, currentPage_, selectedLinkIndex_});
                    if (history_.size() > 20) history_.erase(history_.begin());
                    followLink(p->path);
                    return;
                }
            }
            formatCurrentPage();
            pageRedrawNeeded_ = true;
        });
    } else {
        LOG_INF(TAG, "No infobox payload found for article");
        formatCurrentPage();
    }
}

WikiReaderActivity::WikiReaderActivity
(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                       std::string articleTitle, uint64_t articleOffset,
                                       uint32_t rootSector, uint32_t chunksSector)
    : Activity("WikiReader", renderer, mappedInput),
      scratchScope_(renderer),
      title_(std::move(articleTitle)),
      articleOffset_(articleOffset),
      rootSector_(rootSector),
      chunksSector_(chunksSector),
      searchEngine_(reader_, rootSector) {
    pageStartOffsets_.push_back(0);
}

WikiReaderActivity::~WikiReaderActivity() {
  reader_.unmount();
}

void WikiReaderActivity::restorePageState(int pageIndex, int linkIndex, const std::vector<uint32_t>& offsets,
                                          bool allowFastInitialRefresh) {
  if (!offsets.empty()) {
    pageStartOffsets_ = offsets;
    currentPage_ = std::clamp(pageIndex, 0, static_cast<int>(pageStartOffsets_.size()) - 1);
  } else {
    pageStartOffsets_.assign(1, 0);
    currentPage_ = 0;
  }
  pendingRestoreLinkIndex_ = linkIndex;
  needsWakeRefresh_ = !allowFastInitialRefresh;
  allowFastInitialRefresh_ = allowFastInitialRefresh;
}

void WikiReaderActivity::syncResumeStateToConfig() {
  auto& cfg = WikiConfig::get();
  std::strncpy(cfg.resumeTitle, title_.c_str(), sizeof(cfg.resumeTitle) - 1);
  cfg.resumeTitle[sizeof(cfg.resumeTitle) - 1] = '\0';
  cfg.resumeOffset = articleOffset_;
  cfg.resumeLink = selectedLinkIndex_;
  cfg.resumePageOffsets.clear();
  if (currentPage_ >= 0 && currentPage_ < static_cast<int>(pageStartOffsets_.size())) {
    if (currentPage_ < 64) {
      cfg.resumePage = currentPage_;
      cfg.resumePageOffsets.assign(pageStartOffsets_.begin(), pageStartOffsets_.begin() + currentPage_ + 1);
    } else {
      cfg.resumePage = 0;
      cfg.resumePageOffsets.push_back(pageStartOffsets_[currentPage_]);
    }
  } else {
    cfg.resumePage = 0;
    cfg.resumePageOffsets.push_back(0);
  }
}

void WikiReaderActivity::onEnter() {
  Activity::onEnter();
  WikiScratchBuffer::registerRenderer(&renderer);
  if (auto fcm = renderer.getFontCacheManager()) {
    fcm->clearCache();
  }
  WikiScratchBuffer::get();
  closedByUser_ = false;
  pageRedrawNeeded_ = true;
  APP_STATE.openEpubPath = "/.crosspoint/wiki.cfg";
  APP_STATE.saveToFile();
  if (!loaded_) {
    const int screenHeight = renderer.getScreenHeight();
    const auto& metrics = UITheme::getInstance().getMetrics();
    const int headerEndY = metrics.topPadding + metrics.headerHeight;
    const int contentStartY = headerEndY + 8;
    const int viewportHeight = screenHeight - contentStartY - BOTTOM_MARGIN;
    const int fontId = WikiFont::getBodyFontId();
    const int lineHeight = renderer.getLineHeight(fontId) + LINE_SPACING;
    
    linesPerPage_ = std::max(1, viewportHeight / lineHeight);
    viewportWidth_ = renderer.getScreenWidth() - (SIDE_MARGIN * 2);

    if (!reader_.isMounted()) {
      reader_.mount(-1, -1, -1, -1);
    }
    {
      WikiDecompressor decompressor(reader_);
      if (decompressor.beginCluster(chunksSector_, articleOffset_)) {
        char buf[4096];
        size_t n = 0;
        while (n < sizeof(buf)) {
          size_t r = decompressor.read(buf + n, sizeof(buf) - n);
          if (r == 0) break;
          n += r;
        }
        if (n > 0) {
          std::string_view sv(buf, n);
          if (sv.find("[INFOBOX]") != std::string_view::npos) {
            hasInfobox_ = true;
          }
        }
      }
    }
    formatCurrentPage();
    loaded_ = true;
  }
  requestUpdate();
}

void WikiReaderActivity::onExit() {
  Activity::onExit();
  if (auto fcm = renderer.getFontCacheManager()) {
    fcm->clearCache();
  }
  if (closedByUser_) {
    WikiConfig::get().clearResume();
  }
  currentLines_.clear();
  currentLines_.shrink_to_fit();
  currentLineFlags_.clear();
  currentLineFlags_.shrink_to_fit();
  pageStartOffsets_.clear();
  pageStartOffsets_.shrink_to_fit();
  currentToc_.clear();
  currentToc_.shrink_to_fit();
  history_.clear();
  history_.shrink_to_fit();
  hrefPool_.clear();
  hrefPool_.shrink_to_fit();
  pageLinks_.clear();
  pageImages_.clear();
  pageImages_.shrink_to_fit();
  prevPageImageRects_.clear();
  prevPageImageRects_.shrink_to_fit();
  pendingImageRender_ = false;
  imageStage2Triggered_ = false;
  imagesDecodedOnPage_ = false;
  pageLinks_.shrink_to_fit();
  linkRects_.clear();
  linkRects_.shrink_to_fit();
  reader_.unmount();

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.openEpubPath = (WikiConfig::get().resumeTitle[0] != '\0') ? "/.crosspoint/wiki.cfg" : "";
  WikiConfig::save();
  APP_STATE.saveToFile();
}

void WikiReaderActivity::formatCurrentPage() {
  RenderLock lock(*this);
  const int bodyFontId = WikiFont::getBodyFontId();
  const int headingFontId = WikiFont::getHeadingFontId();
  WikiTextFormatter formatter(renderer, reader_, bodyFontId, headingFontId, WikiConfig::get().fontSector);

  currentLines_.clear();
  currentLineFlags_.clear();
  pageLinks_.clear();
  pageImages_.clear();
  linkRects_.clear();
  hrefPool_.clear();
  selectedLinkIndex_ = -1;
  renderedLinkIndex_ = -1;
  pageRedrawNeeded_ = true;
  pendingImageRender_ = false;
  imageStage2Triggered_ = false;
  imagesDecodedOnPage_ = false;

  if (auto fcm = renderer.getFontCacheManager()) {
      fcm->clearCache();
  }

  if (currentPage_ >= static_cast<int>(pageStartOffsets_.size())) {
      return;
  }
  uint32_t startChar = pageStartOffsets_[currentPage_];

  std::vector<ExtractedLink> rawLinks;
  std::string chunk;
  {
    WikiDecompressor decompressor(reader_);
    if (!decompressor.beginCluster(chunksSector_, articleOffset_)) {
      currentLines_.push_back("Error: Failed to read article.");
      currentLineFlags_.push_back(0);
      isEof_ = true;
      return;
    }
    chunk = streamHtmlToPlainTextChunk(decompressor, MAX_DECOMPRESS_BYTES, startChar, 4000, &rawLinks, &hrefPool_, isEof_);
  }
  
  // Stop at the earliest appended payload block (merged infobox, tables, clades) so payload
  // data never leaks into the article page text.
  size_t endDelim = std::string::npos;
  for (const char* marker : {"===INFOBOX===", "===TABLE:", "===CLADE:", "===END==="}) {
      size_t pos = chunk.find(marker);
      if (pos != std::string::npos && (endDelim == std::string::npos || pos < endDelim)) {
          endDelim = pos;
      }
  }
  if (endDelim != std::string::npos) {
      chunk.erase(endDelim);
      isEof_ = true;
  }

  const int fontId = bodyFontId;
  const int lineHeight = renderer.getLineHeight(fontId) + LINE_SPACING;
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentStartY = metrics.topPadding + metrics.headerHeight + 8;

  size_t pLen = chunk.length();
  size_t chunkStartIdx = 0;
  uint32_t charactersWrapped = 0;
  uint8_t currentIndent = 0;

  auto addLinkRect = [&](uint32_t charIdx, uint32_t wordLen, int x, int y, int w, int h) {
      uint32_t charEnd = charIdx + wordLen;
      for (const auto& rl : rawLinks) {
          if (rl.endCharIndex > rl.startCharIndex && charIdx < rl.endCharIndex && charEnd > rl.startCharIndex) {
              int foundIdx = -1;
              for (size_t i = 0; i < pageLinks_.size(); ++i) {
                  if (pageLinks_[i].hrefOffset == rl.hrefOffset) {
                      foundIdx = i;
                      break;
                  }
              }
              if (foundIdx == -1) {
                  PageLink pl;
                  pl.hrefOffset = rl.hrefOffset;
                  pl.hrefLen = rl.hrefLen;
                  pl.rectStartIdx = linkRects_.size();
                  pl.rectCount = 0;
                  pageLinks_.push_back(pl);
                  foundIdx = pageLinks_.size() - 1;
              }
              linkRects_.push_back({static_cast<int16_t>(x), static_cast<int16_t>(y + 3), static_cast<int16_t>(w), static_cast<int16_t>(h - 2)});
              pageLinks_[foundIdx].rectCount++;
              break;
          }
      }
  };

  struct HeadingCheckpoint {
      bool active = false;
      bool canRollback = false;
      size_t linesSize = 0;
      size_t linksSize = 0;
      size_t linkRectsSize = 0;
      size_t imagesSize = 0;
      uint32_t charsWrapped = 0;
      int bodyLinesAfter = 0;
      uint8_t firstHeadingLevel = 0;
  } headingCp;
  bool hasCommittedContentOnPage = false;
  bool pageBreakReached = false;

  auto rollbackHeadingCp = [&]() {
      currentLines_.resize(headingCp.linesSize);
      currentLineFlags_.resize(headingCp.linesSize);
      pageLinks_.resize(headingCp.linksSize);
      linkRects_.resize(headingCp.linkRectsSize);
      pageImages_.resize(headingCp.imagesSize);
      charactersWrapped = headingCp.charsWrapped;
      headingCp.active = false;
  };

  auto onBodyLinePushed = [&]() {
      if (headingCp.active) {
          headingCp.bodyLinesAfter++;
          if (headingCp.bodyLinesAfter >= 3) {
              headingCp.active = false;
              hasCommittedContentOnPage = true;
          }
      } else {
          hasCommittedContentOnPage = true;
      }
  };

  while (chunkStartIdx < pLen && currentLines_.size() < static_cast<size_t>(linesPerPage_)) {
    size_t pEndIdx = chunk.find('\n', chunkStartIdx);
    size_t origChunkStartIdx = chunkStartIdx;
    uint8_t lineFlag = 0;
    while (chunkStartIdx < pEndIdx && chunkStartIdx < pLen) {
        if (chunk[chunkStartIdx] == '\x02') { lineFlag = 2; chunkStartIdx++; }
        else if (chunk[chunkStartIdx] == '\x03') { lineFlag = 3; chunkStartIdx++; }
        else if (chunk[chunkStartIdx] == '\x04' && chunkStartIdx + 1 < pEndIdx && chunkStartIdx + 1 < pLen) {
            currentIndent = chunk[chunkStartIdx + 1];
            chunkStartIdx += 2;
        }
        else break;
    }
    if (lineFlag == 2 || lineFlag == 3) {
        if (!headingCp.active) {
            headingCp.active = true;
            headingCp.canRollback = hasCommittedContentOnPage;
            headingCp.firstHeadingLevel = lineFlag;
            headingCp.bodyLinesAfter = 0;
            headingCp.linesSize = currentLines_.size();
            if (headingCp.linesSize > 0 && currentLines_[headingCp.linesSize - 1].empty() &&
                (currentLineFlags_[headingCp.linesSize - 1] & 0x08) == 0) {
                headingCp.linesSize--;
            }
            headingCp.linksSize = pageLinks_.size();
            headingCp.linkRectsSize = linkRects_.size();
            headingCp.imagesSize = pageImages_.size();
            headingCp.charsWrapped = origChunkStartIdx;
        } else {
            // Another heading arrived while the previous heading group had < 3 body lines.
            // If the previous heading already had >= 2 body lines (or >= 1 body line before a new H2),
            // commit the previous heading and start a new checkpoint here.
            // Otherwise (e.g., H2 followed immediately or after a 1-line hatnote by H3),
            // keep the checkpoint before H2 so H2 and H3 are never split across pages.
            if (headingCp.bodyLinesAfter >= 2 || (headingCp.bodyLinesAfter >= 1 && lineFlag == 2)) {
                hasCommittedContentOnPage = true;
                headingCp.canRollback = true;
                headingCp.firstHeadingLevel = lineFlag;
                headingCp.bodyLinesAfter = 0;
                headingCp.linesSize = currentLines_.size();
                if (headingCp.linesSize > 0 && currentLines_[headingCp.linesSize - 1].empty() &&
                    (currentLineFlags_[headingCp.linesSize - 1] & 0x08) == 0) {
                    headingCp.linesSize--;
                }
                headingCp.linksSize = pageLinks_.size();
                headingCp.linkRectsSize = linkRects_.size();
                headingCp.imagesSize = pageImages_.size();
                headingCp.charsWrapped = origChunkStartIdx;
            } else {
                headingCp.bodyLinesAfter = 0;
            }
        }
    }
    size_t lineLimit = (pEndIdx == std::string::npos) ? pLen : pEndIdx;
    size_t wordStart = chunkStartIdx;
    char lineBuf[1024];
    int lineBufLen = 0;
    lineBuf[0] = '\0';
    size_t lineStartCharIdx = chunkStartIdx;
    bool lineHadSpecialTag = false;
    const bool isHeaderLine = (lineFlag == 2 || lineFlag == 3);

    while (wordStart < lineLimit) {
      while (wordStart < lineLimit && (chunk[wordStart] == ' ' || chunk[wordStart] == '\x04')) {
          if (chunk[wordStart] == '\x04' && wordStart + 1 < lineLimit) {
              currentIndent = chunk[wordStart + 1];
              wordStart += 2;
          } else {
              wordStart++;
          }
      }
      if (wordStart >= lineLimit) break;
      int indentOffset = currentIndent * 16;
      
      size_t wordEnd = wordStart;
      if (chunk[wordStart] == '[') {
          if (wordStart + 5 <= lineLimit && strncmp(&chunk[wordStart], "[IMG:", 5) == 0) {
              size_t tagEnd = wordStart + 5;
              while (tagEnd < lineLimit && chunk[tagEnd] != ']') tagEnd++;
              if (tagEnd < lineLimit) wordEnd = tagEnd + 1;
          } else if (wordStart + 7 <= lineLimit && (strncmp(&chunk[wordStart], "[TABLE:", 7) == 0 || strncmp(&chunk[wordStart], "[CLADE:", 7) == 0)) {
              size_t tagEnd = wordStart + 7;
              while (tagEnd < lineLimit && chunk[tagEnd] != ']') tagEnd++;
              if (tagEnd < lineLimit) wordEnd = tagEnd + 1;
          } else if (wordStart + 9 <= lineLimit && strncmp(&chunk[wordStart], "[INFOBOX]", 9) == 0) {
              wordEnd = wordStart + 9;
          }
      }
      
      if (wordEnd == wordStart) {
          while (wordEnd < lineLimit && chunk[wordEnd] != ' ' && chunk[wordEnd] != '\x04') wordEnd++;
      }

      size_t copyLen = wordEnd - wordStart;
      

      if (copyLen > 5 && strncmp(&chunk[wordStart], "[IMG:", 5) == 0 && chunk[wordEnd - 1] == ']') {
          std::string filename = std::string(&chunk[wordStart + 5], copyLen - 6);
          size_t origWordStart = wordStart;
          wordStart = wordEnd;
          lineHadSpecialTag = true;
          
          // If lineBuf only contains a list bullet "•" (from a Wikipedia <ul class="gallery">), discard the bullet and unindent
          if (lineBufLen >= 3 && strncmp(lineBuf, "\xE2\x80\xA2", 3) == 0) {
              bool onlyBullet = true;
              for (int bi = 3; bi < lineBufLen; ++bi) {
                  if (lineBuf[bi] != ' ') { onlyBullet = false; break; }
              }
              if (onlyBullet) {
                  lineBufLen = 0;
                  lineBuf[0] = '\0';
                  currentIndent = 0;
              }
          }

          // Flush current line
          if (lineBufLen > 0) {
              currentLines_.push_back(std::string(lineBuf, lineBufLen));
              currentLineFlags_.push_back(lineFlag | (currentIndent << 4));
              if (lineFlag == 0) onBodyLinePushed();
              lineBufLen = 0;
              if (currentLines_.size() >= static_cast<size_t>(linesPerPage_)) {
                  if (headingCp.active && headingCp.canRollback && headingCp.bodyLinesAfter < 2) {
                      rollbackHeadingCp();
                  } else {
                      charactersWrapped = origWordStart;
                  }
                  pageBreakReached = true;
                  break;
              }
          }
          
          uint64_t searchVal = 0;
          uint32_t imgSector = 0;
          uint32_t offsetInSector = 0;
          int imgHeight = 0;
          int imgWidth = 0;
          if (searchEngine_.search(filename.c_str(), searchVal)) {
              uint64_t absoluteBytes = (searchVal >> 32) * 32;
              imgSector = chunksSector_ + (absoluteBytes / 512);
              offsetInSector = static_cast<uint32_t>(absoluteBytes % 512);
              WikiJpeg::getDimensions(reader_, imgSector, offsetInSector, imgWidth, imgHeight);
          }
          
          if (imgHeight > 0) {
              int linesToSkip = (imgHeight + lineHeight - 1) / lineHeight;
              int remainingLines = linesPerPage_ - static_cast<int>(currentLines_.size());
              
              if (currentLines_.size() > 0 && linesToSkip > remainingLines) {
                  bool onlyHeadingsOnPage = (headingCp.active && !headingCp.canRollback && headingCp.bodyLinesAfter < 2);
                  if (onlyHeadingsOnPage || remainingLines >= linesPerPage_ / 2) {
                      // Keep the image on the current page by popping any trailing blank spacing line
                      // and scaling the image proportionally to fit the available height below the heading/text.
                      if (!currentLines_.empty() && currentLines_.back().empty() &&
                          (currentLineFlags_.back() & 0x08) == 0) {
                          currentLines_.pop_back();
                          currentLineFlags_.pop_back();
                          remainingLines = linesPerPage_ - static_cast<int>(currentLines_.size());
                      }
                      int maxAvailHeight = remainingLines * lineHeight;
                      if (imgHeight > maxAvailHeight && maxAvailHeight > 0) {
                          imgWidth = std::max(1, (imgWidth * maxAvailHeight) / imgHeight);
                          imgHeight = maxAvailHeight;
                          linesToSkip = remainingLines;
                      }
                  } else if (headingCp.active && headingCp.canRollback && headingCp.bodyLinesAfter < 2) {
                      // Roll back the uncommitted heading group (including any paired H2+H3) so the
                      // heading(s) and image move together to the top of the next page.
                      rollbackHeadingCp();
                      pageBreakReached = true;
                      break;
                  } else {
                      charactersWrapped = origWordStart;
                      pageBreakReached = true;
                      break;
                  }
              }
              
              int y = contentStartY + (currentLines_.size() % linesPerPage_) * lineHeight;
              int x = SIDE_MARGIN + (viewportWidth_ - imgWidth) / 2; // Center horizontally
              if (x < SIDE_MARGIN) x = SIDE_MARGIN;
              
              pageImages_.push_back({x, y, imgWidth, imgHeight, imgSector, static_cast<uint16_t>(offsetInSector)});
              
              for (int i = 0; i < linesToSkip; i++) {
                  currentLines_.push_back(""); // blank line reserved for image
                  currentLineFlags_.push_back(0x08);
                  if (currentLines_.size() >= static_cast<size_t>(linesPerPage_)) {
                      break;
                  }
              }

              if (linesToSkip >= 3) {
                  headingCp.active = false;
                  hasCommittedContentOnPage = true;
              } else {
                  for (int i = 0; i < linesToSkip; ++i) {
                      onBodyLinePushed();
                  }
              }
          }
          
          if (currentLines_.size() >= static_cast<size_t>(linesPerPage_)) {
              if (headingCp.active && headingCp.canRollback && headingCp.bodyLinesAfter < 2) {
                  rollbackHeadingCp();
              } else {
                  charactersWrapped = wordStart;
              }
              pageBreakReached = true;
              break;
          }
          
          continue;
      }
      
      if (copyLen == 9 && strncmp(&chunk[wordStart], "[INFOBOX]", 9) == 0) {
          hasInfobox_ = true;
          lineHadSpecialTag = true;
          wordStart = wordEnd;
          continue;
      }

      if (copyLen > 1000) copyLen = 1000;
      char wordBuf[1024];
      memcpy(wordBuf, chunk.data() + wordStart, copyLen);
      wordBuf[copyLen] = '\0';
      int wordW = formatter.getTextAdvance(wordBuf, isHeaderLine);
      
      bool isTableLink = (copyLen >= 9 && (strncmp(&wordBuf[0], "[TABLE:", 7) == 0 || strncmp(&wordBuf[0], "[CLADE:", 7) == 0) && wordBuf[copyLen - 1] == ']');
      int wordStartX = 0;
      int wordY = 0;

      if (lineBufLen == 0) {
        memcpy(lineBuf, wordBuf, copyLen);
        lineBuf[copyLen] = '\0';
        lineBufLen = copyLen;
        wordY = contentStartY + (currentLines_.size() % linesPerPage_) * lineHeight;
        wordStartX = SIDE_MARGIN + indentOffset;
        addLinkRect(wordStart, copyLen, wordStartX, wordY, wordW, lineHeight);
      } else {
        int w = 0;
        if (lineBufLen + 1 + copyLen >= 1023) {
            w = viewportWidth_ + 1;
        } else {
            lineBuf[lineBufLen] = ' ';
            memcpy(lineBuf + lineBufLen + 1, wordBuf, copyLen);
            lineBuf[lineBufLen + 1 + copyLen] = '\0';
            w = formatter.getTextAdvance(lineBuf, isHeaderLine);
        }
        
        if (w <= viewportWidth_ - indentOffset) {
          lineBuf[lineBufLen] = '\0';
          wordStartX = SIDE_MARGIN + indentOffset + formatter.getTextAdvance(lineBuf, isHeaderLine);
          wordStartX += formatter.getTextAdvance(" ", isHeaderLine);
          
          lineBuf[lineBufLen] = ' '; 
          lineBufLen += 1 + copyLen;
          wordY = contentStartY + (currentLines_.size() % linesPerPage_) * lineHeight;
          addLinkRect(wordStart, copyLen, wordStartX, wordY, wordW, lineHeight);
        } else {
          currentLines_.push_back(std::string(lineBuf, lineBufLen));
          currentLineFlags_.push_back(lineFlag | (currentIndent << 4));
          if (lineFlag == 0) onBodyLinePushed();
          charactersWrapped = wordStart;
          if (currentLines_.size() >= static_cast<size_t>(linesPerPage_)) {
              if (headingCp.active && headingCp.canRollback && headingCp.bodyLinesAfter < 2) {
                  rollbackHeadingCp();
              }
              pageBreakReached = true;
              break;
          }
          memcpy(lineBuf, wordBuf, copyLen);
          lineBuf[copyLen] = '\0';
          lineBufLen = copyLen;
          lineStartCharIdx = wordStart;
          wordY = contentStartY + (currentLines_.size() % linesPerPage_) * lineHeight;
          wordStartX = SIDE_MARGIN + indentOffset;
          addLinkRect(wordStart, copyLen, wordStartX, wordY, wordW, lineHeight);
        }
      }
      
      if (isTableLink) {
          std::string href = std::string(&wordBuf[1], copyLen - 2); // e.g. "TABLE:1"
          size_t hrefOff = hrefPool_.size();
          hrefPool_.append(href);
          PageLink pl;
          pl.hrefOffset = hrefOff;
          pl.hrefLen = href.length();
          pl.rectStartIdx = linkRects_.size();
          pl.rectCount = 1;
          pageLinks_.push_back(pl);
          linkRects_.push_back({wordStartX, wordY, wordW, lineHeight});
      }

      wordStart = wordEnd;
    }

    if (pageBreakReached) {
      break;
    }

    if (currentLines_.size() < static_cast<size_t>(linesPerPage_) && lineBufLen > 0) {
      currentLines_.push_back(std::string(lineBuf, lineBufLen));
      currentLineFlags_.push_back(lineFlag | (currentIndent << 4));
      if (lineFlag == 0) onBodyLinePushed();
      charactersWrapped = (pEndIdx == std::string::npos) ? pLen : pEndIdx + 1;
    } else if (currentLines_.size() < static_cast<size_t>(linesPerPage_) && lineBufLen == 0) {
      // Do not push blank lines at the very top of a page, for special-tag-only lines ([INFOBOX]/[IMG:...]),
      // or consecutively after another blank text line
      bool prevIsBlankTextLine = !currentLines_.empty() && currentLines_.back().empty() && ((currentLineFlags_.back() & 0x08) == 0);
      if (!lineHadSpecialTag && !currentLines_.empty() && !prevIsBlankTextLine) {
          currentLines_.push_back("");
          currentLineFlags_.push_back(lineFlag | (currentIndent << 4));
      }
      charactersWrapped = (pEndIdx == std::string::npos) ? pLen : pEndIdx + 1;
    }
    chunkStartIdx = (pEndIdx == std::string::npos) ? pLen : pEndIdx + 1;
  }

  // If the page filled up while a trailing heading group (H2, H3, or H2+H3) has fewer than 2 lines
  // of body content beneath it (and we are not at the end of the article), roll back the heading group
  // so it starts cleanly at the top of the next page.
  if (!pageBreakReached && (chunkStartIdx < pLen || !isEof_) &&
      headingCp.active && headingCp.canRollback && headingCp.bodyLinesAfter < 2) {
    rollbackHeadingCp();
  }

  if ((currentLines_.size() == static_cast<size_t>(linesPerPage_) || charactersWrapped > 0) && charactersWrapped < pLen) {
      if (currentPage_ == static_cast<int>(pageStartOffsets_.size()) - 1) {
          uint32_t textCharsWrapped = 0;
          for (size_t i = 0; i < charactersWrapped; ++i) {
              if (chunk[i] == '\x02' || chunk[i] == '\x03') {
                  continue;
              } else if (chunk[i] == '\x04') {
                  i++; // Skip the depth byte
                  continue;
              }
              textCharsWrapped++;
          }
          pageStartOffsets_.push_back(startChar + textCharsWrapped);
      }
      isEof_ = false;
  } else if (chunkStartIdx >= pLen && !isEof_) {
      isEof_ = true;
  }
  {
      std::string compactPool;
      for (const auto& pl : pageLinks_) {
          compactPool.append(hrefPool_, pl.hrefOffset, pl.hrefLen);
      }
      uint16_t curOff = 0;
      for (auto& pl : pageLinks_) {
          pl.hrefOffset = curOff;
          curOff += pl.hrefLen;
      }
      hrefPool_ = std::move(compactPool);
  }
  chunk.clear();
  chunk.shrink_to_fit();
  rawLinks.clear();
  rawLinks.shrink_to_fit();
  currentLines_.shrink_to_fit();
  currentLineFlags_.shrink_to_fit();
  pageLinks_.shrink_to_fit();
  linkRects_.shrink_to_fit();
  hrefPool_.shrink_to_fit();

  if (auto fcm = renderer.getFontCacheManager()) {
      char bodyPrewarm[1536];
      char headPrewarm[512];
      size_t bodyLen = 0;
      size_t headLen = 0;
      for (size_t i = 0; i < currentLines_.size(); ++i) {
          if (currentLines_[i].empty()) continue;
          uint8_t flag = currentLineFlags_[i] & 0x0F;
          const std::string& ln = currentLines_[i];
          if (flag == 2 || flag == 3) {
              size_t copyN = std::min(ln.size(), sizeof(headPrewarm) - 1 - headLen);
              if (copyN > 0) {
                  std::memcpy(headPrewarm + headLen, ln.data(), copyN);
                  headLen += copyN;
              }
          } else if ((flag & 0x08) == 0) {
              size_t copyN = std::min(ln.size(), sizeof(bodyPrewarm) - 1 - bodyLen);
              if (copyN > 0) {
                  std::memcpy(bodyPrewarm + bodyLen, ln.data(), copyN);
                  bodyLen += copyN;
              }
          }
      }
      bodyPrewarm[bodyLen] = '\0';
      headPrewarm[headLen] = '\0';
      if (bodyLen > 0) fcm->prewarmCache(bodyFontId, bodyPrewarm, 0x01);
      if (headLen > 0) fcm->prewarmCache(headingFontId, headPrewarm, 0x02);
  }

  if (pendingRestoreLinkIndex_ >= 0) {
      if (!pageLinks_.empty()) {
          selectedLinkIndex_ = std::min(pendingRestoreLinkIndex_, static_cast<int>(pageLinks_.size()) - 1);
      }
      pendingRestoreLinkIndex_ = -1;
  } else if (pageLinks_.size() > 0) {
      selectedLinkIndex_ = 0;
  }
  if (allowFastInitialRefresh_) {
      needsFullRefresh_ = false;
      allowFastInitialRefresh_ = false;
  }
  pendingImageRender_ = !pageImages_.empty();
  imageStage2Triggered_ = false;
  imagesDecodedOnPage_ = false;
  imageDecodeReadyAtMs_ = millis() + IMAGE_DECODE_DEBOUNCE_MS;
  syncResumeStateToConfig();
}

void WikiReaderActivity::onNextPage() {
  if (currentPage_ + 1 < static_cast<int>(pageStartOffsets_.size()) || (!isEof_)) {
    currentPage_++;
    formatCurrentPage();
    requestUpdate();
  }
}

void WikiReaderActivity::loadTOC() {
    WikiDecompressor decompressor(reader_);
    if (!decompressor.beginCluster(chunksSector_, articleOffset_)) {
        LOG_ERR(TAG, "Failed to decompress for TOC");
        return;
    }
    currentToc_ = extractArticleTOC(decompressor, 500 * 1024);
}

void WikiReaderActivity::jumpTo(uint32_t charIdx) {
    pageStartOffsets_.clear();
    pageStartOffsets_.push_back(charIdx);
    currentPage_ = 0;
    isEof_ = false;
    formatCurrentPage();
    requestUpdate();
}

void WikiReaderActivity::onPrevPage() {
  if (currentPage_ > 0) {
    currentPage_--;
    formatCurrentPage();
    requestUpdate();
  } else {
      if (currentToc_.empty()) {
          loadTOC();
      }
      
      uint32_t currentStart = pageStartOffsets_[0];
      uint32_t targetIdx = 0;
      
      for (auto it = currentToc_.rbegin(); it != currentToc_.rend(); ++it) {
          if (it->charIdx < currentStart) {
              targetIdx = it->charIdx;
              break;
          }
      }
      
      if (currentStart > 0) {
          jumpTo(targetIdx);
      }
  }
}

void WikiReaderActivity::loadArticle(const std::string& title, uint64_t offset) {
  title_ = title;
  articleOffset_ = offset;
  pageStartOffsets_.clear();
  pageStartOffsets_.push_back(0);
  currentPage_ = 0;
  isEof_ = false;
  selectedLinkIndex_ = -1;
  hasInfobox_ = false;
  {
    WikiDecompressor decompressor(reader_);
    if (decompressor.beginCluster(chunksSector_, offset)) {
      char buf[4096];
      size_t n = 0;
      while (n < sizeof(buf)) {
        size_t r = decompressor.read(buf + n, sizeof(buf) - n);
        if (r == 0) break;
        n += r;
      }
      if (n > 0) {
        std::string_view sv(buf, n);
        if (sv.find("[INFOBOX]") != std::string_view::npos) {
          hasInfobox_ = true;
        }
      }
    }
  }
  currentToc_.clear();
  formatCurrentPage();
  requestUpdate();
}

void WikiReaderActivity::onBack() {
  if (!history_.empty()) {
    auto prev = history_.back();
    history_.pop_back();
    loadArticle(prev.title, prev.offset);
    while (currentPage_ < prev.pageIndex && (!isEof_ || currentPage_ + 1 < static_cast<int>(pageStartOffsets_.size()))) {
        currentPage_++;
        formatCurrentPage();
    }
    selectedLinkIndex_ = prev.linkIndex;
    syncResumeStateToConfig();
    requestUpdate();
    return;
  }
  closedByUser_ = true;
  ActivityResult result;
  result.isCancelled = false;
  setResult(std::move(result));
  finish();
}

void WikiReaderActivity::followLink(const std::string& rawHref) {
  if (rawHref.find("TABLE:") == 0 || rawHref.find("CLADE:") == 0) {
      showTableViewer(rawHref);
      return;
  }

  if (rawHref.find("http://") == 0 || rawHref.find("https://") == 0 || rawHref.find("//") == 0) {
      LOG_INF(TAG, "Discarding external link: %s", rawHref.c_str());
      return;
  }
  std::string href = rawHref;
  if (href.find("./") == 0) href = href.substr(2);
  size_t hashPos = href.find('#');
  if (hashPos != std::string::npos) href = href.substr(0, hashPos);
  if (href.empty()) return;

  std::string decoded;
  for (size_t i = 0; i < href.length(); ++i) {
      if (href[i] == '%' && i + 2 < href.length()) {
          decoded += hexToChar(href[i+1], href[i+2]);
          i += 2;
      } else if (href[i] == '&') {
          size_t semi = href.find(';', i);
          if (semi != std::string::npos && semi - i <= 10) {
              std::string ent = href.substr(i + 1, semi - i - 1);
              if (ent.size() > 1 && ent[0] == '#') {
                  int base = 10;
                  size_t numStart = 1;
                  if (ent[1] == 'x' || ent[1] == 'X') {
                      base = 16;
                      numStart = 2;
                  }
                  if (numStart < ent.size()) {
                      char* endptr = nullptr;
                      long val = strtol(ent.c_str() + numStart, &endptr, base);
                      if (*endptr == '\0' && val > 0 && val < 256) {
                          decoded += static_cast<char>(val);
                          i = semi;
                          continue;
                      }
                  }
              } else if (ent == "amp") { decoded += '&'; i = semi; continue; }
              else if (ent == "lt") { decoded += '<'; i = semi; continue; }
              else if (ent == "gt") { decoded += '>'; i = semi; continue; }
              else if (ent == "quot") { decoded += '"'; i = semi; continue; }
              else if (ent == "apos") { decoded += '\''; i = semi; continue; }
          }
          decoded += href[i];
      } else {
          decoded += href[i];
      }
  }
  href = decoded;

  for (char& c : href) {
      if (c == '_') c = ' ';
  }

  if (!href.empty()) {
      href[0] = std::toupper(static_cast<unsigned char>(href[0]));
  }

  uint64_t foundOffset = 0;
  
  if (searchEngine_.search(href.c_str(), foundOffset)) {
      LOG_INF(TAG, "Following exact link to %s", href.c_str());
      WikiConfig::get().addRecentVisited(href.c_str(), foundOffset);
      WikiConfig::save();
      history_.push_back({title_, articleOffset_, currentPage_, selectedLinkIndex_});
      if (history_.size() > 20) history_.erase(history_.begin());
      loadArticle(href, foundOffset);
  } else {
      std::string lowerHref = href;
      for (char& c : lowerHref) {
          c = std::tolower(static_cast<unsigned char>(c));
      }
      
      if (searchEngine_.search(lowerHref.c_str(), foundOffset)) {
          LOG_INF(TAG, "Fallback: Following lower link to %s", lowerHref.c_str());
          WikiConfig::get().addRecentVisited(href.c_str(), foundOffset);
          WikiConfig::save();
          history_.push_back({title_, articleOffset_, currentPage_, selectedLinkIndex_});
          if (history_.size() > 20) history_.erase(history_.begin());
          loadArticle(lowerHref, foundOffset);
      } else {
          LOG_INF(TAG, "Link target not found: %s", href.c_str());
          releasePageMemory();
          if (auto fcm = renderer.getFontCacheManager()) {
              fcm->clearCache();
          }
          startActivityForResult(std::make_unique<WikiSearchActivity>(renderer, mappedInput, href), [this](const ActivityResult&) {
              formatCurrentPage();
              pageRedrawNeeded_ = true;
          });
      }
  }
}

void WikiReaderActivity::loop() {
  if (optionPopup_.isActive()) {
      if (optionPopup_.handleInput(mappedInput, [this](){ requestUpdate(); })) {
          return;
      }
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  int tx = 0;
  int ty = 0;
  if (mappedInput.wasScreenTapped(tx, ty)) {
    if (ty < metrics.topPadding + metrics.headerHeight) {
      onBack();
      return;
    }
    const int midX = renderer.getScreenWidth() / 2;
    if (tx < midX) {
      onPrevPage();
    } else {
      onNextPage();
    }
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    onPrevPage();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    onNextPage();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    if (selectedLinkIndex_ > 0) {
        selectedLinkIndex_--;
        if (pendingImageRender_) {
            imageDecodeReadyAtMs_ = millis() + IMAGE_DECODE_DEBOUNCE_MS;
        }
        syncResumeStateToConfig();
        requestUpdate();
    } else if (currentPage_ > 0) {
        onPrevPage();
        if (!pageLinks_.empty()) {
            selectedLinkIndex_ = pageLinks_.size() - 1;
            syncResumeStateToConfig();
        }
    }
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    if (selectedLinkIndex_ >= 0 && selectedLinkIndex_ + 1 < static_cast<int>(pageLinks_.size())) {
        selectedLinkIndex_++;
        if (pendingImageRender_) {
            imageDecodeReadyAtMs_ = millis() + IMAGE_DECODE_DEBOUNCE_MS;
        }
        syncResumeStateToConfig();
        requestUpdate();
    } else {
        int oldPage = currentPage_;
        onNextPage();
        if (currentPage_ != oldPage && !pageLinks_.empty()) {
            selectedLinkIndex_ = 0;
            syncResumeStateToConfig();
        }
    }
    return;
  }

  if (mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, 500)) {
    mappedInput.consumeSuppressedRelease();
    
    const char* options[] = {"Table of Contents", "Infobox"};
    optionPopup_.show("Article Menu", options, 2, 0, [this](int selected) {
        if (selected == 0) {
            if (currentToc_.empty()) {
                loadTOC();
            }
            startActivityForResult(
                std::make_unique<WikiTocActivity>(renderer, mappedInput, currentToc_),
                [this](const ActivityResult& result) {
                    if (!result.isCancelled) {
                        if (auto* p = std::get_if<IntervalResult>(&result.data)) {
                            uint32_t targetCharOffset = p->value;
                            history_.push_back({title_, articleOffset_, currentPage_, selectedLinkIndex_});
                            if (history_.size() > 20) history_.erase(history_.begin());
                            jumpTo(targetCharOffset);
                            return;
                        }
                    }
                    pageRedrawNeeded_ = true;
                    pendingImageRender_ = !pageImages_.empty();
                    imageStage2Triggered_ = false;
                }
            );
        } else if (selected == 1) {
            showInfobox();
        }
    });
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (selectedLinkIndex_ >= 0 && selectedLinkIndex_ < static_cast<int>(pageLinks_.size())) {
          const auto& pl = pageLinks_[selectedLinkIndex_];
          std::string href(&hrefPool_[pl.hrefOffset], pl.hrefLen);
          followLink(href);
          return;
      }
  }

  if (mappedInput.wasLongPressed(MappedInputManager::Button::Back, 500)) {
    mappedInput.consumeSuppressedRelease();
    closedByUser_ = true;
    ActivityResult result;
    result.isCancelled = false;
    setResult(std::move(result));
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasBackGesture()) {
    onBack();
    return;
  }

  // Stage 2 deferred image decode trigger with skim debouncing:
  // Only trigger Stage 2 once Stage 1 text has been pushed to the display,
  // no navigation button is being held/pressed, and the debounce window has elapsed.
  if (pendingImageRender_ && !pageRedrawNeeded_ && !imageStage2Triggered_ && !optionPopup_.isActive()) {
    const bool navHeld = mappedInput.isPressed(MappedInputManager::Button::Up) ||
                         mappedInput.isPressed(MappedInputManager::Button::Down) ||
                         mappedInput.isPressed(MappedInputManager::Button::Left) ||
                         mappedInput.isPressed(MappedInputManager::Button::Right) ||
                         mappedInput.isPressed(MappedInputManager::Button::PageBack) ||
                         mappedInput.isPressed(MappedInputManager::Button::PageForward);
    if (navHeld) {
      imageDecodeReadyAtMs_ = millis() + IMAGE_DECODE_DEBOUNCE_MS;
    } else if (static_cast<int32_t>(millis() - imageDecodeReadyAtMs_) >= 0) {
      imageStage2Triggered_ = true;
      requestUpdate();
    }
  }
}

void WikiReaderActivity::render(RenderLock&&) {
  const int bodyFontId = WikiFont::getBodyFontId();
  const int headingFontId = WikiFont::getHeadingFontId();
  WikiTextFormatter formatter(renderer, reader_, bodyFontId, headingFontId, WikiConfig::get().fontSector);

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int fontId = bodyFontId;
  const int lineHeight = renderer.getLineHeight(fontId) + LINE_SPACING;
  const int contentStartY = metrics.topPadding + metrics.headerHeight + 8;

  if (popupWasActive_ && !optionPopup_.isActive()) {
    pageRedrawNeeded_ = true;
    pendingImageRender_ = !pageImages_.empty();
    imageStage2Triggered_ = false;
    popupWasActive_ = false;
  }

  auto drawPageTextLine = [&](size_t i) {
    if (i >= currentLines_.size() || currentLines_[i].empty()) return;
    const int lineY = contentStartY + static_cast<int>(i) * lineHeight;
    int indent = currentLineFlags_[i] >> 4;
    int lineFlag = currentLineFlags_[i] & 0x0F;
    int startX = SIDE_MARGIN + (indent * 16);
    if (lineFlag == 2) {
      formatter.drawText(startX, lineY, currentLines_[i].c_str(), true, true);
      renderer.drawLine(SIDE_MARGIN, lineY + renderer.getLineHeight(headingFontId) + 2,
                        pageWidth - SIDE_MARGIN, lineY + renderer.getLineHeight(headingFontId) + 2, true);
    } else if (lineFlag == 3) {
      formatter.drawText(startX, lineY, currentLines_[i].c_str(), true, true);
    } else {
      formatter.drawText(startX, lineY, currentLines_[i].c_str(), true, false);
    }
  };

  auto drawSingleLink = [&](int linkIdx, bool isSelected) {
    if (linkIdx < 0 || linkIdx >= static_cast<int>(pageLinks_.size())) return;
    const auto& pl = pageLinks_[linkIdx];
    std::string href = hrefPool_.substr(pl.hrefOffset, pl.hrefLen);
    bool isTable = (href.find("TABLE:") == 0 || href.find("CLADE:") == 0);
    for (int i = 0; i < pl.rectCount; ++i) {
      const auto& rect = linkRects_[pl.rectStartIdx + i];
      if (isTable) {
        std::string text = "[" + href + "]";
        if (isSelected) {
          renderer.fillRect(rect.x, rect.y, rect.width, rect.height, true);
          formatter.drawText(rect.x, rect.y, text.c_str(), false, false);
        } else {
          renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);
          renderer.drawRect(rect.x, rect.y, rect.width, rect.height, true);
          formatter.drawText(rect.x, rect.y, text.c_str(), true, false);
        }
      } else {
        if (isSelected) {
          renderer.drawLine(rect.x, rect.y, rect.x + rect.width, rect.y, true);
          renderer.drawLine(rect.x, rect.y + rect.height, rect.x + rect.width, rect.y + rect.height, true);
          renderer.drawLine(rect.x, rect.y, rect.x, rect.y + rect.height, true);
          renderer.drawLine(rect.x + rect.width, rect.y, rect.x + rect.width, rect.y + rect.height, true);
        } else {
          renderer.drawLine(rect.x, rect.y + rect.height, rect.x + rect.width, rect.y + rect.height, true);
        }
      }
    }
  };

  // Fast path: if the page is already in the framebuffer and optionPopup_ is active,
  // just render the popup overlay over the existing framebuffer without re-decoding JPEGs.
  if (!pageRedrawNeeded_ && optionPopup_.isActive()) {
    popupWasActive_ = true;
    optionPopup_.render(renderer);
    renderer.displayBuffer();
    return;
  }

  // Fast incremental link-box update: when stepping through links on the same page,
  // keep the existing framebuffer (including decoded/dithered JPEGs) intact and only
  // update and window-refresh the previous and newly selected link rectangles.
  if (!pageRedrawNeeded_ && !needsFullRefresh_ && !needsWakeRefresh_ && !optionPopup_.isActive() &&
      renderedLinkIndex_ != selectedLinkIndex_) {
    int dirtyMinX = pageWidth;
    int dirtyMinY = pageHeight;
    int dirtyMaxX = 0;
    int dirtyMaxY = 0;
    auto expandDirty = [&](int rx, int ry, int rw, int rh) {
      dirtyMinX = std::min(dirtyMinX, std::max(0, rx));
      dirtyMinY = std::min(dirtyMinY, std::max(0, ry));
      dirtyMaxX = std::max(dirtyMaxX, std::min(pageWidth, rx + rw));
      dirtyMaxY = std::max(dirtyMaxY, std::min(pageHeight, ry + rh));
    };

    if (renderedLinkIndex_ >= 0 && renderedLinkIndex_ < static_cast<int>(pageLinks_.size())) {
      const auto& oldPl = pageLinks_[renderedLinkIndex_];
      std::string oldHref = hrefPool_.substr(oldPl.hrefOffset, oldPl.hrefLen);
      bool oldIsTable = (oldHref.find("TABLE:") == 0 || oldHref.find("CLADE:") == 0);
      if (oldIsTable) {
        drawSingleLink(renderedLinkIndex_, false);
        for (int i = 0; i < oldPl.rectCount; ++i) {
          const auto& rect = linkRects_[oldPl.rectStartIdx + i];
          expandDirty(rect.x - 2, rect.y - 2, rect.width + 4, rect.height + 4);
        }
      } else {
        for (int i = 0; i < oldPl.rectCount; ++i) {
          const auto& rect = linkRects_[oldPl.rectStartIdx + i];
          renderer.drawLine(rect.x, rect.y, rect.x + rect.width, rect.y, false);
          renderer.drawLine(rect.x, rect.y, rect.x, rect.y + rect.height, false);
          renderer.drawLine(rect.x + rect.width, rect.y, rect.x + rect.width, rect.y + rect.height, false);
          int lineIdx = (rect.y - 3 - contentStartY) / lineHeight;
          if (lineIdx >= 0 && lineIdx < static_cast<int>(currentLines_.size())) {
            drawPageTextLine(static_cast<size_t>(lineIdx));
            int lineY = contentStartY + lineIdx * lineHeight;
            expandDirty(0, std::min<int>(rect.y, lineY) - 2, pageWidth, lineHeight + 6);
          } else {
            expandDirty(rect.x - 2, rect.y - 2, rect.width + 4, rect.height + 4);
          }
        }
      }
      // Restore underlines for any unselected links on the page
      for (int linkIdx = 0; linkIdx < static_cast<int>(pageLinks_.size()); ++linkIdx) {
        if (linkIdx != selectedLinkIndex_) {
          drawSingleLink(linkIdx, false);
        }
      }
    }
    if (selectedLinkIndex_ >= 0 && selectedLinkIndex_ < static_cast<int>(pageLinks_.size())) {
      drawSingleLink(selectedLinkIndex_, true);
      const auto& newPl = pageLinks_[selectedLinkIndex_];
      for (int i = 0; i < newPl.rectCount; ++i) {
        const auto& rect = linkRects_[newPl.rectStartIdx + i];
        expandDirty(rect.x - 2, rect.y - 2, rect.width + 4, rect.height + 4);
      }
    }
    renderedLinkIndex_ = selectedLinkIndex_;
    if (dirtyMaxX > dirtyMinX && dirtyMaxY > dirtyMinY) {
      int alignedX = dirtyMinX & ~7;
      int alignedEndX = std::min(pageWidth, (dirtyMaxX + 7) & ~7);
      renderer.displayWindow(alignedX, dirtyMinY, alignedEndX - alignedX, dirtyMaxY - dirtyMinY);
    } else {
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    }
    return;
  }

  // Stage 2: Deferred JPEG decompression & local windowed refresh for pageImages_
  if (!pageRedrawNeeded_ && pendingImageRender_ && imageStage2Triggered_ && !optionPopup_.isActive()) {
    const bool pageTurnPressed = mappedInput.isPressed(MappedInputManager::Button::Up) ||
                                 mappedInput.isPressed(MappedInputManager::Button::Down) ||
                                 mappedInput.isPressed(MappedInputManager::Button::PageBack) ||
                                 mappedInput.isPressed(MappedInputManager::Button::PageForward);
    if (pageTurnPressed) {
      imageStage2Triggered_ = false;
      imageDecodeReadyAtMs_ = millis() + IMAGE_DECODE_DEBOUNCE_MS;
      return;
    }

    int imgMinX = pageWidth;
    int imgMinY = pageHeight;
    int imgMaxX = 0;
    int imgMaxY = 0;
    prevPageImageRects_.clear();

    for (const auto& img : pageImages_) {
      // Clear Stage 1 placeholder border before rendering dithered JPEG pixels
      renderer.fillRect(img.x, img.y, img.width, img.height, false);
      if (!WikiJpeg::render(renderer, reader_, img.sector, img.offsetInSector,
                            img.x, img.y, img.width, img.height,
                            0, pageWidth, 0, pageHeight)) {
        LOG_ERR(TAG, "Failed to open JPEG at sector %u", img.sector);
        renderer.drawRect(img.x, img.y, img.width, img.height, true);
      } else {
        prevPageImageRects_.push_back({static_cast<int16_t>(img.x), static_cast<int16_t>(img.y),
                                       static_cast<int16_t>(img.width), static_cast<int16_t>(img.height)});
      }
      imgMinX = std::min(imgMinX, std::max(0, img.x));
      imgMinY = std::min(imgMinY, std::max(0, img.y));
      imgMaxX = std::max(imgMaxX, std::min(pageWidth, img.x + img.width));
      imgMaxY = std::max(imgMaxY, std::min(pageHeight, img.y + img.height));
    }

    pendingImageRender_ = false;
    imageStage2Triggered_ = false;
    imagesDecodedOnPage_ = true;

    if (imgMaxX > imgMinX && imgMaxY > imgMinY) {
      int alignedX = imgMinX & ~7;
      int alignedEndX = std::min(pageWidth, (imgMaxX + 7) & ~7);
      renderer.displayWindow(alignedX, imgMinY, alignedEndX - alignedX, imgMaxY - imgMinY);
    }
    return;
  }

  if (!pageRedrawNeeded_ && !needsFullRefresh_ && !needsWakeRefresh_) {
    return;
  }

  // Stage 1: Immediate Text Render & Fast Page Turn
  renderer.clearScreen();
  if (!prevPageImageRects_.empty()) {
    for (const auto& r : prevPageImageRects_) {
      renderer.fillRect(r.x, r.y, r.width, r.height, false);
    }
    prevPageImageRects_.clear();
  }

  Rect headerRect{0, static_cast<int16_t>(metrics.topPadding), static_cast<int16_t>(pageWidth), static_cast<int16_t>(metrics.headerHeight)};
  std::string displayTitle = title_;
  for (char& c : displayTitle) {
      if (c == '_') c = ' ';
  }
  GUI.drawHeader(renderer, headerRect, displayTitle.c_str());

  for (size_t i = 0; i < currentLines_.size(); ++i) {
    drawPageTextLine(i);
  }

  for (int linkIdx = 0; linkIdx < static_cast<int>(pageLinks_.size()); ++linkIdx) {
    drawSingleLink(linkIdx, linkIdx == selectedLinkIndex_);
  }

  WikiGUI::drawDoubleButtonHints(renderer, 
      {"Back", "Search"}, 
      {"Select link", "TOC"}, 
      {"Prev link", ""}, 
      {"Next link", ""});

  // Draw subtle placeholder frames for any images on this page; actual SD decompression
  // and JPEG decoding happen asynchronously in Stage 2 if the user stays on the page.
  for (const auto& img : pageImages_) {
      renderer.fillRect(img.x, img.y, img.width, img.height, false);
      renderer.drawRect(img.x, img.y, img.width, img.height, true);
  }

  pageRedrawNeeded_ = false;
  renderedLinkIndex_ = selectedLinkIndex_;

  if (optionPopup_.isActive()) {
      popupWasActive_ = true;
      optionPopup_.render(renderer);
      renderer.displayBuffer();
      return;
  }

  HalDisplay::RefreshMode refreshMode = HalDisplay::FAST_REFRESH;
  if (needsFullRefresh_) {
      refreshMode = HalDisplay::FULL_REFRESH;
      needsFullRefresh_ = false;
      needsWakeRefresh_ = false;
  } else if (needsWakeRefresh_) {
      refreshMode = HalDisplay::HALF_REFRESH;
      needsWakeRefresh_ = false;
  }
  
  renderer.displayBuffer(refreshMode);
  if (pendingImageRender_) {
      imageDecodeReadyAtMs_ = millis() + IMAGE_DECODE_DEBOUNCE_MS;
  }
}


