#pragma once

#include <FreeInkUICore.h>
#include <GfxRenderer.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "SdRawReader.h"
#include "activities/UiListActivity.h"
#include "fontIds.h"

/**
 * @class WikiScratchBuffer
 * @brief Lifecycle-managed heap scratch buffer (45,000 bytes) shared across Wiki decompression
 *        and JPEG decoding. Zero static (.bss/.data) RAM cost when the Wiki subsystem is inactive.
 */
class WikiScratchBuffer {
 public:
  static constexpr size_t BUFFER_SIZE = 45000;
  static constexpr size_t MIN_BUFFER_SIZE = 32768;

  static void registerRenderer(GfxRenderer* renderer);
  static uint8_t* acquire();
  static void release();
  static uint8_t* get();
  static size_t capacity();
  static void beginUse();
  static void endUse();
  static uint8_t* borrowForFontDecompressor(uint32_t neededBytes, uint32_t* outGeneration);
};

class WikiScratchScope {
 public:
  WikiScratchScope() { WikiScratchBuffer::acquire(); }
  explicit WikiScratchScope(GfxRenderer& renderer) {
    WikiScratchBuffer::registerRenderer(&renderer);
    WikiScratchBuffer::acquire();
  }
  ~WikiScratchScope() { WikiScratchBuffer::release(); }
  WikiScratchScope(const WikiScratchScope&) = delete;
  WikiScratchScope& operator=(const WikiScratchScope&) = delete;
};

struct WikiRecentEntry {
  char title[96] = "";
  uint64_t offset = 0;
};

/**
 * @struct WikiConfigData
 * @brief Self-contained Offline Wikipedia archive configuration persisted to /.crosspoint/wiki.cfg.
 */
struct WikiConfigData {
  static constexpr size_t MAX_RECENT_ITEMS = 5;

  uint32_t indexSector = 0;
  uint32_t chunksSector = 0;
  uint32_t fontSector = 0;
  uint8_t hasImages = 1;
  char imagePath[128] = "";

  char resumeTitle[128] = "";
  uint64_t resumeOffset = 0;
  int32_t resumePage = 0;
  int32_t resumeLink = -1;
  std::vector<uint32_t> resumePageOffsets;

  std::vector<WikiRecentEntry> recentSearches;
  std::vector<WikiRecentEntry> recentVisited;

  void clearResume() {
    resumeTitle[0] = '\0';
    resumeOffset = 0;
    resumePage = 0;
    resumeLink = -1;
    resumePageOffsets.clear();
  }

  void addRecentSearch(const char* title, uint64_t offset);
  void addRecentVisited(const char* title, uint64_t offset);
  void clearRecentHistory() {
    recentSearches.clear();
    recentVisited.clear();
  }
};

class WikiConfig {
 public:
  static WikiConfigData& get();
  static bool save();
  static void clear();
  static bool configureFromImageFile(const std::string& imgPath);
  static void onAcquire();
  static void onRelease();
};

namespace WikiFont {
inline int getBodyFontId() {
  return (SETTINGS.fontFamily == CrossPointSettings::LEXENDDECA) ? LEXENDDECA_14_FONT_ID : BITTER_14_FONT_ID;
}

inline int getHeadingFontId() {
  return (SETTINGS.fontFamily == CrossPointSettings::LEXENDDECA) ? LEXENDDECA_16_FONT_ID : BITTER_16_FONT_ID;
}
}  // namespace WikiFont

namespace WikiJpeg {
bool getDimensions(SdRawReader& reader, uint32_t startSector, uint32_t offsetInSector, int& outWidth, int& outHeight);

bool render(GfxRenderer& renderer, SdRawReader& reader, uint32_t startSector, uint32_t offsetInSector, int drawX,
            int drawY, int dstW = 0, int dstH = 0, int clipMinX = 0, int clipMaxX = 0x7FFF, int clipMinY = 0,
            int clipMaxY = 0x7FFF);

bool renderCenteredInBox(GfxRenderer& renderer, SdRawReader& reader, uint32_t startSector, uint32_t offsetInSector,
                         int boxX, int boxW, int minLeftX, int drawY, int clipMinX, int clipMaxX, int clipMinY,
                         int clipMaxY);
}  // namespace WikiJpeg

/**
 * @class WikiFileSelectActivity
 * @brief Self-contained .img file browser activity inside the Wiki module.
 */
class WikiFileSelectActivity final : public UiListActivity {
 public:
  explicit WikiFileSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string basepath = "/");
  ~WikiFileSelectActivity() override = default;

 protected:
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;
  void onBackButton() override;
  void drawFooter() override;

 private:
  void loadFiles();

  std::string basepath_;
  std::vector<std::string> entries_;
  std::vector<freeink::ui::ListItem> rowItems_;
};

/**
 * @class WikiRecentActivity
 * @brief Displays recent searches (up to 5), recently visited articles (up to 5), and a Clear history action.
 */
class WikiRecentActivity final : public Activity {
 public:
  WikiRecentActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  ~WikiRecentActivity() override = default;

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  void activateSelection();

  int selectedIndex_ = 0;
};
