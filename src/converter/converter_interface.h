// Copyright 2010-2021, Google Inc.
// All rights reserved.
#ifndef MOZC_CONVERTER_CONVERTER_INTERFACE_H_
#define MOZC_CONVERTER_CONVERTER_INTERFACE_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "converter/segments.h"
#include "request/conversion_request.h"

namespace mozc {
namespace composer { class Composer; }

struct UserDictionaryLookupResult { std::string key; std::string value; };
struct ExternalConversionSegment {
  std::string key;
  std::string value;
  bool is_reranked = false;
};

class ConverterInterface {
 public:
  ConverterInterface(const ConverterInterface&) = delete;
  ConverterInterface& operator=(const ConverterInterface&) = delete;
  virtual ~ConverterInterface() = default;

  [[nodiscard]] virtual bool StartConversion(const ConversionRequest& request,
                                             Segments* segments) const = 0;
  [[nodiscard]] virtual bool StartReverseConversion(
      Segments* segments, absl::string_view key) const = 0;
  [[nodiscard]] virtual bool StartPrediction(const ConversionRequest& request,
                                             Segments* segments) const = 0;
  [[nodiscard]] virtual bool StartPredictionWithPreviousSuggestion(
      const ConversionRequest& request, const Segment& previous_segment,
      Segments* segments) const = 0;
  virtual void PrependCandidates(const ConversionRequest& request,
                                 const Segment& segment,
                                 Segments* segments) const = 0;
  virtual void FinishConversion(const ConversionRequest& request,
                                Segments* segments) const = 0;

  [[nodiscard]] virtual bool LearnExternalConversionResult(
      const ConversionRequest& request, absl::string_view key,
      absl::string_view value) const { return false; }
  [[nodiscard]] virtual bool LearnExternalConversionSegments(
      const ConversionRequest& request,
      absl::Span<const ExternalConversionSegment> segments) const {
    return false;
  }
  virtual void LookupUserDictionaryPrefixEntries(
      absl::string_view key,
      std::vector<UserDictionaryLookupResult>* results) const {
    if (results != nullptr) results->clear();
  }

  // Read-only query for an exact context-independent preference persisted by
  // UserSegmentHistoryRewriter.  The default keeps lightweight converter
  // implementations source-compatible; the desktop Converter overrides it.
  virtual bool HasUserSegmentHistoryPreference(
      absl::string_view key, absl::string_view value) const {
    return false;
  }

  virtual void CancelConversion(Segments* segments) const = 0;
  virtual void ResetConversion(Segments* segments) const = 0;
  virtual void RevertConversion(Segments* segments) const = 0;
  [[nodiscard]] virtual bool DeleteCandidateFromHistory(
      const Segments& segments, size_t segment_index,
      int candidate_index) const = 0;
  [[nodiscard]] virtual bool ReconstructHistory(
      Segments* segments, absl::string_view preceding_text) const = 0;
  [[nodiscard]] virtual bool CommitSegmentValue(
      Segments* segments, size_t segment_index, int candidate_index) const = 0;
  [[nodiscard]] virtual bool CommitPartialSuggestionSegmentValue(
      Segments* segments, size_t segment_index, int candidate_index,
      absl::string_view current_segment_key,
      absl::string_view new_segment_key) const = 0;
  [[nodiscard]] virtual bool FocusSegmentValue(
      Segments* segments, size_t segment_index, int candidate_index) const = 0;
  [[nodiscard]] virtual bool CommitSegments(
      Segments* segments,
      absl::Span<const size_t> candidate_index) const = 0;
  [[nodiscard]] virtual bool ResizeSegment(
      Segments* segments, const ConversionRequest& request,
      size_t segment_index, int offset_length) const = 0;
  [[nodiscard]] virtual bool ResizeSegments(
      Segments* segments, const ConversionRequest& request,
      size_t start_segment_index,
      absl::Span<const uint8_t> new_size_array) const = 0;
  virtual void CommitContext(const ConversionRequest& request) const = 0;

 protected:
  ConverterInterface() = default;
};

}  // namespace mozc
#endif  // MOZC_CONVERTER_CONVERTER_INTERFACE_H_
