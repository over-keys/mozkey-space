// Copyright 2010-2021, Google Inc.
// All rights reserved.
#ifndef MOZC_CONVERTER_CONVERTER_MOCK_H_
#define MOZC_CONVERTER_CONVERTER_MOCK_H_

#include <cstddef>
#include <cstdint>
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "converter/converter_interface.h"
#include "converter/segments.h"
#include "request/conversion_request.h"
#include "testing/gmock.h"

namespace mozc {
class StrictMockConverter : public ConverterInterface {
 public:
  StrictMockConverter() = default;
  ~StrictMockConverter() override = default;
  MOCK_METHOD(bool, StartConversion,
              (const ConversionRequest&, Segments*), (const, override));
  MOCK_METHOD(bool, StartReverseConversion,
              (Segments*, absl::string_view), (const, override));
  MOCK_METHOD(bool, StartPrediction,
              (const ConversionRequest&, Segments*), (const, override));
  MOCK_METHOD(bool, StartPredictionWithPreviousSuggestion,
              (const ConversionRequest&, const Segment&, Segments*),
              (const, override));
  MOCK_METHOD(void, PrependCandidates,
              (const ConversionRequest&, const Segment&, Segments*),
              (const, override));
  MOCK_METHOD(void, FinishConversion,
              (const ConversionRequest&, Segments*), (const, override));
  MOCK_METHOD(void, CancelConversion, (Segments*), (const, override));
  MOCK_METHOD(void, ResetConversion, (Segments*), (const, override));
  MOCK_METHOD(void, RevertConversion, (Segments*), (const, override));
  MOCK_METHOD(bool, DeleteCandidateFromHistory,
              (const Segments&, size_t, int), (const, override));
  MOCK_METHOD(bool, ReconstructHistory,
              (Segments*, absl::string_view), (const, override));
  MOCK_METHOD(bool, CommitSegmentValue,
              (Segments*, size_t, int), (const, override));
  MOCK_METHOD(bool, CommitPartialSuggestionSegmentValue,
              (Segments*, size_t, int, absl::string_view, absl::string_view),
              (const, override));
  MOCK_METHOD(bool, FocusSegmentValue,
              (Segments*, size_t, int), (const, override));
  MOCK_METHOD(bool, CommitSegments,
              (Segments*, absl::Span<const size_t>), (const, override));
  MOCK_METHOD(bool, ResizeSegment,
              (Segments*, const ConversionRequest&, size_t, int),
              (const, override));
  MOCK_METHOD(bool, ResizeSegments,
              (Segments*, const ConversionRequest&, size_t,
               absl::Span<const uint8_t>), (const, override));
  MOCK_METHOD(void, CommitContext, (const ConversionRequest&),
              (const, override));
  MOCK_METHOD(bool, HasUserSegmentHistoryPreference,
              (absl::string_view, absl::string_view), (const, override));
};
typedef ::testing::NiceMock<StrictMockConverter> MockConverter;
}  // namespace mozc
#endif  // MOZC_CONVERTER_CONVERTER_MOCK_H_
