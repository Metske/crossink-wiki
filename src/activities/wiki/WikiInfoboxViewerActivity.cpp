#include "WikiInfoboxViewerActivity.h"
#include "WikiDecompressor.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include <FontCacheManager.h>
#include <Logging.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

static const char* TAG = "WikiInfoboxViewer";

namespace {
constexpr size_t MAX_STAGE_ENTRIES = 26;
constexpr size_t MAX_STAGE_LINKS = 42;
constexpr size_t MAX_STAGE_TEXT_BYTES = 2200;
constexpr size_t CELL_SPLIT_SOFT_LIMIT = 420;
constexpr size_t CELL_SPLIT_HARD_LIMIT = 560;
constexpr uint32_t IMAGE_DECODE_DEBOUNCE_MS = 65;
} // namespace

WikiInfoboxViewerActivity::WikiInfoboxViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                     SdRawReader& reader, uint32_t rootSector, uint32_t chunksSector,
                                                     std::string title, uint64_t articleOffset,
                                                     uint32_t infoboxStreamOffset, uint32_t infoboxStreamLength)
    : Activity("WikiInfoboxViewer", renderer, mappedInput),
      scratchScope_(renderer),
      searchEngine_(reader, rootSector),
      reader_(reader),
      chunksSector_(chunksSector),
      title_(std::move(title)),
      articleOffset_(articleOffset),
      infoboxStreamOffset_(infoboxStreamOffset),
      infoboxStreamLength_(infoboxStreamLength) {
    stageStartOffsets_.push_back(0);
    stageStartFlags_.push_back(0);
}

void WikiInfoboxViewerActivity::releaseStageMemory() {
    pendingImageRender_ = false;
    imageStage2Triggered_ = false;
    pendingImages_.clear();
    pendingImages_.shrink_to_fit();
    entries_.clear();
    entries_.shrink_to_fit();
    links_.clear();
    links_.shrink_to_fit();
    linkRects_.clear();
    linkRects_.shrink_to_fit();
    hrefPool_.clear();
    hrefPool_.shrink_to_fit();
}

void WikiInfoboxViewerActivity::onEnter() {
    Activity::onEnter();
    WikiScratchBuffer::registerRenderer(&renderer);
    if (auto fcm = renderer.getFontCacheManager()) {
        fcm->clearCache();
    }
    WikiScratchBuffer::get();
    if (!formatter_) {
        const auto& cfg = WikiConfig::get();
        uint32_t fontSec = (cfg.fontSector == UINT32_MAX) ? 0 : cfg.fontSector;
        const int fontId = WikiFont::getBodyFontId();
        formatter_ = std::make_unique<WikiTextFormatter>(renderer, reader_, fontId, fontId, fontSec);
    }
    if (!stageLoaded_) {
        loadStage(0, false);
        stageLoaded_ = true;
    }
    pageRedrawNeeded_ = true;
    requestUpdate();
}

void WikiInfoboxViewerActivity::onExit() {
    Activity::onExit();
    releaseStageMemory();
    stageStartOffsets_.clear();
    stageStartOffsets_.shrink_to_fit();
    stageStartFlags_.clear();
    stageStartFlags_.shrink_to_fit();
    formatter_.reset();
    if (auto fcm = renderer.getFontCacheManager()) {
        fcm->clearCache();
    }
}

int WikiInfoboxViewerActivity::measureTextWidth(const std::string& text, bool isBold) const {
    if (text.empty()) return 0;
    if (formatter_) {
        return formatter_->getTextAdvance(text.c_str(), isBold);
    }
    const int fontId = WikiFont::getBodyFontId();
    return renderer.getTextAdvanceX(fontId, text.c_str(), isBold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
}

std::string WikiInfoboxViewerActivity::cleanText(const std::string& input, std::vector<RawLink>* outLinks) {
    if (outLinks) outLinks->clear();
    std::string out;
    out.reserve(input.length());

    bool inLink = false;
    size_t curLinkStart = 0;
    std::string curLinkTarget;

    size_t i = 0;
    while (i < input.length()) {
        // Skip CSS comments /* ... */
        if (i + 1 < input.length() && input[i] == '/' && input[i + 1] == '*') {
            size_t endComment = input.find("*/", i + 2);
            if (endComment != std::string::npos) {
                i = endComment + 2;
                continue;
            } else {
                break;
            }
        }
        
        // Skip CSS rule blocks like .mw-parser-output ... { ... }
        if (input.compare(i, 17, ".mw-parser-output") == 0 || input.compare(i, 6, "@media") == 0) {
            size_t closeBrace = input.find('}', i);
            if (closeBrace != std::string::npos) {
                i = closeBrace + 1;
                continue;
            }
        }
        
        // HTML tags
        if (input[i] == '<') {
            size_t endTag = input.find('>', i);
            if (endTag != std::string::npos) {
                if (input.compare(i, 3, "<a ") == 0 || input.compare(i, 3, "<a\t") == 0) {
                    // Ensure space before link if glued to preceding word/closing bracket/comma
                    if (!out.empty()) {
                        unsigned char prev = static_cast<unsigned char>(out.back());
                        if (std::isalnum(prev) || prev == ')' || prev == ']' || prev == ':' || prev == ',' || prev == ';') {
                            out += ' ';
                        }
                    }
                    size_t hrefPos = input.find("href=\"", i);
                    if (hrefPos != std::string::npos && hrefPos < endTag) {
                        size_t hrefEnd = input.find('\"', hrefPos + 6);
                        if (hrefEnd != std::string::npos && hrefEnd <= endTag) {
                            curLinkTarget = input.substr(hrefPos + 6, hrefEnd - hrefPos - 6);
                            if (!curLinkTarget.empty()) {
                                inLink = true;
                                curLinkStart = out.length();
                            }
                        }
                    }
                } else if (input.compare(i, 4, "</a>") == 0 || input.compare(i, 4, "</A>") == 0) {
                    if (inLink) {
                        if (outLinks && out.length() > curLinkStart) {
                            outLinks->push_back(RawLink{curLinkStart, out.length(), std::move(curLinkTarget)});
                        }
                        inLink = false;
                        curLinkTarget.clear();
                    }
                    // Ensure space after link if glued to following word or opening parenthesis
                    size_t nextIdx = endTag + 1;
                    if (nextIdx < input.length()) {
                        unsigned char nextCh = static_cast<unsigned char>(input[nextIdx]);
                        if (std::isalnum(nextCh) || nextCh == '(') {
                            out += ' ';
                        }
                    }
                } else if (input.compare(i, 3, "<br") == 0 || input.compare(i, 3, "<p ") == 0 ||
                           input.compare(i, 3, "<p>") == 0 || input.compare(i, 3, "<li") == 0 ||
                           input.compare(i, 4, "</p>") == 0 || input.compare(i, 5, "</li>") == 0) {
                    if (!out.empty() && out.back() != '\n') {
                        out += '\n';
                    }
                }
                i = endTag + 1;
                continue;
            }
        }
        
        // HTML entities
        if (input[i] == '&') {
            if (input.compare(i, 6, "&quot;") == 0 || input.compare(i, 5, "&#34;") == 0) {
                out += '\"'; i += (input[i + 1] == '#' ? 5 : 6); continue;
            } else if (input.compare(i, 6, "&apos;") == 0 || input.compare(i, 5, "&#39;") == 0) {
                out += '\''; i += (input[i + 1] == '#' ? 5 : 6); continue;
            } else if (input.compare(i, 5, "&amp;") == 0) {
                out += '&'; i += 5; continue;
            } else if (input.compare(i, 4, "&lt;") == 0) {
                out += '<'; i += 4; continue;
            } else if (input.compare(i, 4, "&gt;") == 0) {
                out += '>'; i += 4; continue;
            } else if (input.compare(i, 6, "&nbsp;") == 0 || input.compare(i, 6, "&#160;") == 0) {
                out += ' '; i += 6; continue;
            }
        }
        
        // Skip table markers [H], [C...], [R...] and any stray [IMG:...] markers
        if (input[i] == '[' && (input.compare(i, 3, "[H]") == 0 || input.compare(i, 2, "[C") == 0 ||
                                input.compare(i, 2, "[R") == 0 || input.compare(i, 5, "[IMG:") == 0)) {
            size_t closeBracket = input.find(']', i);
            if (closeBracket != std::string::npos) {
                i = closeBracket + 1;
                continue;
            }
        }
        
        // CSS escape sequence \a0 -> space
        if (input.compare(i, 3, "\\a0") == 0) {
            out += ' '; i += 3; continue;
        }

        // Restore spaces stripped across <span class="nowrap"> in sidebar/infobox templates
        if (input.compare(i, 8, "Part ofa") == 0) {
            out += "Part of a"; i += 8; continue;
        }
        if (input.compare(i, 8, "serieson") == 0) {
            out += "series on"; i += 8; continue;
        }
        if (input.compare(i, 8, "Knownfor") == 0) {
            out += "Known for"; i += 8; continue;
        }
        if (input.compare(i, 8, "Areacode") == 0) {
            out += "Area code"; i += 8; continue;
        }
        if (input.compare(i, 9, "Almamater") == 0) {
            out += "Alma mater"; i += 9; continue;
        }
        if (input.compare(i, 8, "Networth") == 0) {
            out += "Net worth"; i += 8; continue;
        }
        if (input.compare(i, 11, "Yearsactive") == 0) {
            out += "Years active"; i += 11; continue;
        }
        if (input.compare(i, 10, "Othernames") == 0) {
            out += "Other names"; i += 10; continue;
        }
        if (input.compare(i, 12, "Restingplace") == 0) {
            out += "Resting place"; i += 12; continue;
        }
        if (input.compare(i, 6, "portal") == 0 &&
            (i + 6 == input.length() || !std::isalpha(static_cast<unsigned char>(input[i + 6]))) &&
            !out.empty() && std::islower(static_cast<unsigned char>(out.back()))) {
            out += " portal"; i += 6; continue;
        }
        
        char c = input[i++];
        if (c == '\r') continue;
        if (!out.empty()) {
            unsigned char prev = static_cast<unsigned char>(out.back());
            unsigned char cur = static_cast<unsigned char>(c);
            if (c == '(' && (prev == ')' || std::isdigit(prev) ||
                             (std::isalpha(prev) && i < input.length() && std::isdigit(static_cast<unsigned char>(input[i]))))) {
                out += ' ';
            } else if (prev == ')' && (std::isdigit(cur) || std::isupper(cur))) {
                out += ' ';
            } else if (std::isdigit(cur) && out.size() >= 5 && out.compare(out.size() - 5, 5, "(aged") == 0) {
                out += ' ';
            } else if (c != ' ' && c != '\n' && c != '\t' && out.size() >= 3 && out.compare(out.size() - 3, 3, "\xE2\x80\xA2") == 0) {
                out += ' ';
            }
        }
        out += c;
    }

    if (inLink && outLinks && out.length() > curLinkStart) {
        outLinks->push_back(RawLink{curLinkStart, out.length(), std::move(curLinkTarget)});
    }
    
    // Trim leading whitespace & newlines
    size_t start = 0;
    while (start < out.length() && (out[start] == ' ' || out[start] == '\n' || out[start] == '\t')) start++;
    // Trim trailing whitespace & newlines
    size_t end = out.length();
    while (end > start && (out[end - 1] == ' ' || out[end - 1] == '\n' || out[end - 1] == '\t')) end--;

    if (outLinks && !outLinks->empty()) {
        size_t writeIdx = 0;
        for (size_t idx = 0; idx < outLinks->size(); ++idx) {
            auto& rl = (*outLinks)[idx];
            size_t s = std::max(rl.startChar, start);
            size_t e = std::min(rl.endChar, end);
            if (e > s) {
                rl.startChar = s - start;
                rl.endChar = e - start;
                if (writeIdx != idx) {
                    (*outLinks)[writeIdx] = std::move(rl);
                }
                writeIdx++;
            }
        }
        outLinks->resize(writeIdx);
    }
    
    if (start == 0 && end == out.length()) {
        return out;
    }
    return out.substr(start, end - start);
}

std::vector<WikiInfoboxViewerActivity::WrappedLine> WikiInfoboxViewerActivity::wrapTextWithTokens(
    const std::string& text, int maxWidth, bool isBold) {
    std::vector<WrappedLine> lines;
    if (text.empty()) return lines;

    WrappedLine currentLine;
    size_t i = 0;
    const size_t n = text.length();

    auto flushCurrentLine = [&]() {
        if (!currentLine.text.empty()) {
            lines.push_back(std::move(currentLine));
            currentLine = WrappedLine{};
        }
    };

    auto placeToken = [&](size_t tokStart, size_t tokEnd) {
        std::string word = text.substr(tokStart, tokEnd - tokStart);
        int wordW = measureTextWidth(word, isBold);

        if (wordW > maxWidth && word.length() > 1) {
            size_t subStart = tokStart;
            std::string sub;
            for (size_t idx = tokStart; idx < tokEnd; ++idx) {
                std::string testSub = sub + text[idx];
                if (measureTextWidth(testSub, isBold) > maxWidth && !sub.empty()) {
                    flushCurrentLine();
                    currentLine.text = sub;
                    currentLine.tokens.push_back(WrappedToken{subStart, idx, 0});
                    flushCurrentLine();
                    subStart = idx;
                    sub = text[idx];
                } else {
                    sub += text[idx];
                }
            }
            if (!sub.empty()) {
                std::string testLine = currentLine.text.empty() ? sub : currentLine.text + " " + sub;
                if (measureTextWidth(testLine, isBold) > maxWidth && !currentLine.text.empty()) {
                    flushCurrentLine();
                }
                size_t lineOff = currentLine.text.empty() ? 0 : currentLine.text.length() + 1;
                if (!currentLine.text.empty()) currentLine.text += ' ';
                currentLine.text += sub;
                currentLine.tokens.push_back(WrappedToken{subStart, tokEnd, lineOff});
            }
            return;
        }

        std::string testLine = currentLine.text.empty() ? word : currentLine.text + " " + word;
        if (measureTextWidth(testLine, isBold) > maxWidth && !currentLine.text.empty()) {
            flushCurrentLine();
        }
        size_t lineOff = currentLine.text.empty() ? 0 : currentLine.text.length() + 1;
        if (!currentLine.text.empty()) currentLine.text += ' ';
        currentLine.text += word;
        currentLine.tokens.push_back(WrappedToken{tokStart, tokEnd, lineOff});
    };

    while (i < n) {
        if (text[i] == '\n') {
            flushCurrentLine();
            ++i;
            continue;
        }
        if (text[i] == ' ' || text[i] == '\t') {
            ++i;
            continue;
        }
        size_t wordStart = i;
        while (i < n && text[i] != ' ' && text[i] != '\t' && text[i] != '\n') {
            ++i;
        }
        placeToken(wordStart, i);
    }
    flushCurrentLine();
    return lines;
}

void WikiInfoboxViewerActivity::appendLinksForLines(const std::vector<WrappedLine>& wrappedLines,
                                                    const std::vector<RawLink>& rawLinks,
                                                    int baseLineX, int firstLineY, int lineStepY, int rectHeight,
                                                    bool isBold, bool centerLines) {
    if (rawLinks.empty() || wrappedLines.empty()) return;

    for (const auto& rl : rawLinks) {
        if (rl.target.empty() || rl.endChar <= rl.startChar) continue;
        if (hrefPool_.size() + rl.target.size() > 65000 || linkRects_.size() > 65000) break;

        uint16_t rectStartIdx = static_cast<uint16_t>(linkRects_.size());
        uint16_t rectCount = 0;
        int minY = 0;
        int maxY = 0;

        int lineY = firstLineY;
        for (const auto& wl : wrappedLines) {
            bool hasOverlap = false;
            size_t firstSliceStart = 0;
            size_t lastSliceEnd = 0;

            for (const auto& tok : wl.tokens) {
                size_t ovStart = std::max(tok.textStart, rl.startChar);
                size_t ovEnd = std::min(tok.textEnd, rl.endChar);
                if (ovStart < ovEnd) {
                    size_t sliceStart = tok.lineOffset + (ovStart - tok.textStart);
                    size_t sliceEnd = tok.lineOffset + (ovEnd - tok.textStart);
                    if (!hasOverlap) {
                        firstSliceStart = sliceStart;
                        hasOverlap = true;
                    }
                    lastSliceEnd = sliceEnd;
                }
            }

            if (hasOverlap && lastSliceEnd > firstSliceStart && lastSliceEnd <= wl.text.length()) {
                int lineX = baseLineX;
                if (centerLines) {
                    int totalLineW = measureTextWidth(wl.text, isBold);
                    lineX = baseLineX + (viewWidth_ - totalLineW) / 2;
                    if (lineX < baseLineX) lineX = baseLineX;
                }
                int prefixW = measureTextWidth(wl.text.substr(0, firstSliceStart), isBold);
                int subW = measureTextWidth(wl.text.substr(firstSliceStart, lastSliceEnd - firstSliceStart), isBold);
                if (subW > 0) {
                    LinkRect rect{
                        static_cast<int16_t>(lineX + prefixW),
                        static_cast<int16_t>(lineY + 3),
                        static_cast<int16_t>(subW),
                        static_cast<int16_t>(rectHeight)
                    };
                    if (rectCount == 0) {
                        minY = rect.y;
                        maxY = rect.y + rect.h;
                    } else {
                        minY = std::min<int>(minY, rect.y);
                        maxY = std::max<int>(maxY, rect.y + rect.h);
                    }
                    linkRects_.push_back(rect);
                    rectCount++;
                }
            }
            lineY += lineStepY;
        }

        if (rectCount > 0) {
            uint16_t hrefOff = static_cast<uint16_t>(hrefPool_.size());
            uint16_t hrefLen = static_cast<uint16_t>(rl.target.length());
            hrefPool_.append(rl.target);
            links_.push_back(InfoboxLink{hrefOff, hrefLen, rectStartIdx, rectCount, minY, maxY});
        }
    }
}

void WikiInfoboxViewerActivity::loadStage(int stageIdx, bool scrollToBottom) {
    if (stageStartOffsets_.empty()) {
        stageStartOffsets_.push_back(0);
        stageStartFlags_.push_back(0);
    }
    stageIdx = std::clamp(stageIdx, 0, static_cast<int>(stageStartOffsets_.size()) - 1);
    currentStage_ = stageIdx;
    hasNextStage_ = false;

    releaseStageMemory();
    if (auto fcm = renderer.getFontCacheManager()) {
        fcm->clearCache();
    }

    const auto& metrics = UITheme::getInstance().getMetrics();
    const int screenW = renderer.getScreenWidth();
    const int screenH = renderer.getScreenHeight();
    const int headerH = metrics.topPadding + metrics.headerHeight;
    const int bottomBarH = WikiGUI::WIKI_HINTS_HEIGHT + 14;
    const int startX = 16;
    const int renderStartY = headerH + 8;
    const int fontId = WikiFont::getBodyFontId();
    const int lineHeight = renderer.getLineHeight(fontId) + 4;

    viewWidth_ = screenW - 32;
    viewHeight_ = screenH - renderStartY - bottomBarH;

    selectedLinkIndex_ = -1;
    renderedLinkIndex_ = -1;
    renderedScrollY_ = -1;
    pageRedrawNeeded_ = true;

    if (infoboxStreamLength_ == 0) {
        totalContentHeight_ = 0;
        maxScrollY_ = 0;
        scrollY_ = 0;
        return;
    }

    entries_.reserve(MAX_STAGE_ENTRIES + 2);
    links_.reserve(MAX_STAGE_LINKS + 8);
    linkRects_.reserve(MAX_STAGE_LINKS + 16);
    hrefPool_.reserve(1536);

    size_t stageTextBytes = 0;

    auto pushWrappedEntry = [&](EntryType type, const std::string& k, const std::vector<RawLink>& kLinks,
                                const std::string& v, const std::vector<RawLink>& vLinks,
                                const std::string& imgKey, bool isContinuation, bool hasMoreInRow) {
        InfoboxEntry entry;
        entry.type = type;
        entry.hasMoreInRow = hasMoreInRow;
        entry.linkStartIdx = static_cast<uint16_t>(links_.size());

        if (type == EntryType::HEADER) {
            auto wrappedKey = wrapTextWithTokens(k, viewWidth_ - 8, true);
            appendLinksForLines(wrappedKey, kLinks, startX, 2, lineHeight + 2, lineHeight - 2, true, false);
            entry.keyLines.reserve(wrappedKey.size());
            for (auto& wl : wrappedKey) {
                stageTextBytes += wl.text.size();
                entry.keyLines.push_back(std::move(wl.text));
            }
            int h = static_cast<int>(entry.keyLines.size()) * (lineHeight + 2) + (hasMoreInRow ? 2 : 14);
            entry.height = std::max(h, lineHeight + (hasMoreInRow ? 2 : 8));
        } else if (type == EntryType::IMAGE) {
            entry.imageKey = imgKey;
            auto wrappedCap = wrapTextWithTokens(v, viewWidth_ - 20, false);
            appendLinksForLines(wrappedCap, vLinks, startX, 0, lineHeight, lineHeight - 2, false, true);
            entry.valueLines.reserve(wrappedCap.size());
            for (auto& wl : wrappedCap) {
                stageTextBytes += wl.text.size();
                entry.valueLines.push_back(std::move(wl.text));
            }
        } else if (type == EntryType::KEY_VALUE) {
            auto wrappedKey = wrapTextWithTokens(k, viewWidth_ - 8, true);
            int keyStartY = 2;
            appendLinksForLines(wrappedKey, kLinks, startX, keyStartY, lineHeight, lineHeight - 2, true, false);
            entry.keyLines.reserve(wrappedKey.size());
            for (auto& wl : wrappedKey) {
                stageTextBytes += wl.text.size();
                entry.keyLines.push_back(std::move(wl.text));
            }

            entry.indentValue = !entry.keyLines.empty() || isContinuation;
            const int indentX = entry.indentValue ? (startX + 16) : startX;
            const int valMaxWidth = entry.indentValue ? (viewWidth_ - 24) : (viewWidth_ - 8);
            auto wrappedVal = wrapTextWithTokens(v, valMaxWidth, false);
            int valStartY = keyStartY + static_cast<int>(entry.keyLines.size()) * lineHeight;
            appendLinksForLines(wrappedVal, vLinks, indentX, valStartY, lineHeight, lineHeight - 2, false, false);
            entry.valueLines.reserve(wrappedVal.size());
            for (auto& wl : wrappedVal) {
                stageTextBytes += wl.text.size();
                entry.valueLines.push_back(std::move(wl.text));
            }

            int kH = static_cast<int>(entry.keyLines.size()) * lineHeight;
            int vH = static_cast<int>(entry.valueLines.size()) * lineHeight;
            entry.height = kH + vH + (hasMoreInRow ? 2 : 10);
        }

        entry.linkCount = static_cast<uint16_t>(links_.size() - entry.linkStartIdx);
        entries_.push_back(std::move(entry));
    };

    auto processCells = [&](const std::string& cell0, const std::string& cell1,
                            bool hasPipe, bool isContinuation, bool hasMoreInRow) {
        if (cell0.empty() && cell1.empty()) return;

        // Check for one or more [IMG:...] markers in cell0 or cell1 (e.g. infobox photo montages)
        const bool imgInCell0 = (cell0.find("[IMG:") != std::string::npos);
        const bool imgInCell1 = (!imgInCell0 && hasPipe && cell1.find("[IMG:") != std::string::npos);
        if (imgInCell0 || imgInCell1) {
            const std::string& imgSrc = imgInCell0 ? cell0 : cell1;
            size_t imgPos = imgSrc.find("[IMG:");
            size_t imgEnd = imgSrc.find(']', imgPos + 5);
            if (imgEnd != std::string::npos) {
                if (imgInCell0 && imgPos > 0) {
                    std::vector<RawLink> preLinks;
                    std::string preClean = cleanText(cell0.substr(0, imgPos), &preLinks);
                    if (!preClean.empty()) {
                        pushWrappedEntry(EntryType::KEY_VALUE, "", {}, preClean, preLinks, "", isContinuation, true);
                    }
                } else if (imgInCell1 && !cell0.empty()) {
                    std::vector<RawLink> kLinks;
                    std::string kClean = cleanText(cell0, &kLinks);
                    if (!kClean.empty()) {
                        pushWrappedEntry(EntryType::HEADER, kClean, kLinks, "", {}, "", false, true);
                    }
                }

                size_t curPos = imgPos;
                while (curPos != std::string::npos && curPos < imgSrc.size()) {
                    size_t curEnd = imgSrc.find(']', curPos + 5);
                    if (curEnd == std::string::npos) break;

                    std::string imageKey = imgSrc.substr(curPos + 5, curEnd - (curPos + 5));
                    size_t nextImgPos = imgSrc.find("[IMG:", curEnd + 1);
                    const bool hasNextImg = (nextImgPos != std::string::npos &&
                                             imgSrc.find(']', nextImgPos + 5) != std::string::npos);

                    std::string captionRaw;
                    if (hasNextImg) {
                        captionRaw = imgSrc.substr(curEnd + 1, nextImgPos - (curEnd + 1));
                    } else if (imgInCell0 && hasPipe && !cell1.empty()) {
                        std::string tail = imgSrc.substr(curEnd + 1);
                        captionRaw = tail.empty() ? cell1 : (tail + "\n" + cell1);
                    } else {
                        captionRaw = imgSrc.substr(curEnd + 1);
                    }

                    std::vector<RawLink> capLinks;
                    std::string capClean = cleanText(captionRaw, &capLinks);
                    pushWrappedEntry(EntryType::IMAGE, "", {}, capClean, capLinks, imageKey, false,
                                     hasNextImg ? true : hasMoreInRow);
                    curPos = hasNextImg ? nextImgPos : std::string::npos;
                }
                return;
            }
        }

        if (!hasPipe) {
            std::vector<RawLink> rowLinks;
            std::string cleaned = cleanText(cell0, &rowLinks);
            if (!cleaned.empty()) {
                bool isExplicitHeader = (!isContinuation) &&
                                        ((cell0.find("[H]") != std::string::npos) ||
                                         (currentStage_ == 0 && entries_.empty()));
                if (isExplicitHeader) {
                    pushWrappedEntry(EntryType::HEADER, cleaned, rowLinks, "", {}, "", false, hasMoreInRow);
                } else {
                    pushWrappedEntry(EntryType::KEY_VALUE, "", {}, cleaned, rowLinks, "", isContinuation, hasMoreInRow);
                }
            }
        } else {
            std::vector<RawLink> kLinks;
            std::vector<RawLink> vLinks;
            std::string k = cleanText(cell0, &kLinks);
            std::string v = cleanText(cell1, &vLinks);
            if (v.empty() && !k.empty() && !isContinuation) {
                pushWrappedEntry(EntryType::HEADER, k, kLinks, "", {}, "", false, hasMoreInRow);
            } else if (!k.empty() || !v.empty()) {
                pushWrappedEntry(EntryType::KEY_VALUE, k, kLinks, v, vLinks, "", isContinuation, hasMoreInRow);
            }
        }
    };

    auto isStageFull = [&]() -> bool {
        return entries_.size() >= MAX_STAGE_ENTRIES ||
               links_.size() >= MAX_STAGE_LINKS ||
               stageTextBytes >= MAX_STAGE_TEXT_BYTES;
    };

    uint32_t nextStageOffset = 0;
    uint8_t nextStageFlags = 0;
    bool stoppedForStageLimit = false;

    {
        WikiDecompressor decompressor(reader_);
        if (decompressor.beginCluster(chunksSector_, articleOffset_)) {
            uint32_t skipRemaining = infoboxStreamOffset_ + stageStartOffsets_[stageIdx];
            char ioBuf[512];
            while (skipRemaining > 0 && !decompressor.isDone()) {
                size_t toRead = std::min<size_t>(sizeof(ioBuf), skipRemaining);
                size_t r = decompressor.read(ioBuf, toRead);
                if (r == 0) break;
                skipRemaining -= static_cast<uint32_t>(r);
            }

            uint32_t relPos = stageStartOffsets_[stageIdx];
            const uint32_t endPos = infoboxStreamLength_;
            size_t bufPos = 0;
            size_t bufLen = 0;

            auto peekByte = [&](char& out) -> bool {
                if (relPos >= endPos) return false;
                if (bufPos >= bufLen) {
                    size_t toRead = std::min<size_t>(sizeof(ioBuf), endPos - relPos);
                    bufLen = decompressor.read(ioBuf, toRead);
                    bufPos = 0;
                    if (bufLen == 0) return false;
                }
                out = ioBuf[bufPos];
                return true;
            };

            auto getByte = [&](char& out) -> bool {
                if (!peekByte(out)) return false;
                bufPos++;
                relPos++;
                return true;
            };

            const uint8_t initFlags = stageStartFlags_[stageIdx];
            bool hasPipe = (initFlags & 0x01) != 0;
            bool isContinuation = (initFlags & 0x02) != 0;
            bool inTag = false;
            bool inAnchor = false;
            std::string cell0;
            std::string cell1;
            cell0.reserve(256);
            cell1.reserve(640);

            auto updateTagState = [&](const std::string& cell, char ch) {
                if (ch == '<') {
                    inTag = true;
                } else if (ch == '>') {
                    inTag = false;
                    size_t len = cell.size();
                    if (len >= 4) {
                        if (cell.compare(len - 4, 4, "</a>") == 0 || cell.compare(len - 4, 4, "</A>") == 0) {
                            inAnchor = false;
                        }
                    }
                    if (!inAnchor && len >= 3) {
                        size_t lt = cell.rfind('<');
                        if (lt != std::string::npos && lt + 3 <= len) {
                            if (cell.compare(lt, 3, "<a ") == 0 || cell.compare(lt, 3, "<a\t") == 0) {
                                inAnchor = true;
                            }
                        }
                    }
                }
            };

            auto canSplitCell = [](const std::string& s) -> bool {
                size_t imgPos = s.rfind("[IMG:");
                if (imgPos != std::string::npos) {
                    size_t imgEnd = s.find(']', imgPos + 5);
                    if (imgEnd == std::string::npos) {
                        return false;
                    }
                    // Do not split across the \n\n between [IMG:...] and its caption
                    bool hasTextAfterImg = false;
                    for (size_t k = imgEnd + 1; k < s.size(); ++k) {
                        if (s[k] != ' ' && s[k] != '\n' && s[k] != '\r' && s[k] != '\t') {
                            hasTextAfterImg = true;
                            break;
                        }
                    }
                    if (!hasTextAfterImg) {
                        return false;
                    }
                }
                return true;
            };

            char c = 0;
            while (getByte(c)) {
                if (c == '\r') continue;

                if (c == '\\') {
                    char next = 0;
                    if (peekByte(next)) {
                        if (next == 'n' || next == '|' || next == '\\') {
                            getByte(next);
                            std::string& active = hasPipe ? cell1 : cell0;
                            if (next == 'n') {
                                active += '\n';
                                if (!inTag && !inAnchor && active.size() >= CELL_SPLIT_SOFT_LIMIT && canSplitCell(active)) {
                                    // Consume any consecutive escaped \n sequences before splitting
                                    while (true) {
                                        char p1 = 0;
                                        if (!peekByte(p1) || p1 != '\\') break;
                                        // Only consume if it is \n
                                        if (bufPos + 1 < bufLen && ioBuf[bufPos + 1] == 'n') {
                                            char dummy = 0;
                                            getByte(dummy);
                                            getByte(dummy);
                                        } else {
                                            break;
                                        }
                                    }
                                    processCells(cell0, cell1, hasPipe, isContinuation, /*hasMoreInRow=*/true);
                                    cell0.clear();
                                    cell1.clear();
                                    isContinuation = true;
                                    if (isStageFull() && relPos < endPos) {
                                        nextStageOffset = relPos;
                                        nextStageFlags = (hasPipe ? 0x01 : 0x00) | 0x02;
                                        stoppedForStageLimit = true;
                                        break;
                                    }
                                }
                            } else if (next == '|') {
                                active += '|';
                            } else {
                                active += '\\';
                            }
                            continue;
                        }
                    }
                }

                if (c == '|' && !hasPipe && !inTag) {
                    hasPipe = true;
                    inAnchor = false;
                    continue;
                }

                if (c == '\n') {
                    if (!cell0.empty() || !cell1.empty()) {
                        processCells(cell0, cell1, hasPipe, isContinuation, /*hasMoreInRow=*/false);
                    }
                    cell0.clear();
                    cell1.clear();
                    hasPipe = false;
                    isContinuation = false;
                    inTag = false;
                    inAnchor = false;

                    if (isStageFull() && relPos < endPos) {
                        char p = 0;
                        while (peekByte(p) && (p == '\r' || p == '\n')) {
                            getByte(p);
                        }
                        if (relPos < endPos) {
                            nextStageOffset = relPos;
                            nextStageFlags = 0;
                            stoppedForStageLimit = true;
                            break;
                        }
                    }
                    continue;
                }

                std::string& active = hasPipe ? cell1 : cell0;
                active += c;
                updateTagState(active, c);

                if (!inTag && !inAnchor && active.size() >= CELL_SPLIT_HARD_LIMIT &&
                    (c == '>' || c == ' ') && canSplitCell(active)) {
                    processCells(cell0, cell1, hasPipe, isContinuation, /*hasMoreInRow=*/true);
                    cell0.clear();
                    cell1.clear();
                    isContinuation = true;
                    if (isStageFull() && relPos < endPos) {
                        nextStageOffset = relPos;
                        nextStageFlags = (hasPipe ? 0x01 : 0x00) | 0x02;
                        stoppedForStageLimit = true;
                        break;
                    }
                }
            }

            if (!stoppedForStageLimit && (!cell0.empty() || !cell1.empty())) {
                processCells(cell0, cell1, hasPipe, isContinuation, /*hasMoreInRow=*/false);
            }
        }
    }

    if (stoppedForStageLimit) {
        hasNextStage_ = true;
        if (static_cast<size_t>(stageIdx + 1) >= stageStartOffsets_.size()) {
            stageStartOffsets_.push_back(nextStageOffset);
            stageStartFlags_.push_back(nextStageFlags);
        } else {
            stageStartOffsets_[stageIdx + 1] = nextStageOffset;
            stageStartFlags_[stageIdx + 1] = nextStageFlags;
        }
    }

    entries_.shrink_to_fit();
    links_.shrink_to_fit();
    linkRects_.shrink_to_fit();
    hrefPool_.shrink_to_fit();

    // Pass 2 (after WikiDecompressor has closed and released WikiScratchBuffer):
    // resolve any [IMG:...] dimensions and assign final Y positions to entries and link rects.
    int curY = 0;
    for (auto& entry : entries_) {
        entry.y = curY;
        int linkOffsetY = entry.y;

        if (entry.type == EntryType::IMAGE) {
            uint64_t searchVal = 0;
            if (!entry.imageKey.empty() && searchEngine_.search(entry.imageKey.c_str(), searchVal)) {
                uint64_t absoluteBytes = (searchVal >> 32) * 32;
                entry.imgSector = chunksSector_ + (absoluteBytes / 512);
                entry.imgOffsetInSector = absoluteBytes % 512;
                WikiJpeg::getDimensions(reader_, entry.imgSector, entry.imgOffsetInSector, entry.imgWidth, entry.imgHeight);
                if (entry.imgHeight <= 0) entry.imgHeight = 150;
                if (entry.imgWidth <= 0) entry.imgWidth = 150;
                if (entry.imgHeight > 240) {
                    entry.imgWidth = (entry.imgWidth * 240) / entry.imgHeight;
                    entry.imgHeight = 240;
                }
                if (entry.imgWidth > viewWidth_) {
                    entry.imgHeight = (entry.imgHeight * viewWidth_) / entry.imgWidth;
                    entry.imgWidth = viewWidth_;
                }
            } else {
                entry.imgSector = 0;
                entry.imgWidth = 0;
                entry.imgHeight = 0;
            }
            entry.imageKey.clear();
            entry.imageKey.shrink_to_fit();

            int capStartOffset = (entry.imgHeight > 0) ? (entry.imgHeight + 8) : 4;
            int capH = entry.valueLines.empty() ? 0 : (static_cast<int>(entry.valueLines.size()) * lineHeight + 6);
            entry.height = (entry.imgHeight > 0) ? (entry.imgHeight + capH + 16) : (capH > 0 ? capH + 6 : 0);
            linkOffsetY = entry.y + capStartOffset;
        }

        for (uint16_t li = 0; li < entry.linkCount; ++li) {
            size_t linkIdx = static_cast<size_t>(entry.linkStartIdx) + li;
            if (linkIdx >= links_.size()) break;
            auto& lnk = links_[linkIdx];
            lnk.minY += linkOffsetY;
            lnk.maxY += linkOffsetY;
            for (uint16_t ri = 0; ri < lnk.rectCount; ++ri) {
                size_t rectIdx = static_cast<size_t>(lnk.rectStartIdx) + ri;
                if (rectIdx < linkRects_.size()) {
                    linkRects_[rectIdx].y = static_cast<int16_t>(linkRects_[rectIdx].y + linkOffsetY);
                }
            }
        }

        curY += entry.height;
    }

    totalContentHeight_ = curY;
    maxScrollY_ = std::max(0, totalContentHeight_ - viewHeight_);
    scrollY_ = scrollToBottom ? maxScrollY_ : 0;
    LOG_INF(TAG, "Loaded infobox stage %d: %zu entries, %zu links, hasNext=%d, freeHeap=%u",
            currentStage_, entries_.size(), links_.size(), hasNextStage_ ? 1 : 0, ESP.getFreeHeap());
}

void WikiInfoboxViewerActivity::loop() {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasBackGesture()) {
        finish();
        return;
    }

    auto isLinkVisible = [&](int idx) -> bool {
        if (idx < 0 || idx >= static_cast<int>(links_.size())) return false;
        const auto& lnk = links_[idx];
        return (lnk.maxY > scrollY_ && lnk.minY < scrollY_ + viewHeight_);
    };

    auto scrollLinkIntoView = [&](int idx) {
        if (idx < 0 || idx >= static_cast<int>(links_.size())) return;
        const auto& lnk = links_[idx];
        if (lnk.minY < scrollY_) {
            scrollY_ = std::max(0, lnk.minY - 12);
        } else if (lnk.maxY > scrollY_ + viewHeight_) {
            scrollY_ = std::min(maxScrollY_, lnk.maxY - viewHeight_ + 12);
        }
    };

    // Side buttons (Up / Down): scroll up and down the infobox (advancing across stages at boundaries)
    if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
        if (scrollY_ < maxScrollY_) {
            int pageStep = std::max(40, viewHeight_ - 40);
            scrollY_ = std::min(maxScrollY_, scrollY_ + pageStep);
            if (selectedLinkIndex_ != -1 && !isLinkVisible(selectedLinkIndex_)) {
                selectedLinkIndex_ = -1;
            }
            pendingImageRender_ = false;
            imageStage2Triggered_ = false;
            pendingImages_.clear();
            requestUpdate();
        } else if (hasNextStage_) {
            loadStage(currentStage_ + 1, /*scrollToBottom=*/false);
            requestUpdate();
        }
        return;
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
        if (scrollY_ > 0) {
            int pageStep = std::max(40, viewHeight_ - 40);
            scrollY_ = std::max(0, scrollY_ - pageStep);
            if (selectedLinkIndex_ != -1 && !isLinkVisible(selectedLinkIndex_)) {
                selectedLinkIndex_ = -1;
            }
            pendingImageRender_ = false;
            imageStage2Triggered_ = false;
            pendingImages_.clear();
            requestUpdate();
        } else if (currentStage_ > 0) {
            loadStage(currentStage_ - 1, /*scrollToBottom=*/true);
            requestUpdate();
        }
        return;
    }

    // Bottom-right double button (Right / Left): select next / previous link
    if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
        if (!links_.empty()) {
            if (selectedLinkIndex_ == -1) {
                int found = -1;
                for (int i = 0; i < static_cast<int>(links_.size()); ++i) {
                    if (isLinkVisible(i)) {
                        found = i;
                        break;
                    }
                }
                if (found == -1) {
                    for (int i = 0; i < static_cast<int>(links_.size()); ++i) {
                        if (links_[i].minY >= scrollY_) {
                            found = i;
                            break;
                        }
                    }
                }
                selectedLinkIndex_ = (found != -1) ? found : 0;
            } else if (selectedLinkIndex_ + 1 < static_cast<int>(links_.size())) {
                selectedLinkIndex_++;
            } else if (hasNextStage_) {
                loadStage(currentStage_ + 1, /*scrollToBottom=*/false);
                if (!links_.empty()) {
                    selectedLinkIndex_ = 0;
                }
            } else {
                selectedLinkIndex_ = 0;
            }
            scrollLinkIntoView(selectedLinkIndex_);
            if (scrollY_ != renderedScrollY_) {
                pendingImageRender_ = false;
                imageStage2Triggered_ = false;
                pendingImages_.clear();
            } else if (pendingImageRender_) {
                imageStage2Triggered_ = false;
                stage1RenderCompleteMs_ = millis();
            }
            requestUpdate();
        } else if (hasNextStage_) {
            loadStage(currentStage_ + 1, /*scrollToBottom=*/false);
            requestUpdate();
        }
        return;
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
        if (!links_.empty()) {
            if (selectedLinkIndex_ == -1) {
                int found = -1;
                for (int i = static_cast<int>(links_.size()) - 1; i >= 0; --i) {
                    if (isLinkVisible(i)) {
                        found = i;
                        break;
                    }
                }
                if (found == -1) {
                    for (int i = static_cast<int>(links_.size()) - 1; i >= 0; --i) {
                        if (links_[i].maxY <= scrollY_) {
                            found = i;
                            break;
                        }
                    }
                }
                selectedLinkIndex_ = (found != -1) ? found : static_cast<int>(links_.size()) - 1;
            } else if (selectedLinkIndex_ > 0) {
                selectedLinkIndex_--;
            } else if (currentStage_ > 0) {
                loadStage(currentStage_ - 1, /*scrollToBottom=*/true);
                if (!links_.empty()) {
                    selectedLinkIndex_ = static_cast<int>(links_.size()) - 1;
                }
            } else {
                selectedLinkIndex_ = static_cast<int>(links_.size()) - 1;
            }
            scrollLinkIntoView(selectedLinkIndex_);
            if (scrollY_ != renderedScrollY_) {
                pendingImageRender_ = false;
                imageStage2Triggered_ = false;
                pendingImages_.clear();
            } else if (pendingImageRender_) {
                imageStage2Triggered_ = false;
                stage1RenderCompleteMs_ = millis();
            }
            requestUpdate();
        } else if (currentStage_ > 0) {
            loadStage(currentStage_ - 1, /*scrollToBottom=*/true);
            requestUpdate();
        }
        return;
    }

    // Confirm button: follow the currently selected link
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        if (selectedLinkIndex_ >= 0 && selectedLinkIndex_ < static_cast<int>(links_.size())) {
            const auto& lnk = links_[selectedLinkIndex_];
            std::string target = hrefPool_.substr(lnk.hrefOffset, lnk.hrefLen);
            ActivityResult result;
            result.isCancelled = false;
            result.data = FilePathResult{std::move(target)};
            setResult(std::move(result));
            finish();
            return;
        }
    }

    // Stage 2 deferred image decode & windowed refresh (skim debounced)
    if (pendingImageRender_) {
        const bool anyNavActive =
            mappedInput.isPressed(MappedInputManager::Button::PageBack) ||
            mappedInput.isPressed(MappedInputManager::Button::PageForward) ||
            mappedInput.isPressed(MappedInputManager::Button::Up) ||
            mappedInput.isPressed(MappedInputManager::Button::Down) ||
            mappedInput.isPressed(MappedInputManager::Button::Left) ||
            mappedInput.isPressed(MappedInputManager::Button::Right) ||
            mappedInput.isPressed(MappedInputManager::Button::Confirm) ||
            mappedInput.isPressed(MappedInputManager::Button::Back);
        if (anyNavActive) {
            stage1RenderCompleteMs_ = millis();
        } else if (!imageStage2Triggered_ &&
                   (millis() - stage1RenderCompleteMs_) >= IMAGE_DECODE_DEBOUNCE_MS) {
            imageStage2Triggered_ = true;
            requestUpdate();
        }
    }
}

void WikiInfoboxViewerActivity::renderPendingImages() {
    pendingImageRender_ = false;
    imageStage2Triggered_ = false;
    if (pendingImages_.empty()) {
        return;
    }

    const int screenW = renderer.getScreenWidth();
    const int screenH = renderer.getScreenHeight();
    int minX = screenW;
    int minY = screenH;
    int maxX = 0;
    int maxY = 0;

    for (const auto& img : pendingImages_) {
        if (img.boxW <= 0 || img.boxH <= 0 || img.imgSector == 0) continue;
        renderer.fillRect(img.boxX, img.boxY, img.boxW, img.boxH, false);
        WikiJpeg::render(renderer, reader_, img.imgSector, img.imgOffsetInSector,
                         img.imgDrawX, img.imgDrawY, img.dstW, img.dstH,
                         img.clipMinX, img.clipMaxX, img.clipMinY, img.clipMaxY);
        minX = std::min<int>(minX, img.boxX);
        minY = std::min<int>(minY, img.boxY);
        maxX = std::max<int>(maxX, img.boxX + img.boxW);
        maxY = std::max<int>(maxY, img.boxY + img.boxH);
    }
    pendingImages_.clear();

    if (maxX > minX && maxY > minY) {
        int wx = std::max(0, (minX / 8) * 8);
        int wxEnd = std::min(screenW, ((maxX + 7) / 8) * 8);
        int wy = std::max(0, minY);
        int wyEnd = std::min(screenH, maxY);
        if (wxEnd > wx && wyEnd > wy) {
            renderer.displayWindow(wx, wy, wxEnd - wx, wyEnd - wy);
        }
    }
}

void WikiInfoboxViewerActivity::renderLinkHighlightUpdate(int oldLinkIdx, int newLinkIdx) {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const int screenW = renderer.getScreenWidth();
    const int screenH = renderer.getScreenHeight();
    const int headerH = metrics.topPadding + metrics.headerHeight;
    const int startX = 16;
    const int renderStartY = headerH + 8;
    const int fontId = WikiFont::getBodyFontId();
    const int lineHeight = renderer.getLineHeight(fontId) + 4;

    int minX = screenW;
    int minY = screenH;
    int maxX = 0;
    int maxY = 0;

    auto expandForLink = [&](int linkIdx) {
        if (linkIdx < 0 || linkIdx >= static_cast<int>(links_.size())) return;
        const auto& lnk = links_[linkIdx];
        for (uint16_t ri = 0; ri < lnk.rectCount; ++ri) {
            const auto& rect = linkRects_[lnk.rectStartIdx + ri];
            int ry = renderStartY + rect.y - scrollY_;
            if (ry < renderStartY || ry + rect.h > renderStartY + viewHeight_) continue;
            minX = std::min<int>(minX, std::max(0, rect.x - 2));
            minY = std::min<int>(minY, std::max(0, ry - 4));
            maxX = std::max<int>(maxX, std::min(screenW, rect.x + rect.w + 3));
            maxY = std::max<int>(maxY, std::min(screenH, ry + rect.h + 3));
        }
    };

    expandForLink(oldLinkIdx);
    expandForLink(newLinkIdx);

    if (oldLinkIdx >= 0 && oldLinkIdx < static_cast<int>(links_.size())) {
        const auto& oldLnk = links_[oldLinkIdx];
        for (uint16_t ri = 0; ri < oldLnk.rectCount; ++ri) {
            const auto& rect = linkRects_[oldLnk.rectStartIdx + ri];
            int ry = renderStartY + rect.y - scrollY_;
            if (ry < renderStartY || ry + rect.h > renderStartY + viewHeight_) continue;
            renderer.drawLine(rect.x, ry, rect.x + rect.w, ry, false);
            renderer.drawLine(rect.x, ry, rect.x, ry + rect.h, false);
            renderer.drawLine(rect.x + rect.w, ry, rect.x + rect.w, ry + rect.h, false);
        }

        // Redraw only visible text lines overlapping the dirty vertical span (without touching decoded JPEGs)
        for (const auto& entry : entries_) {
            int itemScreenY = renderStartY + entry.y - scrollY_;
            if (itemScreenY + entry.height < minY || itemScreenY >= maxY) continue;
            if (entry.type == EntryType::HEADER) {
                int textY = itemScreenY + 2;
                for (const auto& line : entry.keyLines) {
                    if (textY >= renderStartY && textY + lineHeight <= renderStartY + viewHeight_ &&
                        textY + lineHeight >= minY && textY <= maxY) {
                        if (formatter_) formatter_->drawText(startX, textY, line.c_str(), true, true);
                        else renderer.drawText(fontId, startX, textY, line.c_str(), true);
                    }
                    textY += lineHeight + 2;
                }
            } else if (entry.type == EntryType::IMAGE) {
                int capY = itemScreenY + ((entry.imgHeight > 0) ? (entry.imgHeight + 8) : 4);
                for (const auto& line : entry.valueLines) {
                    if (capY >= renderStartY && capY + lineHeight <= renderStartY + viewHeight_ &&
                        capY + lineHeight >= minY && capY <= maxY) {
                        int w = measureTextWidth(line, false);
                        int capX = startX + (viewWidth_ - w) / 2;
                        if (capX < startX) capX = startX;
                        if (formatter_) formatter_->drawText(capX, capY, line.c_str(), true, false);
                        else renderer.drawText(fontId, capX, capY, line.c_str(), true);
                    }
                    capY += lineHeight;
                }
            } else if (entry.type == EntryType::KEY_VALUE) {
                int textY = itemScreenY + 2;
                for (const auto& line : entry.keyLines) {
                    if (textY >= renderStartY && textY + lineHeight <= renderStartY + viewHeight_ &&
                        textY + lineHeight >= minY && textY <= maxY) {
                        if (formatter_) formatter_->drawText(startX, textY, line.c_str(), true, true);
                        else renderer.drawText(fontId, startX, textY, line.c_str(), true);
                    }
                    textY += lineHeight;
                }
                const int indentX = entry.indentValue ? (startX + 16) : startX;
                for (const auto& line : entry.valueLines) {
                    if (textY >= renderStartY && textY + lineHeight <= renderStartY + viewHeight_ &&
                        textY + lineHeight >= minY && textY <= maxY) {
                        if (formatter_) formatter_->drawText(indentX, textY, line.c_str(), true, false);
                        else renderer.drawText(fontId, indentX, textY, line.c_str(), true);
                    }
                    textY += lineHeight;
                }
            }
        }
    }

    // Redraw link underlines & active selection box for old and new links
    auto drawLinkRects = [&](int linkIdx) {
        if (linkIdx < 0 || linkIdx >= static_cast<int>(links_.size())) return;
        const auto& lnk = links_[linkIdx];
        for (uint16_t ri = 0; ri < lnk.rectCount; ++ri) {
            const auto& rect = linkRects_[lnk.rectStartIdx + ri];
            int ry = renderStartY + rect.y - scrollY_;
            if (ry < renderStartY || ry + rect.h > renderStartY + viewHeight_) continue;
            if (linkIdx == selectedLinkIndex_) {
                renderer.drawLine(rect.x, ry, rect.x + rect.w, ry, true);
                renderer.drawLine(rect.x, ry + rect.h, rect.x + rect.w, ry + rect.h, true);
                renderer.drawLine(rect.x, ry, rect.x, ry + rect.h, true);
                renderer.drawLine(rect.x + rect.w, ry, rect.x + rect.w, ry + rect.h, true);
            } else {
                renderer.drawLine(rect.x, ry + rect.h, rect.x + rect.w, ry + rect.h, true);
            }
        }
    };
    drawLinkRects(oldLinkIdx);
    drawLinkRects(newLinkIdx);

    renderedLinkIndex_ = selectedLinkIndex_;

    if (maxX > minX && maxY > minY) {
        int wx = std::max(0, (minX / 8) * 8);
        int wxEnd = std::min(screenW, ((maxX + 7) / 8) * 8);
        int wy = std::max(0, minY);
        int wyEnd = std::min(screenH, maxY);
        if (wxEnd > wx && wyEnd > wy) {
            renderer.displayWindow(wx, wy, wxEnd - wx, wyEnd - wy);
        }
    }
}

void WikiInfoboxViewerActivity::render(RenderLock&& lock) {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const int screenW = renderer.getScreenWidth();
    const int screenH = renderer.getScreenHeight();
    const int headerH = metrics.topPadding + metrics.headerHeight;
    const int bottomBarH = WikiGUI::WIKI_HINTS_HEIGHT + 14;
    const int startX = 16;
    const int renderStartY = headerH + 8;
    const int fontId = WikiFont::getBodyFontId();
    const int lineHeight = renderer.getLineHeight(fontId) + 4;

    auto drawLinksOverlay = [&]() {
        for (int linkIdx = 0; linkIdx < static_cast<int>(links_.size()); ++linkIdx) {
            const auto& lnk = links_[linkIdx];
            for (uint16_t ri = 0; ri < lnk.rectCount; ++ri) {
                const auto& rect = linkRects_[lnk.rectStartIdx + ri];
                int ry = renderStartY + rect.y - scrollY_;
                if (ry < renderStartY || ry + rect.h > renderStartY + viewHeight_) {
                    continue;
                }
                if (linkIdx == selectedLinkIndex_) {
                    renderer.drawLine(rect.x, ry, rect.x + rect.w, ry, true);
                    renderer.drawLine(rect.x, ry + rect.h, rect.x + rect.w, ry + rect.h, true);
                    renderer.drawLine(rect.x, ry, rect.x, ry + rect.h, true);
                    renderer.drawLine(rect.x + rect.w, ry, rect.x + rect.w, ry + rect.h, true);
                } else {
                    renderer.drawLine(rect.x, ry + rect.h, rect.x + rect.w, ry + rect.h, true);
                }
            }
        }
    };

    // Fast incremental updates when scrolling has not changed:
    if (!pageRedrawNeeded_ && scrollY_ == renderedScrollY_) {
        if (renderedLinkIndex_ != selectedLinkIndex_) {
            renderLinkHighlightUpdate(renderedLinkIndex_, selectedLinkIndex_);
            if (pendingImageRender_ && imageStage2Triggered_) {
                renderPendingImages();
            }
            return;
        }
        if (pendingImageRender_ && imageStage2Triggered_) {
            renderPendingImages();
            return;
        }
        return;
    }

    pendingImages_.clear();
    pendingImageRender_ = false;
    imageStage2Triggered_ = false;

    renderer.clearScreen();

    // Header
    Rect headerRect{0, metrics.topPadding, screenW, metrics.headerHeight};
    char headerTitle[32];
    if (currentStage_ > 0 || hasNextStage_) {
        std::snprintf(headerTitle, sizeof(headerTitle), "Infobox (%d)", currentStage_ + 1);
    } else {
        std::snprintf(headerTitle, sizeof(headerTitle), "Infobox");
    }
    GUI.drawHeader(renderer, headerRect, headerTitle, title_.c_str());

    // Scroll indicator on right edge
    if (maxScrollY_ > 0) {
        int trackH = viewHeight_;
        int thumbH = std::max(20, (viewHeight_ * viewHeight_) / std::max(viewHeight_ + 1, totalContentHeight_));
        int thumbY = renderStartY + (scrollY_ * (trackH - thumbH)) / maxScrollY_;
        renderer.drawLine(screenW - 4, renderStartY, screenW - 4, renderStartY + trackH, true);
        renderer.fillRect(screenW - 5, thumbY, 3, thumbH, true);
    }

    for (const auto& entry : entries_) {
        int itemScreenY = renderStartY + entry.y - scrollY_;
        
        // Visibility check
        if (itemScreenY + entry.height < renderStartY || itemScreenY >= renderStartY + viewHeight_) {
            continue;
        }

        if (entry.type == EntryType::HEADER) {
            int textY = itemScreenY + 2;
            for (const auto& line : entry.keyLines) {
                if (textY >= renderStartY && textY + lineHeight <= renderStartY + viewHeight_) {
                    if (formatter_) formatter_->drawText(startX, textY, line.c_str(), true, true);
                    else renderer.drawText(fontId, startX, textY, line.c_str(), true);
                }
                textY += lineHeight + 2;
            }
            if (!entry.hasMoreInRow) {
                int divY = itemScreenY + entry.height - 3;
                if (divY >= renderStartY && divY < renderStartY + viewHeight_) {
                    renderer.drawLine(startX, divY, startX + viewWidth_, divY, true);
                    renderer.drawLine(startX, divY + 1, startX + viewWidth_, divY + 1, true);
                }
            }
        } else if (entry.type == EntryType::IMAGE) {
            int imgDrawHeight = entry.imgHeight;
            if (entry.imgSector != 0) {
                int clipMinX = startX;
                int clipMaxX = startX + viewWidth_;
                int clipMinY = std::max(renderStartY, itemScreenY);
                int clipMaxY = std::min(renderStartY + viewHeight_, itemScreenY + entry.imgHeight + 8);
                int dstW = entry.imgWidth;
                int dstH = entry.imgHeight;
                imgDrawHeight = dstH;
                int imgDrawX = startX + (viewWidth_ / 2) - (dstW / 2);
                if (imgDrawX < startX) imgDrawX = startX;
                int imgDrawY = itemScreenY + 4;

                int boxX = std::max(clipMinX, imgDrawX);
                int boxY = std::max(clipMinY, imgDrawY);
                int boxR = std::min(clipMaxX, imgDrawX + dstW);
                int boxB = std::min(clipMaxY, imgDrawY + dstH);
                if (boxR > boxX && boxB > boxY) {
                    renderer.drawRect(boxX, boxY, boxR - boxX, boxB - boxY, true);
                    pendingImages_.push_back(PendingImageRect{
                        entry.imgSector,
                        entry.imgOffsetInSector,
                        static_cast<int16_t>(imgDrawX),
                        static_cast<int16_t>(imgDrawY),
                        static_cast<int16_t>(dstW),
                        static_cast<int16_t>(dstH),
                        static_cast<int16_t>(clipMinX),
                        static_cast<int16_t>(clipMaxX),
                        static_cast<int16_t>(clipMinY),
                        static_cast<int16_t>(clipMaxY),
                        static_cast<int16_t>(boxX),
                        static_cast<int16_t>(boxY),
                        static_cast<int16_t>(boxR - boxX),
                        static_cast<int16_t>(boxB - boxY)});
                }
            }
            
            // Draw image caption below image
            int capY = itemScreenY + ((imgDrawHeight > 0) ? (imgDrawHeight + 8) : 4);
            for (const auto& line : entry.valueLines) {
                if (capY >= renderStartY && capY + lineHeight <= renderStartY + viewHeight_) {
                    int w = measureTextWidth(line, false);
                    int capX = startX + (viewWidth_ - w) / 2;
                    if (capX < startX) capX = startX;
                    if (formatter_) formatter_->drawText(capX, capY, line.c_str(), true, false);
                    else renderer.drawText(fontId, capX, capY, line.c_str(), true);
                }
                capY += lineHeight;
            }
            if (!entry.hasMoreInRow) {
                int divY = itemScreenY + entry.height - 2;
                if (divY >= renderStartY && divY < renderStartY + viewHeight_) {
                    renderer.drawLine(startX, divY, startX + viewWidth_, divY, true);
                }
            }
        } else if (entry.type == EntryType::KEY_VALUE) {
            // Stacked Card: Key on top line (bold)
            int textY = itemScreenY + 2;
            for (const auto& line : entry.keyLines) {
                if (textY >= renderStartY && textY + lineHeight <= renderStartY + viewHeight_) {
                    if (formatter_) formatter_->drawText(startX, textY, line.c_str(), true, true);
                    else renderer.drawText(fontId, startX, textY, line.c_str(), true);
                }
                textY += lineHeight;
            }
            
            // Value indented below key (or aligned to startX if key is empty and not a continuation)
            const int indentX = entry.indentValue ? (startX + 16) : startX;
            for (const auto& line : entry.valueLines) {
                if (textY >= renderStartY && textY + lineHeight <= renderStartY + viewHeight_) {
                    if (formatter_) formatter_->drawText(indentX, textY, line.c_str(), true, false);
                    else renderer.drawText(fontId, indentX, textY, line.c_str(), true);
                }
                textY += lineHeight;
            }
            
            // Subtle row divider rule only after the final chunk of a row
            if (!entry.hasMoreInRow) {
                int divY = itemScreenY + entry.height - 2;
                if (divY >= renderStartY && divY < renderStartY + viewHeight_) {
                    renderer.drawLine(startX, divY, startX + viewWidth_, divY, true);
                }
            }
        }
    }

    drawLinksOverlay();
    pageRedrawNeeded_ = false;
    renderedScrollY_ = scrollY_;
    renderedLinkIndex_ = selectedLinkIndex_;

    // Bottom Bar Chrome
    int bottomBarY = screenH - bottomBarH;
    renderer.fillRect(0, bottomBarY, screenW, bottomBarH, false);
    renderer.drawLine(0, bottomBarY, screenW - 1, bottomBarY, true);
    if (!links_.empty()) {
        WikiGUI::drawDoubleButtonHints(renderer, {"Back", ""}, {"Select link", ""}, {"Prev link", ""}, {"Next link", ""});
    } else {
        WikiGUI::drawDoubleButtonHints(renderer, {"Back", ""}, {"", ""}, {"", ""}, {"", ""});
    }

    renderer.displayBuffer(HalDisplay::FAST_REFRESH);

    if (!pendingImages_.empty()) {
        pendingImageRender_ = true;
        imageStage2Triggered_ = false;
        stage1RenderCompleteMs_ = millis();
    }
}
