// Copyright 2010-2021, Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "session/zenz_local_alignment.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/string_view.h"
#include "base/strings/unicode.h"

namespace mozc {
namespace session {
namespace {

struct Utf8Text {
  std::u32string chars;
  std::vector<size_t> byte_offsets;
};

std::optional<Utf8Text> DecodeUtf8(absl::string_view text) {
  if (!strings::IsValidUtf8(text)) {
    return std::nullopt;
  }
  Utf8Text result;
  result.chars = strings::Utf8ToUtf32(text);
  result.byte_offsets.reserve(result.chars.size() + 1);
  result.byte_offsets.push_back(0);
  size_t byte_pos = 0;
  while (byte_pos < text.size()) {
    byte_pos += strings::OneCharLen(text[byte_pos]);
    result.byte_offsets.push_back(byte_pos);
  }
  if (result.byte_offsets.size() != result.chars.size() + 1 ||
      result.byte_offsets.back() != text.size()) {
    return std::nullopt;
  }
  return result;
}

bool WithinDpLimits(size_t source_chars, size_t target_chars,
                    size_t max_chars_per_side, size_t max_dp_cells) {
  if (source_chars > max_chars_per_side || target_chars > max_chars_per_side) {
    return false;
  }
  if (source_chars == std::numeric_limits<size_t>::max() ||
      target_chars == std::numeric_limits<size_t>::max()) {
    return false;
  }
  const size_t rows = source_chars + 1;
  const size_t cols = target_chars + 1;
  return cols != 0 && rows <= max_dp_cells / cols;
}

struct EditDistanceTable {
  size_t source_size = 0;
  size_t target_size = 0;
  std::vector<uint16_t> forward;
  std::vector<uint16_t> backward;
  uint16_t total = 0;

  size_t Index(size_t i, size_t j) const {
    return i * (target_size + 1) + j;
  }
  uint16_t Forward(size_t i, size_t j) const {
    return forward[Index(i, j)];
  }
  uint16_t Backward(size_t i, size_t j) const {
    return backward[Index(i, j)];
  }
};

EditDistanceTable BuildEditDistanceTable(const std::u32string& source,
                                         const std::u32string& target) {
  EditDistanceTable table;
  table.source_size = source.size();
  table.target_size = target.size();
  const size_t rows = source.size() + 1;
  const size_t cols = target.size() + 1;
  table.forward.assign(rows * cols, 0);
  table.backward.assign(rows * cols, 0);

  for (size_t i = 0; i <= source.size(); ++i) {
    table.forward[table.Index(i, 0)] = static_cast<uint16_t>(i);
  }
  for (size_t j = 0; j <= target.size(); ++j) {
    table.forward[table.Index(0, j)] = static_cast<uint16_t>(j);
  }
  for (size_t i = 1; i <= source.size(); ++i) {
    for (size_t j = 1; j <= target.size(); ++j) {
      const uint16_t deletion = table.Forward(i - 1, j) + 1;
      const uint16_t insertion = table.Forward(i, j - 1) + 1;
      const uint16_t substitution =
          table.Forward(i - 1, j - 1) +
          static_cast<uint16_t>(source[i - 1] != target[j - 1]);
      table.forward[table.Index(i, j)] =
          std::min({deletion, insertion, substitution});
    }
  }

  for (size_t i = source.size() + 1; i-- > 0;) {
    for (size_t j = target.size() + 1; j-- > 0;) {
      if (i == source.size() && j == target.size()) {
        table.backward[table.Index(i, j)] = 0;
        continue;
      }
      uint16_t best = std::numeric_limits<uint16_t>::max();
      if (i < source.size()) {
        best = std::min<uint16_t>(best, table.Backward(i + 1, j) + 1);
      }
      if (j < target.size()) {
        best = std::min<uint16_t>(best, table.Backward(i, j + 1) + 1);
      }
      if (i < source.size() && j < target.size()) {
        best = std::min<uint16_t>(
            best, table.Backward(i + 1, j + 1) +
                      static_cast<uint16_t>(source[i] != target[j]));
      }
      table.backward[table.Index(i, j)] = best;
    }
  }
  table.total = table.Forward(source.size(), target.size());
  return table;
}

uint16_t EditDistanceRange(const std::u32string& source, size_t source_begin,
                           size_t source_end, const std::u32string& target,
                           size_t target_begin, size_t target_end) {
  const size_t source_size = source_end - source_begin;
  const size_t target_size = target_end - target_begin;
  std::vector<uint16_t> previous(target_size + 1);
  std::vector<uint16_t> current(target_size + 1);
  for (size_t j = 0; j <= target_size; ++j) {
    previous[j] = static_cast<uint16_t>(j);
  }
  for (size_t i = 1; i <= source_size; ++i) {
    current[0] = static_cast<uint16_t>(i);
    for (size_t j = 1; j <= target_size; ++j) {
      const uint16_t deletion = previous[j] + 1;
      const uint16_t insertion = current[j - 1] + 1;
      const uint16_t substitution =
          previous[j - 1] +
          static_cast<uint16_t>(source[source_begin + i - 1] !=
                                target[target_begin + j - 1]);
      current[j] = std::min({deletion, insertion, substitution});
    }
    previous.swap(current);
  }
  return previous[target_size];
}

std::vector<std::pair<size_t, size_t>> FindOccurrences(
    const std::u32string& text, const std::u32string& needle) {
  std::vector<std::pair<size_t, size_t>> result;
  if (needle.empty() || needle.size() > text.size()) {
    return result;
  }
  for (size_t begin = 0; begin + needle.size() <= text.size(); ++begin) {
    if (std::equal(needle.begin(), needle.end(), text.begin() + begin)) {
      result.push_back({begin, begin + needle.size()});
    }
  }
  return result;
}

ZenzLocalTextSpan MakeSpan(const Utf8Text& text, size_t begin, size_t end) {
  return {begin, end, text.byte_offsets[begin], text.byte_offsets[end]};
}

bool CandidateCostMatches(const EditDistanceTable& table, size_t source_begin,
                          size_t source_end, size_t target_begin,
                          size_t target_end, uint16_t local_cost) {
  const uint32_t total =
      static_cast<uint32_t>(table.Forward(source_begin, target_begin)) +
      local_cost + table.Backward(source_end, target_end);
  return total == table.total;
}

struct Candidate {
  ZenzLocalPreference preference;
  size_t zenz_begin = 0;
  size_t zenz_end = 0;
  size_t mozc_begin = 0;
  size_t mozc_end = 0;
  uint16_t local_cost = 0;
  bool conflict = false;
};

bool CandidatesOverlap(const Candidate& lhs, const Candidate& rhs) {
  return (lhs.zenz_begin < rhs.zenz_end && rhs.zenz_begin < lhs.zenz_end) ||
         (lhs.mozc_begin < rhs.mozc_end && rhs.mozc_begin < lhs.mozc_end);
}

bool CandidatesCanCoexist(const Utf8Text& zenz, const Utf8Text& mozc,
                          const EditDistanceTable& table,
                          const Candidate& first_candidate,
                          const Candidate& second_candidate) {
  const Candidate* first = &first_candidate;
  const Candidate* second = &second_candidate;
  if (second->zenz_begin < first->zenz_begin) {
    std::swap(first, second);
  }
  if (first->zenz_end > second->zenz_begin ||
      first->mozc_end > second->mozc_begin) {
    return false;
  }
  const uint16_t gap_cost =
      EditDistanceRange(zenz.chars, first->zenz_end, second->zenz_begin,
                        mozc.chars, first->mozc_end, second->mozc_begin);
  const uint32_t total =
      static_cast<uint32_t>(
          table.Forward(first->zenz_begin, first->mozc_begin)) +
      first->local_cost + gap_cost + second->local_cost +
      table.Backward(second->zenz_end, second->mozc_end);
  return total == table.total;
}

bool CandidateSetCanCoexist(const Utf8Text& zenz, const Utf8Text& mozc,
                            const EditDistanceTable& table,
                            std::vector<Candidate*> candidates) {
  if (candidates.empty()) {
    return true;
  }
  std::sort(candidates.begin(), candidates.end(),
            [](const Candidate* lhs, const Candidate* rhs) {
              return lhs->zenz_begin < rhs->zenz_begin;
            });
  uint32_t total =
      table.Forward(candidates.front()->zenz_begin,
                    candidates.front()->mozc_begin) +
      candidates.front()->local_cost;
  for (size_t i = 1; i < candidates.size(); ++i) {
    const Candidate& previous = *candidates[i - 1];
    const Candidate& current = *candidates[i];
    if (previous.zenz_end > current.zenz_begin ||
        previous.mozc_end > current.mozc_begin) {
      return false;
    }
    total += EditDistanceRange(zenz.chars, previous.zenz_end,
                               current.zenz_begin, mozc.chars,
                               previous.mozc_end, current.mozc_begin);
    total += current.local_cost;
  }
  total += table.Backward(candidates.back()->zenz_end,
                          candidates.back()->mozc_end);
  return total == table.total;
}

std::optional<ZenzAlignedSurfacePair> FindUniqueOptimalSurfacePairDecoded(
    const Utf8Text& source, const Utf8Text& target,
    const std::u32string& source_surface, const std::u32string& target_surface,
    const EditDistanceTable& table) {
  const auto source_occurrences = FindOccurrences(source.chars, source_surface);
  const auto target_occurrences = FindOccurrences(target.chars, target_surface);
  if (source_occurrences.empty() || target_occurrences.empty()) {
    return std::nullopt;
  }
  const uint16_t local_cost =
      EditDistanceRange(source_surface, 0, source_surface.size(), target_surface,
                        0, target_surface.size());
  std::optional<ZenzAlignedSurfacePair> unique;
  for (const auto& [source_begin, source_end] : source_occurrences) {
    for (const auto& [target_begin, target_end] : target_occurrences) {
      if (!CandidateCostMatches(table, source_begin, source_end, target_begin,
                                target_end, local_cost)) {
        continue;
      }
      if (unique.has_value()) {
        return std::nullopt;
      }
      unique = ZenzAlignedSurfacePair{MakeSpan(source, source_begin, source_end),
                                      MakeSpan(target, target_begin, target_end)};
    }
  }
  return unique;
}


std::optional<ZenzLocalTextSpan> ProjectUniqueOwnedSpanDecoded(
    const Utf8Text& source, const Utf8Text& target,
    const ZenzLocalTextSpan& source_span, const EditDistanceTable& table) {
  if (source_span.char_begin > source_span.char_end ||
      source_span.char_end > source.chars.size()) {
    return std::nullopt;
  }
  if (source.chars == target.chars) {
    return MakeSpan(target, source_span.char_begin, source_span.char_end);
  }

  const std::u32string owned_surface(
      source.chars.begin() + source_span.char_begin,
      source.chars.begin() + source_span.char_end);
  if (owned_surface.empty()) {
    return std::nullopt;
  }

  // Keep ownership only when exactly one target span, including an empty
  // deletion span, is compatible with an optimal whole-string alignment. Do
  // not prefer a shorter exact occurrence over an equally optimal expanded
  // span. An insertion at an ownership boundary can belong either inside or
  // outside that ownership; without an edit position from the caller, that is
  // genuinely ambiguous and must fail closed.
  const size_t owned_chars = source_span.char_end - source_span.char_begin;
  std::optional<ZenzLocalTextSpan> unique;
  for (size_t target_begin = 0; target_begin <= target.chars.size();
       ++target_begin) {
    const size_t suffix_chars = target.chars.size() - target_begin;
    std::vector<uint16_t> previous(suffix_chars + 1);
    std::vector<uint16_t> current(suffix_chars + 1);
    for (size_t j = 0; j <= suffix_chars; ++j) {
      previous[j] = static_cast<uint16_t>(j);
    }
    for (size_t i = 1; i <= owned_chars; ++i) {
      current[0] = static_cast<uint16_t>(i);
      for (size_t j = 1; j <= suffix_chars; ++j) {
        const uint16_t deletion = previous[j] + 1;
        const uint16_t insertion = current[j - 1] + 1;
        const uint16_t substitution =
            previous[j - 1] +
            static_cast<uint16_t>(
                source.chars[source_span.char_begin + i - 1] !=
                target.chars[target_begin + j - 1]);
        current[j] = std::min({deletion, insertion, substitution});
      }
      previous.swap(current);
    }
    for (size_t suffix_end = 0; suffix_end <= suffix_chars; ++suffix_end) {
      const size_t target_end = target_begin + suffix_end;
      if (!CandidateCostMatches(table, source_span.char_begin,
                                source_span.char_end, target_begin, target_end,
                                previous[suffix_end])) {
        continue;
      }
      if (unique.has_value()) {
        return std::nullopt;
      }
      unique = MakeSpan(target, target_begin, target_end);
    }
  }
  return unique;
}

}  // namespace

bool ZenzLocalTextSpansOverlap(const ZenzLocalTextSpan& lhs,
                                 const ZenzLocalTextSpan& rhs) {
  return lhs.char_begin < rhs.char_end && rhs.char_begin < lhs.char_end;
}

std::optional<ZenzAlignedSurfacePair> FindUniqueOptimalSurfacePair(
    absl::string_view source_value, absl::string_view target_value,
    absl::string_view source_surface, absl::string_view target_surface,
    size_t max_chars_per_side, size_t max_dp_cells) {
  const auto source = DecodeUtf8(source_value);
  const auto target = DecodeUtf8(target_value);
  const auto source_surface_text = DecodeUtf8(source_surface);
  const auto target_surface_text = DecodeUtf8(target_surface);
  if (!source.has_value() || !target.has_value() ||
      !source_surface_text.has_value() || !target_surface_text.has_value() ||
      source_surface_text->chars.empty() || target_surface_text->chars.empty() ||
      !WithinDpLimits(source->chars.size(), target->chars.size(),
                      max_chars_per_side, max_dp_cells)) {
    return std::nullopt;
  }
  const EditDistanceTable table =
      BuildEditDistanceTable(source->chars, target->chars);
  return FindUniqueOptimalSurfacePairDecoded(
      *source, *target, source_surface_text->chars, target_surface_text->chars,
      table);
}

ZenzTextAlignedLocalRepairResult ApplyLocalPreferencesByTextAlignment(
    absl::string_view full_key, absl::string_view zenz_value,
    absl::string_view mozc_value,
    const std::vector<ZenzLocalPreference>& preferences,
    size_t max_chars_per_side, size_t max_dp_cells) {
  ZenzTextAlignedLocalRepairResult result;
  result.value = std::string(zenz_value);
  if (full_key.empty() || zenz_value.empty() || mozc_value.empty() ||
      preferences.empty()) {
    return result;
  }

  const auto key = DecodeUtf8(full_key);
  const auto zenz = DecodeUtf8(zenz_value);
  const auto mozc = DecodeUtf8(mozc_value);
  if (!key.has_value() || !zenz.has_value() || !mozc.has_value() ||
      !WithinDpLimits(zenz->chars.size(), mozc->chars.size(),
                      max_chars_per_side, max_dp_cells)) {
    return result;
  }
  const EditDistanceTable table =
      BuildEditDistanceTable(zenz->chars, mozc->chars);

  std::vector<Candidate> candidates;
  candidates.reserve(preferences.size());
  for (const ZenzLocalPreference& preference : preferences) {
    const auto key_surface = DecodeUtf8(preference.key);
    const auto raw_surface = DecodeUtf8(preference.disfavored_value);
    const auto preferred_surface = DecodeUtf8(preference.preferred_value);
    if (!key_surface.has_value() || !raw_surface.has_value() ||
        !preferred_surface.has_value() || key_surface->chars.empty() ||
        raw_surface->chars.empty() || preferred_surface->chars.empty() ||
        raw_surface->chars == preferred_surface->chars) {
      continue;
    }

    // Eligibility only. Never use this reading occurrence as a surface anchor.
    if (FindOccurrences(key->chars, key_surface->chars).size() != 1) {
      continue;
    }

    const auto raw_occurrences =
        FindOccurrences(zenz->chars, raw_surface->chars);
    const auto preferred_occurrences =
        FindOccurrences(mozc->chars, preferred_surface->chars);
    if (raw_occurrences.empty() || preferred_occurrences.empty()) {
      continue;
    }
    const uint16_t local_cost =
        EditDistanceRange(raw_surface->chars, 0, raw_surface->chars.size(),
                          preferred_surface->chars, 0,
                          preferred_surface->chars.size());

    std::optional<Candidate> unique;
    bool ambiguous = false;
    for (const auto& [zenz_begin, zenz_end] : raw_occurrences) {
      for (const auto& [mozc_begin, mozc_end] : preferred_occurrences) {
        if (!CandidateCostMatches(table, zenz_begin, zenz_end, mozc_begin,
                                  mozc_end, local_cost)) {
          continue;
        }
        if (unique.has_value()) {
          ambiguous = true;
          break;
        }
        unique = Candidate{preference, zenz_begin, zenz_end, mozc_begin,
                           mozc_end, local_cost, false};
      }
      if (ambiguous) {
        break;
      }
    }
    if (!ambiguous && unique.has_value()) {
      candidates.push_back(std::move(*unique));
    }
  }

  // Reject every candidate participating in a conflict. Do not search for a
  // maximum compatible subset: ambiguity is intentionally fail-closed.
  for (size_t i = 0; i < candidates.size(); ++i) {
    for (size_t j = i + 1; j < candidates.size(); ++j) {
      Candidate& lhs = candidates[i];
      Candidate& rhs = candidates[j];
      const bool overlap = CandidatesOverlap(lhs, rhs);
      const bool order_reversed =
          (lhs.zenz_begin < rhs.zenz_begin && lhs.mozc_begin > rhs.mozc_begin) ||
          (rhs.zenz_begin < lhs.zenz_begin && rhs.mozc_begin > lhs.mozc_begin);
      if (overlap || order_reversed ||
          !CandidatesCanCoexist(*zenz, *mozc, table, lhs, rhs)) {
        lhs.conflict = true;
        rhs.conflict = true;
      }
    }
  }

  std::vector<Candidate*> survivors;
  for (Candidate& candidate : candidates) {
    if (!candidate.conflict) {
      survivors.push_back(&candidate);
    }
  }
  if (survivors.empty() ||
      !CandidateSetCanCoexist(*zenz, *mozc, table, survivors)) {
    return result;
  }
  std::sort(survivors.begin(), survivors.end(),
            [](const Candidate* lhs, const Candidate* rhs) {
              return lhs->zenz_begin < rhs->zenz_begin;
            });

  std::string repaired;
  repaired.reserve(zenz_value.size());
  size_t previous_char = 0;
  size_t repaired_chars = 0;
  for (Candidate* candidate : survivors) {
    const size_t gap_byte_begin = zenz->byte_offsets[previous_char];
    const size_t gap_byte_end = zenz->byte_offsets[candidate->zenz_begin];
    repaired.append(zenz_value.substr(gap_byte_begin,
                                      gap_byte_end - gap_byte_begin));
    repaired_chars += candidate->zenz_begin - previous_char;

    ZenzLocalPreference applied = candidate->preference;
    // The reading occurrence is unique by the eligibility gate. Keep it only
    // as transient provenance for compatibility; it is never used to infer the
    // text span below.
    const auto key_surface = DecodeUtf8(applied.key);
    if (!key_surface.has_value()) {
      return ZenzTextAlignedLocalRepairResult{std::string(zenz_value), 0, {}};
    }
    const auto key_occurrences = FindOccurrences(key->chars, key_surface->chars);
    if (key_occurrences.size() != 1) {
      return ZenzTextAlignedLocalRepairResult{std::string(zenz_value), 0, {}};
    }
    applied.reading_begin =
        key->byte_offsets[key_occurrences[0].first];
    applied.has_reading_begin = true;
    applied.raw_zenz_span =
        MakeSpan(*zenz, candidate->zenz_begin, candidate->zenz_end);
    applied.mozc_span =
        MakeSpan(*mozc, candidate->mozc_begin, candidate->mozc_end);
    applied.displayed_span.char_begin = repaired_chars;
    applied.displayed_span.byte_begin = repaired.size();

    repaired.append(candidate->preference.preferred_value);
    const auto preferred_surface = DecodeUtf8(candidate->preference.preferred_value);
    if (!preferred_surface.has_value()) {
      return ZenzTextAlignedLocalRepairResult{std::string(zenz_value), 0, {}};
    }
    repaired_chars += preferred_surface->chars.size();
    applied.displayed_span.char_end = repaired_chars;
    applied.displayed_span.byte_end = repaired.size();
    applied.has_text_spans = true;
    result.applied_preferences.push_back(std::move(applied));
    previous_char = candidate->zenz_end;
  }
  repaired.append(zenz_value.substr(zenz->byte_offsets[previous_char]));
  result.value = std::move(repaired);
  result.repaired_count = static_cast<int>(result.applied_preferences.size());
  return result;
}

std::vector<bool> SelectJointlyOptimalAlignedSurfacePairs(
    absl::string_view source_value, absl::string_view target_value,
    const std::vector<std::optional<ZenzAlignedSurfacePair>>& pairs,
    size_t max_chars_per_side, size_t max_dp_cells) {
  std::vector<bool> result(pairs.size(), false);
  if (pairs.empty()) {
    return result;
  }

  const auto source = DecodeUtf8(source_value);
  const auto target = DecodeUtf8(target_value);
  if (!source.has_value() || !target.has_value() ||
      !WithinDpLimits(source->chars.size(), target->chars.size(),
                      max_chars_per_side, max_dp_cells)) {
    return result;
  }
  const EditDistanceTable table =
      BuildEditDistanceTable(source->chars, target->chars);

  std::vector<Candidate> candidates;
  std::vector<size_t> candidate_indices;
  candidates.reserve(pairs.size());
  candidate_indices.reserve(pairs.size());
  for (size_t i = 0; i < pairs.size(); ++i) {
    if (!pairs[i].has_value()) {
      continue;
    }
    const ZenzLocalTextSpan& source_span = pairs[i]->source;
    const ZenzLocalTextSpan& target_span = pairs[i]->target;
    if (source_span.char_begin > source_span.char_end ||
        source_span.char_end > source->chars.size() ||
        target_span.char_begin > target_span.char_end ||
        target_span.char_end > target->chars.size()) {
      continue;
    }
    const uint16_t local_cost = EditDistanceRange(
        source->chars, source_span.char_begin, source_span.char_end,
        target->chars, target_span.char_begin, target_span.char_end);
    if (!CandidateCostMatches(table, source_span.char_begin,
                              source_span.char_end, target_span.char_begin,
                              target_span.char_end, local_cost)) {
      continue;
    }
    candidates.push_back(Candidate{ZenzLocalPreference(),
                                   source_span.char_begin, source_span.char_end,
                                   target_span.char_begin, target_span.char_end,
                                   local_cost, false});
    candidate_indices.push_back(i);
  }

  for (size_t i = 0; i < candidates.size(); ++i) {
    for (size_t j = i + 1; j < candidates.size(); ++j) {
      Candidate& lhs = candidates[i];
      Candidate& rhs = candidates[j];
      const bool order_reversed =
          (lhs.zenz_begin < rhs.zenz_begin && lhs.mozc_begin > rhs.mozc_begin) ||
          (rhs.zenz_begin < lhs.zenz_begin && rhs.mozc_begin > lhs.mozc_begin);
      if (CandidatesOverlap(lhs, rhs) || order_reversed ||
          !CandidatesCanCoexist(*source, *target, table, lhs, rhs)) {
        lhs.conflict = true;
        rhs.conflict = true;
      }
    }
  }

  std::vector<Candidate*> survivors;
  std::vector<size_t> survivor_indices;
  for (size_t i = 0; i < candidates.size(); ++i) {
    if (!candidates[i].conflict) {
      survivors.push_back(&candidates[i]);
      survivor_indices.push_back(candidate_indices[i]);
    }
  }
  if (survivors.empty() ||
      !CandidateSetCanCoexist(*source, *target, table, survivors)) {
    return result;
  }
  for (const size_t index : survivor_indices) {
    result[index] = true;
  }
  return result;
}

std::optional<ZenzLocalTextSpan> ProjectUniqueOwnedSpan(
    absl::string_view source_value, absl::string_view target_value,
    const ZenzLocalTextSpan& source_span, size_t max_chars_per_side,
    size_t max_dp_cells) {
  const auto source = DecodeUtf8(source_value);
  const auto target = DecodeUtf8(target_value);
  if (!source.has_value() || !target.has_value() ||
      !WithinDpLimits(source->chars.size(), target->chars.size(),
                      max_chars_per_side, max_dp_cells)) {
    return std::nullopt;
  }
  const EditDistanceTable table =
      BuildEditDistanceTable(source->chars, target->chars);
  return ProjectUniqueOwnedSpanDecoded(*source, *target, source_span, table);
}

std::vector<std::optional<ZenzLocalTextSpan>> ProjectOwnedSpansConsistently(
    absl::string_view source_value, absl::string_view target_value,
    const std::vector<ZenzLocalTextSpan>& source_spans,
    size_t max_chars_per_side, size_t max_dp_cells) {
  std::vector<std::optional<ZenzLocalTextSpan>> result(source_spans.size());
  if (source_spans.empty()) {
    return result;
  }

  const auto source = DecodeUtf8(source_value);
  const auto target = DecodeUtf8(target_value);
  if (!source.has_value() || !target.has_value() ||
      !WithinDpLimits(source->chars.size(), target->chars.size(),
                      max_chars_per_side, max_dp_cells)) {
    return result;
  }
  const EditDistanceTable table =
      BuildEditDistanceTable(source->chars, target->chars);

  std::vector<Candidate> candidates;
  std::vector<size_t> candidate_indices;
  candidates.reserve(source_spans.size());
  candidate_indices.reserve(source_spans.size());
  for (size_t i = 0; i < source_spans.size(); ++i) {
    const ZenzLocalTextSpan& source_span = source_spans[i];
    const std::optional<ZenzLocalTextSpan> target_span =
        ProjectUniqueOwnedSpanDecoded(*source, *target, source_span, table);
    if (!target_span.has_value()) {
      continue;
    }
    const uint16_t local_cost = EditDistanceRange(
        source->chars, source_span.char_begin, source_span.char_end,
        target->chars, target_span->char_begin, target_span->char_end);
    candidates.push_back(Candidate{ZenzLocalPreference(),
                                   source_span.char_begin, source_span.char_end,
                                   target_span->char_begin, target_span->char_end,
                                   local_cost, false});
    candidate_indices.push_back(i);
  }

  // Individual uniqueness is insufficient: two uniquely projected spans may
  // cross or may belong to different optimal alignments. Invalidate every span
  // participating in such a conflict, while retaining unrelated ownership.
  for (size_t i = 0; i < candidates.size(); ++i) {
    for (size_t j = i + 1; j < candidates.size(); ++j) {
      Candidate& lhs = candidates[i];
      Candidate& rhs = candidates[j];
      const bool order_reversed =
          (lhs.zenz_begin < rhs.zenz_begin && lhs.mozc_begin > rhs.mozc_begin) ||
          (rhs.zenz_begin < lhs.zenz_begin && rhs.mozc_begin > lhs.mozc_begin);
      if (CandidatesOverlap(lhs, rhs) || order_reversed ||
          !CandidatesCanCoexist(*source, *target, table, lhs, rhs)) {
        lhs.conflict = true;
        rhs.conflict = true;
      }
    }
  }

  std::vector<Candidate*> survivors;
  std::vector<size_t> survivor_indices;
  for (size_t i = 0; i < candidates.size(); ++i) {
    if (!candidates[i].conflict) {
      survivors.push_back(&candidates[i]);
      survivor_indices.push_back(candidate_indices[i]);
    }
  }
  if (survivors.empty()) {
    return result;
  }
  if (!CandidateSetCanCoexist(*source, *target, table, survivors)) {
    // A higher-order inconsistency should be extraordinarily rare after the
    // pairwise checks. Do not guess which owner is responsible.
    return result;
  }

  for (size_t i = 0; i < survivors.size(); ++i) {
    result[survivor_indices[i]] =
        MakeSpan(*target, survivors[i]->mozc_begin, survivors[i]->mozc_end);
  }
  return result;
}


std::vector<AppliedLocalFeedbackDecision> ClassifyAppliedLocalFeedback(
    absl::string_view displayed_value, absl::string_view final_value,
    const std::vector<ZenzLocalPreference>& applied_preferences,
    const std::vector<ZenzValidatedLocalEdit>& validated_edits,
    size_t max_chars_per_side, size_t max_dp_cells) {
  std::vector<AppliedLocalFeedbackDecision> decisions(applied_preferences.size());
  if (applied_preferences.empty() || displayed_value.empty() || final_value.empty() ||
      displayed_value == final_value) {
    return decisions;
  }

  enum class ExplicitEditKind { kNone, kRawRevert, kValidatedThird };
  struct ExplicitEdit {
    ExplicitEditKind kind = ExplicitEditKind::kNone;
    std::optional<ZenzAlignedSurfacePair> pair;
    std::string third_value;
  };
  std::vector<ExplicitEdit> explicit_edits(applied_preferences.size());
  std::vector<std::optional<ZenzAlignedSurfacePair>> explicit_pairs(
      applied_preferences.size());

  auto same_source_span = [](const ZenzLocalTextSpan& lhs,
                             const ZenzLocalTextSpan& rhs) {
    return lhs.char_begin == rhs.char_begin && lhs.char_end == rhs.char_end &&
           lhs.byte_begin == rhs.byte_begin && lhs.byte_end == rhs.byte_end;
  };

  for (size_t i = 0; i < applied_preferences.size(); ++i) {
    const ZenzLocalPreference& preference = applied_preferences[i];
    if (!preference.has_text_spans) {
      continue;
    }

    // Raw restoration is semantic rejection of the existing Local, not a new
    // Local-learning event. Test it before generic ownership projection so an
    // insertion exactly at the old preferred boundary (取扱 -> 取扱い) cannot
    // be mistaken for preservation of the shorter surface.
    const auto raw_revert = FindUniqueOptimalSurfacePair(
        displayed_value, final_value, preference.preferred_value,
        preference.disfavored_value, max_chars_per_side, max_dp_cells);
    if (raw_revert.has_value() &&
        same_source_span(raw_revert->source, preference.displayed_span)) {
      explicit_edits[i].kind = ExplicitEditKind::kRawRevert;
      explicit_edits[i].pair = raw_revert;
      explicit_pairs[i] = raw_revert;
      continue;
    }

    // A third spelling may become a new rule only when the unchanged learning
    // pipeline has already validated its reading. Text alignment supplies only
    // attribution to this displayed Local span; it never supplies reading
    // evidence by itself.
    std::optional<ZenzAlignedSurfacePair> third_pair;
    std::string third_value;
    bool third_ambiguous = false;
    for (const ZenzValidatedLocalEdit& edit : validated_edits) {
      if (edit.key != preference.key ||
          edit.disfavored_value != preference.preferred_value ||
          edit.preferred_value.empty() ||
          edit.preferred_value == preference.preferred_value ||
          edit.preferred_value == preference.disfavored_value) {
        continue;
      }
      const auto pair = FindUniqueOptimalSurfacePair(
          displayed_value, final_value, edit.disfavored_value,
          edit.preferred_value, max_chars_per_side, max_dp_cells);
      if (!pair.has_value() ||
          !same_source_span(pair->source, preference.displayed_span)) {
        continue;
      }
      if (third_pair.has_value()) {
        third_ambiguous = true;
        break;
      }
      third_pair = pair;
      third_value = edit.preferred_value;
    }
    if (!third_ambiguous && third_pair.has_value()) {
      explicit_edits[i].kind = ExplicitEditKind::kValidatedThird;
      explicit_edits[i].pair = third_pair;
      explicit_edits[i].third_value = std::move(third_value);
      explicit_pairs[i] = third_pair;
    }
  }

  const std::vector<bool> keep_explicit =
      SelectJointlyOptimalAlignedSurfacePairs(
          displayed_value, final_value, explicit_pairs, max_chars_per_side,
          max_dp_cells);

  std::vector<size_t> generic_indices;
  std::vector<ZenzLocalTextSpan> generic_spans;
  generic_indices.reserve(applied_preferences.size());
  generic_spans.reserve(applied_preferences.size());
  for (size_t i = 0; i < applied_preferences.size(); ++i) {
    if (i < keep_explicit.size() && keep_explicit[i] &&
        explicit_edits[i].kind != ExplicitEditKind::kNone) {
      decisions[i].rejected = true;
      if (explicit_edits[i].kind == ExplicitEditKind::kValidatedThird) {
        decisions[i].validated_third_value = explicit_edits[i].third_value;
      }
      continue;
    }
    if (!applied_preferences[i].has_text_spans) {
      continue;
    }
    generic_indices.push_back(i);
    generic_spans.push_back(applied_preferences[i].displayed_span);
  }

  const auto projected = ProjectOwnedSpansConsistently(
      displayed_value, final_value, generic_spans, max_chars_per_side,
      max_dp_cells);
  for (size_t j = 0; j < generic_indices.size(); ++j) {
    if (j >= projected.size() || !projected[j].has_value() ||
        projected[j]->byte_end > final_value.size()) {
      continue;
    }
    const size_t i = generic_indices[j];
    const ZenzLocalTextSpan& span = *projected[j];
    const absl::string_view final_surface = final_value.substr(
        span.byte_begin, span.byte_end - span.byte_begin);
    if (final_surface != applied_preferences[i].preferred_value) {
      decisions[i].rejected = true;
    }
  }
  return decisions;
}

}  // namespace session
}  // namespace mozc
