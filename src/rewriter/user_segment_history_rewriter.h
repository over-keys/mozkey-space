// Copyright 2010-2021, Google Inc.
// All rights reserved.
#ifndef MOZC_REWRITER_USER_SEGMENT_HISTORY_REWRITER_H_
#define MOZC_REWRITER_USER_SEGMENT_HISTORY_REWRITER_H_
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <string>
#include <vector>
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "converter/candidate.h"
#include "converter/segments.h"
#include "dictionary/pos_group.h"
#include "dictionary/pos_matcher.h"
#include "request/conversion_request.h"
#include "rewriter/rewriter_interface.h"
#include "storage/lru_cache.h"
#include "storage/lru_storage.h"
namespace mozc {
class UserSegmentHistoryRewriter : public RewriterInterface {
 public:
  UserSegmentHistoryRewriter(const dictionary::PosMatcher& pos_matcher,
                             const dictionary::PosGroup& pos_group);
  bool Rewrite(const ConversionRequest&, Segments*) const override;
  void Finish(const ConversionRequest&, const Segments&) override;
  bool Sync() override;
  bool Reload() override;
  void Clear() override;
  void Revert(const Segments&) override;
  bool ClearHistoryEntry(const Segments&, size_t, int) override;

  bool HasUserSegmentHistoryPreference(
      std::string_view key, std::string_view value) const override {
    if (storage_ == nullptr || key.empty() || value.empty()) return false;
    std::string feature_key("C\t");
    feature_key.append(key.data(), key.size());
    feature_key.push_back('\t');
    feature_key.append(value.data(), value.size());
    uint32_t access_time = 0;
    return storage_->Lookup(feature_key, &access_time) != nullptr;
  }
 private:
  friend class UserSegmentHistoryRewriterTestPeer;
  struct Score {
    constexpr void Update(Score other) {
      score=std::max(score,other.score);
      last_access_time=std::max(last_access_time,other.last_access_time);
    }
    friend constexpr bool operator>(Score a, Score b) {
      return a.score==b.score ? a.last_access_time>b.last_access_time : a.score>b.score;
    }
    uint32_t score,last_access_time;
  };
  struct ScoreCandidate : public Score {
    ScoreCandidate(Score s,const converter::Candidate* c):Score(s),candidate(c){}
    const converter::Candidate* candidate;
  };
  struct RevertEntry { std::string key,value; size_t value_begin=0,value_end=0; };
  struct PendingRevert { std::vector<RevertEntry> entries; std::string committed_value; };
  static Segments MakeLearningSegmentsFromInnerSegments(
      const ConversionRequest&, const Segments&);
  bool IsAvailable(const ConversionRequest&,const Segments&) const;
  Score GetScore(const ConversionRequest&,const Segments&,size_t,int) const;
  bool Replaceable(const ConversionRequest&,const converter::Candidate&,
                   const converter::Candidate&) const;
  void RememberFirstCandidate(const ConversionRequest&,const Segments&,size_t,
                              size_t,size_t,std::vector<RevertEntry>&);
  void RememberNumberPreference(const Segment&,std::vector<RevertEntry>&);
  bool RewriteNumber(Segment*) const;
  bool ShouldRewrite(const Segment&,size_t*) const;
  void InsertTriggerKey(const Segment&);
  bool IsPunctuation(const Segment&,const converter::Candidate&) const;
  bool SortCandidates(absl::Span<const ScoreCandidate>,Segment*) const;
  Score Fetch(absl::string_view,uint32_t) const;
  void Insert(absl::string_view,absl::string_view,size_t,size_t,bool,
              std::vector<RevertEntry>&);
  void MaybeInsertRevertEntry(absl::string_view,absl::string_view,size_t,size_t,
                              std::vector<RevertEntry>&);
  void MaybeApplyPendingRevert(const ConversionRequest&) const;
  bool DeleteEntry(absl::string_view);
  std::unique_ptr<storage::LruStorage> storage_;
  const dictionary::PosMatcher* pos_matcher_;
  const dictionary::PosGroup* pos_group_;
  storage::LruCache<uint64_t,PendingRevert> revert_cache_;
  static std::optional<PendingRevert> pending_revert_;
};
}  // namespace mozc
#endif
