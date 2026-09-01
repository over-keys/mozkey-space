// Copyright 2010-2021, Google Inc.
// All rights reserved.
#ifndef MOZC_REWRITER_MERGER_REWRITER_H_
#define MOZC_REWRITER_MERGER_REWRITER_H_
#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>
#include "absl/log/check.h"
#include "converter/segments.h"
#include "protocol/commands.pb.h"
#include "protocol/config.pb.h"
#include "request/conversion_request.h"
#include "rewriter/rewriter_interface.h"
namespace mozc {
class MergerRewriter : public RewriterInterface {
 public:
  MergerRewriter() = default;
  ~MergerRewriter() override = default;
  MergerRewriter(const MergerRewriter&) = delete;
  MergerRewriter& operator=(const MergerRewriter&) = delete;
  void AddRewriter(std::unique_ptr<RewriterInterface> r) {
    DCHECK(r); rewriters_.push_back(std::move(r));
  }
  std::optional<ResizeSegmentsRequest> CheckResizeSegmentsRequest(
      const ConversionRequest& request, const Segments& segments) const override {
    if (segments.resized()) return std::nullopt;
    for (const auto& r : rewriters_) {
      auto x = r->CheckResizeSegmentsRequest(request, segments);
      if (x.has_value()) return x;
    }
    return std::nullopt;
  }
  bool Rewrite(const ConversionRequest& request, Segments* segments) const override {
    if (segments == nullptr) return false;
    const CapabilityType type = [&] {
      switch (request.request_type()) {
        case ConversionRequest::CONVERSION: return CONVERSION;
        case ConversionRequest::PREDICTION:
        case ConversionRequest::PARTIAL_PREDICTION: return PREDICTION;
        case ConversionRequest::SUGGESTION:
        case ConversionRequest::PARTIAL_SUGGESTION: return SUGGESTION;
        default: return NOT_AVAILABLE;
      }
    }();
    bool updated=false;
    for (const auto& r : rewriters_) if (r->capability(request)&type)
      updated |= r->Rewrite(request, segments);
    if (request.request_type()==ConversionRequest::SUGGESTION &&
        segments->conversion_segments_size()==1 &&
        !request.request().mixed_conversion()) {
      const size_t max_suggestions=request.config().suggestions_size();
      Segment* s=segments->mutable_conversion_segment(0);
      const size_t n=s->candidates_size();
      if (n>max_suggestions) s->erase_candidates(max_suggestions,n-max_suggestions);
    }
    return updated;
  }
  bool Focus(Segments* segments,size_t i,int c) const override {
    bool result=false; for (const auto& r:rewriters_) result|=r->Focus(segments,i,c); return result;
  }
  void Finish(const ConversionRequest& req,const Segments& seg) override {
    for (const auto& r:rewriters_) r->Finish(req,seg);
  }
  void Revert(const Segments& seg) override { for(const auto& r:rewriters_) r->Revert(seg); }
  bool ClearHistoryEntry(const Segments& seg,size_t i,int c) override {
    bool result=false; for(const auto& r:rewriters_) result|=r->ClearHistoryEntry(seg,i,c); return result;
  }
  bool Sync() override { bool result=false; for(const auto& r:rewriters_) result|=r->Sync(); return result; }
  bool Reload() override { bool result=false; for(const auto& r:rewriters_) result|=r->Reload(); return result; }
  void Clear() override { for(const auto& r:rewriters_) r->Clear(); }
  bool HasUserSegmentHistoryPreference(
      std::string_view key, std::string_view value) const override {
    for (const auto& r : rewriters_) {
      if (r->HasUserSegmentHistoryPreference(key, value)) return true;
    }
    return false;
  }
 private:
  std::vector<std::unique_ptr<RewriterInterface>> rewriters_;
};
}  // namespace mozc
#endif
