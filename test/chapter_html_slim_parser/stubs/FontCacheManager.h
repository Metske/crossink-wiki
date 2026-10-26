#pragma once

#include <EpdFontFamily.h>

class FontCacheManager {
 public:
  class ScopedPrewarm {
   public:
    explicit ScopedPrewarm(FontCacheManager*) {}
  };

  void prewarmWidthCache(int, EpdFontFamily::Style) {}
  void releaseSdFontCaches() {}
};
