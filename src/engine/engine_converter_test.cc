// Copyright 2010-2021, Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
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

// Tests for session converter.
//
// Note that we have a lot of tests which assume that the converter fills
// T13Ns. If you want to add test case related to T13Ns, please make sure
// you set T13Ns to the result for a mock converter.

#include "engine/engine_converter.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "absl/log/check.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "base/util.h"
#include "composer/composer.h"
#include "composer/table.h"
#include "converter/attribute.h"
#include "converter/candidate.h"
#include "converter/converter_mock.h"
#include "converter/inner_segment.h"
#include "converter/segments.h"
#include "converter/segments_matchers.h"
#include "data_manager/testing/mock_data_manager.h"
#include "engine/candidate_list.h"
#include "engine/engine_converter_interface.h"
#include "protocol/candidate_window.pb.h"
#include "protocol/commands.pb.h"
#include "protocol/config.pb.h"
#include "request/conversion_request.h"
#include "request/request_test_util.h"
#include "testing/gmock.h"
#include "testing/gunit.h"
#include "testing/mozctest.h"
#include "testing/testing_util.h"
#include "transliteration/transliteration.h"

namespace mozc {
namespace engine {
namespace {

using ::mozc::commands::Context;
using ::mozc::commands::Request;
using ::mozc::config::Config;
using ::testing::_;
using ::testing::DoAll;
using ::testing::Eq;
using ::testing::Mock;
using ::testing::Pointee;
using ::testing::Property;
using ::testing::Return;
using ::testing::SaveArgPointee;
using ::testing::SetArgPointee;

constexpr char kChars_Aiueo[] = "あいうえお";
constexpr char kChars_Mo[] = "も";
constexpr char kChars_Mozuku[] = "もずく";
constexpr char kChars_Mozukusu[] = "もずくす";
constexpr char kChars_Momonga[] = "ももんが";
}  // namespace

void AddSegmentWithSingleCandidate(Segments* segments, absl::string_view key,
                                   absl::string_view value) {
  Segment* seg = segments->add_segment();
  seg->set_key(key);
  converter::Candidate* cand = seg->add_candidate();
  cand->key.assign(key.data(), key.size());
  cand->content_key = cand->key;
  cand->value.assign(value.data(), value.size());
  cand->content_value = cand->value;
}

class EngineConverterTest : public testing::TestWithTempUserProfile {
 protected:
  void SetUp() override {
    config_ = std::make_shared<Config>();
    config_->set_use_cascading_window(true);
    request_ = std::make_shared<Request>();

    table_ = std::make_shared<composer::Table>();
    table_->InitializeWithRequestAndConfig(*request_, *config_);
    composer_ =
        std::make_unique<composer::Composer>(table_, *request_, *config_);
  }

  void TearDown() override {
    table_.reset();
    composer_.reset();
  }

  static void GetSegments(const EngineConverter& converter, Segments* dest) {
    CHECK(dest);
    *dest = converter.segments_;
  }

  static const Segments& GetSegments(const EngineConverter& converter) {
    return converter.segments_;
  }

  static void SetSegments(const Segments& src, EngineConverter* converter) {
    CHECK(converter);
    converter->segments_ = src;
  }

  static const commands::Result& GetResult(const EngineConverter& converter) {
    return converter.result_;
  }

  static const CandidateList& GetCandidateList(
      const EngineConverter& converter) {
    return converter.candidate_list_;
  }

  static EngineConverterInterface::State GetState(
      const EngineConverter& converter) {
    return converter.state_;
  }

  static void SetState(EngineConverterInterface::State state,
                       EngineConverter* converter) {
    converter->state_ = state;
  }

  static void SetCurrentConversionUsesUserHistory(
      bool use_history, EngineConverter* converter) {
    converter->current_conversion_uses_user_history_ = use_history;
  }

  static size_t GetSegmentIndex(const EngineConverter& converter) {
    return converter.segment_index_;
  }

  static bool IsCandidateListVisible(const EngineConverter& converter) {
    return converter.candidate_list_visible_;
  }

  static const commands::Request& GetRequest(const EngineConverter& converter) {
    return *converter.request_;
  }

  static void GetPreedit(const EngineConverter& converter, size_t index,
                         size_t size, std::string* conversion) {
    converter.GetPreedit(index, size, conversion);
  }

  static void GetConversion(const EngineConverter& converter, size_t index,
                            size_t size, std::string* conversion) {
    converter.GetConversion(index, size, conversion);
  }

  static void AppendCandidateList(ConversionRequest::RequestType request_type,
                                  EngineConverter* converter) {
    ConversionRequest::Options unused_options;
    converter->SetRequestType(request_type, unused_options);
    converter->AppendCandidateList();
  }

  // set result for "あいうえお"
  static void SetAiueo(Segments* segments) {
    segments->Clear();
    Segment* segment;
    converter::Candidate* candidate;

    segment = segments->add_segment();
    segment->set_key("あいうえお");
    candidate = segment->add_candidate();
    candidate->key = "あいうえお";
    candidate->content_key = candidate->key;
    candidate->value = candidate->key;
    candidate->content_value = candidate->value;
    candidate = segment->add_candidate();
    candidate->key = "あいうえお";
    candidate->content_key = candidate->key;
    candidate->value = "アイウエオ";
    candidate->content_value = candidate->value;
  }

  // set result for "かまぼこのいんぼう"
  static void SetKamaboko(Segments* segments) {
    Segment* segment;
    converter::Candidate* candidate;

    segments->Clear();
    segment = segments->add_segment();

    segment->set_key("かまぼこの");
    candidate = segment->add_candidate();
    candidate->key = "かまぼこの";
    candidate->content_key = candidate->key;
    candidate->value = "かまぼこの";
    candidate->content_value = candidate->value;
    candidate = segment->add_candidate();
    candidate->key = "かまぼこの";
    candidate->content_key = candidate->key;
    candidate->value = "カマボコの";
    candidate->content_value = candidate->value;
    segment = segments->add_segment();
    segment->set_key("いんぼう");
    candidate = segment->add_candidate();
    candidate->key = "いんぼう";
    candidate->content_key = candidate->key;
    candidate->value = "陰謀";
    candidate->content_value = candidate->value;
    candidate = segment->add_candidate();
    candidate->key = "いんぼう";
    candidate->content_key = candidate->key;
    candidate->value = "印房";
    candidate->content_value = candidate->value;

    // Set dummy T13Ns
    std::vector<converter::Candidate>* meta_candidates =
        segment->mutable_meta_candidates();
    meta_candidates->resize(transliteration::NUM_T13N_TYPES);
    for (size_t i = 0; i < transliteration::NUM_T13N_TYPES; ++i) {
      meta_candidates->at(i).value = segment->key();
      meta_candidates->at(i).content_value = segment->key();
      meta_candidates->at(i).content_key = segment->key();
    }
  }

  // set T13N candidates to segments using composer
  static void FillT13Ns(Segments* segments,
                        const composer::Composer* composer) {
    size_t composition_pos = 0;
    for (Segment& segment : segments->conversion_segments()) {
      const size_t composition_len = Util::CharsLen(segment.key());
      std::vector<std::string> t13ns;
      composer->GetSubTransliterations(composition_pos, composition_len,
                                       &t13ns);
      std::vector<converter::Candidate>* meta_candidates =
          segment.mutable_meta_candidates();
      meta_candidates->resize(transliteration::NUM_T13N_TYPES);
      for (size_t j = 0; j < transliteration::NUM_T13N_TYPES; ++j) {
        meta_candidates->at(j).value = t13ns[j];
        meta_candidates->at(j).content_value = t13ns[j];
        meta_candidates->at(j).key = segment.key();
        meta_candidates->at(j).content_key = segment.key();
      }
      composition_pos += composition_len;
    }
  }

  // Sets the result for "like"
  void SetLike(Segments* segments) {
    composer_->InsertCharacterKeyAndPreedit("li", "ぃ");
    composer_->InsertCharacterKeyAndPreedit("ke", "け");

    Segment* segment;
    converter::Candidate* candidate;

    segments->Clear();
    segment = segments->add_segment();

    segment->set_key("ぃ");
    candidate = segment->add_candidate();
    candidate->value = "ぃ";

    candidate = segment->add_candidate();
    candidate->value = "ィ";

    segment = segments->add_segment();
    segment->set_key("け");
    candidate = segment->add_candidate();
    candidate->value = "家";
    candidate = segment->add_candidate();
    candidate->value = "け";

    FillT13Ns(segments, composer_.get());
  }

  static Segments GetSegmentsTest() {
    Segments segments;
    Segment* segment = segments.add_segment();
    converter::Candidate* candidate;
    segment->set_key("てすと");
    candidate = segment->add_candidate();
    candidate->value = "テスト";
    candidate->key = "てすと";
    candidate->content_key = "てすと";
    return segments;
  }

  static void InsertASCIISequence(absl::string_view text,
                                  composer::Composer* composer) {
    for (size_t i = 0; i < text.size(); ++i) {
      commands::KeyEvent key;
      key.set_key_code(text[i]);
      composer->InsertCharacterKeyEvent(key);
    }
  }

  static void ExpectSameEngineConverter(const EngineConverter& lhs,
                                        const EngineConverter& rhs) {
    EXPECT_EQ(lhs.IsActive(), rhs.IsActive());
    EXPECT_EQ(IsCandidateListVisible(lhs), IsCandidateListVisible(rhs));
    EXPECT_EQ(GetSegmentIndex(lhs), GetSegmentIndex(rhs));

    EXPECT_EQ(lhs.conversion_preferences().use_history,
              rhs.conversion_preferences().use_history);
    EXPECT_EQ(IsCandidateListVisible(lhs), IsCandidateListVisible(rhs));

    Segments segments_lhs, segments_rhs;
    GetSegments(lhs, &segments_lhs);
    GetSegments(rhs, &segments_rhs);
    EXPECT_EQ(segments_lhs.segments_size(), segments_rhs.segments_size());
    for (size_t i = 0; i < segments_lhs.segments_size(); ++i) {
      const Segment& segment_lhs = segments_lhs.segment(i);
      const Segment& segment_rhs = segments_rhs.segment(i);
      EXPECT_EQ(segment_lhs.key(), segment_rhs.key()) << " i=" << i;
      EXPECT_EQ(segment_lhs.segment_type(), segment_rhs.segment_type())
          << " i=" << i;
      EXPECT_EQ(segment_lhs.candidates_size(), segment_rhs.candidates_size());
    }

    const CandidateList& candidate_list_lhs = GetCandidateList(lhs);
    const CandidateList& candidate_list_rhs = GetCandidateList(rhs);
    EXPECT_EQ(candidate_list_lhs.name(), candidate_list_rhs.name());
    EXPECT_EQ(candidate_list_lhs.page_size(), candidate_list_rhs.page_size());
    EXPECT_EQ(candidate_list_lhs.size(), candidate_list_rhs.size());
    EXPECT_EQ(candidate_list_lhs.last_index(), candidate_list_rhs.last_index());
    EXPECT_EQ(candidate_list_lhs.focused_id(), candidate_list_rhs.focused_id());
    EXPECT_EQ(candidate_list_lhs.focused_index(),
              candidate_list_rhs.focused_index());
    EXPECT_EQ(candidate_list_lhs.focused(), candidate_list_rhs.focused());

    for (int i = 0; i < candidate_list_lhs.size(); ++i) {
      const Candidate& candidate_lhs = candidate_list_lhs.candidate(i);
      const Candidate& candidate_rhs = candidate_list_rhs.candidate(i);
      EXPECT_EQ(candidate_lhs.id(), candidate_rhs.id());
      EXPECT_EQ(candidate_lhs.attributes(), candidate_rhs.attributes());
      EXPECT_EQ(candidate_lhs.HasSubcandidateList(),
                candidate_rhs.HasSubcandidateList());
      if (candidate_lhs.HasSubcandidateList()) {
        EXPECT_EQ(candidate_lhs.subcandidate_list().size(),
                  candidate_rhs.subcandidate_list().size());
      }
    }

    EXPECT_PROTO_EQ(GetResult(lhs), GetResult(rhs));
    EXPECT_PROTO_EQ(GetRequest(lhs), GetRequest(rhs));
  }

  static ::testing::AssertionResult ExpectSelectedCandidateIndices(
      const char*, const char*, const EngineConverter& converter,
      absl::Span<const int> expected) {
    absl::Span<const int> actual = converter.selected_candidate_indices_;

    if (expected.size() != actual.size()) {
      return ::testing::AssertionFailure()
             << "Indices size mismatch.\n"
             << "Expected: " << expected.size() << "\n"
             << "Actual:   " << actual.size();
    }

    for (size_t i = 0; i < expected.size(); ++i) {
      if (expected[i] != actual[i]) {
        return ::testing::AssertionFailure()
               << "Index mismatch.\n"
               << "Expected: " << expected[i] << "\n"
               << "Actual:   " << actual[i];
      }
    }

    return ::testing::AssertionSuccess();
  }

  static void SetCommandCandidate(Segments* segments, int segment_index,
                                  int candidate_index,
                                  converter::Candidate::Command command) {
    segments->mutable_conversion_segment(segment_index)
        ->mutable_candidate(candidate_index)
        ->attributes |= converter::Attribute::COMMAND_CANDIDATE;
    segments->mutable_conversion_segment(segment_index)
        ->mutable_candidate(candidate_index)
        ->command = command;
  }

  std::unique_ptr<composer::Composer> composer_;
  std::shared_ptr<composer::Table> table_;
  std::shared_ptr<Request> request_;
  std::shared_ptr<Config> config_;

 private:
  const testing::MockDataManager mock_data_manager_;
};

#define EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, indices) \
  EXPECT_PRED_FORMAT2(ExpectSelectedCandidateIndices, converter, indices);

TEST_F(EngineConverterTest, Convert) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);
  {
    Segments segments;
    SetAiueo(&segments);
    composer_->InsertCharacterPreedit("あいうえお");
    FillT13Ns(&segments, composer_.get());
    EXPECT_CALL(*mock_converter, StartConversion(_, _))
        .WillOnce(DoAll(SetArgPointee<1>(segments), Return(true)));
  }

  std::vector<int> expected_indices;
  EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);

  composer_->InsertCharacterPreedit(kChars_Aiueo);
  EXPECT_TRUE(converter.Convert(*composer_));
  ASSERT_TRUE(converter.IsActive());
  expected_indices.push_back(0);
  EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);

  commands::Output output;
  converter.FillOutput(*composer_, &output);
  EXPECT_FALSE(output.has_result());
  EXPECT_TRUE(output.has_preedit());
  EXPECT_FALSE(output.has_candidate_window());

  const commands::Preedit& conversion = output.preedit();
  EXPECT_EQ(conversion.segment_size(), 1);
  EXPECT_EQ(conversion.segment(0).annotation(),
            commands::Preedit::Segment::HIGHLIGHT);
  EXPECT_EQ(conversion.segment(0).value(), kChars_Aiueo);
  EXPECT_EQ(conversion.segment(0).key(), kChars_Aiueo);

  // Converter should be active before submission
  EXPECT_TRUE(converter.IsActive());
  EXPECT_FALSE(IsCandidateListVisible(converter));

  converter.Commit(*composer_, Context::default_instance());
  composer_->Reset();
  output.Clear();
  converter.FillOutput(*composer_, &output);
  EXPECT_TRUE(output.has_result());
  EXPECT_FALSE(output.has_preedit());
  EXPECT_FALSE(output.has_candidate_window());
  expected_indices.clear();
  EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);

  const commands::Result& result = output.result();
  EXPECT_EQ(result.value(), kChars_Aiueo);
  EXPECT_EQ(result.key(), kChars_Aiueo);

  // Converter should be inactive after submission
  EXPECT_FALSE(converter.IsActive());
  EXPECT_FALSE(IsCandidateListVisible(converter));
}

TEST_F(EngineConverterTest, ConvertWithSpellingCorrection) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);
  {
    Segments segments;
    SetAiueo(&segments);
    composer_->InsertCharacterPreedit("あいうえお");
    FillT13Ns(&segments, composer_.get());
    segments.mutable_conversion_segment(0)->mutable_candidate(0)->attributes |=

        converter::Attribute::SPELLING_CORRECTION;
    EXPECT_CALL(*mock_converter, StartConversion(_, _))
        .WillOnce(DoAll(SetArgPointee<1>(segments), Return(true)));
  }

  composer_->InsertCharacterPreedit(kChars_Aiueo);
  EXPECT_TRUE(converter.Convert(*composer_));
  ASSERT_TRUE(converter.IsActive());
  EXPECT_TRUE(IsCandidateListVisible(converter));
}

TEST_F(EngineConverterTest, ConvertWithA11yTalkbackEnabled) {
  request_->set_is_a11y_talkback_enabled(true);
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);
  {
    Segments segments;
    SetAiueo(&segments);
    composer_->InsertCharacterPreedit("あいうえお");
    FillT13Ns(&segments, composer_.get());
    EXPECT_CALL(*mock_converter, StartConversion(_, _))
        .WillOnce(DoAll(SetArgPointee<1>(segments), Return(true)));
  }

  composer_->InsertCharacterPreedit(kChars_Aiueo);
  EXPECT_TRUE(converter.Convert(*composer_));
  ASSERT_TRUE(converter.IsActive());
  EXPECT_TRUE(IsCandidateListVisible(converter));
}

TEST_F(EngineConverterTest, IsReadingEquivalentUsesAlternativeReverseReading) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);

  Segments reverse_segments;
  Segment* segment = reverse_segments.add_segment();
  segment->set_key("一昨日");
  segment->add_candidate()->value = "おととい";
  segment->add_candidate()->value = "いっさくじつ";

  EXPECT_CALL(*mock_converter, StartReverseConversion(_, "一昨日"))
      .WillOnce(DoAll(SetArgPointee<0>(reverse_segments), Return(true)));

  EXPECT_TRUE(converter.IsReadingEquivalent("一昨日", "いっさくじつ"));
}

TEST_F(EngineConverterTest, IsReadingEquivalentRejectsDroppedReading) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);

  Segments reverse_segments;
  Segment* segment = reverse_segments.add_segment();
  segment->set_key("今日はゆっくり行っています");
  segment->add_candidate()->value = "きょうはゆっくりいっています";

  EXPECT_CALL(*mock_converter,
              StartReverseConversion(_, "今日はゆっくり行っています"))
      .WillOnce(DoAll(SetArgPointee<0>(reverse_segments), Return(true)));

  EXPECT_FALSE(converter.IsReadingEquivalent(
      "今日はゆっくり行っています",
      "きょうはとうきょうにはいっています"));
}

TEST_F(EngineConverterTest, GetUniqueReadingSurfaceAlignment) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);

  Segments reverse_segments;
  Segment* s0 = reverse_segments.add_segment();
  s0->set_key("ちょっと");
  s0->add_candidate()->value = "ちょっと";
  Segment* s1 = reverse_segments.add_segment();
  s1->set_key("離席");
  s1->add_candidate()->value = "りせき";
  s1->add_candidate()->value = "はなれせき";
  Segment* s2 = reverse_segments.add_segment();
  s2->set_key("します");
  s2->add_candidate()->value = "します";

  EXPECT_CALL(*mock_converter,
              StartReverseConversion(_, "ちょっと離席します"))
      .WillOnce(DoAll(SetArgPointee<0>(reverse_segments), Return(true)));

  std::vector<ReadingSurfaceAlignmentSegment> alignment;
  ASSERT_TRUE(converter.GetUniqueReadingSurfaceAlignment(
      "ちょっと離席します", "ちょっとりせきします", &alignment));
  ASSERT_EQ(alignment.size(), 3);
  EXPECT_EQ(alignment[0].reading, "ちょっと");
  EXPECT_EQ(alignment[0].surface, "ちょっと");
  EXPECT_EQ(alignment[1].reading, "りせき");
  EXPECT_EQ(alignment[1].surface, "離席");
  EXPECT_EQ(alignment[2].reading, "します");
  EXPECT_EQ(alignment[2].surface, "します");
  EXPECT_EQ(alignment.front().reading_begin, 0);
  EXPECT_EQ(alignment.back().reading_end,
            std::string("ちょっとりせきします").size());
}

TEST_F(EngineConverterTest, GetUniqueReadingSurfaceAlignmentRejectsAmbiguity) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);

  Segments reverse_segments;
  Segment* s0 = reverse_segments.add_segment();
  s0->set_key("甲");
  s0->add_candidate()->value = "あ";
  s0->add_candidate()->value = "あい";
  Segment* s1 = reverse_segments.add_segment();
  s1->set_key("乙");
  s1->add_candidate()->value = "いう";
  s1->add_candidate()->value = "う";

  EXPECT_CALL(*mock_converter, StartReverseConversion(_, "甲乙"))
      .WillOnce(DoAll(SetArgPointee<0>(reverse_segments), Return(true)));

  std::vector<ReadingSurfaceAlignmentSegment> alignment;
  EXPECT_FALSE(converter.GetUniqueReadingSurfaceAlignment(
      "甲乙", "あいう", &alignment));
  EXPECT_TRUE(alignment.empty());
}

TEST_F(EngineConverterTest, ConvertToTransliteration) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);
  {
    Segments segments;
    SetAiueo(&segments);
    composer_->InsertCharacterKeyAndPreedit("aiueo", kChars_Aiueo);
    FillT13Ns(&segments, composer_.get());
    EXPECT_CALL(*mock_converter, StartConversion(_, _))
        .WillOnce(DoAll(SetArgPointee<1>(segments), Return(true)));
  }

  EXPECT_TRUE(converter.ConvertToTransliteration(*composer_,
                                                 transliteration::HALF_ASCII));
  std::vector<int> expected_indices = {0};
  {  // Check the conversion #1
    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());
    EXPECT_TRUE(output.has_preedit());
    EXPECT_FALSE(output.has_candidate_window());

    const commands::Preedit& conversion = output.preedit();
    EXPECT_EQ(conversion.segment_size(), 1);
    EXPECT_EQ(conversion.segment(0).value(), "aiueo");
    EXPECT_FALSE(IsCandidateListVisible(converter));
    EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);
  }

  EXPECT_TRUE(converter.ConvertToTransliteration(*composer_,
                                                 transliteration::HALF_ASCII));
  {  // Check the conversion #2
    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());
    EXPECT_TRUE(output.has_preedit());
    EXPECT_FALSE(output.has_candidate_window());

    const commands::Preedit& conversion = output.preedit();
    EXPECT_EQ(conversion.segment_size(), 1);
    EXPECT_EQ(conversion.segment(0).value(), "AIUEO");
    EXPECT_FALSE(IsCandidateListVisible(converter));
    EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);
  }

  EXPECT_TRUE(converter.ConvertToTransliteration(*composer_,
                                                 transliteration::FULL_ASCII));
  {  // Check the conversion #3
    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());
    EXPECT_TRUE(output.has_preedit());
    EXPECT_FALSE(output.has_candidate_window());

    const commands::Preedit& conversion = output.preedit();
    EXPECT_EQ(conversion.segment_size(), 1);
    EXPECT_EQ(conversion.segment(0).value(), "ＡＩＵＥＯ");
    EXPECT_FALSE(IsCandidateListVisible(converter));
    EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);
  }

  converter.Commit(*composer_, Context::default_instance());
}

TEST_F(EngineConverterTest, ConvertToTransliterationWithMultipleSegments) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);

  {
    Segments segments;
    SetLike(&segments);
    EXPECT_CALL(*mock_converter, StartConversion(_, _))
        .WillOnce(DoAll(SetArgPointee<1>(segments), Return(true)));
  }

  // Convert
  EXPECT_TRUE(converter.Convert(*composer_));
  std::vector<int> expected_indices = {0, 0};
  {  // Check the conversion #1
    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());
    EXPECT_TRUE(output.has_preedit());
    EXPECT_FALSE(output.has_candidate_window());

    const commands::Preedit& conversion = output.preedit();
    EXPECT_EQ(conversion.segment_size(), 2);
    EXPECT_EQ(conversion.segment(0).value(), "ぃ");
    EXPECT_EQ(conversion.segment(1).value(), "家");
    EXPECT_FALSE(IsCandidateListVisible(converter));
    EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);
  }

  // Convert to half-width alphanumeric.
  EXPECT_TRUE(converter.ConvertToTransliteration(*composer_,
                                                 transliteration::HALF_ASCII));
  {  // Check the conversion #2
    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());
    EXPECT_TRUE(output.has_preedit());
    EXPECT_FALSE(output.has_candidate_window());

    const commands::Preedit& conversion = output.preedit();
    EXPECT_EQ(conversion.segment_size(), 2);
    EXPECT_EQ(conversion.segment(0).value(), "li");
    EXPECT_FALSE(IsCandidateListVisible(converter));
    EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);
  }
}

TEST_F(EngineConverterTest, ConvertToTransliterationWithoutCascadigWindow) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);
  {
    Segments segments;
    {
      Segment* segment;
      converter::Candidate* candidate;
      segment = segments.add_segment();
      segment->set_key("dvd");
      candidate = segment->add_candidate();
      candidate->value = "dvd";
      candidate = segment->add_candidate();
      candidate->value = "DVD";
    }
    {  // Set OperationPreferences
      converter.set_use_cascading_window(false);
      converter.set_selection_shortcut(config::Config::NO_SHORTCUT);
    }
    composer_->InsertCharacterKeyAndPreedit("dvd", "ｄｖｄ");
    FillT13Ns(&segments, composer_.get());

    EXPECT_CALL(*mock_converter, StartConversion(_, _))
        .WillOnce(DoAll(SetArgPointee<1>(segments), Return(true)));
  }

  EXPECT_TRUE(converter.ConvertToTransliteration(*composer_,
                                                 transliteration::FULL_ASCII));
  std::vector<int> expected_indices = {0};
  {  // Check the conversion #1
    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());
    EXPECT_TRUE(output.has_preedit());
    EXPECT_FALSE(output.has_candidate_window());

    const commands::Preedit& conversion = output.preedit();
    EXPECT_EQ(conversion.segment_size(), 1);
    EXPECT_EQ(conversion.segment(0).value(), "ｄｖｄ");
    EXPECT_FALSE(IsCandidateListVisible(converter));
    EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);
  }

  EXPECT_TRUE(converter.ConvertToTransliteration(*composer_,
                                                 transliteration::FULL_ASCII));
  {  // Check the conversion #2
    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());
    EXPECT_TRUE(output.has_preedit());
    EXPECT_FALSE(output.has_candidate_window());

    const commands::Preedit& conversion = output.preedit();
    EXPECT_EQ(conversion.segment_size(), 1);
    EXPECT_EQ(conversion.segment(0).value(), "ＤＶＤ");
    EXPECT_FALSE(IsCandidateListVisible(converter));
    EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);
  }

  EXPECT_TRUE(converter.ConvertToTransliteration(*composer_,
                                                 transliteration::FULL_ASCII));
  {  // Check the conversion #3
    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());
    EXPECT_TRUE(output.has_preedit());
    EXPECT_FALSE(output.has_candidate_window());

    const commands::Preedit& conversion = output.preedit();
    EXPECT_EQ(conversion.segment_size(), 1);
    EXPECT_EQ(conversion.segment(0).value(), "Ｄｖｄ");
    EXPECT_FALSE(IsCandidateListVisible(converter));
    EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);
  }
}

TEST_F(EngineConverterTest, MultiSegmentsConversion) {
  const std::string kKamabokono = "かまぼこの";
  const std::string kInbou = "いんぼう";

  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);
  {
    Segments segments;
    SetKamaboko(&segments);
    composer_->InsertCharacterPreedit(kKamabokono + kInbou);
    FillT13Ns(&segments, composer_.get());
    EXPECT_CALL(*mock_converter, StartConversion(_, _))
        .WillOnce(DoAll(SetArgPointee<1>(segments), Return(true)));
  }
  EXPECT_TRUE(converter.Convert(*composer_));
  std::vector<int> expected_indices = {0, 0};
  {
    EXPECT_EQ(GetSegmentIndex(converter), 0);
    EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);

    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());
    EXPECT_TRUE(output.has_preedit());
    EXPECT_FALSE(output.has_candidate_window());

    const commands::Preedit& conversion = output.preedit();
    ASSERT_EQ(conversion.segment_size(), 2);
    EXPECT_EQ(conversion.segment(0).annotation(),
              commands::Preedit::Segment::HIGHLIGHT);
    EXPECT_EQ(conversion.segment(0).key(), kKamabokono);
    EXPECT_EQ(conversion.segment(0).value(), kKamabokono);

    EXPECT_EQ(conversion.segment(1).annotation(),
              commands::Preedit::Segment::UNDERLINE);
    EXPECT_EQ(conversion.segment(1).key(), kInbou);
    EXPECT_EQ(conversion.segment(1).value(), "陰謀");
  }

  // Test for candidates [CandidateNext]
  EXPECT_FALSE(IsCandidateListVisible(converter));
  converter.CandidateNext(*composer_);
  expected_indices[0] += 1;
  {
    EXPECT_TRUE(IsCandidateListVisible(converter));
    EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);
  }

  // Test for candidates [CandidatePrev]
  converter.CandidatePrev();
  expected_indices[0] -= 1;
  {
    EXPECT_TRUE(IsCandidateListVisible(converter));
    EXPECT_EQ(GetSegmentIndex(converter), 0);
    EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);
    EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);

    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());
    EXPECT_TRUE(output.has_preedit());
    EXPECT_TRUE(output.has_candidate_window());

    const commands::CandidateWindow& candidate_window =
        output.candidate_window();
    ASSERT_EQ(candidate_window.size(),
              3);  // two candidates + one t13n sub list.
    EXPECT_EQ(candidate_window.position(), 0);
    EXPECT_EQ(candidate_window.candidate(0).value(), kKamabokono);
    EXPECT_EQ(candidate_window.candidate(1).value(), "カマボコの");
    EXPECT_EQ(candidate_window.candidate(2).value(), "そのほかの文字種");
  }

  // Test for segment motion. [SegmentFocusRight]
  converter.SegmentFocusRight();
  {
    EXPECT_EQ(GetSegmentIndex(converter), 1);
    EXPECT_FALSE(IsCandidateListVisible(converter));
    converter.SetCandidateListVisible(true);

    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());
    EXPECT_TRUE(output.has_preedit());
    EXPECT_TRUE(output.has_candidate_window());
    EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);

    const commands::CandidateWindow& candidate_window =
        output.candidate_window();
    EXPECT_EQ(candidate_window.focused_index(), 0);
    ASSERT_EQ(candidate_window.size(),
              3);  // two candidates + one t13n sub list.
    EXPECT_EQ(candidate_window.position(), 5);
    EXPECT_EQ(candidate_window.candidate(0).value(), "陰謀");
    EXPECT_EQ(candidate_window.candidate(1).value(), "印房");
    EXPECT_EQ(candidate_window.candidate(2).value(), "そのほかの文字種");
  }

  // Test for segment motion. [SegmentFocusLeft]
  converter.SegmentFocusLeft();
  {
    EXPECT_EQ(GetSegmentIndex(converter), 0);
    EXPECT_FALSE(IsCandidateListVisible(converter));
    converter.SetCandidateListVisible(true);
    EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);

    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());
    EXPECT_TRUE(output.has_preedit());
    EXPECT_TRUE(output.has_candidate_window());

    const commands::CandidateWindow& candidate_window =
        output.candidate_window();
    EXPECT_EQ(candidate_window.focused_index(), 0);
    ASSERT_EQ(candidate_window.size(),
              3);  // two candidates + one t13n sub list.
    EXPECT_EQ(candidate_window.position(), 0);
    EXPECT_EQ(candidate_window.candidate(0).value(), kKamabokono);
    EXPECT_EQ(candidate_window.candidate(1).value(), "カマボコの");
    EXPECT_EQ(candidate_window.candidate(2).value(), "そのほかの文字種");
  }

  // Test for segment motion. [SegmentFocusLeft] at the head of segments.
  // http://b/2990134
  // Focus changing at the tail of segments to right,
  // and at the head of segments to left, should work.
  converter.SegmentFocusLeft();
  {
    EXPECT_EQ(GetSegmentIndex(converter), 1);
    EXPECT_FALSE(IsCandidateListVisible(converter));
    converter.SetCandidateListVisible(true);
    EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);

    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());
    EXPECT_TRUE(output.has_preedit());
    EXPECT_TRUE(output.has_candidate_window());

    const commands::CandidateWindow& candidate_window =
        output.candidate_window();
    EXPECT_EQ(candidate_window.focused_index(), 0);
    ASSERT_EQ(candidate_window.size(),
              3);  // two candidates + one t13n sub list.
    EXPECT_EQ(candidate_window.position(), 5);
    EXPECT_EQ(candidate_window.candidate(0).value(), "陰謀");
    EXPECT_EQ(candidate_window.candidate(1).value(), "印房");
    EXPECT_EQ(candidate_window.candidate(2).value(), "そのほかの文字種");
  }

  // Test for segment motion. [SegmentFocusRight] at the tail of segments.
  // http://b/2990134
  // Focus changing at the tail of segments to right,
  // and at the head of segments to left, should work.
  converter.SegmentFocusRight();
  {
    EXPECT_FALSE(IsCandidateListVisible(converter));
    converter.SetCandidateListVisible(true);
    EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);

    commands::Output output;
    EXPECT_EQ(GetSegmentIndex(converter), 0);
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());
    EXPECT_TRUE(output.has_preedit());
    EXPECT_TRUE(output.has_candidate_window());

    const commands::CandidateWindow& candidate_window =
        output.candidate_window();
    EXPECT_EQ(candidate_window.focused_index(), 0);
    ASSERT_EQ(candidate_window.size(),
              3);  // two candidates + one t13n sub list.
    EXPECT_EQ(candidate_window.position(), 0);
    EXPECT_EQ(candidate_window.candidate(0).value(), kKamabokono);
    EXPECT_EQ(candidate_window.candidate(1).value(), "カマボコの");
    EXPECT_EQ(candidate_window.candidate(2).value(), "そのほかの文字種");
  }

  // Test for candidate motion. [CandidateNext]
  converter.SegmentFocusRight();  // Focus to the last segment.
  EXPECT_EQ(GetSegmentIndex(converter), 1);
  converter.CandidateNext(*composer_);
  expected_indices[1] += 1;
  {
    EXPECT_TRUE(IsCandidateListVisible(converter));
    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());
    EXPECT_TRUE(output.has_preedit());
    EXPECT_TRUE(output.has_candidate_window());
    EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);

    const commands::CandidateWindow& candidate_window =
        output.candidate_window();
    EXPECT_EQ(candidate_window.focused_index(), 1);
    ASSERT_EQ(candidate_window.size(),
              3);  // two candidates + one t13n sub list.
    EXPECT_EQ(candidate_window.position(), 5);
    EXPECT_EQ(candidate_window.candidate(0).value(), "陰謀");
    EXPECT_EQ(candidate_window.candidate(1).value(), "印房");
    EXPECT_EQ(candidate_window.candidate(2).value(), "そのほかの文字種");

    const commands::Preedit& conversion = output.preedit();
    EXPECT_EQ(conversion.segment(0).value(), kKamabokono);
    EXPECT_EQ(conversion.segment(1).value(), "印房");
  }

  // Test for segment motion again [SegmentFocusLeftEdge] [SegmentFocusLast]
  // The positions of "陰謀" and "印房" should be swapped.
  {
    Segments fixed_segments;
    SetKamaboko(&fixed_segments);
    EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);

    ASSERT_EQ(fixed_segments.segment(1).candidate(0).value, "陰謀");
    ASSERT_EQ(fixed_segments.segment(1).candidate(1).value, "印房");
    // swap the values.
    fixed_segments.mutable_segment(1)->mutable_candidate(0)->value.swap(
        fixed_segments.mutable_segment(1)->mutable_candidate(1)->value);
    ASSERT_EQ(fixed_segments.segment(1).candidate(0).value, "印房");
    ASSERT_EQ(fixed_segments.segment(1).candidate(1).value, "陰謀");
    EXPECT_CALL(*mock_converter, CommitSegmentValue(_, _, _))
        .WillRepeatedly(DoAll(SetArgPointee<0>(fixed_segments), Return(true)));
  }
  converter.SegmentFocusLeftEdge();
  {
    EXPECT_EQ(GetSegmentIndex(converter), 0);
    EXPECT_FALSE(IsCandidateListVisible(converter));
    converter.SegmentFocusLast();
    EXPECT_EQ(GetSegmentIndex(converter), 1);
    EXPECT_FALSE(IsCandidateListVisible(converter));
    converter.SetCandidateListVisible(true);

    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());
    EXPECT_TRUE(output.has_preedit());
    EXPECT_TRUE(output.has_candidate_window());

    const commands::CandidateWindow& candidate_window =
        output.candidate_window();
    EXPECT_EQ(candidate_window.focused_index(), 0);
    ASSERT_EQ(candidate_window.size(),
              3);  // two candidates + one t13n sub list.
    EXPECT_EQ(candidate_window.position(), 5);
    EXPECT_EQ(candidate_window.candidate(0).value(), "印房");

    EXPECT_EQ(candidate_window.candidate(1).value(), "陰謀");

    EXPECT_EQ(candidate_window.candidate(2).value(), "そのほかの文字種");

    const commands::Preedit& conversion = output.preedit();
    EXPECT_EQ(conversion.segment(0).value(), kKamabokono);
    EXPECT_EQ(conversion.segment(1).value(), "印房");
  }

  converter.Commit(*composer_, Context::default_instance());
  expected_indices.clear();
  {
    composer_->Reset();
    EXPECT_FALSE(IsCandidateListVisible(converter));
    EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);

    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_TRUE(output.has_result());
    EXPECT_FALSE(output.has_preedit());
    EXPECT_FALSE(output.has_candidate_window());

    const commands::Result& result = output.result();
    EXPECT_EQ(result.value(), "かまぼこの印房");
    EXPECT_EQ(result.key(), "かまぼこのいんぼう");
    EXPECT_FALSE(converter.IsActive());
  }
}

TEST_F(EngineConverterTest, Transliterations) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);
  composer_->InsertCharacterKeyAndPreedit("h", "く");
  composer_->InsertCharacterKeyAndPreedit("J", "ま");

  Segments segments;
  {  // Initialize segments.
    Segment* segment = segments.add_segment();
    segment->set_key("くま");
    segment->add_candidate()->value = "クマー";
  }
  FillT13Ns(&segments, composer_.get());
  EXPECT_CALL(*mock_converter, StartConversion(_, _))
      .WillOnce(DoAll(SetArgPointee<1>(segments), Return(true)));

  EXPECT_TRUE(converter.Convert(*composer_));
  std::vector<int> expected_indices;
  expected_indices.push_back(0);
  EXPECT_FALSE(IsCandidateListVisible(converter));
  EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);

  // Move to the t13n list.
  converter.CandidateNext(*composer_);
  expected_indices[0] = -1;
  EXPECT_TRUE(IsCandidateListVisible(converter));
  EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);

  commands::Output output;
  converter.FillOutput(*composer_, &output);
  EXPECT_FALSE(output.has_result());
  EXPECT_TRUE(output.has_preedit());
  EXPECT_TRUE(output.has_candidate_window());

  const commands::CandidateWindow& candidate_window = output.candidate_window();
  EXPECT_EQ(candidate_window.size(), 2);  // one candidate + one t13n sub list.
  EXPECT_EQ(candidate_window.focused_index(), 1);
  EXPECT_EQ(candidate_window.candidate(1).value(), "そのほかの文字種");

  std::vector<std::string> t13ns;
  composer_->GetTransliterations(&t13ns);

  EXPECT_TRUE(candidate_window.has_sub_candidate_window());
  EXPECT_EQ(candidate_window.sub_candidate_window().size(), t13ns.size());
  EXPECT_EQ(candidate_window.sub_candidate_window().candidate_size(), 9);

  for (size_t i = 0;
       i < candidate_window.sub_candidate_window().candidate_size(); ++i) {
    EXPECT_EQ(candidate_window.sub_candidate_window().candidate(i).value(),
              t13ns[i]);
  }
}

TEST_F(EngineConverterTest, T13NWithResegmentation) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);
  {
    Segments segments;
    Segment* segment = segments.add_segment();
    converter::Candidate* candidate;
    CHECK(segment);
    segment->set_key("かまぼこの");
    candidate = segment->add_candidate();
    CHECK(candidate);
    candidate->value = "かまぼこの";

    segment = segments.add_segment();
    CHECK(segment);
    segment->set_key("いんぼう");
    candidate = segment->add_candidate();
    CHECK(candidate);
    candidate->value = "いんぼう";

    InsertASCIISequence("kamabokonoinbou", composer_.get());
    FillT13Ns(&segments, composer_.get());
    EXPECT_CALL(*mock_converter, StartConversion(_, _))
        .WillOnce(DoAll(SetArgPointee<1>(segments), Return(true)));
  }
  EXPECT_TRUE(converter.Convert(*composer_));
  Mock::VerifyAndClearExpectations(&mock_converter);
  std::vector<int> expected_indices = {0, 0};
  EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);

  // Test for segment motion. [SegmentFocusRight]
  converter.SegmentFocusRight();
  EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);
  // Shrink segment
  {
    Segments segments;
    Segment* segment;
    converter::Candidate* candidate;

    segments.Clear();
    segment = segments.add_segment();

    segment->set_key("かまぼこの");
    candidate = segment->add_candidate();
    candidate->value = "かまぼこの";
    candidate = segment->add_candidate();
    candidate->value = "カマボコの";

    segment = segments.add_segment();
    segment->set_key("いんぼ");
    candidate = segment->add_candidate();
    candidate->value = "インボ";

    segment = segments.add_segment();
    segment->set_key("う");
    candidate = segment->add_candidate();
    candidate->value = "ウ";

    FillT13Ns(&segments, composer_.get());
    EXPECT_CALL(*mock_converter, ResizeSegment(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(segments), Return(true)));
  }
  converter.SegmentWidthShrink(*composer_);
  Mock::VerifyAndClearExpectations(&mock_converter);
  expected_indices.push_back(0);
  EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);

  // Convert to half katakana. Expected index should be 0.
  converter.ConvertToTransliteration(*composer_,
                                     transliteration::HALF_KATAKANA);
  expected_indices[0] = 0;
  {
    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());
    EXPECT_TRUE(output.has_preedit());
    const commands::Preedit& preedit = output.preedit();
    EXPECT_EQ(preedit.segment_size(), 3);
    EXPECT_EQ(preedit.segment(1).value(), "ｲﾝﾎﾞ");
    EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);
  }
}

TEST_F(EngineConverterTest, ConvertToHalfWidth) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);
  std::vector<int> expected_indices;
  composer_->InsertCharacterKeyAndPreedit("a", "あ");
  composer_->InsertCharacterKeyAndPreedit("b", "ｂ");
  composer_->InsertCharacterKeyAndPreedit("c", "ｃ");

  Segments segments;
  {  // Initialize segments.
    Segment* segment = segments.add_segment();
    segment->set_key("あｂｃ");
    segment->add_candidate()->value = "あべし";
  }
  FillT13Ns(&segments, composer_.get());
  EXPECT_CALL(*mock_converter, StartConversion(_, _))
      .WillOnce(DoAll(SetArgPointee<1>(segments), Return(true)));
  EXPECT_TRUE(converter.ConvertToHalfWidth(*composer_));
  expected_indices.push_back(0);
  EXPECT_FALSE(IsCandidateListVisible(converter));
  EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);

  {  // Make sure the output
    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());
    EXPECT_TRUE(output.has_preedit());
    EXPECT_FALSE(output.has_candidate_window());

    const commands::Preedit& conversion = output.preedit();
    EXPECT_EQ(conversion.segment_size(), 1);
    EXPECT_EQ(conversion.segment(0).value(), "ｱbc");
  }

  // Composition will be transliterated to "ａｂｃ".
  EXPECT_TRUE(converter.ConvertToTransliteration(*composer_,
                                                 transliteration::FULL_ASCII));
  EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);
  {  // Make sure the output
    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());
    EXPECT_TRUE(output.has_preedit());
    EXPECT_FALSE(output.has_candidate_window());

    const commands::Preedit& conversion = output.preedit();
    EXPECT_EQ(conversion.segment_size(), 1);
    EXPECT_EQ(conversion.segment(0).value(), "ａｂｃ");
  }

  EXPECT_TRUE(converter.ConvertToHalfWidth(*composer_));
  EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);
  EXPECT_FALSE(IsCandidateListVisible(converter));
  {  // Make sure the output
    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());
    EXPECT_TRUE(output.has_preedit());
    EXPECT_FALSE(output.has_candidate_window());

    const commands::Preedit& conversion = output.preedit();
    EXPECT_EQ(conversion.segment_size(), 1);
    EXPECT_EQ(conversion.segment(0).value(), "abc");
  }

  EXPECT_TRUE(converter.ConvertToHalfWidth(*composer_));
  EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);
  EXPECT_FALSE(IsCandidateListVisible(converter));
  {  // Make sure the output
    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());
    EXPECT_TRUE(output.has_preedit());
    EXPECT_FALSE(output.has_candidate_window());

    const commands::Preedit& conversion = output.preedit();
    EXPECT_EQ(conversion.segment_size(), 1);
    EXPECT_EQ(conversion.segment(0).value(), "ABC");
  }
}

TEST_F(EngineConverterTest, ConvertToHalfWidth2) {
  // http://b/2517514
  // ConvertToHalfWidth converts punctuations differently w/ or w/o kana.
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);
  composer_->InsertCharacterKeyAndPreedit("q", "ｑ");
  composer_->InsertCharacterKeyAndPreedit(",", "、");
  composer_->InsertCharacterKeyAndPreedit(".", "。");

  Segments segments;
  {  // Initialize segments.
    Segment* segment = segments.add_segment();
    segment->set_key("ｑ、。");
    segment->add_candidate()->value = "q,.";
    segment->add_candidate()->value = "q､｡";
  }
  FillT13Ns(&segments, composer_.get());
  EXPECT_CALL(*mock_converter, StartConversion(_, _))
      .WillOnce(DoAll(SetArgPointee<1>(segments), Return(true)));
  EXPECT_TRUE(converter.ConvertToHalfWidth(*composer_));
  std::vector<int> expected_indices;
  expected_indices.push_back(0);
  EXPECT_FALSE(IsCandidateListVisible(converter));
  EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);

  {  // Make sure the output
    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());
    EXPECT_TRUE(output.has_preedit());
    EXPECT_FALSE(output.has_candidate_window());

    const commands::Preedit& conversion = output.preedit();
    EXPECT_EQ(conversion.segment_size(), 1);
    EXPECT_EQ(conversion.segment(0).value(), "q､｡");
  }
}

TEST_F(EngineConverterTest, SwitchKanaTypeFromCompositionMode) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);
  composer_->InsertCharacterKeyAndPreedit("a", "あ");
  composer_->InsertCharacterKeyAndPreedit("b", "ｂ");
  composer_->InsertCharacterKeyAndPreedit("c", "ｃ");

  Segments segments;
  {  // Initialize segments.
    Segment* segment = segments.add_segment();
    segment->set_key("あｂｃ");
    segment->add_candidate()->value = "あべし";
  }
  FillT13Ns(&segments, composer_.get());
  EXPECT_CALL(*mock_converter, StartConversion(_, _))
      .WillOnce(DoAll(SetArgPointee<1>(segments), Return(true)));
  EXPECT_TRUE(converter.SwitchKanaType(*composer_));
  std::vector<int> expected_indices = {0};
  EXPECT_FALSE(IsCandidateListVisible(converter));
  EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);

  {  // Make sure the output
    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());
    EXPECT_TRUE(output.has_preedit());
    EXPECT_FALSE(output.has_candidate_window());

    const commands::Preedit& conversion = output.preedit();
    EXPECT_EQ(conversion.segment_size(), 1);
    EXPECT_EQ(conversion.segment(0).value(), "アｂｃ");
  }

  EXPECT_TRUE(converter.SwitchKanaType(*composer_));
  EXPECT_FALSE(IsCandidateListVisible(converter));
  EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);
  {
    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());
    EXPECT_TRUE(output.has_preedit());
    EXPECT_FALSE(output.has_candidate_window());

    const commands::Preedit& conversion = output.preedit();
    EXPECT_EQ(conversion.segment_size(), 1);
    EXPECT_EQ(conversion.segment(0).value(), "ｱbc");
  }

  EXPECT_TRUE(converter.SwitchKanaType(*composer_));
  EXPECT_FALSE(IsCandidateListVisible(converter));
  EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);
  {
    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());
    EXPECT_TRUE(output.has_preedit());
    EXPECT_FALSE(output.has_candidate_window());

    const commands::Preedit& conversion = output.preedit();
    EXPECT_EQ(conversion.segment_size(), 1);
    EXPECT_EQ(conversion.segment(0).value(), "あｂｃ");
  }
}

TEST_F(EngineConverterTest, SwitchKanaTypeFromConversionMode) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);
  composer_->EditErase();
  composer_->InsertCharacterKeyAndPreedit("ka", "か");
  composer_->InsertCharacterKeyAndPreedit("n", "ん");
  composer_->InsertCharacterKeyAndPreedit("ji", "じ");

  Segments segments;
  {  // Initialize segments.
    Segment* segment = segments.add_segment();
    segment->set_key("かんじ");
    segment->add_candidate()->value = "漢字";
  }
  FillT13Ns(&segments, composer_.get());
  EXPECT_CALL(*mock_converter, StartConversion(_, _))
      .WillOnce(DoAll(SetArgPointee<1>(segments), Return(true)));
  EXPECT_TRUE(converter.Convert(*composer_));
  std::vector<int> expected_indices = {0};
  EXPECT_FALSE(IsCandidateListVisible(converter));
  EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);

  {  // Make sure the output
    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());
    EXPECT_TRUE(output.has_preedit());
    EXPECT_FALSE(output.has_candidate_window());

    const commands::Preedit& conversion = output.preedit();
    EXPECT_EQ(conversion.segment_size(), 1);
    EXPECT_EQ(conversion.segment(0).value(), "漢字");
  }

  EXPECT_TRUE(converter.SwitchKanaType(*composer_));
  EXPECT_FALSE(IsCandidateListVisible(converter));
  EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);
  {
    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());
    EXPECT_TRUE(output.has_preedit());
    EXPECT_FALSE(output.has_candidate_window());

    const commands::Preedit& conversion = output.preedit();
    EXPECT_EQ(conversion.segment_size(), 1);
    EXPECT_EQ(conversion.segment(0).value(), "かんじ");
  }

  EXPECT_TRUE(converter.SwitchKanaType(*composer_));
  EXPECT_FALSE(IsCandidateListVisible(converter));
  EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);
  {
    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());
    EXPECT_TRUE(output.has_preedit());
    EXPECT_FALSE(output.has_candidate_window());

    const commands::Preedit& conversion = output.preedit();
    EXPECT_EQ(conversion.segment_size(), 1);
    EXPECT_EQ(conversion.segment(0).value(), "カンジ");
  }

  EXPECT_TRUE(converter.SwitchKanaType(*composer_));
  EXPECT_FALSE(IsCandidateListVisible(converter));
  EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);
  {
    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());
    EXPECT_TRUE(output.has_preedit());
    EXPECT_FALSE(output.has_candidate_window());

    const commands::Preedit& conversion = output.preedit();
    EXPECT_EQ(conversion.segment_size(), 1);
    EXPECT_EQ(conversion.segment(0).value(), "ｶﾝｼﾞ");
  }

  EXPECT_TRUE(converter.SwitchKanaType(*composer_));
  EXPECT_FALSE(IsCandidateListVisible(converter));
  EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);
  {
    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());
    EXPECT_TRUE(output.has_preedit());
    EXPECT_FALSE(output.has_candidate_window());

    const commands::Preedit& conversion = output.preedit();
    EXPECT_EQ(conversion.segment_size(), 1);
    EXPECT_EQ(conversion.segment(0).value(), "かんじ");
  }
}

TEST_F(EngineConverterTest, ResizeSegmentsFailedInSwitchKanaType) {
  const auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);

  // ResizeSegments() is called when the conversion result has multiple
  // segments. Let the underlying converter return the result with two segments.
  Segments segments;
  AddSegmentWithSingleCandidate(&segments, "かな", "カナ");
  AddSegmentWithSingleCandidate(&segments, "たいぷ", "タイプ");
  EXPECT_CALL(*mock_converter, StartConversion(_, _))
      .WillOnce(DoAll(SetArgPointee<1>(segments), Return(true)));

  // Suppose that ResizeSegments() fails for "かな|たいぷ" (UTF8-length is 5).
  static constexpr std::array<const uint8_t, 1> new_sizes_array = {5};
  static constexpr absl::Span<const uint8_t> new_sizes = new_sizes_array;
  EXPECT_CALL(*mock_converter, ResizeSegments(_, _, 0, new_sizes))
      .WillOnce(Return(false));

  // FocusSegmentValue() is called in the last step.
  EXPECT_CALL(*mock_converter, FocusSegmentValue(_, 0, 0))
      .WillOnce(Return(true));

  // Calling SwitchKanaType() with the above set up doesn't crash.
  EXPECT_TRUE(converter.SwitchKanaType(*composer_));
}

TEST_F(EngineConverterTest, CommitFirstSegment) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);
  Segments segments;
  SetKamaboko(&segments);
  composer_->InsertCharacterPreedit("かまぼこ");
  FillT13Ns(&segments, composer_.get());
  EXPECT_CALL(*mock_converter, StartConversion(_, _))
      .WillOnce(DoAll(SetArgPointee<1>(segments), Return(true)));

  const std::string kKamabokono = "かまぼこの";
  const std::string kInbou = "いんぼう";

  composer_->InsertCharacterPreedit(kKamabokono + kInbou);
  EXPECT_TRUE(converter.Convert(*composer_));
  Mock::VerifyAndClearExpectations(&mock_converter);
  std::vector<int> expected_indices = {0, 0};
  EXPECT_FALSE(IsCandidateListVisible(converter));
  EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);

  {  // Check the conversion.
    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());
    EXPECT_TRUE(output.has_preedit());
    EXPECT_FALSE(output.has_candidate_window());

    const commands::Preedit& conversion = output.preedit();
    EXPECT_EQ(conversion.segment(0).value(), kKamabokono);
    EXPECT_EQ(conversion.segment(1).value(), "陰謀");
  }

  EXPECT_CALL(*mock_converter, FocusSegmentValue(_, 0, 1))
      .WillOnce(Return(true));
  converter.CandidateNext(*composer_);
  Mock::VerifyAndClearExpectations(&mock_converter);
  expected_indices[0] += 1;
  EXPECT_TRUE(IsCandidateListVisible(converter));
  EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);

  {  // Check the conversion.
    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());
    EXPECT_TRUE(output.has_preedit());
    EXPECT_TRUE(output.has_candidate_window());

    const commands::Preedit& conversion = output.preedit();
    EXPECT_EQ(conversion.segment(0).value(), "カマボコの");
    EXPECT_EQ(conversion.segment(1).value(), "陰謀");
  }

  {  // Initialization of CommitSegments.
    Segments segments_after_submit;
    Segment* segment = segments_after_submit.add_segment();
    segment->set_key("いんぼう");
    segment->add_candidate()->value = "陰謀";
    segment->add_candidate()->value = "印房";
    EXPECT_CALL(*mock_converter, CommitSegments(_, _))
        .WillOnce(DoAll(SetArgPointee<0>(segments), Return(true)));
  }
  size_t size;
  converter.CommitFirstSegment(*composer_, Context::default_instance(), &size);
  expected_indices.erase(expected_indices.begin(),
                         expected_indices.begin() + 1);
  EXPECT_FALSE(IsCandidateListVisible(converter));
  EXPECT_EQ(size, Util::CharsLen(kKamabokono));
  EXPECT_TRUE(converter.IsActive());
  EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);
}

TEST_F(EngineConverterTest, CommitHeadToFocusedSegments) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);
  const std::string kIberiko = "いべりこ";
  const std::string kNekowo = "ねこを";
  const std::string kItadaita = "いただいた";
  {  // Three segments as the result of conversion.
    Segments segments;
    Segment* segment;
    converter::Candidate* candidate;

    segment = segments.add_segment();
    segment->set_key(kIberiko);
    candidate = segment->add_candidate();
    candidate->value = "イベリコ";

    segment = segments.add_segment();
    segment->set_key(kNekowo);
    candidate = segment->add_candidate();
    candidate->value = "猫を";

    segment = segments.add_segment();
    segment->set_key(kItadaita);
    candidate = segment->add_candidate();
    candidate->value = "頂いた";

    EXPECT_CALL(*mock_converter, StartConversion(_, _))
        .WillOnce(DoAll(SetArgPointee<1>(segments), Return(true)));
  }

  composer_->InsertCharacterPreedit(kIberiko + kNekowo + kItadaita);
  EXPECT_TRUE(converter.Convert(*composer_));
  Mock::VerifyAndClearExpectations(&mock_converter);
  // Here [イベリコ]|猫を|頂いた

  EXPECT_CALL(*mock_converter, CommitSegmentValue(_, 0, 0))
      .WillOnce(Return(true));
  converter.SegmentFocusRight();
  // Here イベリコ|[猫を]|頂いた

  {  // Initialization of CommitSegments.
    Segments segments;
    Segment* segment;
    converter::Candidate* candidate;

    segment = segments.add_segment();
    segment->set_key(kItadaita);
    candidate = segment->add_candidate();
    candidate->value = "頂いた";
    EXPECT_CALL(*mock_converter, CommitSegments(_, _))
        .WillOnce(DoAll(SetArgPointee<0>(segments), Return(true)));
  }
  size_t size;
  converter.CommitHeadToFocusedSegments(*composer_, Context::default_instance(),
                                        &size);
  // Here 頂いた
  EXPECT_FALSE(IsCandidateListVisible(converter));
  EXPECT_EQ(size, Util::CharsLen(kIberiko + kNekowo));
  EXPECT_TRUE(converter.IsActive());
}

TEST_F(EngineConverterTest, CommitHeadToFocusedSegmentsAtLastSegment) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);

  Segments segments;
  SetKamaboko(&segments);
  EXPECT_CALL(*mock_converter, StartConversion(_, _))
      .WillOnce(DoAll(SetArgPointee<1>(segments), Return(true)));

  const std::string kKamabokono = "かまぼこの";
  const std::string kInbou = "いんぼう";

  composer_->InsertCharacterPreedit(kKamabokono + kInbou);
  EXPECT_TRUE(converter.Convert(*composer_));
  // Here [かまぼこの]|陰謀

  converter.SegmentFocusRight();
  // Here かまぼこの|[陰謀]

  size_t size;
  // All the segments should be committed.
  converter.CommitHeadToFocusedSegments(*composer_, Context::default_instance(),
                                        &size);
  EXPECT_FALSE(IsCandidateListVisible(converter));
  EXPECT_EQ(size, 0);
  EXPECT_FALSE(converter.IsActive());
}

TEST_F(EngineConverterTest, CommitConvertedBracketPairText) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);
  Segments segments;
  const std::string kKakko = "かっこ";

  {  // Initialize segments.
    segments.Clear();
    Segment* segment = segments.add_segment();
    converter::Candidate* candidate;
    segment->set_key(kKakko);
    candidate = segment->add_candidate();
    candidate->value = "（）";
    candidate->key = kKakko;
    candidate->content_key = kKakko;
    candidate = segment->add_candidate();
    candidate->value = "「」";
    candidate->key = kKakko;
    candidate->content_key = kKakko;
  }

  composer_->InsertCharacterPreedit(kKakko);

  // Suggestion
  EXPECT_CALL(*mock_converter, StartPrediction(_, _))
      .WillOnce(DoAll(SetArgPointee<1>(segments), Return(true)));
  ASSERT_TRUE(converter.Suggest(*composer_, Context::default_instance()));
  std::vector<int> expected_indices = {0};
  EXPECT_TRUE(IsCandidateListVisible(converter));
  EXPECT_TRUE(converter.IsActive());

  {  // Check the candidate list.
    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());
    EXPECT_TRUE(output.has_preedit());
    EXPECT_TRUE(output.has_candidate_window());

    const commands::Preedit& preedit = output.preedit();
    EXPECT_EQ(preedit.segment_size(), 1);
    EXPECT_EQ(preedit.segment(0).value(), kKakko);

    const commands::CandidateWindow& candidate_window =
        output.candidate_window();
    EXPECT_EQ(candidate_window.size(), 2);
    EXPECT_EQ(candidate_window.candidate(0).value(), "（）");
    EXPECT_FALSE(candidate_window.has_focused_index());
    EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);
  }

  EXPECT_CALL(*mock_converter, CommitSegmentValue(_, 0, 1))
      .WillOnce(Return(true));
  // FinishConversion is expected to return empty Segments.
  EXPECT_CALL(*mock_converter, FinishConversion(_, _))
      .WillOnce(SetArgPointee<1>(Segments()));

  size_t committed_key_size = 0;
  converter.CommitSuggestionByIndex(1, *composer_, Context::default_instance(),
                                    &committed_key_size);
  expected_indices.clear();
  composer_->Reset();
  EXPECT_FALSE(IsCandidateListVisible(converter));
  EXPECT_FALSE(converter.IsActive());
  EXPECT_EQ(committed_key_size, EngineConverter::kConsumedAllCharacters);

  {  // Check the result
    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_TRUE(output.has_result());
    EXPECT_FALSE(output.has_preedit());
    EXPECT_FALSE(output.has_candidate_window());

    const commands::Result& result = output.result();
    EXPECT_EQ(result.value(), "「」");
    EXPECT_EQ(result.key(), kKakko);
    EXPECT_EQ(result.cursor_offset(), -1);
    EXPECT_EQ(GetState(converter), EngineConverterInterface::COMPOSITION);
    EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);
  }

  EXPECT_FALSE(converter.IsActive());
}

TEST_F(EngineConverterTest, CommitPreedit) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);

  std::vector<int> expected_indices;
  EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);
  composer_->InsertCharacterPreedit(kChars_Aiueo);
  EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);
  converter.CommitPreedit(*composer_, Context::default_instance());
  composer_->Reset();
  EXPECT_FALSE(IsCandidateListVisible(converter));
  EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);

  {  // Check the result
    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_TRUE(output.has_result());
    EXPECT_FALSE(output.has_preedit());
    EXPECT_FALSE(output.has_candidate_window());

    const commands::Result& result = output.result();
    EXPECT_EQ(result.value(), kChars_Aiueo);
    EXPECT_EQ(result.key(), kChars_Aiueo);

    EXPECT_EQ(result.tokens_size(), 1);
    EXPECT_EQ(result.tokens(0).key(), kChars_Aiueo);
    EXPECT_EQ(result.tokens(0).value(), kChars_Aiueo);
    EXPECT_EQ(result.tokens(0).lid(), -1);
    EXPECT_EQ(result.tokens(0).rid(), -1);
  }
  EXPECT_FALSE(converter.IsActive());
}

TEST_F(EngineConverterTest, CommitPreeditBracketPairText) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);

  std::vector<int> expected_indices;
  EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);
  composer_->InsertCharacterPreedit("（）");
  EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);
  converter.CommitPreedit(*composer_, Context::default_instance());
  composer_->Reset();
  EXPECT_FALSE(IsCandidateListVisible(converter));
  EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);

  {  // Check the result
    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_TRUE(output.has_result());
    EXPECT_FALSE(output.has_preedit());
    EXPECT_FALSE(output.has_candidate_window());

    const commands::Result& result = output.result();
    EXPECT_EQ(result.value(), "（）");
    EXPECT_EQ(result.key(), "（）");
    EXPECT_EQ(result.cursor_offset(), -1);
  }

  EXPECT_FALSE(converter.IsActive());
}

TEST_F(EngineConverterTest, ClearSegmentsBeforeSuggest) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);

  // Call Suggest() and sets the segments of converter to the following one.
  const Segments& segments = GetSegmentsTest();
  EXPECT_CALL(*mock_converter, StartPrediction(_, _))
      .WillOnce(DoAll(SetArgPointee<1>(segments), Return(true)));
  composer_->InsertCharacterPreedit("てすと");
  EXPECT_TRUE(converter.Suggest(*composer_, Context::default_instance()));
  Mock::VerifyAndClearExpectations(&mock_converter);

  // Then, call Suggest() again. It should be called with brand new segments.
  Segments empty;
  EXPECT_CALL(*mock_converter,
              StartPrediction(_, Pointee(EqualsSegments(empty))))
      .WillOnce(DoAll(SetArgPointee<1>(segments), Return(true)));
  EXPECT_TRUE(converter.Suggest(*composer_, Context::default_instance()));
}

TEST_F(EngineConverterTest, PredictIsNotCalledInPredictionState) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);

  // Call Predict() and sets the segments of converter to the following one. By
  // calling Predict(), converter enters PREDICTION state.
  const Segments& segments = GetSegmentsTest();
  EXPECT_CALL(*mock_converter, StartPredictionWithPreviousSuggestion(_, _, _))
      .WillOnce(DoAll(SetArgPointee<2>(segments), Return(true)));
  composer_->InsertCharacterPreedit("てすと");
  EXPECT_TRUE(converter.Predict(*composer_));
  Mock::VerifyAndClearExpectations(&mock_converter);

  // Then, call Predict() again. PredictForRequest() is not called.
  EXPECT_CALL(*mock_converter, StartPredictionWithPreviousSuggestion(_, _, _))
      .Times(0);
  EXPECT_CALL(*mock_converter, PrependCandidates(_, _, _))
      .WillOnce(SetArgPointee<2>(segments));
  EXPECT_TRUE(converter.Predict(*composer_));
}

TEST_F(EngineConverterTest, CommitSuggestionByIndex) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);
  Segments segments;
  {  // Initialize mock segments for suggestion
    Segment* segment = segments.add_segment();
    converter::Candidate* candidate;
    segment->set_key(kChars_Mo);
    candidate = segment->add_candidate();
    candidate->value = kChars_Mozukusu;
    candidate->key = kChars_Mozukusu;
    candidate->content_key = kChars_Mozukusu;
    candidate = segment->add_candidate();
    candidate->value = kChars_Momonga;
    candidate->key = kChars_Momonga;
    candidate->content_key = kChars_Momonga;
  }
  composer_->InsertCharacterPreedit(kChars_Mo);

  // Suggestion
  EXPECT_CALL(*mock_converter, StartPrediction(_, _))
      .WillOnce(DoAll(SetArgPointee<1>(segments), Return(true)));
  ASSERT_TRUE(converter.Suggest(*composer_, Context::default_instance()));
  std::vector<int> expected_indices = {0};
  EXPECT_TRUE(IsCandidateListVisible(converter));
  EXPECT_TRUE(converter.IsActive());

  {  // Check the candidate list
    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());
    EXPECT_TRUE(output.has_preedit());
    EXPECT_TRUE(output.has_candidate_window());

    const commands::Preedit& preedit = output.preedit();
    EXPECT_EQ(preedit.segment_size(), 1);
    EXPECT_EQ(preedit.segment(0).value(), kChars_Mo);

    const commands::CandidateWindow& candidate_window =
        output.candidate_window();
    EXPECT_EQ(candidate_window.size(), 2);
    EXPECT_EQ(candidate_window.candidate(0).value(), kChars_Mozukusu);
    EXPECT_FALSE(candidate_window.has_focused_index());
    EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);
  }

  EXPECT_CALL(*mock_converter, CommitSegmentValue(_, 0, 1))
      .WillOnce(Return(true));
  // FinishConversion is expected to return empty Segments.
  EXPECT_CALL(*mock_converter, FinishConversion(_, _))
      .WillOnce(SetArgPointee<1>(Segments()));

  size_t committed_key_size = 0;
  converter.CommitSuggestionByIndex(1, *composer_, Context::default_instance(),
                                    &committed_key_size);
  expected_indices.clear();
  composer_->Reset();
  EXPECT_FALSE(IsCandidateListVisible(converter));
  EXPECT_FALSE(converter.IsActive());
  EXPECT_EQ(committed_key_size, EngineConverter::kConsumedAllCharacters);

  {  // Check the result
    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_TRUE(output.has_result());
    EXPECT_FALSE(output.has_preedit());
    EXPECT_FALSE(output.has_candidate_window());

    const commands::Result& result = output.result();
    EXPECT_EQ(result.value(), kChars_Momonga);
    EXPECT_EQ(result.key(), kChars_Momonga);
    EXPECT_EQ(GetState(converter), EngineConverterInterface::COMPOSITION);
    EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);
  }
}

TEST_F(EngineConverterTest, CommitSuggestionById) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);
  Segments segments;
  {  // Initialize mock segments for suggestion
    Segment* segment = segments.add_segment();
    converter::Candidate* candidate;
    segment->set_key(kChars_Mo);
    candidate = segment->add_candidate();
    candidate->value = kChars_Mozukusu;
    candidate->key = kChars_Mozukusu;
    candidate->content_key = kChars_Mozukusu;
    candidate = segment->add_candidate();
    candidate->value = kChars_Momonga;
    candidate->key = kChars_Momonga;
    candidate->content_key = kChars_Momonga;
  }
  composer_->InsertCharacterPreedit(kChars_Mo);

  // Suggestion
  EXPECT_CALL(*mock_converter, StartPrediction(_, _))
      .WillOnce(DoAll(SetArgPointee<1>(segments), Return(true)));
  EXPECT_TRUE(converter.Suggest(*composer_, Context::default_instance()));
  Mock::VerifyAndClearExpectations(&mock_converter);

  std::vector<int> expected_indices = {0};
  EXPECT_TRUE(IsCandidateListVisible(converter));
  EXPECT_TRUE(converter.IsActive());
  EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);

  // FinishConversion is expected to return empty Segments.
  constexpr int kCandidateIndex = 1;
  EXPECT_CALL(*mock_converter, FinishConversion(_, _))
      .WillOnce(SetArgPointee<1>(Segments()));
  EXPECT_CALL(*mock_converter, CommitSegmentValue(_, 0, kCandidateIndex))
      .WillOnce(DoAll(SetArgPointee<0>(segments), Return(true)));
  size_t committed_key_size = 0;
  converter.CommitSuggestionById(kCandidateIndex, *composer_,
                                 Context::default_instance(),
                                 &committed_key_size);
  Mock::VerifyAndClearExpectations(&mock_converter);
  expected_indices.clear();
  composer_->Reset();
  EXPECT_FALSE(IsCandidateListVisible(converter));
  EXPECT_FALSE(converter.IsActive());
  EXPECT_EQ(committed_key_size, EngineConverter::kConsumedAllCharacters);
  EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);

  {  // Check the result
    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_TRUE(output.has_result());
    EXPECT_FALSE(output.has_preedit());
    EXPECT_FALSE(output.has_candidate_window());

    const commands::Result& result = output.result();
    EXPECT_EQ(result.value(), kChars_Momonga);
    EXPECT_EQ(result.key(), kChars_Momonga);
    EXPECT_EQ(GetState(converter), EngineConverterInterface::COMPOSITION);
  }
}

TEST_F(EngineConverterTest, PartialPrediction) {
  request_test_util::FillMobileRequest(request_.get());
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);
  Segments segments1, segments2;
  Segments suggestion_segments;
  const std::string kChars_Kokode = "ここで";
  const std::string kChars_Hakimonowo = "はきものを";

  {  // Initialize mock segments for partial prediction
    Segment* segment = segments1.add_segment();
    converter::Candidate* candidate;
    segment->set_key(kChars_Kokode);
    candidate = segment->add_candidate();
    candidate->value = "此処では";
    candidate->key = kChars_Kokode;
    candidate->content_key = kChars_Kokode;
    candidate->attributes = converter::Attribute::PARTIALLY_KEY_CONSUMED;
    candidate->consumed_key_size = Util::CharsLen(kChars_Kokode);
  }

  // Suggestion that matches to the same key by its prefix.
  // Should not be used by partial prediction.
  {
    Segment* segment = suggestion_segments.add_segment();
    converter::Candidate* candidate;
    segment->set_key(kChars_Kokode);
    candidate = segment->add_candidate();
    candidate->value = "ここでは着物を";
    candidate->key = "ここではきものを";
    candidate->content_key = candidate->key;
    candidate = segment->add_candidate();
  }

  {  // Initialize mock segments for prediction
    Segment* segment = segments2.add_segment();
    converter::Candidate* candidate;
    segment->set_key(kChars_Hakimonowo);
    candidate = segment->add_candidate();
    candidate->value = "此処では";
    candidate->key = kChars_Hakimonowo;
    candidate->content_key = kChars_Hakimonowo;
  }

  // "ここではきものを|"    ("|" is cursor position)
  composer_->InsertCharacterPreedit(kChars_Kokode + kChars_Hakimonowo);
  composer_->MoveCursorToEnd();
  // Prediction for "ここではきものを".
  EXPECT_CALL(*mock_converter, StartPrediction(_, _))
      .WillOnce(DoAll(SetArgPointee<1>(suggestion_segments), Return(true)));
  EXPECT_TRUE(converter.Suggest(*composer_, Context::default_instance()));
  Mock::VerifyAndClearExpectations(&mock_converter);
  std::vector<int> expected_indices = {0};
  EXPECT_TRUE(IsCandidateListVisible(converter));
  EXPECT_TRUE(converter.IsActive());
  EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);

  // "|ここではきものを"    ("|" is cursor position)
  composer_->MoveCursorTo(0);

  // Prediction for "ここではきものを".
  EXPECT_CALL(*mock_converter, StartPrediction(_, _))
      .WillOnce(DoAll(SetArgPointee<1>(suggestion_segments), Return(true)));
  EXPECT_TRUE(converter.Suggest(*composer_, Context::default_instance()));
  Mock::VerifyAndClearExpectations(&mock_converter);
  EXPECT_TRUE(IsCandidateListVisible(converter));
  EXPECT_TRUE(converter.IsActive());
  EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);

  // "ここで|はきものを"    ("|" is cursor position)
  composer_->MoveCursorTo(3);

  // Partial prediction for "ここで"
  EXPECT_CALL(*mock_converter, StartPrediction(_, _))
      .WillOnce(DoAll(SetArgPointee<1>(segments1), Return(true)));
  EXPECT_TRUE(converter.Suggest(*composer_, Context::default_instance()));
  Mock::VerifyAndClearExpectations(&mock_converter);
  EXPECT_TRUE(IsCandidateListVisible(converter));
  EXPECT_TRUE(converter.IsActive());
  EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);

  // commit partial suggestion
  size_t committed_key_size = 0;
  EXPECT_CALL(*mock_converter,
              CommitPartialSuggestionSegmentValue(_, _, _, _, _))
      .WillOnce(DoAll(SetArgPointee<0>(segments2), Return(true)));
  converter.CommitSuggestionById(0, *composer_, Context::default_instance(),
                                 &committed_key_size);
  Mock::VerifyAndClearExpectations(&mock_converter);
  EXPECT_EQ(committed_key_size, Util::CharsLen(kChars_Kokode));
  // Indices should be {0} since there is another segment.
  EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);

  {  // Check the result
    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_TRUE(output.has_result());

    const commands::Result& result = output.result();
    EXPECT_EQ(result.value(), "此処では");
    EXPECT_EQ(result.key(), kChars_Kokode);
    EXPECT_EQ(GetState(converter), EngineConverterInterface::SUGGESTION);
  }
}

TEST_F(EngineConverterTest, SuggestAndPredict) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);
  const Segments suggest_segments = []() {
    // Initialize mock segments for suggestion
    Segments segments;
    Segment* segment = segments.add_segment();
    converter::Candidate* candidate;
    segment->set_key(kChars_Mo);
    candidate = segment->add_candidate();
    candidate->value = kChars_Mozukusu;
    candidate->content_key = kChars_Mozukusu;
    candidate = segment->add_candidate();
    candidate->value = kChars_Momonga;
    candidate->content_key = kChars_Momonga;
    return segments;
  }();
  composer_->InsertCharacterPreedit(kChars_Mo);

  // Action 1. Suggest()
  // ["もずくす", "ももんが"] will be suggested.
  EXPECT_CALL(*mock_converter, StartPrediction(_, _))
      .WillOnce(DoAll(SetArgPointee<1>(suggest_segments), Return(true)));
  EXPECT_TRUE(converter.Suggest(*composer_, Context::default_instance()));

  Mock::VerifyAndClearExpectations(&mock_converter);

  std::vector<int> expected_indices = {0};
  EXPECT_TRUE(IsCandidateListVisible(converter));
  EXPECT_TRUE(converter.IsActive());
  EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);

  {  // Check the candidate list
    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());
    EXPECT_TRUE(output.has_preedit());
    EXPECT_TRUE(output.has_candidate_window());
    EXPECT_TRUE(output.candidate_window().has_footer());
#if defined(CHANNEL_DEV) && defined(GOOGLE_JAPANESE_INPUT_BUILD)
    EXPECT_FALSE(output.candidate_window().footer().has_label());
    EXPECT_TRUE(output.candidate_window().footer().has_sub_label());
#else   // CHANNEL_DEV && GOOGLE_JAPANESE_INPUT_BUILD
    EXPECT_TRUE(output.candidate_window().footer().has_label());
    EXPECT_FALSE(output.candidate_window().footer().has_sub_label());
#endif  // CHANNEL_DEV && GOOGLE_JAPANESE_INPUT_BUILD
    EXPECT_FALSE(output.candidate_window().footer().index_visible());
    EXPECT_FALSE(output.candidate_window().footer().logo_visible());

    const commands::CandidateWindow& candidate_window =
        output.candidate_window();
    EXPECT_EQ(candidate_window.size(), 2);
    EXPECT_EQ(candidate_window.candidate(0).value(), kChars_Mozukusu);
    EXPECT_FALSE(candidate_window.has_focused_index());
  }

  // Action 2. Predict()
  // ["もずくす", "ももんが"] will be shown as prediction.
  // "もずくす" is focused.
  //
  // Since Suggest() was called, the converter stores its results internally. In
  // this case, the prediction is not triggered.
  EXPECT_CALL(*mock_converter, StartPredictionWithPreviousSuggestion(_, _, _))
      .Times(0);
  // b/417900855: Post processing should be called for the previous segments.
  // We have different rewriter set for prediction and suggestion.
  EXPECT_CALL(*mock_converter,
              PrependCandidates(
                  _, EqualsSegment(suggest_segments.conversion_segment(0)), _))
      .WillOnce(SetArgPointee<2>(suggest_segments));
  EXPECT_TRUE(converter.Predict(*composer_));

  Mock::VerifyAndClearExpectations(&mock_converter);

  EXPECT_TRUE(IsCandidateListVisible(converter));
  EXPECT_TRUE(converter.IsActive());
  EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);
  {
    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());
    EXPECT_TRUE(output.has_preedit());
    EXPECT_TRUE(output.has_candidate_window());
    EXPECT_FALSE(output.candidate_window().footer().has_label());
    EXPECT_TRUE(output.candidate_window().footer().index_visible());
    EXPECT_TRUE(output.candidate_window().footer().logo_visible());

    // Check the conversion
    const commands::Preedit& conversion = output.preedit();
    EXPECT_EQ(conversion.segment_size(), 1);
    EXPECT_EQ(conversion.segment(0).value(), kChars_Mozukusu);

    // Check the candidate list
    const commands::CandidateWindow& candidate_window =
        output.candidate_window();
    // Candidates should be the same as suggestion
    EXPECT_EQ(candidate_window.size(), 2);
    EXPECT_EQ(candidate_window.candidate(0).value(), kChars_Mozukusu);
    EXPECT_EQ(candidate_window.candidate(1).value(), kChars_Momonga);
    EXPECT_TRUE(candidate_window.has_focused_index());
    EXPECT_EQ(candidate_window.focused_index(), 0);
  }

  // Action 3. CandidateNext()
  // "ももんが" is focused.

  EXPECT_CALL(*mock_converter, FocusSegmentValue(_, 0, 1))
      .WillOnce(Return(true));
  converter.CandidateNext(*composer_);

  Mock::VerifyAndClearExpectations(&mock_converter);

  // Action 4. CandidateNext()
  // Prediction is expanded.
  // converter returns:
  // ["もずくす", "ももんが", "もずく", "ももんが", "モンドリアン"]
  // output is deduped:
  // ["もずくす", "ももんが", "もずく", "モンドリアン"]
  // "もずく" is focused.
  EXPECT_CALL(*mock_converter, FocusSegmentValue(_, 0, 2))
      .WillOnce(Return(true));
  const Segments expanded_segments = []() {
    Segments segments;
    // Initialize mock segments for prediction
    Segment* segment = segments.add_segment();
    converter::Candidate* candidate;
    segment->set_key(kChars_Mo);
    // From previous suggestion
    candidate = segment->add_candidate();
    candidate->value = kChars_Mozukusu;
    candidate->content_key = kChars_Mozukusu;
    candidate = segment->add_candidate();
    candidate->value = kChars_Momonga;
    candidate->content_key = kChars_Momonga;
    // From prediction
    candidate = segment->add_candidate();
    candidate->value = kChars_Mozuku;
    candidate->content_key = kChars_Mozuku;
    candidate = segment->add_candidate();
    candidate->value = kChars_Momonga;
    candidate->content_key = kChars_Momonga;
    candidate = segment->add_candidate();
    candidate->value = "モンドリアン";
    candidate->content_key = "もんどりあん";
    return segments;
  }();
  EXPECT_CALL(*mock_converter,
              StartPredictionWithPreviousSuggestion(
                  _, EqualsSegment(suggest_segments.conversion_segment(0)), _))
      .WillRepeatedly(DoAll(SetArgPointee<2>(expanded_segments), Return(true)));
  converter.CandidateNext(*composer_);
  Mock::VerifyAndClearExpectations(&mock_converter);
  expected_indices[0] += 2;
  EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);

  {
    // Check the candidate list
    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());
    EXPECT_TRUE(output.has_preedit());
    EXPECT_TRUE(output.has_candidate_window());

    const commands::CandidateWindow& candidate_window =
        output.candidate_window();
    // Candidates should be merged with the previous suggestions.
    EXPECT_EQ(candidate_window.size(), 4);
    EXPECT_EQ(candidate_window.candidate(0).value(), kChars_Mozukusu);
    EXPECT_EQ(candidate_window.candidate(1).value(), kChars_Momonga);
    EXPECT_EQ(candidate_window.candidate(2).value(), kChars_Mozuku);
    EXPECT_EQ(candidate_window.candidate(3).value(), "モンドリアン");
    EXPECT_TRUE(candidate_window.has_focused_index());
  }

  // Action 5. CandidateNext()
  // "モンドリアン" is focused.
  EXPECT_CALL(*mock_converter, FocusSegmentValue(_, 0, 4))
      .WillOnce(Return(true));
  converter.CandidateNext(*composer_);
  Mock::VerifyAndClearExpectations(&mock_converter);
  expected_indices[0] += 1;
  EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);

  // Action 6. Commit()
  // Commit "モンドリアン".
  EXPECT_CALL(*mock_converter, CommitSegmentValue(_, 0, 4))
      .WillOnce(Return(true));
  EXPECT_CALL(*mock_converter, FinishConversion(_, _));
  converter.Commit(*composer_, Context::default_instance());
  Mock::VerifyAndClearExpectations(&mock_converter);
  composer_->Reset();
  expected_indices.clear();
  EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);
  {  // Check the submitted value
    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_TRUE(output.has_result());
    EXPECT_FALSE(output.has_preedit());
    EXPECT_FALSE(output.has_candidate_window());

    const commands::Result& result = output.result();
    EXPECT_EQ(result.value(), "モンドリアン");
    EXPECT_EQ(result.key(), "もんどりあん");
  }

  // Action 7. Predict() (Next Word Prediction)
  // After commit, the state should be reset. Thus, calling prediction before
  // suggestion should trigger StartPrediction().
  const Segment kEmptySegment;
  const Segments nwp_segments = []() {
    Segments segments;
    Segment* segment = segments.add_segment();
    converter::Candidate* candidate;
    candidate = segment->add_candidate();
    candidate->value = "が";
    candidate->content_key = "が";
    candidate = segment->add_candidate();
    candidate->value = "の";
    candidate->content_key = "の";
    return segments;
  }();
  EXPECT_CALL(*mock_converter, StartPredictionWithPreviousSuggestion(
                                   _, EqualsSegment(kEmptySegment), _))
      .WillOnce(DoAll(SetArgPointee<2>(nwp_segments), Return(true)));
  expected_indices.push_back(0);
  EXPECT_TRUE(converter.Predict(*composer_));
  Mock::VerifyAndClearExpectations(&mock_converter);
  EXPECT_TRUE(converter.IsActive());
  EXPECT_SELECTED_CANDIDATE_INDICES_EQ(converter, expected_indices);
  {
    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());
    EXPECT_TRUE(output.has_preedit());
    EXPECT_TRUE(output.has_candidate_window());

    // Check the composition
    const commands::Preedit& composition = output.preedit();
    EXPECT_EQ(composition.segment_size(), 1);
    EXPECT_EQ(composition.segment(0).value(), "が");

    // Check the candidate list
    const commands::CandidateWindow& candidate_window =
        output.candidate_window();
    // Candidates should NOT be merged with the previous suggestions.
    EXPECT_EQ(candidate_window.size(), 2);
    EXPECT_EQ(candidate_window.candidate(0).value(), "が");
    EXPECT_EQ(candidate_window.candidate(1).value(), "の");
    EXPECT_TRUE(candidate_window.has_focused_index());
  }
}

TEST_F(EngineConverterTest, SuggestFillIncognitoCandidateWords) {
  Segments segments;
  {  // Initialize mock segments for suggestion
    Segment* segment = segments.add_segment();
    converter::Candidate* candidate;
    segment->set_key(kChars_Mo);
    candidate = segment->add_candidate();
    candidate->value = kChars_Mozukusu;
    candidate->content_key = kChars_Mozukusu;
    candidate = segment->add_candidate();
    candidate->value = kChars_Momonga;
    candidate->content_key = kChars_Momonga;
  }
  composer_->InsertCharacterPreedit(kChars_Mo);

  // A matcher to test if the given conversion request sets incognito_mode().
  constexpr auto IsIncognitoConversionRequest = [](bool is_incognito) {
    return Property(&ConversionRequest::incognito_mode, Eq(is_incognito));
  };
  {
    request_->set_fill_incognito_candidate_words(false);
    auto mock_converter = std::make_shared<MockConverter>();
    EngineConverter converter(mock_converter, request_, config_);
    EXPECT_CALL(*mock_converter,
                StartPrediction(IsIncognitoConversionRequest(false), _))
        .WillOnce(DoAll(SetArgPointee<1>(segments), Return(true)));
    EXPECT_TRUE(converter.Suggest(*composer_, Context::default_instance()));
    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());
    EXPECT_TRUE(output.has_preedit());
    EXPECT_TRUE(output.has_candidate_window());
    EXPECT_FALSE(output.has_incognito_candidate_words());
  }
  {
    request_->set_fill_incognito_candidate_words(true);
    auto mock_converter = std::make_shared<MockConverter>();
    EngineConverter converter(mock_converter, request_, config_);
    EXPECT_CALL(*mock_converter,
                StartPrediction(IsIncognitoConversionRequest(false), _))
        .WillOnce(DoAll(SetArgPointee<1>(segments), Return(true)));
    EXPECT_CALL(*mock_converter,
                StartPrediction(IsIncognitoConversionRequest(true), _))
        .WillOnce(DoAll(SetArgPointee<1>(segments), Return(true)));
    EXPECT_TRUE(converter.Suggest(*composer_, Context::default_instance()));
    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());
    EXPECT_TRUE(output.has_preedit());
    EXPECT_TRUE(output.has_candidate_window());
    EXPECT_TRUE(output.has_incognito_candidate_words());
  }
}

TEST_F(EngineConverterTest, OnePhaseSuggestion) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);
  request_->set_mixed_conversion(true);
  Segments segments;
  {  // Initialize mock segments for suggestion (internally prediction)
    Segment* segment = segments.add_segment();
    converter::Candidate* candidate;
    segment->set_key(kChars_Mo);
    candidate = segment->add_candidate();
    candidate->value = kChars_Mozuku;
    candidate->content_key = kChars_Mozuku;
    candidate = segment->add_candidate();
    candidate->value = kChars_Momonga;
    candidate->content_key = kChars_Momonga;
    candidate = segment->add_candidate();
    candidate->value = "モンドリアン";
    candidate->content_key = "もんどりあん";
  }
  composer_->InsertCharacterPreedit(kChars_Mo);

  // Suggestion (internally prediction)
  // Use "prediction" mock as this suggestion uses prediction internally.
  EXPECT_CALL(*mock_converter, StartPrediction(_, _))
      .WillOnce(DoAll(SetArgPointee<1>(segments), Return(true)));
  EXPECT_TRUE(converter.Suggest(*composer_, Context::default_instance()));
  Mock::VerifyAndClearExpectations(&mock_converter);
  EXPECT_TRUE(IsCandidateListVisible(converter));
  EXPECT_TRUE(converter.IsActive());

  {  // Check the candidate list
    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());
    EXPECT_TRUE(output.has_preedit());
    EXPECT_TRUE(output.has_candidate_window());

    const commands::CandidateWindow& candidate_window =
        output.candidate_window();
    EXPECT_EQ(candidate_window.size(), 3);
    EXPECT_EQ(candidate_window.candidate(0).value(), kChars_Mozuku);
    EXPECT_EQ(candidate_window.candidate(1).value(), kChars_Momonga);
    EXPECT_EQ(candidate_window.candidate(2).value(), "モンドリアン");
    EXPECT_FALSE(candidate_window.has_focused_index());
  }
}

TEST_F(EngineConverterTest, SuppressSuggestionWhenNotRequested) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);
  composer_->InsertCharacterPreedit(kChars_Mo);

  // Suggestion
  EXPECT_CALL(*mock_converter, StartPrediction(_, _)).Times(0);
  // No candidates should be visible because we are on password field.

  ConversionPreferences conversion_preferences =
      converter.conversion_preferences();
  conversion_preferences.request_suggestion = false;
  EXPECT_FALSE(converter.SuggestWithPreferences(
      *composer_, Context::default_instance(), conversion_preferences));
  EXPECT_FALSE(IsCandidateListVisible(converter));
  EXPECT_FALSE(converter.IsActive());
}

TEST_F(EngineConverterTest, SuppressSuggestionOnPasswordField) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);

  composer_->SetInputFieldType(Context::PASSWORD);
  composer_->InsertCharacterPreedit(kChars_Mo);

  // Suggestion
  EXPECT_CALL(*mock_converter, StartPrediction(_, _)).Times(0);

  // No candidates should be visible because we are on password field.
  EXPECT_FALSE(converter.Suggest(*composer_, Context::default_instance()));
  EXPECT_FALSE(IsCandidateListVisible(converter));
  EXPECT_FALSE(converter.IsActive());
}

TEST_F(EngineConverterTest, ReportsPersistentMozcUserHistoryPreference) {
  auto mock_converter = std::make_shared<MockConverter>();
  EXPECT_CALL(*mock_converter,
              HasUserSegmentHistoryPreference("りせきします", "離席します"))
      .WillRepeatedly(Return(true));
  EngineConverter converter(mock_converter, request_, config_);
  SetState(EngineConverterInterface::CONVERSION, &converter);

  Segments segments;
  AddSegmentWithSingleCandidate(&segments, "りせきします", "離席します");
  SetSegments(segments, &converter);

  std::vector<UserHistoryConversionPreference> preferences;
  converter.GetUserHistoryConversionPreferences(&preferences);

  ASSERT_EQ(preferences.size(), 1);
  EXPECT_EQ(preferences[0].key, "りせきします");
  EXPECT_EQ(preferences[0].value, "離席します");
}

TEST_F(EngineConverterTest, FindsLearnedPreferenceInsideLongerConversion) {
  auto mock_converter = std::make_shared<MockConverter>();
  EXPECT_CALL(*mock_converter, HasUserSegmentHistoryPreference(_, _))
      .WillRepeatedly(Return(false));
  EXPECT_CALL(*mock_converter,
              HasUserSegmentHistoryPreference("りせきします", "離席します"))
      .WillRepeatedly(Return(true));
  EngineConverter converter(mock_converter, request_, config_);
  SetState(EngineConverterInterface::CONVERSION, &converter);

  Segments segments;
  AddSegmentWithSingleCandidate(&segments, "ちょっとりせきします",
                                "ちょっと離席します");
  converter::Candidate* candidate =
      segments.mutable_conversion_segment(0)->mutable_candidate(0);
  candidate->inner_segment_boundary = converter::BuildInnerSegmentBoundary(
      {
          {12, 12, 12, 12},
          {9, 6, 9, 6},
          {9, 9, 9, 9},
      },
      candidate->key, candidate->value);
  SetSegments(segments, &converter);

  std::vector<UserHistoryConversionPreference> preferences;
  converter.GetUserHistoryConversionPreferences(&preferences);

  ASSERT_EQ(preferences.size(), 1);
  EXPECT_EQ(preferences[0].key, "りせきします");
  EXPECT_EQ(preferences[0].value, "離席します");
}

TEST_F(EngineConverterTest, DoesNotReportPreferenceWhenHistoryIsDisabled) {
  auto mock_converter = std::make_shared<MockConverter>();
  EXPECT_CALL(*mock_converter, HasUserSegmentHistoryPreference(_, _)).Times(0);
  EngineConverter converter(mock_converter, request_, config_);
  SetState(EngineConverterInterface::CONVERSION, &converter);
  SetCurrentConversionUsesUserHistory(false, &converter);

  Segments segments;
  AddSegmentWithSingleCandidate(&segments, "りせきします", "離席します");
  SetSegments(segments, &converter);

  std::vector<UserHistoryConversionPreference> preferences;
  converter.GetUserHistoryConversionPreferences(&preferences);
  EXPECT_TRUE(preferences.empty());
}

TEST_F(EngineConverterTest, DoesNotReportPreferenceInNoHistoryConfig) {
  config_->set_history_learning_level(Config::NO_HISTORY);
  auto mock_converter = std::make_shared<MockConverter>();
  EXPECT_CALL(*mock_converter, HasUserSegmentHistoryPreference(_, _)).Times(0);
  EngineConverter converter(mock_converter, request_, config_);
  SetState(EngineConverterInterface::CONVERSION, &converter);

  Segments segments;
  AddSegmentWithSingleCandidate(&segments, "りせきします", "離席します");
  SetSegments(segments, &converter);

  std::vector<UserHistoryConversionPreference> preferences;
  converter.GetUserHistoryConversionPreferences(&preferences);
  EXPECT_TRUE(preferences.empty());
}

TEST_F(EngineConverterTest, SkipsAmbiguousRepeatedLearnedPreference) {
  auto mock_converter = std::make_shared<MockConverter>();
  EXPECT_CALL(*mock_converter, HasUserSegmentHistoryPreference(_, _)).Times(0);
  EngineConverter converter(mock_converter, request_, config_);
  SetState(EngineConverterInterface::CONVERSION, &converter);

  Segments segments;
  AddSegmentWithSingleCandidate(&segments, "りせきします", "離席します");
  AddSegmentWithSingleCandidate(&segments, "りせきします", "離席します");
  SetSegments(segments, &converter);

  std::vector<UserHistoryConversionPreference> preferences;
  converter.GetUserHistoryConversionPreferences(&preferences);
  EXPECT_TRUE(preferences.empty());
}

TEST_F(EngineConverterTest, TransientRerankedAttributesAloneDoNotCreatePreference) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);
  SetState(EngineConverterInterface::CONVERSION, &converter);

  Segments segments;
  AddSegmentWithSingleCandidate(&segments, "りせきします", "離席します");
  converter::Candidate* candidate =
      segments.mutable_conversion_segment(0)->mutable_candidate(0);
  candidate->attributes |=
      converter::Attribute::BEST_CANDIDATE |
      converter::Attribute::USER_SEGMENT_HISTORY_REWRITER |
      converter::Attribute::RERANKED;
  SetSegments(segments, &converter);

  std::vector<UserHistoryConversionPreference> preferences;
  converter.GetUserHistoryConversionPreferences(&preferences);
  EXPECT_TRUE(preferences.empty());
}

TEST_F(EngineConverterTest, AppendCandidateList) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);
  SetState(EngineConverterInterface::CONVERSION, &converter);
  converter.set_use_cascading_window(true);
  Segments segments;

  {
    SetAiueo(&segments);
    composer_->InsertCharacterPreedit("あいうえお");
    FillT13Ns(&segments, composer_.get());

    SetSegments(segments, &converter);
    AppendCandidateList(ConversionRequest::CONVERSION, &converter);
    const CandidateList& candidate_list = GetCandidateList(converter);
    // 3 == hiragana cand, katakana cand and sub candidate list.
    EXPECT_EQ(candidate_list.size(), 3);
    EXPECT_TRUE(candidate_list.focused());
    size_t sub_cand_list_count = 0;
    for (size_t i = 0; i < candidate_list.size(); ++i) {
      if (candidate_list.candidate(i).HasSubcandidateList()) {
        ++sub_cand_list_count;
      }
    }
    // Sub candidate list for T13N.
    EXPECT_EQ(sub_cand_list_count, 1);
  }
  {
    Segment* segment = segments.mutable_conversion_segment(0);
    converter::Candidate* candidate = segment->add_candidate();
    candidate->value = "あいうえお_2";
    // New meta candidates.
    // They should be ignored.
    std::vector<converter::Candidate>* meta_candidates =
        segment->mutable_meta_candidates();
    meta_candidates->clear();
    meta_candidates->resize(1);
    meta_candidates->at(0).value = "t13nValue";
    meta_candidates->at(0).content_value = "t13nValue";
    meta_candidates->at(0).content_key = segment->key();

    SetSegments(segments, &converter);
    AppendCandidateList(ConversionRequest::CONVERSION, &converter);
    const CandidateList& candidate_list = GetCandidateList(converter);
    // 4 == hiragana cand, katakana cand, hiragana cand2
    // and sub candidate list.
    EXPECT_EQ(candidate_list.size(), 4);
    EXPECT_TRUE(candidate_list.focused());
    size_t sub_cand_list_count = 0;
    std::set<int> id_set;
    for (size_t i = 0; i < candidate_list.size(); ++i) {
      if (candidate_list.candidate(i).HasSubcandidateList()) {
        ++sub_cand_list_count;
      } else {
        // No duplicate ids are expected.
        int id = candidate_list.candidate(i).id();
        // EXPECT_EQ(iterator, iterator) might cause compile error in specific
        // environment.
        EXPECT_TRUE(id_set.end() == id_set.find(id));
        id_set.insert(id);
      }
    }
    // Sub candidate list shouldn't be duplicated.
    EXPECT_EQ(sub_cand_list_count, 1);
  }
}

TEST_F(EngineConverterTest, AppendCandidateListForRequestTypes) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);
  SetState(EngineConverterInterface::SUGGESTION, &converter);
  Segments segments;

  {
    SetAiueo(&segments);
    composer_->InsertCharacterPreedit("あいうえお");
    FillT13Ns(&segments, composer_.get());
    SetSegments(segments, &converter);
    AppendCandidateList(ConversionRequest::SUGGESTION, &converter);
    const CandidateList& candidate_list = GetCandidateList(converter);
    EXPECT_FALSE(candidate_list.focused());
  }

  segments.Clear();
  {
    SetAiueo(&segments);
    composer_->InsertCharacterPreedit("あいうえお");
    FillT13Ns(&segments, composer_.get());
    SetSegments(segments, &converter);
    AppendCandidateList(ConversionRequest::PARTIAL_SUGGESTION, &converter);
    const CandidateList& candidate_list = GetCandidateList(converter);
    EXPECT_FALSE(candidate_list.focused());
  }

  segments.Clear();
  {
    SetAiueo(&segments);
    composer_->InsertCharacterPreedit("あいうえお");
    FillT13Ns(&segments, composer_.get());
    SetSegments(segments, &converter);
    AppendCandidateList(ConversionRequest::PARTIAL_PREDICTION, &converter);
    const CandidateList& candidate_list = GetCandidateList(converter);
    EXPECT_FALSE(candidate_list.focused());
  }
}

TEST_F(EngineConverterTest, ReloadConfig) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);
  Segments segments;
  SetAiueo(&segments);
  composer_->InsertCharacterPreedit("あいうえお");
  FillT13Ns(&segments, composer_.get());
  EXPECT_CALL(*mock_converter, StartConversion(_, _))
      .WillOnce(DoAll(SetArgPointee<1>(segments), Return(true)));

  composer_->InsertCharacterPreedit("aiueo");
  EXPECT_TRUE(converter.Convert(*composer_));
  converter.SetCandidateListVisible(true);

  {  // Set OperationPreferences
    converter.set_use_cascading_window(false);
    converter.set_selection_shortcut(config::Config::SHORTCUT_123456789);
    EXPECT_TRUE(IsCandidateListVisible(converter));
  }
  {  // Check the config update
    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());
    EXPECT_TRUE(output.has_preedit());
    EXPECT_TRUE(output.has_candidate_window());

    const commands::CandidateWindow& candidate_window =
        output.candidate_window();
    EXPECT_EQ(candidate_window.candidate(0).annotation().shortcut(), "1");
    EXPECT_EQ(candidate_window.candidate(1).annotation().shortcut(), "2");
  }

  {  // Set OperationPreferences #2
    converter.set_use_cascading_window(false);
    converter.set_selection_shortcut(config::Config::NO_SHORTCUT);
  }
  {  // Check the config update
    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());
    EXPECT_TRUE(output.has_preedit());
    EXPECT_TRUE(output.has_candidate_window());

    const commands::CandidateWindow& candidate_window =
        output.candidate_window();
    EXPECT_TRUE(candidate_window.candidate(0).annotation().shortcut().empty());
    EXPECT_TRUE(candidate_window.candidate(1).annotation().shortcut().empty());
  }
}

TEST_F(EngineConverterTest, OutputAllCandidateWords) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);
  Segments segments;
  SetKamaboko(&segments);
  const std::string kKamabokono = "かまぼこの";
  const std::string kInbou = "いんぼう";
  composer_->InsertCharacterPreedit(kKamabokono + kInbou);
  FillT13Ns(&segments, composer_.get());

  commands::Output output;

  EXPECT_CALL(*mock_converter, StartConversion(_, _))
      .WillOnce(DoAll(SetArgPointee<1>(segments), Return(true)));
  EXPECT_TRUE(converter.Convert(*composer_));
  Mock::VerifyAndClearExpectations(&mock_converter);
  {
    ASSERT_TRUE(converter.IsActive());
    EXPECT_FALSE(IsCandidateListVisible(converter));

    output.Clear();
    converter.PopOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());
    EXPECT_TRUE(output.has_preedit());
    EXPECT_FALSE(output.has_candidate_window());
    EXPECT_TRUE(output.has_all_candidate_words());

    EXPECT_EQ(output.all_candidate_words().focused_index(), 0);
    EXPECT_EQ(output.all_candidate_words().category(), commands::CONVERSION);
    // [ "かまぼこの", "カマボコの", "カマボコノ" (t13n), "かまぼこの" (t13n),
    //   "ｶﾏﾎﾞｺﾉ" (t13n) ]
    EXPECT_EQ(output.all_candidate_words().candidates_size(), 5);
  }

  EXPECT_CALL(*mock_converter, FocusSegmentValue(_, 0, 1))
      .WillOnce(Return(true));
  converter.CandidateNext(*composer_);
  Mock::VerifyAndClearExpectations(&mock_converter);
  {
    ASSERT_TRUE(converter.IsActive());
    EXPECT_TRUE(IsCandidateListVisible(converter));

    output.Clear();
    converter.PopOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());
    EXPECT_TRUE(output.has_preedit());
    EXPECT_TRUE(output.has_candidate_window());
    EXPECT_TRUE(output.has_all_candidate_words());

    EXPECT_EQ(output.all_candidate_words().focused_index(), 1);
    EXPECT_EQ(output.all_candidate_words().category(), commands::CONVERSION);
    // [ "かまぼこの", "カマボコの", "カマボコノ" (t13n), "かまぼこの" (t13n),
    //   "ｶﾏﾎﾞｺﾉ" (t13n) ]
    EXPECT_EQ(output.all_candidate_words().candidates_size(), 5);
  }

  EXPECT_CALL(*mock_converter, CommitSegmentValue(_, 0, 1))
      .WillOnce(Return(true));
  converter.SegmentFocusRight();
  Mock::VerifyAndClearExpectations(&mock_converter);
  {
    ASSERT_TRUE(converter.IsActive());
    EXPECT_FALSE(IsCandidateListVisible(converter));

    output.Clear();
    converter.PopOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());
    EXPECT_TRUE(output.has_preedit());
    EXPECT_FALSE(output.has_candidate_window());
    EXPECT_TRUE(output.has_all_candidate_words());

    EXPECT_EQ(output.all_candidate_words().focused_index(), 0);
    EXPECT_EQ(output.all_candidate_words().category(), commands::CONVERSION);
    // [ "陰謀", "印房", "インボウ" (t13n), "いんぼう" (t13n), "ｲﾝﾎﾞｳ" (t13n) ]
    EXPECT_EQ(output.all_candidate_words().candidates_size(), 5);
  }
}

TEST_F(EngineConverterTest, GetPreeditAndGetConversion) {
  Segments segments;

  Segment* segment;
  converter::Candidate* candidate;

  segment = segments.add_segment();
  segment->set_segment_type(Segment::HISTORY);
  segment->set_key("[key:history1]");
  candidate = segment->add_candidate();
  candidate->content_key = "[content_key:history1-1]";
  candidate = segment->add_candidate();
  candidate->content_key = "[content_key:history1-2]";

  segment = segments.add_segment();
  segment->set_segment_type(Segment::FREE);
  segment->set_key("[key:conversion1]");
  candidate = segment->add_candidate();
  candidate->key = "[key:conversion1-1]";
  candidate->content_key = "[content_key:conversion1-1]";
  candidate->value = "[value:conversion1-1]";
  candidate->content_value = "[content_value:conversion1-1]";
  candidate = segment->add_candidate();
  candidate->key = "[key:conversion1-2]";
  candidate->content_key = "[content_key:conversion1-2]";
  candidate->value = "[value:conversion1-2]";
  candidate->content_value = "[content_value:conversion1-2]";
  {
    // PREDICTION
    auto mock_converter = std::make_shared<MockConverter>();
    EngineConverter converter(mock_converter, request_, config_);
    EXPECT_CALL(*mock_converter, StartPredictionWithPreviousSuggestion(_, _, _))
        .WillOnce(DoAll(SetArgPointee<2>(segments), Return(true)));
    EXPECT_CALL(*mock_converter, FocusSegmentValue(_, 0, 1))
        .WillOnce(Return(true));
    converter.Predict(*composer_);
    converter.CandidateNext(*composer_);
    std::string preedit;
    GetPreedit(converter, 0, 1, &preedit);
    EXPECT_EQ(preedit, "[content_key:conversion1-2]");
    std::string conversion;
    GetConversion(converter, 0, 1, &conversion);
    EXPECT_EQ(conversion, "[value:conversion1-2]");
  }
  {
    // SUGGESTION
    auto mock_converter = std::make_shared<MockConverter>();
    EngineConverter converter(mock_converter, request_, config_);
    EXPECT_CALL(*mock_converter, StartPrediction(_, _))
        .WillOnce(DoAll(SetArgPointee<1>(segments), Return(true)));
    converter.Suggest(*composer_, Context::default_instance());
    std::string preedit;
    GetPreedit(converter, 0, 1, &preedit);
    EXPECT_EQ(preedit, "[content_key:conversion1-1]");
    std::string conversion;
    GetConversion(converter, 0, 1, &conversion);
    EXPECT_EQ(conversion, "[value:conversion1-1]");
  }
  segment = segments.add_segment();
  segment->set_segment_type(Segment::FREE);
  segment->set_key("[key:conversion2]");
  candidate = segment->add_candidate();
  candidate->key = "[key:conversion2-1]";
  candidate->content_key = "[content_key:conversion2-1]";
  candidate->value = "[value:conversion2-1]";
  candidate->content_value = "[content_value:conversion2-1]";
  candidate = segment->add_candidate();
  candidate->key = "[key:conversion2-2]";
  candidate->content_key = "[content_key:conversion2-2]";
  candidate->value = "[value:conversion2-2]";
  candidate->content_value = "[content_value:conversion2-2]";
  {
    // CONVERSION
    auto mock_converter = std::make_shared<MockConverter>();
    EngineConverter converter(mock_converter, request_, config_);
    EXPECT_CALL(*mock_converter, StartConversion(_, _))
        .WillOnce(DoAll(SetArgPointee<1>(segments), Return(true)));
    EXPECT_CALL(*mock_converter, FocusSegmentValue(_, 0, 1))
        .WillOnce(Return(true));
    converter.Convert(*composer_);
    converter.CandidateNext(*composer_);
    std::string preedit;
    GetPreedit(converter, 0, 2, &preedit);
    EXPECT_EQ(preedit, "[key:conversion1][key:conversion2]");
    std::string conversion;
    GetConversion(converter, 0, 2, &conversion);
    EXPECT_EQ(conversion, "[value:conversion1-2][value:conversion2-1]");
  }
}

TEST_F(EngineConverterTest, GetAndSetSegments) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);
  Segments segments;

  // Set history segments.
  const std::string kHistoryInput[] = {"車で", "行く"};
  for (size_t i = 0; i < std::size(kHistoryInput); ++i) {
    Segment* segment = segments.add_segment();
    segment->set_segment_type(Segment::HISTORY);
    converter::Candidate* candidate = segment->add_candidate();
    candidate->value = kHistoryInput[i];
  }
  EXPECT_CALL(*mock_converter, FinishConversion(_, _))
      .WillOnce(SetArgPointee<1>(segments));
  converter.CommitPreedit(*composer_, Context::default_instance());

  Segments src;
  GetSegments(converter, &src);
  ASSERT_EQ(src.history_segments_size(), 2);
  EXPECT_EQ(src.history_segment(0).candidate(0).value, "車で");
  EXPECT_EQ(src.history_segment(1).candidate(0).value, "行く");

  src.mutable_history_segment(0)->mutable_candidate(0)->value = "歩いて";
  Segment* segment = src.add_segment();
  segment->set_segment_type(Segment::FREE);
  converter::Candidate* candidate = segment->add_candidate();
  candidate->value = "?";

  SetSegments(src, &converter);

  Segments dest;
  GetSegments(converter, &dest);

  ASSERT_EQ(dest.history_segments_size(), 2);
  ASSERT_EQ(dest.conversion_segments_size(), 1);
  EXPECT_EQ(dest.history_segment(0).candidate(0).value,
            src.history_segment(0).candidate(0).value);
  EXPECT_EQ(dest.history_segment(1).candidate(0).value,
            src.history_segment(1).candidate(0).value);
  EXPECT_EQ(dest.history_segment(2).candidate(0).value,
            src.history_segment(2).candidate(0).value);
}

TEST_F(EngineConverterTest, Clone) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter src(mock_converter, request_, config_);

  const std::string kKamabokono = "かまぼこの";
  const std::string kInbou = "いんぼう";
  const std::string kInbouKanji = "陰謀";

  {  // create source converter
    Segments segments;
    SetKamaboko(&segments);

    EXPECT_CALL(*mock_converter, StartConversion(_, _))
        .WillRepeatedly(DoAll(SetArgPointee<1>(segments), Return(true)));

    src.set_use_cascading_window(false);
    src.set_selection_shortcut(config::Config::SHORTCUT_123456789);
  }

  {  // validation
    // Copy and validate
    std::unique_ptr<EngineConverter> dest(src.Clone());
    ASSERT_TRUE(dest.get() != nullptr);
    ExpectSameEngineConverter(src, *dest);

    // Convert source
    EXPECT_TRUE(src.Convert(*composer_));
    EXPECT_TRUE(src.IsActive());

    // Convert destination and validate
    EXPECT_TRUE(dest->Convert(*composer_));
    ExpectSameEngineConverter(src, *dest);

    // Copy converted and validate
    dest.reset(src.Clone());
    ASSERT_TRUE(dest.get() != nullptr);
    ExpectSameEngineConverter(src, *dest);
  }
}

// Suggest() in the suggestion state was not accepted.  (http://b/1948334)
TEST_F(EngineConverterTest, Issue1948334) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);
  Segments segments;
  {  // Initialize mock segments for the first suggestion
    Segment* segment = segments.add_segment();
    converter::Candidate* candidate;
    segment->set_key(kChars_Mo);
    candidate = segment->add_candidate();
    candidate->value = kChars_Mozukusu;
    candidate->content_key = kChars_Mozukusu;
    candidate = segment->add_candidate();
    candidate->value = kChars_Momonga;
    candidate->content_key = kChars_Momonga;
  }
  composer_->InsertCharacterPreedit(kChars_Mo);

  // Suggestion
  EXPECT_CALL(*mock_converter, StartPrediction(_, _))
      .WillRepeatedly(DoAll(SetArgPointee<1>(segments), Return(true)));
  EXPECT_TRUE(converter.Suggest(*composer_, Context::default_instance()));
  Mock::VerifyAndClearExpectations(&mock_converter);
  EXPECT_TRUE(converter.IsActive());

  segments.Clear();
  {  // Initialize mock segments for the second suggestion
    Segment* segment = segments.add_segment();
    converter::Candidate* candidate;
    segment->set_key("もず");
    candidate = segment->add_candidate();
    candidate->value = kChars_Mozukusu;
    candidate->content_key = kChars_Mozukusu;
  }
  composer_->InsertCharacterPreedit("もず");

  // Suggestion
  EXPECT_CALL(*mock_converter, StartPrediction(_, _))
      .WillRepeatedly(DoAll(SetArgPointee<1>(segments), Return(true)));
  EXPECT_TRUE(converter.Suggest(*composer_, Context::default_instance()));
  Mock::VerifyAndClearExpectations(&mock_converter);
  EXPECT_TRUE(converter.IsActive());

  {  // Check the candidate list
    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());
    EXPECT_TRUE(output.has_preedit());
    EXPECT_TRUE(output.has_candidate_window());

    const commands::CandidateWindow& candidate_window =
        output.candidate_window();
    // Candidates should be merged with the previous suggestions.
    EXPECT_EQ(candidate_window.size(), 1);
    EXPECT_EQ(candidate_window.candidate(0).value(), kChars_Mozukusu);
    EXPECT_FALSE(candidate_window.has_focused_index());
  }
}

TEST_F(EngineConverterTest, Issue1960362) {
  // Testcase against http://b/1960362, a candidate list was not
  // updated when ConvertToTransliteration changed the size of segments.

  table_->AddRule("zyu", "ZYU", "");
  table_->AddRule("jyu", "ZYU", "");
  table_->AddRule("tt", "XTU", "t");
  table_->AddRule("ta", "TA", "");

  composer_->InsertCharacter("j");
  composer_->InsertCharacter("y");
  composer_->InsertCharacter("u");
  composer_->InsertCharacter("t");

  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);

  Segments segments;
  {
    Segment* segment;
    converter::Candidate* candidate;

    segment = segments.add_segment();
    segment->set_key("ZYU");
    candidate = segment->add_candidate();
    candidate->value = "[ZYU]";
    candidate->content_key = "[ZYU]";

    segment = segments.add_segment();
    segment->set_key("t");
    candidate = segment->add_candidate();
    candidate->value = "[t]";
    candidate->content_key = "[t]";
  }

  Segments resized_segments;
  {
    Segment* segment = resized_segments.add_segment();
    converter::Candidate* candidate;
    segment->set_key("ZYUt");
    candidate = segment->add_candidate();
    candidate->value = "[ZYUt]";
    candidate->content_key = "[ZYUt]";
  }
  FillT13Ns(&segments, composer_.get());
  FillT13Ns(&resized_segments, composer_.get());

  EXPECT_CALL(*mock_converter, StartConversion(_, _))
      .WillRepeatedly(DoAll(SetArgPointee<1>(segments), Return(true)));
  EXPECT_CALL(*mock_converter, ResizeSegment(_, _, _, _))
      .WillRepeatedly(DoAll(SetArgPointee<0>(resized_segments), Return(true)));
  EXPECT_TRUE(converter.ConvertToTransliteration(*composer_,
                                                 transliteration::HALF_ASCII));
  EXPECT_FALSE(IsCandidateListVisible(converter));

  commands::Output output;
  converter.FillOutput(*composer_, &output);
  EXPECT_FALSE(output.has_result());
  EXPECT_TRUE(output.has_preedit());
  EXPECT_FALSE(output.has_candidate_window());

  const commands::Preedit& conversion = output.preedit();
  EXPECT_EQ(conversion.segment(0).value(), "jyut");
}

TEST_F(EngineConverterTest, Issue1978201) {
  // This is a unittest against http://b/1978201
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);
  Segments segments;
  composer_->InsertCharacterPreedit(kChars_Mo);

  {  // Initialize mock segments for prediction
    Segment* segment = segments.add_segment();
    converter::Candidate* candidate;
    segment->set_key(kChars_Mo);
    candidate = segment->add_candidate();
    candidate->value = kChars_Mozuku;
    candidate->content_key = kChars_Mozuku;
    candidate = segment->add_candidate();
    candidate->value = kChars_Momonga;
    candidate->content_key = kChars_Momonga;
  }

  // Prediction
  EXPECT_CALL(*mock_converter, StartPredictionWithPreviousSuggestion(_, _, _))
      .WillRepeatedly(DoAll(SetArgPointee<2>(segments), Return(true)));

  EXPECT_TRUE(converter.Predict(*composer_));
  EXPECT_TRUE(converter.IsActive());

  {  // Check the conversion
    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());
    EXPECT_TRUE(output.has_preedit());
    EXPECT_TRUE(output.has_candidate_window());

    const commands::Preedit& conversion = output.preedit();
    EXPECT_EQ(conversion.segment_size(), 1);
    EXPECT_EQ(conversion.segment(0).value(), kChars_Mozuku);
  }

  // Meaningless segment manipulations.
  converter.SegmentWidthShrink(*composer_);
  converter.SegmentFocusLeft();
  converter.SegmentFocusLast();

  {  // Check the conversion again
    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());
    EXPECT_TRUE(output.has_preedit());
    EXPECT_FALSE(output.has_candidate_window());

    const commands::Preedit& conversion = output.preedit();
    EXPECT_EQ(conversion.segment_size(), 1);
    EXPECT_EQ(conversion.segment(0).value(), kChars_Mozuku);
  }
}

TEST_F(EngineConverterTest, Issue1981020) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);
  // "〜〜〜〜" U+301C * 4
  const std::string wave_dash_301c = "〜〜〜〜";
  composer_->InsertCharacterPreedit(wave_dash_301c);
  Segments segments;
  EXPECT_CALL(*mock_converter, FinishConversion(_, _))
      .WillOnce(SaveArgPointee<1>(&segments));
  converter.CommitPreedit(*composer_, Context::default_instance());

#ifdef _WIN32
  // "～～～～" U+FF5E * 4
  const std::string fullwidth_tilde_ff5e = "～～～～";
  EXPECT_EQ(segments.conversion_segment(0).candidate(0).value,
            fullwidth_tilde_ff5e);
  EXPECT_EQ(segments.conversion_segment(0).candidate(0).content_value,
            fullwidth_tilde_ff5e);
#else   // _WIN32
  EXPECT_EQ(segments.conversion_segment(0).candidate(0).value, wave_dash_301c);
  EXPECT_EQ(segments.conversion_segment(0).candidate(0).content_value,
            wave_dash_301c);
#endif  // _WIN32
}

TEST_F(EngineConverterTest, Issue2029557) {
  // Unittest against http://b/2029557
  // a<tab><F6> raised a DCHECK error.
  auto mock_converter = std::make_shared<MockConverter>();

  EngineConverter converter(mock_converter, request_, config_);
  // Composition (as "a")
  composer_->InsertCharacterPreedit("a");

  // Prediction (as <tab>)
  Segments segments;
  SetAiueo(&segments);
  EXPECT_CALL(*mock_converter, StartPredictionWithPreviousSuggestion(_, _, _))
      .WillOnce(DoAll(SetArgPointee<2>(segments), Return(true)));
  EXPECT_TRUE(converter.Predict(*composer_));
  EXPECT_TRUE(converter.IsActive());

  // Transliteration (as <F6>)
  segments.Clear();
  Segment* segment = segments.add_segment();
  segment->set_key("a");
  converter::Candidate* candidate = segment->add_candidate();
  candidate->value = "a";

  FillT13Ns(&segments, composer_.get());
  EXPECT_CALL(*mock_converter, StartConversion(_, _))
      .WillOnce(DoAll(SetArgPointee<1>(segments), Return(true)));
  EXPECT_TRUE(converter.ConvertToTransliteration(*composer_,
                                                 transliteration::HIRAGANA));
  EXPECT_TRUE(converter.IsActive());
}

TEST_F(EngineConverterTest, Issue2031986) {
  // Unittest against http://b/2031986
  // aaaaa<Shift+Enter> raised a CRT error.
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);

  {  // Initialize a suggest result triggered by "aaaa".
    Segments segments;
    Segment* segment = segments.add_segment();
    segment->set_key("aaaa");
    converter::Candidate* candidate;
    candidate = segment->add_candidate();
    candidate->value = "AAAA";
    candidate = segment->add_candidate();
    candidate->value = "Aaaa";
    EXPECT_CALL(*mock_converter, StartPrediction(_, _))
        .WillOnce(DoAll(SetArgPointee<1>(segments), Return(true)));
  }
  // Get suggestion
  composer_->InsertCharacterPreedit("aaaa");
  EXPECT_TRUE(converter.Suggest(*composer_, Context::default_instance()));
  EXPECT_TRUE(converter.IsActive());

  {  // Initialize no suggest result triggered by "aaaaa".
    Segments segments;
    Segment* segment = segments.add_segment();
    segment->set_key("aaaaa");
    EXPECT_CALL(*mock_converter, StartPrediction(_, _))
        .WillOnce(DoAll(SetArgPointee<1>(segments), Return(false)));
  }
  // Hide suggestion
  composer_->InsertCharacterPreedit("a");
  EXPECT_FALSE(converter.Suggest(*composer_, Context::default_instance()));
  EXPECT_FALSE(converter.IsActive());
}

TEST_F(EngineConverterTest, Issue2040116) {
  // Unittest against http://b/2040116
  //
  // It happens when the first Predict returns results but the next
  // MaybeExpandPrediction does not return any results.  That's a
  // trick by GoogleSuggest.
  auto mock_converter = std::make_shared<MockConverter>();

  EngineConverter converter(mock_converter, request_, config_);
  composer_->InsertCharacterPreedit("G");

  {
    // Initialize no predict result.
    Segments segments;
    Segment* segment = segments.add_segment();
    segment->set_key("G");
    EXPECT_CALL(*mock_converter, StartPredictionWithPreviousSuggestion(_, _, _))
        .WillOnce(DoAll(SetArgPointee<2>(segments), Return(false)));
  }
  // Get prediction
  EXPECT_FALSE(converter.Predict(*composer_));
  Mock::VerifyAndClearExpectations(&mock_converter);
  EXPECT_FALSE(converter.IsActive());

  {
    // Initialize a suggest result triggered by "G".
    Segments segments;
    Segment* segment = segments.add_segment();
    segment->set_key("G");
    converter::Candidate* candidate;
    candidate = segment->add_candidate();
    candidate->value = "GoogleSuggest";
    EXPECT_CALL(*mock_converter, StartPredictionWithPreviousSuggestion(_, _, _))
        .WillOnce(DoAll(SetArgPointee<2>(segments), Return(true)));
  }
  // Get prediction again
  EXPECT_TRUE(converter.Predict(*composer_));
  Mock::VerifyAndClearExpectations(&mock_converter);
  EXPECT_TRUE(converter.IsActive());

  {  // Check the conversion.
    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());
    EXPECT_TRUE(output.has_preedit());
    EXPECT_TRUE(output.has_candidate_window());

    const commands::Preedit& conversion = output.preedit();
    EXPECT_EQ(conversion.segment_size(), 1);
    EXPECT_EQ(conversion.segment(0).value(), "GoogleSuggest");
  }

  {
    // Initialize no predict result triggered by "G".  It's possible
    // by Google Suggest.
    Segments segments;
    Segment* segment = segments.add_segment();
    segment->set_key("G");
    EXPECT_CALL(*mock_converter, StartPredictionWithPreviousSuggestion(_, _, _))
        .Times(0);
  }
  // Hide prediction
  EXPECT_CALL(*mock_converter, FocusSegmentValue(_, 0, 0));
  converter.CandidateNext(*composer_);
  Mock::VerifyAndClearExpectations(&mock_converter);
  EXPECT_TRUE(converter.IsActive());

  {  // Check the conversion.
    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());
    EXPECT_TRUE(output.has_preedit());
    EXPECT_TRUE(output.has_candidate_window());

    const commands::Preedit& conversion = output.preedit();
    EXPECT_EQ(conversion.segment_size(), 1);
    EXPECT_EQ(conversion.segment(0).value(), "GoogleSuggest");

    const commands::CandidateWindow& candidate_window =
        output.candidate_window();
    EXPECT_EQ(candidate_window.candidate_size(), 1);
  }
}

TEST_F(EngineConverterTest, GetReadingText) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);

  const char* kKanjiAiueo = "阿伊宇江於";
  // Set up Segments for reverse conversion.
  Segments reverse_segments;
  Segment* segment;
  segment = reverse_segments.add_segment();
  segment->set_key(kKanjiAiueo);
  converter::Candidate* candidate;
  candidate = segment->add_candidate();
  // For reverse conversion, key is the original kanji string.
  candidate->key = kKanjiAiueo;
  candidate->value = kChars_Aiueo;
  EXPECT_CALL(*mock_converter,
              StartReverseConversion(_, absl::string_view(kKanjiAiueo)))
      .WillOnce(DoAll(SetArgPointee<0>(reverse_segments), Return(true)));
  std::string reading;
  EXPECT_TRUE(converter.GetReadingText(kKanjiAiueo, &reading));
  EXPECT_EQ(reading, kChars_Aiueo);
}

TEST_F(EngineConverterTest, ZeroQuerySuggestion) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);

  // Set up a mock suggestion result.
  Segments segments;
  Segment* segment;
  segment = segments.add_segment();
  segment->set_key("");
  segment->add_candidate()->value = "search";
  segment->add_candidate()->value = "input";
  EXPECT_CALL(*mock_converter, StartPrediction(_, _))
      .WillOnce(DoAll(SetArgPointee<1>(segments), Return(true)));

  EXPECT_TRUE(composer_->Empty());
  EXPECT_TRUE(converter.Suggest(*composer_, Context::default_instance()));
  EXPECT_TRUE(IsCandidateListVisible(converter));
  EXPECT_TRUE(converter.IsActive());

  {  // Check the output
    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());
    EXPECT_FALSE(output.has_preedit());
    EXPECT_TRUE(output.has_candidate_window());

    const commands::CandidateWindow& candidate_window =
        output.candidate_window();
    EXPECT_EQ(candidate_window.size(), 2);
    EXPECT_EQ(candidate_window.candidate(0).value(), "search");
    EXPECT_EQ(candidate_window.candidate(1).value(), "input");
  }
}

TEST(EngineConverterResetTest, Reset) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter);
  EXPECT_CALL(*mock_converter, ResetConversion(_));
  converter.Reset();
}

TEST(EngineConverterRevertTest, Revert) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter);
  EXPECT_CALL(*mock_converter, RevertConversion(_));
  converter.Revert();
}

TEST_F(EngineConverterTest, DeleteCandidateFromHistory) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);

  EXPECT_CALL(*mock_converter, DeleteCandidateFromHistory(_, _, _))
      .WillRepeatedly(Return(true));

  // No valid segments / focused candidate list
  EXPECT_FALSE(converter.DeleteCandidateFromHistory(std::nullopt));
  EXPECT_FALSE(converter.DeleteCandidateFromHistory(1));

  {
    // Set segments
    Segments segments;
    SetAiueo(&segments);
    composer_->InsertCharacterPreedit(kChars_Aiueo);
    FillT13Ns(&segments, composer_.get());
    EXPECT_CALL(*mock_converter, StartConversion(_, _))
        .WillOnce(DoAll(SetArgPointee<1>(segments), Return(true)));

    converter.Convert(*composer_);
  }
  EXPECT_TRUE(converter.DeleteCandidateFromHistory(std::nullopt));
  EXPECT_TRUE(converter.DeleteCandidateFromHistory(1));
  // Invalid candidate id
  EXPECT_FALSE(converter.DeleteCandidateFromHistory(-100));
}

TEST_F(EngineConverterTest, CommitHead) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);
  composer_->InsertCharacterPreedit(kChars_Aiueo);

  size_t committed_size;
  converter.CommitHead(1, *composer_, &committed_size);
  EXPECT_EQ(committed_size, 1);
  composer_->DeleteAt(0);

  commands::Output output;
  converter.FillOutput(*composer_, &output);
  EXPECT_TRUE(output.has_result());
  EXPECT_FALSE(output.has_candidate_window());

  const commands::Result& result = output.result();
  EXPECT_EQ(result.value(), "あ");
  EXPECT_EQ(result.key(), "あ");
  std::string preedit = composer_->GetStringForPreedit();
  EXPECT_EQ(preedit, "いうえお");

  converter.CommitHead(3, *composer_, &committed_size);
  EXPECT_EQ(committed_size, 3);
  composer_->DeleteAt(0);
  composer_->DeleteAt(0);
  composer_->DeleteAt(0);
  converter.FillOutput(*composer_, &output);
  EXPECT_TRUE(output.has_result());
  EXPECT_FALSE(output.has_candidate_window());

  const commands::Result& result2 = output.result();
  EXPECT_EQ(result2.value(), "いうえ");
  EXPECT_EQ(result2.key(), "いうえ");
  preedit = composer_->GetStringForPreedit();
  EXPECT_EQ(preedit, "お");
}

TEST_F(EngineConverterTest, CommandCandidate) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);
  Segments segments;
  SetAiueo(&segments);
  composer_->InsertCharacterPreedit("あいうえお");
  FillT13Ns(&segments, composer_.get());
  // set COMMAND_CANDIDATE.
  SetCommandCandidate(&segments, 0, 0, converter::Candidate::DEFAULT_COMMAND);
  EXPECT_CALL(*mock_converter, StartConversion(_, _))
      .WillOnce(DoAll(SetArgPointee<1>(segments), Return(true)));

  composer_->InsertCharacterPreedit(kChars_Aiueo);
  EXPECT_TRUE(converter.Convert(*composer_));

  converter.Commit(*composer_, Context::default_instance());
  commands::Output output;
  converter.FillOutput(*composer_, &output);
  EXPECT_FALSE(output.has_result());
}

TEST_F(EngineConverterTest, CommandCandidateWithCommitCommands) {
  const std::string kKamabokono = "かまぼこの";
  const std::string kInbou = "いんぼう";
  composer_->InsertCharacterPreedit(kKamabokono + kInbou);

  {
    // The first candidate is a command candidate, so
    // CommitFirstSegment resets all conversion.
    auto mock_converter = std::make_shared<MockConverter>();
    EngineConverter converter(mock_converter, request_, config_);
    Segments segments;
    SetKamaboko(&segments);
    SetCommandCandidate(&segments, 0, 0, converter::Candidate::DEFAULT_COMMAND);
    EXPECT_CALL(*mock_converter, StartConversion(_, _))
        .WillOnce(DoAll(SetArgPointee<1>(segments), Return(true)));
    converter.Convert(*composer_);

    size_t committed_size = 0;
    converter.CommitFirstSegment(*composer_, Context::default_instance(),
                                 &committed_size);
    EXPECT_EQ(committed_size, 0);

    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(converter.IsActive());
    EXPECT_FALSE(output.has_result());
  }

  {
    // The second candidate is a command candidate, so
    // CommitFirstSegment commits all conversion.
    auto mock_converter = std::make_shared<MockConverter>();
    EngineConverter converter(mock_converter, request_, config_);
    Segments segments;
    SetKamaboko(&segments);
    SetCommandCandidate(&segments, 1, 0, converter::Candidate::DEFAULT_COMMAND);
    EXPECT_CALL(*mock_converter, StartConversion(_, _))
        .WillOnce(DoAll(SetArgPointee<1>(segments), Return(true)));
    converter.Convert(*composer_);

    size_t committed_size = 0;
    converter.CommitFirstSegment(*composer_, Context::default_instance(),
                                 &committed_size);
    EXPECT_EQ(committed_size, Util::CharsLen(kKamabokono));

    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_TRUE(converter.IsActive());
    EXPECT_TRUE(output.has_result());
  }

  {
    // The selected suggestion with Id is a command candidate.
    auto mock_converter = std::make_shared<MockConverter>();
    EngineConverter converter(mock_converter, request_, config_);
    Segments segments;
    SetAiueo(&segments);
    SetCommandCandidate(&segments, 0, 0, converter::Candidate::DEFAULT_COMMAND);
    EXPECT_CALL(*mock_converter, StartPrediction(_, _))
        .WillOnce(DoAll(SetArgPointee<1>(segments), Return(true)));
    converter.Suggest(*composer_, Context::default_instance());

    size_t committed_size = 0;
    EXPECT_FALSE(converter.CommitSuggestionById(
        0, *composer_, Context::default_instance(), &committed_size));
    EXPECT_EQ(committed_size, 0);
  }

  {
    // The selected suggestion with Index is a command candidate.
    auto mock_converter = std::make_shared<MockConverter>();
    EngineConverter converter(mock_converter, request_, config_);
    Segments segments;
    SetAiueo(&segments);
    SetCommandCandidate(&segments, 0, 1, converter::Candidate::DEFAULT_COMMAND);
    EXPECT_CALL(*mock_converter, StartPrediction(_, _))
        .WillOnce(DoAll(SetArgPointee<1>(segments), Return(true)));
    converter.Suggest(*composer_, Context::default_instance());

    size_t committed_size = 0;
    EXPECT_FALSE(converter.CommitSuggestionByIndex(
        1, *composer_, Context::default_instance(), &committed_size));
    EXPECT_EQ(committed_size, 0);
  }
}

TEST_F(EngineConverterTest, ExecuteCommandCandidate) {
  // Enable Incognito mode
  {
    config_->set_incognito_mode(false);
    auto mock_converter = std::make_shared<MockConverter>();
    EngineConverter converter(mock_converter, request_, config_);

    Segments segments;
    SetAiueo(&segments);
    SetCommandCandidate(&segments, 0, 0,
                        converter::Candidate::ENABLE_INCOGNITO_MODE);
    EXPECT_CALL(*mock_converter, StartConversion(_, _))
        .WillOnce(DoAll(SetArgPointee<1>(segments), Return(true)));

    composer_->InsertCharacterPreedit(kChars_Aiueo);
    EXPECT_TRUE(converter.Convert(*composer_));

    converter.Commit(*composer_, Context::default_instance());
    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());

    // The config in the |output| has the updated value, but |config_| keeps
    // the previous value.
    EXPECT_TRUE(output.has_config());
    EXPECT_TRUE(output.config().incognito_mode());
    EXPECT_FALSE(config_->incognito_mode());
  }

  // Disable Incognito mode
  {
    config_->set_incognito_mode(false);
    auto mock_converter = std::make_shared<MockConverter>();
    EngineConverter converter(mock_converter, request_, config_);

    Segments segments;
    SetAiueo(&segments);
    SetCommandCandidate(&segments, 0, 0,
                        converter::Candidate::DISABLE_INCOGNITO_MODE);
    EXPECT_CALL(*mock_converter, StartConversion(_, _))
        .WillOnce(DoAll(SetArgPointee<1>(segments), Return(true)));

    composer_->InsertCharacterPreedit(kChars_Aiueo);
    EXPECT_TRUE(converter.Convert(*composer_));

    converter.Commit(*composer_, Context::default_instance());
    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());

    // The config in the |output| has the updated value, but |config_| keeps
    // the previous value.
    EXPECT_TRUE(output.has_config());
    EXPECT_FALSE(output.config().incognito_mode());
    EXPECT_FALSE(config_->incognito_mode());
  }

  // Enable Presentation mode
  {
    config_->set_presentation_mode(false);
    auto mock_converter = std::make_shared<MockConverter>();
    EngineConverter converter(mock_converter, request_, config_);

    Segments segments;
    SetAiueo(&segments);
    SetCommandCandidate(&segments, 0, 0,
                        converter::Candidate::ENABLE_PRESENTATION_MODE);
    EXPECT_CALL(*mock_converter, StartConversion(_, _))
        .WillOnce(DoAll(SetArgPointee<1>(segments), Return(true)));

    composer_->InsertCharacterPreedit(kChars_Aiueo);
    EXPECT_TRUE(converter.Convert(*composer_));

    converter.Commit(*composer_, Context::default_instance());
    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());

    // The config in the |output| has the updated value, but |config_| keeps
    // the previous value.
    EXPECT_TRUE(output.has_config());
    EXPECT_TRUE(output.config().presentation_mode());
    EXPECT_FALSE(config_->presentation_mode());
  }

  // Disable Presentation mode
  {
    config_->set_incognito_mode(true);
    auto mock_converter = std::make_shared<MockConverter>();
    EngineConverter converter(mock_converter, request_, config_);

    Segments segments;
    SetAiueo(&segments);
    SetCommandCandidate(&segments, 0, 0,
                        converter::Candidate::DISABLE_PRESENTATION_MODE);
    EXPECT_CALL(*mock_converter, StartConversion(_, _))
        .WillOnce(DoAll(SetArgPointee<1>(segments), Return(true)));

    composer_->InsertCharacterPreedit(kChars_Aiueo);
    EXPECT_TRUE(converter.Convert(*composer_));

    converter.Commit(*composer_, Context::default_instance());
    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());

    // The config in the |output| has the updated value, but |config_| keeps
    // the previous value.
    EXPECT_TRUE(output.has_config());
    EXPECT_FALSE(output.config().presentation_mode());
    EXPECT_FALSE(config_->presentation_mode());
  }
}

TEST_F(EngineConverterTest, PropagateConfigToRenderer) {
  // Disable information_list_config()
  {
    auto mock_converter = std::make_shared<MockConverter>();
    EngineConverter converter(mock_converter, request_, config_);
    Segments segments;
    SetAiueo(&segments);
    composer_->InsertCharacterPreedit("あいうえお");
    FillT13Ns(&segments, composer_.get());
    EXPECT_CALL(*mock_converter, StartConversion(_, _))
        .WillOnce(DoAll(SetArgPointee<1>(segments), Return(true)));

    commands::Output output;
    composer_->InsertCharacterPreedit(kChars_Aiueo);
    converter.Convert(*composer_);

    EXPECT_FALSE(IsCandidateListVisible(converter));
    output.Clear();
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_config());

    converter.CandidateNext(*composer_);
    EXPECT_TRUE(IsCandidateListVisible(converter));
    output.Clear();
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_config());
  }
}

TEST_F(EngineConverterTest, ConversionFail) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);

  // Conversion fails.
  {
    // segments doesn't have any candidates.
    Segments segments;
    segments.add_segment()->set_key(kChars_Aiueo);
    EXPECT_CALL(*mock_converter, StartConversion(_, _))
        .WillOnce(DoAll(SetArgPointee<1>(segments), Return(false)));
    composer_->InsertCharacterPreedit(kChars_Aiueo);

    // Falls back to composition state.
    EXPECT_FALSE(converter.Convert(*composer_));
    Mock::VerifyAndClearExpectations(&mock_converter);
    EXPECT_FALSE(IsCandidateListVisible(converter));
    EXPECT_TRUE(converter.CheckState(EngineConverterInterface::COMPOSITION));

    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());
    EXPECT_TRUE(output.has_preedit());
    EXPECT_FALSE(output.has_candidate_window());
    EXPECT_FALSE(IsCandidateListVisible(converter));
  }

  composer_->Reset();

  // Suggestion succeeds and conversion fails.
  {
    Segments segments;
    SetAiueo(&segments);
    EXPECT_CALL(*mock_converter, StartPrediction(_, _))
        .WillOnce(DoAll(SetArgPointee<1>(segments), Return(true)));
    composer_->InsertCharacterPreedit(kChars_Aiueo);

    EXPECT_TRUE(converter.Suggest(*composer_, Context::default_instance()));
    Mock::VerifyAndClearExpectations(&mock_converter);
    EXPECT_TRUE(IsCandidateListVisible(converter));
    EXPECT_TRUE(converter.CheckState(EngineConverterInterface::SUGGESTION));

    commands::Output output;
    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());
    EXPECT_TRUE(output.has_preedit());
    EXPECT_TRUE(output.has_candidate_window());

    segments.Clear();
    output.Clear();

    // segments doesn't have any candidates.
    segments.add_segment()->set_key(kChars_Aiueo);
    EXPECT_CALL(*mock_converter, StartConversion(_, _))
        .WillOnce(DoAll(SetArgPointee<1>(segments), Return(false)));

    // Falls back to composition state.
    EXPECT_FALSE(converter.Convert(*composer_));
    Mock::VerifyAndClearExpectations(&mock_converter);
    EXPECT_FALSE(IsCandidateListVisible(converter));
    EXPECT_TRUE(converter.CheckState(EngineConverterInterface::COMPOSITION));

    converter.FillOutput(*composer_, &output);
    EXPECT_FALSE(output.has_result());
    EXPECT_TRUE(output.has_preedit());
    EXPECT_FALSE(output.has_candidate_window());
  }
}

TEST_F(EngineConverterTest, ResetByClientRevision) {
  const int32_t kRevision = 0x1234;

  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);
  Context context;

  // Initialize the session converter with given context age.
  EXPECT_CALL(*mock_converter, ResetConversion(_));
  context.set_revision(kRevision);
  converter.OnStartComposition(context);
  EXPECT_CALL(*mock_converter, RevertConversion(_));
  converter.Revert();

  // OnStartComposition with different context age causes Reset()
  EXPECT_CALL(*mock_converter, ResetConversion(_));
  context.set_revision(kRevision + 1);
  converter.OnStartComposition(context);
}

TEST_F(EngineConverterTest, ResetByPrecedingText) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);

  // no preceding_text -> Reset should not be called.
  {
    Segments segments;
    SetAiueo(&segments);
    composer_->InsertCharacterPreedit("あいうえお");
    FillT13Ns(&segments, composer_.get());
    for (Segment& segment : segments) {
      segment.set_segment_type(Segment::HISTORY);
    }
    SetSegments(segments, &converter);
    converter.OnStartComposition(Context::default_instance());
    EXPECT_CALL(*mock_converter, RevertConversion(_));
    converter.Revert();
  }

  // preceding_text == history_segments -> Reset should not be called.
  {
    Segments segments;
    SetAiueo(&segments);
    composer_->InsertCharacterPreedit("あいうえお");
    FillT13Ns(&segments, composer_.get());
    for (Segment& segment : segments) {
      segment.set_segment_type(Segment::HISTORY);
    }
    SetSegments(segments, &converter);
    Context context;
    context.set_preceding_text(kChars_Aiueo);
    converter.OnStartComposition(context);
    EXPECT_CALL(*mock_converter, RevertConversion(_));
    converter.Revert();
  }

  // preceding_text == "" && history_segments != "" -> Reset should be called.
  {
    Segments segments;
    SetAiueo(&segments);
    composer_->InsertCharacterPreedit("あいうえお");
    FillT13Ns(&segments, composer_.get());
    for (Segment& segment : segments) {
      segment.set_segment_type(Segment::HISTORY);
    }
    SetSegments(segments, &converter);
    Context context;
    context.set_preceding_text("");
    EXPECT_CALL(*mock_converter, ResetConversion(_));
    converter.OnStartComposition(context);
    EXPECT_CALL(*mock_converter, RevertConversion(_));
    converter.Revert();
  }

  // preceding_text != "" && preceding_text.EndsWith(history_segments).
  //    -> Reset should not be called.
  {
    Segments segments;
    SetAiueo(&segments);
    composer_->InsertCharacterPreedit("あいうえお");
    FillT13Ns(&segments, composer_.get());
    for (Segment& segment : segments) {
      segment.set_segment_type(Segment::HISTORY);
    }
    SetSegments(segments, &converter);
    Context context;
    context.set_preceding_text(kChars_Aiueo);
    converter.OnStartComposition(context);
  }

  // preceding_text != "" && history_segments.EndsWith(preceding_text).
  //    -> Reset should not be called.
  {
    Segments segments;
    SetAiueo(&segments);
    composer_->InsertCharacterPreedit("あいうえお");
    FillT13Ns(&segments, composer_.get());
    for (Segment& segment : segments) {
      segment.set_segment_type(Segment::HISTORY);
    }
    SetSegments(segments, &converter);
    Context context;
    context.set_preceding_text(kChars_Aiueo);
    converter.OnStartComposition(context);
    EXPECT_CALL(*mock_converter, RevertConversion(_));
    converter.Revert();
  }
}

TEST_F(EngineConverterTest, ReconstructHistoryByPrecedingText) {
  constexpr uint16_t kId = 1234;
  constexpr char kKey[] = "1";
  constexpr char kValue[] = "1";

  // Set up the result which mock_converter.ReconstructHistory() returns.
  Segments mock_result;
  {
    Segment* segment = mock_result.add_segment();
    segment->set_key(kKey);
    segment->set_segment_type(Segment::HISTORY);
    converter::Candidate* candidate = segment->push_back_candidate();
    candidate->rid = kId;
    candidate->lid = kId;
    candidate->content_key = kKey;
    candidate->key = kKey;
    candidate->content_value = kValue;
    candidate->value = kValue;
    candidate->attributes = converter::Attribute::NO_LEARNING;
  }

  // With revision
  {
    auto mock_converter = std::make_shared<MockConverter>();
    EngineConverter converter(mock_converter, request_, config_);

    EXPECT_CALL(*mock_converter, ReconstructHistory(_, absl::string_view(kKey)))
        .WillOnce(DoAll(SetArgPointee<0>(mock_result), Return(true)));

    Context context;
    context.set_revision(0);
    context.set_preceding_text(kKey);
    // History segments should be reconstructed by this call.
    converter.OnStartComposition(context);
    EXPECT_THAT(GetSegments(converter), EqualsSegments(mock_result));

    // Increment the revision. Since the history segments for kKey was already
    // constructed, ReconstructHistory should not be called.
    context.set_revision(1);
    context.set_preceding_text(kKey);
    converter.OnStartComposition(context);
  }

  // Without revision
  {
    auto mock_converter = std::make_shared<MockConverter>();
    EngineConverter converter(mock_converter, request_, config_);

    EXPECT_CALL(*mock_converter, ReconstructHistory(_, absl::string_view(kKey)))
        .WillOnce(DoAll(SetArgPointee<0>(mock_result), Return(true)));

    Context context;
    context.set_preceding_text(kKey);
    converter.OnStartComposition(context);
    // History segments should be reconstructed by this call.
    converter.OnStartComposition(context);
    EXPECT_THAT(GetSegments(converter), EqualsSegments(mock_result));

    // Revision is not present but, since the history segments for kKey was
    // already constructed, ReconstructHistory should not be called.
    context.set_preceding_text(kKey);
    converter.OnStartComposition(context);
  }
}

// Test whether Request::candidate_page_size is correctly propagated to
// CandidateList.page_size in EngineConverter.  The tests for the behavior
// of CandidateList.page_size is in session/internal/candidate_list_test.cc
TEST_F(EngineConverterTest, CandidatePageSize) {
  constexpr size_t kPageSize = 3;
  request_->set_candidate_page_size(kPageSize);
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);
  EXPECT_EQ(GetCandidateList(converter).page_size(), kPageSize);
}

// Test output.result.tokens is filled on commit.
TEST_F(EngineConverterTest, ResultTokens) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);

  Segments segments;
  {
    Segment* segment = segments.add_segment();
    converter::Candidate* candidate;
    segment->set_key("きょうは");
    candidate = segment->add_candidate();
    candidate->key = "きょうは";
    candidate->value = "今日は";
    candidate->content_key = "きょう";
    candidate->content_value = "今日";
    candidate->lid = 100;
    candidate->rid = 101;
  }
  {
    Segment* segment = segments.add_segment();
    converter::Candidate* candidate;
    segment->set_key("はれ");
    candidate = segment->add_candidate();
    candidate->key = "はれ";
    candidate->value = "晴れ";
    candidate->content_key = candidate->key;
    candidate->content_value = candidate->value;
    candidate->lid = 200;
    candidate->rid = 201;
  }
  EXPECT_CALL(*mock_converter, StartConversion(_, _))
      .WillOnce(DoAll(SetArgPointee<1>(segments), Return(true)));

  composer_->InsertCharacterPreedit("きょうははれ");
  EXPECT_TRUE(converter.Convert(*composer_));
  ASSERT_TRUE(converter.IsActive());

  commands::Output output;
  converter.Commit(*composer_, Context::default_instance());
  converter.FillOutput(*composer_, &output);

  EXPECT_TRUE(output.has_result());
  EXPECT_EQ(output.result().tokens_size(), 3);
  EXPECT_EQ(output.result().tokens(0).key(), "きょう");
  EXPECT_EQ(output.result().tokens(0).value(), "今日");
  EXPECT_EQ(output.result().tokens(0).lid(), 100);
  EXPECT_EQ(output.result().tokens(0).rid(), -1);

  EXPECT_EQ(output.result().tokens(1).key(), "は");
  EXPECT_EQ(output.result().tokens(1).value(), "は");
  EXPECT_EQ(output.result().tokens(1).lid(), -1);
  EXPECT_EQ(output.result().tokens(1).rid(), 101);

  EXPECT_EQ(output.result().tokens(2).key(), "はれ");
  EXPECT_EQ(output.result().tokens(2).value(), "晴れ");
  EXPECT_EQ(output.result().tokens(2).lid(), 200);
  EXPECT_EQ(output.result().tokens(2).rid(), 201);
}

TEST_F(EngineConverterTest, ResultTokensWithInnerSegements) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);

  Segments segments;
  {
    Segment* segment = segments.add_segment();
    converter::Candidate* candidate;
    segment->set_key("きょうははれ");
    candidate = segment->add_candidate();
    candidate->key = "きょうははれ";
    candidate->value = "今日は晴れ";
    candidate->content_key = "きょうははれ";
    candidate->content_value = "今日は晴れ";
    candidate->lid = 100;
    candidate->rid = 201;
    candidate->inner_segment_boundary = converter::BuildInnerSegmentBoundary(
        {// len("きょうは"), len("今日"), len("きょう"), len("今日")
         {12, 9, 9, 6},
         // len("はれ"), len("晴れ"), len("はれ"), len("晴れ")
         {6, 6, 6, 6}},
        candidate->key, candidate->value);
  }
  EXPECT_CALL(*mock_converter, StartConversion(_, _))
      .WillOnce(DoAll(SetArgPointee<1>(segments), Return(true)));

  composer_->InsertCharacterPreedit("きょうははれ");
  EXPECT_TRUE(converter.Convert(*composer_));
  ASSERT_TRUE(converter.IsActive());

  commands::Output output;
  converter.Commit(*composer_, Context::default_instance());
  converter.FillOutput(*composer_, &output);

  EXPECT_TRUE(output.has_result());
  EXPECT_EQ(output.result().tokens_size(), 3);
  EXPECT_EQ(output.result().tokens(0).key(), "きょう");
  EXPECT_EQ(output.result().tokens(0).value(), "今日");
  EXPECT_EQ(output.result().tokens(0).lid(), 100);
  EXPECT_EQ(output.result().tokens(0).rid(), -1);

  EXPECT_EQ(output.result().tokens(1).key(), "は");
  EXPECT_EQ(output.result().tokens(1).value(), "は");
  EXPECT_EQ(output.result().tokens(1).lid(), -1);
  EXPECT_EQ(output.result().tokens(1).rid(), -1);

  EXPECT_EQ(output.result().tokens(2).key(), "はれ");
  EXPECT_EQ(output.result().tokens(2).value(), "晴れ");
  EXPECT_EQ(output.result().tokens(2).lid(), -1);
  EXPECT_EQ(output.result().tokens(2).rid(), 201);
}


TEST_F(EngineConverterTest, ExplicitAsciiResidualRepairUsesUniqueBestReading) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);

  auto fill_conversion = [](const ConversionRequest& request,
                            Segments* segments) {
    segments->Clear();
    const std::string key(request.key());
    std::string value = key;
    int cost = 900;
    if (key == "でさ") {
      value = "でさ";
      cost = 500;
    } else if (key == "でし") {
      value = "弟子";
      cost = 300;
    } else if (key == "です") {
      value = "です";
      cost = 100;
    } else if (key == "でせ") {
      value = "でせ";
      cost = 450;
    } else if (key == "でそ") {
      value = "でそ";
      cost = 400;
    }
    Segment* segment = segments->add_segment();
    segment->set_key(key);
    converter::Candidate* candidate = segment->add_candidate();
    candidate->key = key;
    candidate->content_key = key;
    candidate->value = value;
    candidate->content_value = value;
    candidate->cost = cost;
    candidate->wcost = cost;
    return true;
  };
  EXPECT_CALL(*mock_converter, StartConversion(_, _))
      .Times(6)
      .WillRepeatedly(::testing::Invoke(fill_conversion));

  composer_->InsertCharacterKeyAndPreedit("de", "で");
  composer_->InsertCharacterKeyAndPreedit("s", "s");
  ASSERT_TRUE(converter.Convert(*composer_));
  ASSERT_TRUE(converter.TryAsciiResidualCorrection(*composer_));
  EXPECT_TRUE(converter.CurrentConversionHasReadingCorrection());

  const Segments& repaired = GetSegments(converter);
  ASSERT_EQ(repaired.conversion_segments_size(), 1);
  ASSERT_GT(repaired.conversion_segment(0).candidates_size(), 0);
  const converter::Candidate& top = repaired.conversion_segment(0).candidate(0);
  EXPECT_EQ(top.value, "です");
  EXPECT_EQ(top.prefix, "→ ");
  EXPECT_EQ(top.description, "<もしかして>");
  EXPECT_NE(top.attributes & converter::Attribute::SPELLING_CORRECTION, 0);
  EXPECT_EQ(top.attributes & converter::Attribute::TYPING_CORRECTION, 0);
  for (size_t i = 0; i < repaired.conversion_segment(0).candidates_size(); ++i) {
    EXPECT_NE(repaired.conversion_segment(0).candidate(i).attributes &
                  converter::Attribute::NO_LEARNING,
              0);
  }

  std::string finish_key;
  EXPECT_CALL(*mock_converter, FinishConversion(_, _))
      .WillOnce(::testing::Invoke(
          [&finish_key](const ConversionRequest& request, Segments*) {
            finish_key = std::string(request.key());
          }));
  converter.Commit(*composer_, Context::default_instance());
  EXPECT_EQ(finish_key, "です");
}

TEST_F(EngineConverterTest, ExplicitAsciiResidualRepairPreservesHistorySegments) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);

  int call_count = 0;
  EXPECT_CALL(*mock_converter, StartConversion(_, _))
      .Times(6)
      .WillRepeatedly(::testing::Invoke(
          [&call_count](const ConversionRequest& request, Segments* segments) {
            const std::string key(request.key());
            if (call_count == 0) {
              segments->Clear();
              Segment* history = segments->add_segment();
              history->set_segment_type(Segment::HISTORY);
              history->set_key("まえ");
              converter::Candidate* history_candidate = history->add_candidate();
              history_candidate->key = "まえ";
              history_candidate->value = "前";
              history_candidate->content_key = history_candidate->key;
              history_candidate->content_value = history_candidate->value;
            } else {
              EXPECT_EQ(segments->history_segments_size(), 1);
              EXPECT_EQ(segments->history_segment(0).candidate(0).value, "前");
              segments->clear_conversion_segments();
            }
            ++call_count;
            Segment* segment = segments->add_segment();
            segment->set_key(key);
            converter::Candidate* candidate = segment->add_candidate();
            candidate->key = key;
            candidate->content_key = key;
            candidate->value = key;
            candidate->content_value = key;
            candidate->cost = key == "です" ? 100 : 500;
            candidate->wcost = candidate->cost;
            return true;
          }));

  composer_->InsertCharacterKeyAndPreedit("raw", "でs");
  ASSERT_TRUE(converter.Convert(*composer_));
  ASSERT_TRUE(converter.TryAsciiResidualCorrection(*composer_));

  const Segments& repaired = GetSegments(converter);
  ASSERT_EQ(repaired.history_segments_size(), 1);
  EXPECT_EQ(repaired.history_segment(0).candidate(0).value, "前");
  ASSERT_EQ(repaired.conversion_segments_size(), 1);
  EXPECT_EQ(repaired.conversion_segment(0).candidate(0).value, "です");
}

TEST_F(EngineConverterTest, ExplicitAsciiResidualRepairPreservesNoHistoryMode) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);

  EXPECT_CALL(*mock_converter, StartConversion(_, _))
      .Times(6)
      .WillRepeatedly(::testing::Invoke(
          [](const ConversionRequest& request, Segments* segments) {
            EXPECT_FALSE(request.options().enable_user_history_for_conversion);
            segments->Clear();
            const std::string key(request.key());
            AddSegmentWithSingleCandidate(segments, key, key);
            auto* candidate =
                segments->mutable_conversion_segment(0)->mutable_candidate(0);
            candidate->cost = key == "です" ? 100 : 500;
            candidate->wcost = candidate->cost;
            return true;
          }));

  composer_->InsertCharacterKeyAndPreedit("raw", "でs");
  ConversionPreferences preferences = converter.conversion_preferences();
  preferences.use_history = false;
  ASSERT_TRUE(converter.ConvertWithPreferences(*composer_, preferences));
  ASSERT_TRUE(converter.TryAsciiResidualCorrection(*composer_));
  EXPECT_EQ(GetSegments(converter).conversion_segment(0).candidate(0).value,
            "です");
}

TEST_F(EngineConverterTest, ExplicitAsciiResidualRepairWorksInsideKana) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);

  EXPECT_CALL(*mock_converter, StartConversion(_, _))
      .Times(6)
      .WillRepeatedly(::testing::Invoke(
          [](const ConversionRequest& request, Segments* segments) {
            segments->Clear();
            const std::string key(request.key());
            Segment* segment = segments->add_segment();
            segment->set_key(key);
            converter::Candidate* candidate = segment->add_candidate();
            candidate->key = key;
            candidate->content_key = key;
            candidate->value = key == "おつかれさまです" ? "お疲れ様です" : key;
            candidate->content_value = candidate->value;
            candidate->cost = key == "おつかれさまです" ? 100 : 500;
            candidate->wcost = candidate->cost;
            return true;
          }));

  composer_->InsertCharacterKeyAndPreedit("raw", "おつかれsまです");
  ASSERT_TRUE(converter.Convert(*composer_));
  ASSERT_TRUE(converter.TryAsciiResidualCorrection(*composer_));
  EXPECT_TRUE(converter.CurrentConversionHasReadingCorrection());
  const Segments& result = GetSegments(converter);
  ASSERT_EQ(result.conversion_segments_size(), 1);
  EXPECT_EQ(result.conversion_segment(0).candidate(0).value, "お疲れ様です");
  EXPECT_EQ(result.conversion_segment(0).candidate(0).prefix, "→ ");
  EXPECT_EQ(result.conversion_segment(0).candidate(0).description,
            "<もしかして>");
}

TEST_F(EngineConverterTest, ExplicitAsciiResidualRepairFailsClosedOnTie) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);

  EXPECT_CALL(*mock_converter, StartConversion(_, _))
      .Times(6)
      .WillRepeatedly(::testing::Invoke(
          [](const ConversionRequest& request, Segments* segments) {
            segments->Clear();
            const std::string key(request.key());
            Segment* segment = segments->add_segment();
            segment->set_key(key);
            converter::Candidate* candidate = segment->add_candidate();
            candidate->key = key;
            candidate->content_key = key;
            candidate->value = key;
            candidate->content_value = key;
            candidate->cost = key == "でs" ? 900 : 100;
            candidate->wcost = candidate->cost;
            return true;
          }));

  composer_->InsertCharacterKeyAndPreedit("de", "で");
  composer_->InsertCharacterKeyAndPreedit("s", "s");
  ASSERT_TRUE(converter.Convert(*composer_));
  EXPECT_FALSE(converter.TryAsciiResidualCorrection(*composer_));
  EXPECT_FALSE(converter.CurrentConversionHasReadingCorrection());
  const Segments& result = GetSegments(converter);
  ASSERT_EQ(result.conversion_segments_size(), 1);
  EXPECT_EQ(result.conversion_segment(0).candidate(0).value, "でs");
}

TEST_F(EngineConverterTest,
       ExplicitAsciiResidualRepairRejectsNestedMozcReadingCorrection) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);

  EXPECT_CALL(*mock_converter, StartConversion(_, _))
      .Times(6)
      .WillRepeatedly(::testing::Invoke(
          [](const ConversionRequest& request, Segments* segments) {
            segments->Clear();
            const std::string key(request.key());
            AddSegmentWithSingleCandidate(segments, key, key);
            auto* candidate =
                segments->mutable_conversion_segment(0)->mutable_candidate(0);
            // Make でし look artificially best, but only through another
            // reading-changing Mozc correction.  The residual repair must
            // ignore that probe and choose the best self-standing reading.
            if (key == "でし") {
              candidate->value = "別の補正";
              candidate->content_value = candidate->value;
              candidate->cost = 10;
              candidate->attributes |=
                  converter::Attribute::TYPING_CORRECTION;
            } else if (key == "です") {
              candidate->cost = 100;
            } else {
              candidate->cost = 500;
            }
            candidate->wcost = candidate->cost;
            return true;
          }));

  composer_->InsertCharacterKeyAndPreedit("raw", "でs");
  ASSERT_TRUE(converter.Convert(*composer_));
  ASSERT_TRUE(converter.TryAsciiResidualCorrection(*composer_));
  const Segments& repaired = GetSegments(converter);
  ASSERT_EQ(repaired.conversion_segments_size(), 1);
  EXPECT_EQ(repaired.conversion_segment(0).candidate(0).value, "です");
}

TEST_F(EngineConverterTest,
       ExplicitAsciiResidualRepairRejectsNestedKeyCorrectorReadingCorrection) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);

  EXPECT_CALL(*mock_converter, StartConversion(_, _))
      .Times(6)
      .WillRepeatedly(::testing::Invoke(
          [](const ConversionRequest& request, Segments* segments) {
            segments->Clear();
            const std::string key(request.key());
            AddSegmentWithSingleCandidate(segments, key, key);
            auto* candidate =
                segments->mutable_conversion_segment(0)->mutable_candidate(0);
            // Model the legacy KeyCorrector path: the candidate key remains the
            // requested reading, while invisible provenance records that Mozc
            // used another dictionary reading internally.  Such a probe must
            // not outrank a reading that stands on its own.
            if (key == "でし") {
              candidate->value = "別の内部補正";
              candidate->content_value = candidate->value;
              candidate->cost = 10;
              candidate->attributes |=
                  converter::Attribute::KEY_EXPANDED_IN_DICTIONARY;
            } else if (key == "です") {
              candidate->cost = 100;
            } else {
              candidate->cost = 500;
            }
            candidate->wcost = candidate->cost;
            return true;
          }));

  composer_->InsertCharacterKeyAndPreedit("raw", "でs");
  ASSERT_TRUE(converter.Convert(*composer_));
  ASSERT_TRUE(converter.TryAsciiResidualCorrection(*composer_));
  const Segments& repaired = GetSegments(converter);
  ASSERT_EQ(repaired.conversion_segments_size(), 1);
  EXPECT_EQ(repaired.conversion_segment(0).candidate(0).value, "です");
}

TEST_F(EngineConverterTest, ExplicitAsciiResidualRepairRejectsUnsafeForms) {
  for (const std::string& input :
       {"でss", "でn", "でS", "でj", "でf", "でv", "でq",
        "でx", "でl", "でc", "3sです", "s", "sです", "デs"}) {
    auto mock_converter = std::make_shared<MockConverter>();
    EngineConverter converter(mock_converter, request_, config_);
    EXPECT_CALL(*mock_converter, StartConversion(_, _))
        .Times(1)
        .WillOnce(::testing::Invoke(
            [](const ConversionRequest& request, Segments* segments) {
              segments->Clear();
              const std::string key(request.key());
              AddSegmentWithSingleCandidate(segments, key, key);
              segments->mutable_conversion_segment(0)->mutable_candidate(0)->cost =
                  100;
              return true;
            }));

    composer_->Reset();
    composer_->InsertCharacterKeyAndPreedit("raw", input);
    ASSERT_TRUE(converter.Convert(*composer_));
    EXPECT_FALSE(converter.TryAsciiResidualCorrection(*composer_));
    EXPECT_FALSE(converter.CurrentConversionHasReadingCorrection());
  }
}

TEST_F(EngineConverterTest, ExplicitAsciiResidualRepairRequiresStandardRomanInput) {
  for (const bool use_kana_mode : {true, false}) {
    auto mock_converter = std::make_shared<MockConverter>();
    EngineConverter converter(mock_converter, request_, config_);
    if (use_kana_mode) {
      config_->set_preedit_method(config::Config::KANA);
      config_->clear_custom_roman_table();
    } else {
      config_->set_preedit_method(config::Config::ROMAN);
      config_->set_custom_roman_table("custom");
    }

    EXPECT_CALL(*mock_converter, StartConversion(_, _))
        .Times(1)
        .WillOnce(::testing::Invoke(
            [](const ConversionRequest& request, Segments* segments) {
              segments->Clear();
              AddSegmentWithSingleCandidate(segments, request.key(),
                                            request.key());
              return true;
            }));

    composer_->Reset();
    composer_->InsertCharacterKeyAndPreedit("raw", "でs");
    ASSERT_TRUE(converter.Convert(*composer_));
    EXPECT_FALSE(converter.TryAsciiResidualCorrection(*composer_));
  }

  config_->set_preedit_method(config::Config::ROMAN);
  config_->clear_custom_roman_table();
}

TEST_F(EngineConverterTest, ExplicitAsciiResidualRepairSupportsYRow) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);
  EXPECT_CALL(*mock_converter, StartConversion(_, _))
      .Times(4)
      .WillRepeatedly(::testing::Invoke(
          [](const ConversionRequest& request, Segments* segments) {
            segments->Clear();
            const std::string key(request.key());
            AddSegmentWithSingleCandidate(segments, key, key);
            auto* candidate =
                segments->mutable_conversion_segment(0)->mutable_candidate(0);
            candidate->cost = key == "へやです" ? 100 : 500;
            candidate->wcost = candidate->cost;
            return true;
          }));
  composer_->InsertCharacterKeyAndPreedit("raw", "へyです");
  ASSERT_TRUE(converter.Convert(*composer_));
  ASSERT_TRUE(converter.TryAsciiResidualCorrection(*composer_));
  EXPECT_EQ(GetSegments(converter).conversion_segment(0).candidate(0).value,
            "へやです");
}

TEST_F(EngineConverterTest, ExplicitAsciiResidualRepairSupportsWRow) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);
  EXPECT_CALL(*mock_converter, StartConversion(_, _))
      .Times(3)
      .WillRepeatedly(::testing::Invoke(
          [](const ConversionRequest& request, Segments* segments) {
            segments->Clear();
            const std::string key(request.key());
            AddSegmentWithSingleCandidate(segments, key, key);
            auto* candidate =
                segments->mutable_conversion_segment(0)->mutable_candidate(0);
            candidate->cost = key == "ほんをよみます" ? 100 : 500;
            candidate->wcost = candidate->cost;
            return true;
          }));
  composer_->InsertCharacterKeyAndPreedit("raw", "ほんwよみます");
  ASSERT_TRUE(converter.Convert(*composer_));
  ASSERT_TRUE(converter.TryAsciiResidualCorrection(*composer_));
  EXPECT_EQ(GetSegments(converter).conversion_segment(0).candidate(0).value,
            "ほんをよみます");
}

TEST_F(EngineConverterTest, MozcSolvedAsciiResidualStillEstablishesCorrectedKey) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);
  EXPECT_CALL(*mock_converter, StartConversion(_, _))
      .Times(6)
      .WillRepeatedly(::testing::Invoke(
          [](const ConversionRequest& request, Segments* segments) {
            segments->Clear();
            const std::string key(request.key());
            AddSegmentWithSingleCandidate(segments, key,
                                          key == "でs" ? "です" : key);
            auto* candidate =
                segments->mutable_conversion_segment(0)->mutable_candidate(0);
            candidate->cost = key == "です" ? 100 : 500;
            candidate->wcost = candidate->cost;
            return true;
          }));
  composer_->InsertCharacterKeyAndPreedit("raw", "でs");
  ASSERT_TRUE(converter.Convert(*composer_));
  ASSERT_TRUE(converter.TryAsciiResidualCorrection(*composer_));
  EXPECT_TRUE(converter.CurrentConversionHasReadingCorrection());
  const auto& top = GetSegments(converter).conversion_segment(0).candidate(0);
  EXPECT_EQ(top.value, "です");
  EXPECT_EQ(top.prefix, "→ ");
  EXPECT_EQ(top.description, "<もしかして>");
  EXPECT_NE(top.attributes & converter::Attribute::NO_LEARNING, 0);

  std::string finish_key;
  EXPECT_CALL(*mock_converter, FinishConversion(_, _))
      .WillOnce(::testing::Invoke(
          [&finish_key](const ConversionRequest& request, Segments*) {
            finish_key = std::string(request.key());
          }));
  converter.Commit(*composer_, Context::default_instance());
  EXPECT_EQ(finish_key, "です");
}

TEST_F(EngineConverterTest, TypingCorrectionAttributeBlocksNormalZenz) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);
  Segments segments;
  AddSegmentWithSingleCandidate(&segments, "ほにゃにいく", "本屋に行く");
  segments.mutable_conversion_segment(0)->mutable_candidate(0)->attributes |=
      converter::Attribute::TYPING_CORRECTION;
  EXPECT_CALL(*mock_converter, StartConversion(_, _))
      .WillOnce(DoAll(SetArgPointee<1>(segments), Return(true)));

  composer_->InsertCharacterKeyAndPreedit("honyaniiku", "ほにゃにいく");
  ASSERT_TRUE(converter.Convert(*composer_));
  EXPECT_TRUE(converter.CurrentConversionHasReadingCorrection());
}

TEST_F(EngineConverterTest, FullwidthAlphabetResidualStillRunsRepair) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);

  EXPECT_CALL(*mock_converter, StartConversion(_, _))
      .Times(6)
      .WillRepeatedly(::testing::Invoke(
          [](const ConversionRequest& request, Segments* segments) {
            segments->Clear();
            const std::string key(request.key());
            AddSegmentWithSingleCandidate(
                segments, key, key == "でs" ? "でｓ" : key);
            auto* candidate =
                segments->mutable_conversion_segment(0)->mutable_candidate(0);
            candidate->cost = key == "です" ? 100 : 500;
            candidate->wcost = candidate->cost;
            return true;
          }));

  composer_->InsertCharacterKeyAndPreedit("raw", "でs");
  ASSERT_TRUE(converter.Convert(*composer_));
  EXPECT_FALSE(converter.CurrentConversionHasReadingCorrection());
  ASSERT_TRUE(converter.TryAsciiResidualCorrection(*composer_));
  EXPECT_EQ(GetSegments(converter).conversion_segment(0).candidate(0).value,
            "です");
}

TEST_F(EngineConverterTest, OverlongAsciiResidualFailsClosedWithoutProbes) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);

  EXPECT_CALL(*mock_converter, StartConversion(_, _))
      .Times(1)
      .WillOnce(::testing::Invoke(
          [](const ConversionRequest& request, Segments* segments) {
            segments->Clear();
            AddSegmentWithSingleCandidate(segments, request.key(), request.key());
            return true;
          }));

  std::string input;
  for (int i = 0; i < 128; ++i) {
    input.append("あ");
  }
  input.push_back('s');
  composer_->InsertCharacterKeyAndPreedit("raw", input);
  ASSERT_TRUE(converter.Convert(*composer_));
  EXPECT_FALSE(converter.TryAsciiResidualCorrection(*composer_));
  EXPECT_FALSE(converter.CurrentConversionHasReadingCorrection());
}

TEST_F(EngineConverterTest, AsciiResidualResizeUsesCorrectedKeyWithoutSpellingCorrection) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);

  EXPECT_CALL(*mock_converter, StartConversion(_, _))
      .Times(6)
      .WillRepeatedly(::testing::Invoke(
          [](const ConversionRequest& request, Segments* segments) {
            segments->Clear();
            const std::string key(request.key());
            AddSegmentWithSingleCandidate(segments, key, key);
            auto* candidate =
                segments->mutable_conversion_segment(0)->mutable_candidate(0);
            candidate->cost = key == "です" ? 100 : 500;
            candidate->wcost = candidate->cost;
            return true;
          }));

  composer_->InsertCharacterKeyAndPreedit("raw", "でs");
  ASSERT_TRUE(converter.Convert(*composer_));
  ASSERT_TRUE(converter.TryAsciiResidualCorrection(*composer_));

  EXPECT_CALL(*mock_converter, ResizeSegment(_, _, _, _))
      .WillOnce(::testing::Invoke(
          [](Segments*, const ConversionRequest& request, size_t, int) {
            EXPECT_EQ(request.key(), "です");
            EXPECT_FALSE(request.config().use_spelling_correction());
            return true;
          }));
  converter.SegmentWidthExpand(*composer_);
}

TEST_F(EngineConverterTest, MozcReadingCorrectionSkipsAsciiRepair) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);
  Segments segments;
  AddSegmentWithSingleCandidate(&segments, "でs", "です");
  segments.mutable_conversion_segment(0)->mutable_candidate(0)->attributes |=
      converter::Attribute::SPELLING_CORRECTION;
  EXPECT_CALL(*mock_converter, StartConversion(_, _))
      .Times(1)
      .WillOnce(DoAll(SetArgPointee<1>(segments), Return(true)));

  composer_->InsertCharacterKeyAndPreedit("raw", "でs");
  ASSERT_TRUE(converter.Convert(*composer_));
  EXPECT_FALSE(converter.TryAsciiResidualCorrection(*composer_));
  EXPECT_TRUE(converter.CurrentConversionHasReadingCorrection());
  EXPECT_EQ(GetSegments(converter).conversion_segment(0).candidate(0).value,
            "です");
}

TEST_F(EngineConverterTest, KeyCorrectorCandidateIsReadingCorrection) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);

  Segments segments;
  Segment* segment = segments.add_segment();
  segment->set_key("ほにゃにいく");
  converter::Candidate* candidate = segment->add_candidate();
  // KeyCorrectedNodeListBuilder deliberately restores the original key.
  // The provenance marker, not a key mismatch, identifies this path.
  candidate->key = "ほにゃにいく";
  candidate->content_key = candidate->key;
  candidate->value = "本屋に行く";
  candidate->content_value = candidate->value;
  candidate->attributes |= converter::Attribute::KEY_EXPANDED_IN_DICTIONARY;
  EXPECT_CALL(*mock_converter, StartConversion(_, _))
      .WillOnce(DoAll(SetArgPointee<1>(segments), Return(true)));

  composer_->InsertCharacterPreedit("ほにゃにいく");
  ASSERT_TRUE(converter.Convert(*composer_));
  EXPECT_TRUE(converter.CurrentConversionHasReadingCorrection());
}

TEST_F(EngineConverterTest, MozcReadingCorrectionIsExposedToSession) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);
  Segments segments;
  AddSegmentWithSingleCandidate(&segments, "ほにゃ", "本屋");
  segments.mutable_conversion_segment(0)->mutable_candidate(0)->attributes |=
      converter::Attribute::SPELLING_CORRECTION;
  EXPECT_CALL(*mock_converter, StartConversion(_, _))
      .WillOnce(DoAll(SetArgPointee<1>(segments), Return(true)));

  composer_->InsertCharacterKeyAndPreedit("honya", "ほにゃ");
  ASSERT_TRUE(converter.Convert(*composer_));
  EXPECT_TRUE(converter.CurrentConversionHasReadingCorrection());
}

TEST_F(EngineConverterTest, CommitContext) {
  auto mock_converter = std::make_shared<MockConverter>();
  EngineConverter converter(mock_converter, request_, config_);

  EXPECT_CALL(*mock_converter, CommitContext(_)).WillOnce(Return());
  converter.CommitContext(*composer_, Context::default_instance());
}

}  // namespace engine
}  // namespace mozc
