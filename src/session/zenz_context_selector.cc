#include "session/zenz_context_selector.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include "absl/strings/string_view.h"
#include "base/util.h"

namespace mozc {
namespace session {
namespace {

std::vector<char32_t> ToCodepoints(const absl::string_view text) {
  std::vector<char32_t> codepoints;

  for (ConstChar32Iterator iter(text); !iter.Done(); iter.Next()) {
    codepoints.push_back(iter.Get());
  }

  return codepoints;
}

bool IsHorizontalBlankSpace(const char32_t codepoint) {
  return codepoint == U' ' || codepoint == U'\t' || codepoint == U'　';
}

size_t LogicalLineBreakLength(const std::vector<char32_t>& codepoints,
                              const size_t index) {
  if (index >= codepoints.size()) {
    return 0;
  }

  if (codepoints[index] == U'\r') {
    if (index + 1 < codepoints.size() && codepoints[index + 1] == U'\n') {
      return 2;
    }
    return 1;
  }

  return codepoints[index] == U'\n' ? 1 : 0;
}

// Returns the first character position after the final logical line break.
// Every real LF, CRLF, or CR is a hard semantic boundary for Zenz context.
size_t FindCurrentLineStart(const std::vector<char32_t>& codepoints) {
  size_t line_start = 0;
  size_t i = 0;

  while (i < codepoints.size()) {
    const size_t break_len = LogicalLineBreakLength(codepoints, i);
    if (break_len == 0) {
      ++i;
      continue;
    }
    line_start = i + break_len;
    i += break_len;
  }

  return line_start;
}

// Returns the character position immediately before the first logical line
// break. If no break exists, returns codepoints.size().
size_t FindFirstLineBoundary(const std::vector<char32_t>& codepoints) {
  size_t i = 0;

  while (i < codepoints.size()) {
    const size_t break_len = LogicalLineBreakLength(codepoints, i);
    if (break_len != 0) {
      return i;
    }
    ++i;
  }

  return codepoints.size();
}

bool IsSentenceTerminator(const char32_t codepoint) {
  switch (codepoint) {
    case U'。':
    case U'！':
    case U'？':
    case U'!':
    case U'?':
      return true;
    default:
      return false;
  }
}

bool IsSentenceTrailingCloser(const char32_t codepoint) {
  switch (codepoint) {
    case U'」':
    case U'』':
    case U'）':
    case U'］':
    case U'】':
    case U'〉':
    case U'》':
    case U'”':
    case U'’':
    case U')':
    case U']':
    case U'"':
      return true;
    default:
      return false;
  }
}

// Finds the end position of the first sentence, capped by |limit|.
// Repeated terminators and immediately following closing characters are kept.
size_t FindFirstSentenceEnd(const std::vector<char32_t>& codepoints,
                            const size_t limit) {
  for (size_t i = 0; i < limit; ++i) {
    if (!IsSentenceTerminator(codepoints[i])) {
      continue;
    }

    size_t end = i + 1;
    while (end < limit &&
           (IsSentenceTerminator(codepoints[end]) ||
            IsSentenceTrailingCloser(codepoints[end]))) {
      ++end;
    }
    return end;
  }

  return limit;
}

}  // namespace

std::string ZenzContextSelector::SelectLeft(const absl::string_view text,
                                            const size_t max_chars) const {
  if (text.empty() || max_chars == 0) {
    return "";
  }

  const std::vector<char32_t> codepoints = ToCodepoints(text);
  if (codepoints.empty()) {
    return "";
  }

  const size_t line_start = FindCurrentLineStart(codepoints);
  const size_t available_chars = codepoints.size() - line_start;
  if (available_chars == 0) {
    return "";
  }

  const size_t selected_start =
      available_chars <= max_chars ? line_start : codepoints.size() - max_chars;
  const size_t selected_chars = codepoints.size() - selected_start;

  return std::string(Util::Utf8SubString(text, selected_start, selected_chars));
}

std::string ZenzContextSelector::SelectRight(const absl::string_view text,
                                             const size_t max_chars) const {
  if (text.empty() || max_chars == 0) {
    return "";
  }

  const std::vector<char32_t> codepoints = ToCodepoints(text);
  if (codepoints.empty()) {
    return "";
  }

  // Whitespace followed immediately by a line break carries no useful right
  // context and must not expose text from the next logical line.
  size_t leading_blank_end = 0;
  while (leading_blank_end < codepoints.size() &&
         IsHorizontalBlankSpace(codepoints[leading_blank_end])) {
    ++leading_blank_end;
  }
  if (LogicalLineBreakLength(codepoints, leading_blank_end) != 0) {
    return "";
  }

  const size_t line_limit = FindFirstLineBoundary(codepoints);
  const size_t sentence_limit = FindFirstSentenceEnd(codepoints, line_limit);
  const size_t selected_chars = std::min(max_chars, sentence_limit);

  if (selected_chars == 0) {
    return "";
  }

  return std::string(Util::Utf8SubString(text, 0, selected_chars));
}

}  // namespace session
}  // namespace mozc
