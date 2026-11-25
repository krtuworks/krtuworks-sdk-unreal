#pragma once

#include "KrtuworksCore/Export.h"

#include <map>
#include <string>
#include <vector>

namespace krtu
{
/// Every sentence the ready-made screens show, in English (the JS package's table plus the keys only a game on
/// a desktop or a console needs). A game passes its own, any subset in any language, as UiOptions::texts; `{name}`
/// marks a value filled in at show time. Keys are the same everywhere (`age.title`, `device.go` …).
using UiTexts = std::map<std::string, std::string>;

/// The English table.
KRTU_API const UiTexts& uiDefaultTexts();

/// The game's sentence for `key`, else the English one (else the key itself), with `{name}` filled from `vars`.
KRTU_API std::string textFor(const UiTexts& texts, const std::string& key, const std::map<std::string, std::string>& vars = {});

/// One piece of a sentence: plain text, or the name of a `{name}` mark.
struct KRTU_API TextSegment
{
    std::string text;
    bool mark = false;
};

/// The sentence split around its `{name}` marks, for a screen that puts something other than text there (the terms
/// sentence's two links): "I accept the ", mark "terms", " and the ", mark "privacy", ".".
KRTU_API std::vector<TextSegment> textSegments(const UiTexts& texts, const std::string& key);
}  // namespace krtu
