#include "LanguageRegistry.h"

#include <algorithm>
#include <array>

#include "HyphenationCommon.h"
// German Liang hyphenation data costs ~206 KB of flash; leave it opt-in so the
// ESP32-C3 OTA slot fits Offline Wikipedia + Stable Pages alongside WolfSSL TLS 1.3.
#ifndef CPR_ENABLE_GERMAN_HYPHENATION
#define CPR_ENABLE_GERMAN_HYPHENATION 0
#endif
#if CPR_ENABLE_GERMAN_HYPHENATION
#include "generated/hyph-de.trie.h"
#endif
#include "generated/hyph-en.trie.h"
#include "generated/hyph-es.trie.h"
#include "generated/hyph-fr.trie.h"
#include "generated/hyph-it.trie.h"
#include "generated/hyph-pl.trie.h"
#include "generated/hyph-pt.trie.h"
#include "generated/hyph-ru.trie.h"
#include "generated/hyph-sv.trie.h"
#include "generated/hyph-uk.trie.h"

namespace {

// English hyphenation patterns (3/3 minimum prefix/suffix length)
LanguageHyphenator englishHyphenator(en_patterns, isLatinLetter, toLowerLatin, 3, 3);
LanguageHyphenator frenchHyphenator(fr_patterns, isLatinLetter, toLowerLatin);
#if CPR_ENABLE_GERMAN_HYPHENATION
LanguageHyphenator germanHyphenator(de_patterns, isLatinLetter, toLowerLatin);
#endif
LanguageHyphenator russianHyphenator(ru_patterns, isCyrillicLetter, toLowerCyrillic);
LanguageHyphenator spanishHyphenator(es_patterns, isLatinLetter, toLowerLatin);
LanguageHyphenator italianHyphenator(it_patterns, isLatinLetter, toLowerLatin);
LanguageHyphenator swedishHyphenator(sv_patterns, isLatinLetter, toLowerLatin);
LanguageHyphenator ukrainianHyphenator(uk_patterns, isCyrillicLetter, toLowerCyrillic);
LanguageHyphenator polishHyphenator(pl_patterns, isLatinLetter, toLowerLatin);
LanguageHyphenator portugueseHyphenator(pt_patterns, isLatinLetter, toLowerLatin);

#if CPR_ENABLE_GERMAN_HYPHENATION
using EntryArray = std::array<LanguageEntry, 10>;
#else
using EntryArray = std::array<LanguageEntry, 9>;
#endif

const EntryArray& entries() {
  static const EntryArray kEntries = {{{"english", "en", &englishHyphenator},
                                       {"french", "fr", &frenchHyphenator},
#if CPR_ENABLE_GERMAN_HYPHENATION
                                       {"german", "de", &germanHyphenator},
#endif
                                       {"russian", "ru", &russianHyphenator},
                                       {"spanish", "es", &spanishHyphenator},
                                       {"italian", "it", &italianHyphenator},
                                       {"polish", "pl", &polishHyphenator},
                                       {"portuguese", "pt", &portugueseHyphenator},
                                       {"swedish", "sv", &swedishHyphenator},
                                       {"ukrainian", "uk", &ukrainianHyphenator}}};
  return kEntries;
}

}  // namespace

const LanguageHyphenator* getLanguageHyphenatorForPrimaryTag(const std::string& primaryTag) {
  const auto& allEntries = entries();
  const auto it = std::find_if(allEntries.begin(), allEntries.end(),
                               [&primaryTag](const LanguageEntry& entry) { return primaryTag == entry.primaryTag; });
  return (it != allEntries.end()) ? it->hyphenator : nullptr;
}

LanguageEntryView getLanguageEntries() {
  const auto& allEntries = entries();
  return LanguageEntryView{allEntries.data(), allEntries.size()};
}
