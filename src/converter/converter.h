// Copyright 2010-2021, Google Inc.
// All rights reserved.
#ifndef MOZC_CONVERTER_CONVERTER_H_
#define MOZC_CONVERTER_CONVERTER_H_
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "absl/log/check.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "converter/candidate.h"
#include "converter/converter_interface.h"
#include "converter/history_reconstructor.h"
#include "converter/immutable_converter_interface.h"
#include "converter/reverse_converter.h"
#include "converter/segments.h"
#include "dictionary/dictionary_interface.h"
#include "dictionary/pos_matcher.h"
#include "engine/modules.h"
#include "prediction/predictor_interface.h"
#include "prediction/result.h"
#include "request/conversion_request.h"
#include "rewriter/rewriter_interface.h"
namespace mozc { namespace converter {
class Converter final : public ConverterInterface {
 public:
  using ImmutableConverterFactory=std::function<std::unique_ptr<const ImmutableConverterInterface>(const engine::Modules&)>;
  using PredictorFactory=std::function<std::unique_ptr<prediction::PredictorInterface>(const engine::Modules&,const ConverterInterface&,const ImmutableConverterInterface&)>;
  using RewriterFactory=std::function<std::unique_ptr<RewriterInterface>(const engine::Modules&)>;
  Converter(std::unique_ptr<engine::Modules>,const ImmutableConverterFactory&,const PredictorFactory&,const RewriterFactory&);
  Converter()=delete;
  [[nodiscard]] bool StartConversion(const ConversionRequest&,Segments*) const override;
  [[nodiscard]] bool StartReverseConversion(Segments*,absl::string_view) const override;
  [[nodiscard]] bool StartPrediction(const ConversionRequest&,Segments*) const override;
  [[nodiscard]] bool StartPredictionWithPreviousSuggestion(const ConversionRequest&,const Segment&,Segments*) const override;
  void PrependCandidates(const ConversionRequest&,const Segment&,Segments*) const override;
  void FinishConversion(const ConversionRequest&,Segments*) const override;
  [[nodiscard]] bool LearnExternalConversionResult(const ConversionRequest&,absl::string_view,absl::string_view) const override;
  [[nodiscard]] bool LearnExternalConversionSegments(const ConversionRequest&,absl::Span<const ExternalConversionSegment>) const override;
  void CancelConversion(Segments*) const override;
  void ResetConversion(Segments*) const override;
  void RevertConversion(Segments*) const override;
  [[nodiscard]] bool DeleteCandidateFromHistory(const Segments&,size_t,int) const override;
  [[nodiscard]] bool ReconstructHistory(Segments*,absl::string_view) const override;
  [[nodiscard]] bool CommitSegmentValue(Segments*,size_t,int) const override;
  [[nodiscard]] bool CommitPartialSuggestionSegmentValue(Segments*,size_t,int,absl::string_view,absl::string_view) const override;
  [[nodiscard]] bool FocusSegmentValue(Segments*,size_t,int) const override;
  [[nodiscard]] bool CommitSegments(Segments*,absl::Span<const size_t>) const override;
  [[nodiscard]] bool ResizeSegment(Segments*,const ConversionRequest&,size_t,int) const override;
  [[nodiscard]] bool ResizeSegments(Segments*,const ConversionRequest&,size_t,absl::Span<const uint8_t>) const override;
  void CommitContext(const ConversionRequest&) const override;
  void ApplyConversion(Segments*,const ConversionRequest&) const;
  bool Reload(); bool Sync(); bool Wait();
  bool AddUserHistory(absl::string_view,absl::string_view);
  prediction::PredictorInterface& predictor() const { DCHECK(predictor_); return *predictor_; }
  RewriterInterface& rewriter() const { DCHECK(rewriter_); return *rewriter_; }
  const ImmutableConverterInterface& immutable_converter() const { DCHECK(immutable_converter_); return *immutable_converter_; }
  engine::Modules& modules() const { DCHECK(modules_); return *modules_; }
  static std::vector<prediction::Result> MakeLearningResults(const Segments&);
  static prediction::Result MakeHistoryResult(const Segments&);
  void LookupUserDictionaryPrefixEntries(absl::string_view,std::vector<UserDictionaryLookupResult>*) const override;
  bool HasUserSegmentHistoryPreference(
      absl::string_view key, absl::string_view value) const override {
    return rewriter_->HasUserSegmentHistoryPreference(
        std::string_view(key.data(), key.size()),
        std::string_view(value.data(), value.size()));
  }
 private:
  friend class ConverterTestPeer;
  void CompletePosIds(Candidate*) const;
  bool CommitSegmentValueInternal(Segments*,size_t,int,Segment::SegmentType) const;
  static void MaybeSetConsumedKeySizeToCandidate(size_t,Candidate*);
  static void MaybeSetConsumedKeySizeToSegment(size_t,Segment*);
  void RewriteAndSuppressCandidates(const ConversionRequest&,Segments*) const;
  void TrimCandidates(const ConversionRequest&,Segments*) const;
  bool GetLastConnectivePart(absl::string_view,std::string*,std::string*,uint16_t*) const;
  std::optional<std::string> GetReading(absl::string_view,bool multi_segment=false) const;
  void PopulateReadingOfCommittedCandidateIfMissing(Segments*) const;
  bool PredictForRequestWithSegments(const ConversionRequest&,Segments*) const;
  void ApplyPostProcessing(const ConversionRequest&,Segments*) const;
  std::unique_ptr<engine::Modules> modules_;
  std::unique_ptr<const ImmutableConverterInterface> immutable_converter_;
  std::unique_ptr<prediction::PredictorInterface> predictor_;
  std::unique_ptr<RewriterInterface> rewriter_;
  const dictionary::PosMatcher& pos_matcher_;
  const dictionary::UserDictionaryInterface& user_dictionary_;
  const HistoryReconstructor history_reconstructor_;
  const ReverseConverter reverse_converter_;
  const uint16_t general_noun_id_=std::numeric_limits<uint16_t>::max();
};
} }  // namespace mozc::converter
#endif
