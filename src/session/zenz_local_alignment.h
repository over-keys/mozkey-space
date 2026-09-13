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

#ifndef MOZC_SESSION_ZENZ_LOCAL_ALIGNMENT_H_
#define MOZC_SESSION_ZENZ_LOCAL_ALIGNMENT_H_

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "absl/strings/string_view.h"
#include "session/zenz_feedback_store.h"

namespace mozc {
namespace session {

struct ZenzTextAlignedLocalRepairResult {
  std::string value;
  int repaired_count = 0;
  std::vector<ZenzLocalPreference> applied_preferences;
};

struct ZenzAlignedSurfacePair {
  ZenzLocalTextSpan source;
  ZenzLocalTextSpan target;
};

struct ZenzValidatedLocalEdit {
  std::string key;
  std::string disfavored_value;
  std::string preferred_value;
};

struct AppliedLocalFeedbackDecision {
  bool rejected = false;
  std::optional<std::string> validated_third_value;
};

// Applies mature Local rules by bounded Unicode edit-distance alignment between
// the current raw Zenz and Mozc strings. Reverse conversion is not consulted.
// The stored reading is only an eligibility gate and never proves a surface
// position. Each applied preference receives transient raw/Mozc/displayed spans.
ZenzTextAlignedLocalRepairResult ApplyLocalPreferencesByTextAlignment(
    absl::string_view full_key, absl::string_view zenz_value,
    absl::string_view mozc_value,
    const std::vector<ZenzLocalPreference>& preferences,
    size_t max_chars_per_side = 256, size_t max_dp_cells = 70000);

// Returns the unique occurrence pair of source_surface/target_surface that can
// be constrained as one block on an optimal whole-string alignment. Overlapping
// occurrences are considered. Returns nullopt for zero or multiple valid pairs,
// invalid UTF-8, or DP-limit overflow.
std::optional<ZenzAlignedSurfacePair> FindUniqueOptimalSurfacePair(
    absl::string_view source_value, absl::string_view target_value,
    absl::string_view source_surface, absl::string_view target_surface,
    size_t max_chars_per_side = 256, size_t max_dp_cells = 70000);

// Given individually admissible source/target span pairs for the same whole
// string edit, returns which pairs can be retained together. Conflicting pairs
// fail closed while unrelated pairs remain.
std::vector<bool> SelectJointlyOptimalAlignedSurfacePairs(
    absl::string_view source_value, absl::string_view target_value,
    const std::vector<std::optional<ZenzAlignedSurfacePair>>& pairs,
    size_t max_chars_per_side = 256, size_t max_dp_cells = 70000);

// Projects one already-owned source span through a later string transformation.
// A boundary insertion/deletion is ambiguous unless the complete optimal edit
// fixes ownership uniquely.
std::optional<ZenzLocalTextSpan> ProjectUniqueOwnedSpan(
    absl::string_view source_value, absl::string_view target_value,
    const ZenzLocalTextSpan& source_span, size_t max_chars_per_side = 256,
    size_t max_dp_cells = 70000);

std::vector<std::optional<ZenzLocalTextSpan>> ProjectOwnedSpansConsistently(
    absl::string_view source_value, absl::string_view target_value,
    const std::vector<ZenzLocalTextSpan>& source_spans,
    size_t max_chars_per_side = 256, size_t max_dp_cells = 70000);

bool ZenzLocalTextSpansOverlap(const ZenzLocalTextSpan& lhs,
                               const ZenzLocalTextSpan& rhs);

// Classifies edits made after Local repairs were displayed. Explicit restoration
// of the stored raw surface rejects the existing Local before generic ownership
// projection is attempted. A third spelling is returned only when it is also in
// validated_edits, which must come from the existing reading-validated learning
// path. Text alignment alone never creates a new Local rule.
std::vector<AppliedLocalFeedbackDecision> ClassifyAppliedLocalFeedback(
    absl::string_view displayed_value, absl::string_view final_value,
    const std::vector<ZenzLocalPreference>& applied_preferences,
    const std::vector<ZenzValidatedLocalEdit>& validated_edits,
    size_t max_chars_per_side = 256, size_t max_dp_cells = 70000);

}  // namespace session
}  // namespace mozc

#endif  // MOZC_SESSION_ZENZ_LOCAL_ALIGNMENT_H_
