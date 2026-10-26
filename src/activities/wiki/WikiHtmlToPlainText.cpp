#include "WikiHtmlToPlainText.h"
#include "WikiDecompressor.h"
#include <Epub/htmlEntities.h>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <algorithm>

namespace {

std::string cleanLatexMath(std::string text) {
    auto replaceAll = [](std::string& str, const std::string& from, const std::string& to) {
        size_t start_pos = 0;
        while((start_pos = str.find(from, start_pos)) != std::string::npos) {
            str.replace(start_pos, from.length(), to);
            start_pos += to.length(); 
        }
    };

    replaceAll(text, "\\displaystyle", "");
    replaceAll(text, "/displaystyle", "");
    replaceAll(text, "\\textstyle", "");
    replaceAll(text, "\\scriptstyle", "");
    
    auto removeCommand = [&](const std::string& cmd) {
        size_t pos = 0;
        while ((pos = text.find(cmd, pos)) != std::string::npos) {
            size_t argStart = text.find('{', pos);
            if (argStart != std::string::npos && argStart < pos + cmd.length() + 2) {
                int depth = 1;
                size_t argEnd = argStart + 1;
                while (argEnd < text.length() && depth > 0) {
                    if (text[argEnd] == '{') depth++;
                    if (text[argEnd] == '}') depth--;
                    argEnd++;
                }
                if (depth == 0) {
                    text.erase(argEnd - 1, 1);
                    text.erase(pos, argStart - pos + 1);
                    continue;
                }
            }
            text.erase(pos, cmd.length());
        }
    };
    
    removeCommand("\\mathbf");
    removeCommand("\\mathrm");
    removeCommand("\\text");
    removeCommand("\\mathit");

    size_t fracPos = 0;
    while ((fracPos = text.find("\\frac")) != std::string::npos) {
        size_t aStart = text.find('{', fracPos);
        if (aStart == std::string::npos) { text.replace(fracPos, 5, " frac "); continue; }
        int depth = 1;
        size_t aEnd = aStart + 1;
        while (aEnd < text.length() && depth > 0) {
            if (text[aEnd] == '{') depth++;
            if (text[aEnd] == '}') depth--;
            aEnd++;
        }
        if (depth != 0) { text.replace(fracPos, 5, " frac "); continue; }
        
        size_t bStart = text.find('{', aEnd - 1);
        if (bStart == std::string::npos) { text.replace(fracPos, 5, " frac "); continue; }
        depth = 1;
        size_t bEnd = bStart + 1;
        while (bEnd < text.length() && depth > 0) {
            if (text[bEnd] == '{') depth++;
            if (text[bEnd] == '}') depth--;
            bEnd++;
        }
        if (depth != 0) { text.replace(fracPos, 5, " frac "); continue; }
        
        std::string a = text.substr(aStart + 1, aEnd - aStart - 2);
        std::string b = text.substr(bStart + 1, bEnd - bStart - 2);
        
        std::string rep = "(" + a + "/" + b + ")";
        text.replace(fracPos, bEnd - fracPos, rep);
    }
    
    replaceAll(text, "\\propto", "∝");
    replaceAll(text, "\\infty", "∞");
    replaceAll(text, "\\rho", "ρ");
    replaceAll(text, "\\left(", "(");
    replaceAll(text, "\\right)", ")");
    replaceAll(text, "\\approx", "≈");
    replaceAll(text, "\\cdot", "·");
    replaceAll(text, "\\times", "×");
    replaceAll(text, "\\pi", "π");
    replaceAll(text, "\\theta", "θ");
    replaceAll(text, "\\alpha", "α");
    replaceAll(text, "\\beta", "β");
    replaceAll(text, "\\gamma", "γ");
    replaceAll(text, "\\Delta", "Δ");
    replaceAll(text, "\\sum", "∑");
    replaceAll(text, "\\int", "∫");
    replaceAll(text, "\\le", "≤");
    replaceAll(text, "\\ge", "≥");
    replaceAll(text, "\\neq", "≠");
    replaceAll(text, "\\pm", "±");
    replaceAll(text, "\\rightarrow", "→");
    replaceAll(text, "\\Rightarrow", "⇒");
    replaceAll(text, "\\,", " ");
    replaceAll(text, "\\;", " ");
    replaceAll(text, "\\ ", " ");
    replaceAll(text, "\\\\", " ");
    
    replaceAll(text, "_{", "_");
    replaceAll(text, "^{", "^");
    replaceAll(text, "}", "");
    replaceAll(text, "{", "");
    
    replaceAll(text, "  ", " ");
    
    return text;
}

bool isTagStart(const std::string& input, size_t pos) {
  if (pos + 1 >= input.size()) return false;
  const unsigned char next = input[pos + 1];
  return next == '/' || next == '!' || next == '?' || std::isalpha(next);
}

enum class TagBreak : uint8_t { None, Line, Paragraph };

TagBreak tagBreak(const std::string& input, size_t start, size_t end) {
  while (start < end && (input[start] == '/' || std::isspace(static_cast<unsigned char>(input[start])))) start++;
  const size_t nameStart = start;
  while (start < end && std::isalnum(static_cast<unsigned char>(input[start]))) start++;
  const size_t len = start - nameStart;
  if (len == 0) return TagBreak::None;

  const auto equals = [&](const char* name) {
    const size_t nameLen = strlen(name);
    if (len != nameLen) return false;
    for (size_t i = 0; i < len; i++) {
      if (std::tolower(static_cast<unsigned char>(input[nameStart + i])) != name[i]) return false;
    }
    return true;
  };

  if (equals("p") || equals("h1") || equals("h2") || equals("h3") || equals("h4") || equals("h5") || equals("h6") ||
      equals("hr")) {
    return TagBreak::Paragraph;
  }
  if (equals("br") || equals("div") || equals("li") || equals("tr")) return TagBreak::Line;
  return TagBreak::None;
}

template <typename AddFn>
void appendCodepoint(AddFn add, uint32_t codepoint) {
  if (codepoint == 0 || codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) return;
  
  if (codepoint <= 0x7F) {
    add(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7FF) {
    add(static_cast<char>(0xC0 | (codepoint >> 6)));
    add(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else if (codepoint <= 0xFFFF) {
    add(static_cast<char>(0xE0 | (codepoint >> 12)));
    add(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    add(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else {
    add(static_cast<char>(0xF0 | (codepoint >> 18)));
    add(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
    add(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    add(static_cast<char>(0x80 | (codepoint & 0x3F)));
  }
}

template <typename AddFn>
bool appendNumericEntity(AddFn add, const char* entity, size_t len) {
  if (len < 4 || entity[0] != '&' || entity[1] != '#' || entity[len - 1] != ';') return false;
  size_t pos = 2;
  uint32_t base = 10;
  if (entity[pos] == 'x' || entity[pos] == 'X') {
    base = 16;
    pos++;
  }
  if (pos == len - 1) return false;

  uint32_t value = 0;
  for (; pos < len - 1; pos++) {
    const unsigned char c = entity[pos];
    uint32_t digit;
    if (c >= '0' && c <= '9')
      digit = c - '0';
    else if (base == 16 && c >= 'a' && c <= 'f')
      digit = c - 'a' + 10;
    else if (base == 16 && c >= 'A' && c <= 'F')
      digit = c - 'A' + 10;
    else
      return false;
    if (value > (0x10FFFF - digit) / base) return false;
    value = value * base + digit;
  }
  appendCodepoint(add, value);
  return true;
}

template <int Mode>
void appendBreak(std::string& output, size_t& count, size_t numBreaks = 1, size_t startCharIdx = 0, size_t maxChars = 0xFFFFFFFF) {
  if constexpr (Mode == 0) {
      count += numBreaks;
  } else {
      while (!output.empty() && output.back() == ' ') output.pop_back();
      if (output.empty()) return;
      size_t existing = 0;
      while (existing < output.size() && output[output.size() - existing - 1] == '\n') existing++;
      while (existing++ < numBreaks) output.push_back('\n');
  }
}

#include <atomic>

template <int Mode>
WikiMetrics streamHtmlToPlainTextImpl(WikiDecompressor& decompressor, size_t maxBytes, std::string& output, std::vector<ExtractedLink>* outLinks, std::string* outHrefPool, std::vector<TOCEntry>* outTOC = nullptr, size_t startCharIdx = 0, size_t maxChars = 0xFFFFFFFF, bool* isEofOut = nullptr, std::atomic<bool>* cancelToken = nullptr) {
  WikiMetrics metrics;
  std::string html;
  html.reserve(2048 + 1024);

  char chunk[2048];
  bool isEof = false;
  size_t totalRead = 0;

  auto fillBuffer = [&]() {
      if (isEof) return;
      while (html.size() < 2048 && !decompressor.isDone() && totalRead < maxBytes) {
          size_t maxToRead = std::min(sizeof(chunk), maxBytes - totalRead);
          size_t n = decompressor.read(chunk, maxToRead);
          if (n == 0) { isEof = true; break; }
          totalRead += n;
          html.append(chunk, n);
      }
  };

  fillBuffer();
  size_t i = 0;
  
  struct ListState { char type; int count; };
  std::vector<ListState> listStack;
  
  auto updateIndent = [&]() {
      if constexpr (Mode == 1) {
          output.push_back('\x04');
          output.push_back(static_cast<char>(listStack.size()));
      } else if constexpr (Mode == 2) {
          if (metrics.exactTextSize >= startCharIdx && metrics.exactTextSize < startCharIdx + maxChars) {
              output.push_back('\x04');
              output.push_back(static_cast<char>(listStack.size()));
          }
      }
  };

  bool inHeading = false;
  uint8_t currentHeadingLevel = 0;
  uint32_t currentHeadingCharIdx = 0;
  std::string currentHeadingText;
  
  bool inMath = false;
  bool inAnnotation = false;
  std::string annotationBuffer;
  
  char lastChar = 0;
  int consecutiveNewlines = 0;

  auto addFormatToken = [&](char token) {
      if constexpr (Mode == 1) {
          output.push_back(token);
      } else if constexpr (Mode == 2) {
          if (metrics.exactTextSize >= startCharIdx && metrics.exactTextSize < startCharIdx + maxChars) {
              output.push_back(token);
          }
      }
  };

  auto addCharRaw = [&](char c) {
      if constexpr (Mode == 2) {
          if (!listStack.empty() && metrics.exactTextSize == startCharIdx) {
              output.push_back('\x04');
              output.push_back(static_cast<char>(listStack.size()));
          }
      }
      if (inMath && !inAnnotation) return;
      if (inAnnotation) {
          annotationBuffer.push_back(c);
          return;
      }

      if (metrics.exactTextSize == 0 && (c == ' ' || c == '\n' || c == '\r' || c == '\t')) {
          return;
      }

      if (inHeading) {
          if (!currentHeadingText.empty() || (c != ' ' && c != '\n' && c != '\r' && c != '\t')) {
              currentHeadingText.push_back(c);
          }
      }

      if constexpr (Mode == 0) {
          metrics.exactTextSize++;
      } else if constexpr (Mode == 1) {
          output.push_back(c);
          metrics.exactTextSize++;
      } else if constexpr (Mode == 2) {
          if (metrics.exactTextSize >= startCharIdx && metrics.exactTextSize < startCharIdx + maxChars) {
              output.push_back(c);
          }
          metrics.exactTextSize++;
      }
      lastChar = c;
      if (c == '\n') consecutiveNewlines++;
      else consecutiveNewlines = 0;
  };
  
  auto addChar = [&](char c) {
      if (c == ' ') {
          if (lastChar == ' ' || lastChar == '\n') return; // Deduplicate spaces
      }
      addCharRaw(c);
  };
  
  auto handleBreak = [&](int numBreaks) {
      // If we need to add breaks, first strip trailing space if any
      // Since we don't have access to output in Mode 0 easily without complexity, 
      // we'll just not worry about popping from output, since we deduplicated spaces anyway.
      // Actually, if lastChar is ' ', it means a space was added. We can just add newlines.
      for (int k = consecutiveNewlines; k < numBreaks; ++k) {
          addCharRaw('\n');
      }
  };

  while (i < html.size()) {
      if (cancelToken && cancelToken->load()) break;

      if constexpr (Mode == 2) {
          if (metrics.exactTextSize >= startCharIdx + maxChars) {
              break;
          }
      }

      if (html.size() - i < 1024 && !isEof) {
          html.erase(0, i);
          i = 0;
          fillBuffer();
      }

      if (html[i] == '<' && isTagStart(html, i)) {
          size_t close = html.find('>', i + 1);
          if (close == std::string::npos) {
              if (isEof) {
                  addChar(html[i++]);
                  continue;
              }
              if (i > 0) {
                  html.erase(0, i);
                  i = 0;
                  fillBuffer();
                  continue;
              } else {
                  addChar(html[i++]);
                  continue;
              }
          }

          size_t nameStart = i + 1;
          while (nameStart < close && (html[nameStart] == '/' || std::isspace(static_cast<unsigned char>(html[nameStart])))) nameStart++;
          size_t nameEnd = nameStart;
          while (nameEnd < close && std::isalnum(static_cast<unsigned char>(html[nameEnd]))) nameEnd++;
          
          bool isA = (nameEnd - nameStart == 1 && (html[nameStart] == 'a' || html[nameStart] == 'A'));
          
          bool isHeadingTag = false;
          char headingLevelChar = 0;
          if (nameEnd - nameStart == 2 && (html[nameStart] == 'h' || html[nameStart] == 'H')) {
              char levelChar = html[nameStart+1];
              if (levelChar == '2' || levelChar == '3') {
                  isHeadingTag = true;
                  headingLevelChar = levelChar;
                  if (html[i+1] == '/') {
                      if (inHeading && outTOC) {
                          outTOC->push_back({currentHeadingLevel, currentHeadingText, currentHeadingCharIdx});
                      }
                      inHeading = false;
                  }
              }
          }

          if (isA) {
            if (html[i+1] != '/') {
              size_t hrefPos = html.find("href", nameEnd);
              if (hrefPos != std::string::npos && hrefPos < close) {
                size_t quotePos = html.find_first_of("\"'", hrefPos);
                if (quotePos != std::string::npos && quotePos < close) {
                  char quote = html[quotePos];
                  size_t endQuote = html.find(quote, quotePos + 1);
                  if (endQuote != std::string::npos && endQuote < close) {
                    size_t hrefLen = endQuote - quotePos - 1;
                    if constexpr (Mode == 0) {
                        if (metrics.exactLinkCount < 250 && metrics.exactHrefSize + hrefLen < 6000) {
                            metrics.exactHrefSize += hrefLen;
                            metrics.exactLinkCount++;
                        }
                    } else if (outLinks && outHrefPool) {
                        bool inRange = (Mode == 1) || (metrics.exactTextSize >= startCharIdx && metrics.exactTextSize < startCharIdx + maxChars);
                        if (inRange && metrics.exactLinkCount < 250 && metrics.exactHrefSize + hrefLen < 6000) {
                            ExtractedLink link;
                            link.hrefOffset = outHrefPool->size();
                            link.hrefLen = hrefLen;
                            outHrefPool->append(html, quotePos + 1, link.hrefLen);
                            link.startCharIndex = output.size();
                            link.endCharIndex = 0;
                            outLinks->push_back(link);
                            metrics.exactLinkCount++;
                            metrics.exactHrefSize += hrefLen;
                        }
                    }
                  }
                }
              }
            } else {
              if constexpr (Mode != 0) {
                  if (outLinks && !outLinks->empty() && outLinks->back().endCharIndex == 0) {
                    outLinks->back().endCharIndex = output.size();
                  }
              }
            }
          }

          auto tagEquals = [&](const char* expected) {
              size_t len = nameEnd - nameStart;
              if (len != strlen(expected)) return false;
              for (size_t k = 0; k < len; ++k) {
                  if (std::tolower(static_cast<unsigned char>(html[nameStart + k])) != expected[k]) return false;
              }
              return true;
          };

          if (tagEquals("math")) {
              if (html[i + 1] != '/') inMath = true;
              else inMath = false;
          }
          if (tagEquals("annotation")) {
              if (html[i + 1] != '/') {
                  inAnnotation = true;
                  annotationBuffer.clear();
              } else {
                  inAnnotation = false;
                  std::string cleaned = cleanLatexMath(annotationBuffer);
                  bool oldInMath = inMath;
                  inMath = false;
                  for (char c : cleaned) {
                      addChar(c);
                  }
                  inMath = oldInMath;
                  annotationBuffer.clear();
              }
          }

          if (tagEquals("ul")) {
              if (html[i + 1] != '/') {
                  listStack.push_back({'u', 0});
                  updateIndent();
              } else if (!listStack.empty()) {
                  listStack.pop_back();
                  updateIndent();
              }
          }
          if (tagEquals("ol")) {
              if (html[i + 1] != '/') {
                  listStack.push_back({'o', 1});
                  updateIndent();
              } else if (!listStack.empty()) {
                  listStack.pop_back();
                  updateIndent();
              }
          }

          const TagBreak separator = tagBreak(html, i + 1, close);
          if (separator == TagBreak::Line)
            handleBreak(1);
          else if (separator == TagBreak::Paragraph)
            handleBreak(2);

          if (isHeadingTag && html[i+1] != '/') {
              inHeading = true;
              currentHeadingLevel = headingLevelChar - '0';
              currentHeadingCharIdx = metrics.exactTextSize;
              if (currentHeadingLevel == 2) addFormatToken('\x02');
              else if (currentHeadingLevel == 3) addFormatToken('\x03');
              currentHeadingText.clear();
          }
            
          if (tagEquals("li") && html[i + 1] != '/') {
              if (!listStack.empty()) {
                  auto& st = listStack.back();
                  std::string prefix;
                  if (st.type == 'u') {
                      prefix = "\xE2\x80\xA2 "; // bullet "• "
                  } else {
                      prefix = std::to_string(st.count++) + ". ";
                  }
                  for (char c : prefix) {
                      addCharRaw(c);
                  }
              }
          }
          
          i = close + 1;
          continue;
      }

      if (html[i] == '&') {
          size_t semicolon = html.find(';', i + 1);
          if (semicolon == std::string::npos && html.size() - i < 20 && !isEof) {
              if (i > 0) {
                  html.erase(0, i);
                  i = 0;
                  fillBuffer();
                  continue;
              }
          }
          semicolon = html.find(';', i + 1);
          if (semicolon != std::string::npos && semicolon - i <= 16) {
              const size_t len = semicolon - i + 1;
              const char* value = lookupHtmlEntity(html.data() + i, len);
              if (value != nullptr) {
                for (size_t k = 0; value[k] != '\0'; ++k) {
                    addChar(value[k]);
                }
                i = semicolon + 1;
                continue;
              }
              if (appendNumericEntity(addChar, html.data() + i, len)) {
                i = semicolon + 1;
                continue;
              }
          }
      }

      const unsigned char c = html[i++];
      if (c == '\r' || c == '\t') {
        addChar(' ');
      } else if (c == '\n') {
        addChar(' ');
      } else {
        addChar(static_cast<char>(c));
      }
  }

  if constexpr (Mode != 0) {
      if (outLinks && !outLinks->empty() && outLinks->back().endCharIndex == 0) {
          outLinks->back().endCharIndex = output.size();
      }
      while (!output.empty() && (output.back() == ' ' || output.back() == '\n')) output.pop_back();
      
  }
  return metrics;
}

}  // namespace

WikiMetrics measureStreamHtmlToPlainText(WikiDecompressor& decompressor, size_t maxBytes) {
    std::string dummy;
    return streamHtmlToPlainTextImpl<0>(decompressor, maxBytes, dummy, nullptr, nullptr);
}

std::string streamHtmlToPlainText(WikiDecompressor& decompressor, size_t maxBytes, size_t exactSize, std::vector<ExtractedLink>* outLinks, std::string* outHrefPool) {
    std::string output;
    output.reserve(exactSize + 128); 
    streamHtmlToPlainTextImpl<1>(decompressor, maxBytes, output, outLinks, outHrefPool);
    output.shrink_to_fit();
    return output;
}

std::string streamHtmlToPlainTextChunk(WikiDecompressor& decompressor, size_t maxBytes, size_t startCharIdx, size_t maxChars, std::vector<ExtractedLink>* outLinks, std::string* outHrefPool, bool& isEof, std::atomic<bool>* cancelToken) {
    std::string output;
    output.reserve(maxChars + 128);
    streamHtmlToPlainTextImpl<2>(decompressor, maxBytes, output, outLinks, outHrefPool, nullptr, startCharIdx, maxChars, &isEof, cancelToken);
    return output;
}
std::vector<TOCEntry> extractArticleTOC(WikiDecompressor& decompressor, size_t maxBytes) {
    std::string dummy;
    std::vector<TOCEntry> toc;
    streamHtmlToPlainTextImpl<0>(decompressor, maxBytes, dummy, nullptr, nullptr, &toc);
    return toc;
}
