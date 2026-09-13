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

#include <optional>
#include <string>
#include <vector>

#include "testing/gunit.h"

namespace mozc {
namespace session {
namespace {

ZenzLocalPreference Preference(absl::string_view key,
                               absl::string_view raw,
                               absl::string_view preferred) {
  ZenzLocalPreference result;
  result.key = std::string(key);
  result.disfavored_value = std::string(raw);
  result.preferred_value = std::string(preferred);
  result.observation_count = 2;
  return result;
}

TEST(ZenzLocalAlignmentTest, KeepsUnregisteredZenzDifference) {
  const ZenzTextAlignedLocalRepairResult result = ApplyLocalPreferencesByTextAlignment(
      "けいりょうかのよちがある", "計量化の余地がある",
      "軽量化の余地が有る",
      {Preference("けいりょうか", "計量化", "軽量化")});

  EXPECT_EQ(result.value, "軽量化の余地がある");
  ASSERT_EQ(result.applied_preferences.size(), 1);
  EXPECT_EQ(result.applied_preferences[0].raw_zenz_span.char_begin, 0);
  EXPECT_EQ(result.applied_preferences[0].raw_zenz_span.char_end, 3);
  EXPECT_EQ(result.applied_preferences[0].mozc_span.char_begin, 0);
  EXPECT_EQ(result.applied_preferences[0].mozc_span.char_end, 3);
  EXPECT_TRUE(result.applied_preferences[0].has_reading_begin);
}

TEST(ZenzLocalAlignmentTest, AppliesMultipleIndependentLocals) {
  const ZenzTextAlignedLocalRepairResult result = ApplyLocalPreferencesByTextAlignment(
      "けいりょうかについてしりょうをまとめる",
      "計量化について史料をまとめる",
      "軽量化について資料を纏める",
      {Preference("けいりょうか", "計量化", "軽量化"),
       Preference("しりょう", "史料", "資料")});

  EXPECT_EQ(result.value, "軽量化について資料をまとめる");
  EXPECT_EQ(result.applied_preferences.size(), 2);
}

TEST(ZenzLocalAlignmentTest, TracksSpansWhenReplacementLengthChanges) {
  const ZenzTextAlignedLocalRepairResult result = ApplyLocalPreferencesByTextAlignment(
      "とりあつかいです", "取扱いです", "取扱です",
      {Preference("とりあつかい", "取扱い", "取扱")});

  EXPECT_EQ(result.value, "取扱です");
  ASSERT_EQ(result.applied_preferences.size(), 1);
  EXPECT_EQ(result.applied_preferences[0].raw_zenz_span.char_begin, 0);
  EXPECT_EQ(result.applied_preferences[0].raw_zenz_span.char_end, 3);
  EXPECT_EQ(result.applied_preferences[0].mozc_span.char_begin, 0);
  EXPECT_EQ(result.applied_preferences[0].mozc_span.char_end, 2);
  EXPECT_EQ(result.applied_preferences[0].displayed_span.char_begin, 0);
  EXPECT_EQ(result.applied_preferences[0].displayed_span.char_end, 2);
}

TEST(ZenzLocalAlignmentTest, RejectsAmbiguousOccurrencePair) {
  const ZenzTextAlignedLocalRepairResult result = ApplyLocalPreferencesByTextAlignment(
      "x", "abba", "baab", {Preference("x", "b", "a")});

  EXPECT_EQ(result.value, "abba");
  EXPECT_TRUE(result.applied_preferences.empty());
}

TEST(ZenzLocalAlignmentTest, RejectsCandidatesThatCannotCoexist) {
  const ZenzTextAlignedLocalRepairResult result = ApplyLocalPreferencesByTextAlignment(
      "xy", "aaba", "baac",
      {Preference("x", "a", "b"), Preference("y", "b", "c")});

  EXPECT_EQ(result.value, "aaba");
  EXPECT_TRUE(result.applied_preferences.empty());
}

TEST(ZenzLocalAlignmentTest, KeepsUnrelatedCandidateWhenOthersConflict) {
  const ZenzTextAlignedLocalRepairResult result = ApplyLocalPreferencesByTextAlignment(
      "xyz", "aabaq", "baacr",
      {Preference("x", "a", "b"), Preference("y", "b", "c"),
       Preference("z", "q", "r")});

  EXPECT_EQ(result.value, "aabar");
  ASSERT_EQ(result.applied_preferences.size(), 1);
  EXPECT_EQ(result.applied_preferences[0].key, "z");
}

TEST(ZenzLocalAlignmentTest, RejectsCrossingCandidateOrder) {
  const ZenzTextAlignedLocalRepairResult result = ApplyLocalPreferencesByTextAlignment(
      "xy", "abbc", "bacb",
      {Preference("x", "b", "a"), Preference("y", "b", "c")});

  EXPECT_EQ(result.value, "abbc");
  EXPECT_TRUE(result.applied_preferences.empty());
}

TEST(ZenzLocalAlignmentTest, ReadingMultiplicityIsEligibilityOnly) {
  const ZenzTextAlignedLocalRepairResult result = ApplyLocalPreferencesByTextAlignment(
      "xx", "計量化", "軽量化", {Preference("x", "計量化", "軽量化")});

  EXPECT_EQ(result.value, "計量化");
  EXPECT_TRUE(result.applied_preferences.empty());
}


TEST(ZenzLocalAlignmentTest, AllowsAdjacentLocals) {
  const ZenzTextAlignedLocalRepairResult result = ApplyLocalPreferencesByTextAlignment(
      "xy", "ab", "cd",
      {Preference("x", "a", "c"), Preference("y", "b", "d")});

  EXPECT_EQ(result.value, "cd");
  EXPECT_EQ(result.applied_preferences.size(), 2);
}

TEST(ZenzLocalAlignmentTest, OverlappingOccurrencesRemainAmbiguous) {
  const auto pair = FindUniqueOptimalSurfacePair("aaa", "aaa", "aa", "aa");
  EXPECT_FALSE(pair.has_value());
}

TEST(ZenzLocalAlignmentTest, DpLimitIncludesBoundaryCells) {
  const std::string key(256, 'x');
  const std::string zenz(256, 'a');
  const std::string mozc(256, 'b');
  const ZenzTextAlignedLocalRepairResult result = ApplyLocalPreferencesByTextAlignment(
      key, zenz, mozc, {Preference(key, zenz, mozc)});

  EXPECT_EQ(result.value, mozc);
  EXPECT_EQ(result.applied_preferences.size(), 1);
}

TEST(ZenzLocalAlignmentTest, DpLimitFailsClosed) {
  const std::string key(257, 'x');
  const std::string zenz(257, 'a');
  const std::string mozc(257, 'b');
  const ZenzTextAlignedLocalRepairResult result = ApplyLocalPreferencesByTextAlignment(
      key, zenz, mozc, {Preference(key, zenz, mozc)});

  EXPECT_EQ(result.value, zenz);
  EXPECT_TRUE(result.applied_preferences.empty());
}

TEST(ZenzLocalAlignmentTest, ProjectsOwnedSpanAcrossUnrelatedEdit) {
  const auto projected = ProjectUniqueOwnedSpan(
      "軽量化の余地がある", "軽量化の余地が有る",
      ZenzLocalTextSpan{0, 3, 0, 9});

  ASSERT_TRUE(projected.has_value());
  EXPECT_EQ(projected->char_begin, 0);
  EXPECT_EQ(projected->char_end, 3);
}

TEST(ZenzLocalAlignmentTest, ProjectsChangedOwnedSpanWhenUnique) {
  const auto projected = ProjectUniqueOwnedSpan(
      "軽量化の余地", "計量化の余地", ZenzLocalTextSpan{0, 3, 0, 9});

  ASSERT_TRUE(projected.has_value());
  EXPECT_EQ(projected->char_begin, 0);
  EXPECT_EQ(projected->char_end, 3);
}

TEST(ZenzLocalAlignmentTest, InvalidatesAmbiguousOwnership) {
  const auto projected = ProjectUniqueOwnedSpan(
      "a", "aa", ZenzLocalTextSpan{0, 1, 0, 1});

  EXPECT_FALSE(projected.has_value());
}


TEST(ZenzLocalAlignmentTest, InvalidatesCrossingOwnedSpanProjections) {
  const std::vector<std::optional<ZenzLocalTextSpan>> projected =
      ProjectOwnedSpansConsistently(
          "ab", "ba", {ZenzLocalTextSpan{0, 1, 0, 1},
                       ZenzLocalTextSpan{1, 2, 1, 2}});

  ASSERT_EQ(projected.size(), 2);
  EXPECT_FALSE(projected[0].has_value());
  EXPECT_FALSE(projected[1].has_value());
}

TEST(ZenzLocalAlignmentTest, OwnershipConflictsDoNotDropUnrelatedSpan) {
  // The unchanged separator keeps the last owner away from the ambiguous edit
  // boundary. Without it, an optimal path may map q to aq rather than q.
  const std::vector<std::optional<ZenzLocalTextSpan>> projected =
      ProjectOwnedSpansConsistently(
          "ab##q", "ba##q", {ZenzLocalTextSpan{0, 1, 0, 1},
                         ZenzLocalTextSpan{1, 2, 1, 2},
                         ZenzLocalTextSpan{4, 5, 4, 5}});

  ASSERT_EQ(projected.size(), 3);
  EXPECT_FALSE(projected[0].has_value());
  EXPECT_FALSE(projected[1].has_value());
  ASSERT_TRUE(projected[2].has_value());
  EXPECT_EQ(projected[2]->char_begin, 4);
  EXPECT_EQ(projected[2]->char_end, 5);
}

TEST(ZenzLocalAlignmentTest, UnchangedSuffixCanHaveAmbiguousOwnership) {
  const auto projected = ProjectUniqueOwnedSpan(
      "abq", "baq", ZenzLocalTextSpan{2, 3, 2, 3});
  EXPECT_FALSE(projected.has_value());
}

TEST(ZenzLocalAlignmentTest,
     InvalidatesOwnedSpansThatNeedDifferentOptimalAlignments) {
  const std::vector<std::optional<ZenzLocalTextSpan>> projected =
      ProjectOwnedSpansConsistently(
          "aaa", "baab", {ZenzLocalTextSpan{0, 1, 0, 1},
                          ZenzLocalTextSpan{2, 3, 2, 3}});

  ASSERT_EQ(projected.size(), 2);
  EXPECT_FALSE(projected[0].has_value());
  EXPECT_FALSE(projected[1].has_value());
}


TEST(ZenzLocalAlignmentTest, BoundaryInsertionInvalidatesGenericOwnership) {
  const auto projected = ProjectUniqueOwnedSpan(
      "取扱して回答", "取扱いして解凍", ZenzLocalTextSpan{0, 2, 0, 6});
  EXPECT_FALSE(projected.has_value());
}

TEST(ZenzLocalAlignmentTest, SurfacePairStillRecognizesExplicitRawExpansion) {
  const auto pair = FindUniqueOptimalSurfacePair(
      "取扱して回答", "取扱いして解凍", "取扱", "取扱い");
  ASSERT_TRUE(pair.has_value());
  EXPECT_EQ(pair->source.char_begin, 0);
  EXPECT_EQ(pair->source.char_end, 2);
  EXPECT_EQ(pair->target.char_begin, 0);
  EXPECT_EQ(pair->target.char_end, 3);
}

TEST(ZenzLocalAlignmentTest, FeedbackDetectsRawRevertBeforeGenericProjection) {
  ZenzLocalPreference applied =
      Preference("とりあつかい", "取扱い", "取扱");
  applied.displayed_span = ZenzLocalTextSpan{0, 2, 0, 6};
  applied.has_text_spans = true;

  const auto decisions = ClassifyAppliedLocalFeedback(
      "取扱して回答", "取扱いして解凍", {applied},
      {ZenzValidatedLocalEdit{"とりあつかい", "取扱", "取扱い"},
       ZenzValidatedLocalEdit{"かいとう", "回答", "解凍"}});

  ASSERT_EQ(decisions.size(), 1);
  EXPECT_TRUE(decisions[0].rejected);
  EXPECT_FALSE(decisions[0].validated_third_value.has_value());
}

TEST(ZenzLocalAlignmentTest, FeedbackLearnsThirdOnlyWhenReadingValidated) {
  ZenzLocalPreference applied =
      Preference("とりあつかい", "取扱い", "取扱");
  applied.displayed_span = ZenzLocalTextSpan{0, 2, 0, 6};
  applied.has_text_spans = true;

  const auto validated = ClassifyAppliedLocalFeedback(
      "取扱して回答", "取り扱いして解凍", {applied},
      {ZenzValidatedLocalEdit{"とりあつかい", "取扱", "取り扱い"},
       ZenzValidatedLocalEdit{"かいとう", "回答", "解凍"}});
  ASSERT_EQ(validated.size(), 1);
  EXPECT_TRUE(validated[0].rejected);
  ASSERT_TRUE(validated[0].validated_third_value.has_value());
  EXPECT_EQ(*validated[0].validated_third_value, "取り扱い");

  const auto unvalidated = ClassifyAppliedLocalFeedback(
      "取扱して回答", "取り扱いして解凍", {applied},
      {ZenzValidatedLocalEdit{"かいとう", "回答", "解凍"}});
  ASSERT_EQ(unvalidated.size(), 1);
  EXPECT_FALSE(unvalidated[0].validated_third_value.has_value());
}

TEST(ZenzLocalAlignmentTest, FeedbackKeepsPreservedLocalNeutral) {
  ZenzLocalPreference applied =
      Preference("とりあつかい", "取扱い", "取扱");
  applied.displayed_span = ZenzLocalTextSpan{0, 2, 0, 6};
  applied.has_text_spans = true;

  const auto decisions = ClassifyAppliedLocalFeedback(
      "取扱して回答", "取扱して解凍", {applied},
      {ZenzValidatedLocalEdit{"かいとう", "回答", "解凍"}});

  ASSERT_EQ(decisions.size(), 1);
  EXPECT_FALSE(decisions[0].rejected);
  EXPECT_FALSE(decisions[0].validated_third_value.has_value());
}

TEST(ZenzLocalAlignmentTest, AlignedSurfacePairSetRejectsCrossingPairs) {
  const auto first = FindUniqueOptimalSurfacePair("ab", "ba", "a", "a");
  const auto second = FindUniqueOptimalSurfacePair("ab", "ba", "b", "b");
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  const std::vector<bool> keep = SelectJointlyOptimalAlignedSurfacePairs(
      "ab", "ba", {first, second});
  ASSERT_EQ(keep.size(), 2);
  EXPECT_FALSE(keep[0]);
  EXPECT_FALSE(keep[1]);
}

}  // namespace
}  // namespace session
}  // namespace mozc
