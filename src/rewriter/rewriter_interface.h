// Copyright 2010-2021, Google Inc.
// All rights reserved.
#ifndef MOZC_REWRITER_REWRITER_INTERFACE_H_
#define MOZC_REWRITER_REWRITER_INTERFACE_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include "converter/segments.h"
#include "request/conversion_request.h"

namespace mozc {
class RewriterInterface {
 public:
  virtual ~RewriterInterface() = default;
  enum CapabilityType { NOT_AVAILABLE=0, CONVERSION=1, PREDICTION=2,
                        SUGGESTION=4, ALL=(1|2|4) };
  virtual int capability(const ConversionRequest&) const { return CONVERSION; }
  struct ResizeSegmentsRequest {
    size_t segment_index = 0;
    using SegmentSizes = std::array<uint8_t, 8>;
    SegmentSizes segment_sizes = {0,0,0,0,0,0,0,0};
  };
  virtual std::optional<ResizeSegmentsRequest> CheckResizeSegmentsRequest(
      const ConversionRequest&, const Segments&) const { return std::nullopt; }
  virtual bool Rewrite(const ConversionRequest&, Segments*) const = 0;
  virtual bool Focus(Segments*, size_t, int) const { return true; }
  virtual void Finish(const ConversionRequest&, const Segments&) {}
  virtual void Revert(const Segments&) {}
  virtual bool ClearHistoryEntry(const Segments&, size_t, int) { return false; }
  virtual bool Sync() { return true; }
  virtual bool Reload() { return true; }
  virtual void Clear() {}

  // Exact read-only query used to protect genuine Mozc user history from a
  // generalized Zenz Local Preference.  Only the user segment history
  // rewriter returns true; composites forward the query to their children.
  virtual bool HasUserSegmentHistoryPreference(
      std::string_view key, std::string_view value) const { return false; }

  enum LegacyRewriterType {
    kDisableUserSegmentHistory=1, kDisableUserBoundaryHistory=2,
    kDisableCollocation=4,
  };
  static bool DisableLaegacyRewriterInMixedConversion(
      const ConversionRequest& request, int mode) {
    return request.request().mixed_conversion() &&
           (request.request().decoder_experiment_params()
                .disable_legacy_rewriter_mode() & mode);
  }
 protected:
  RewriterInterface() = default;
};
}  // namespace mozc
#endif
