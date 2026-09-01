#include "session/zenz_feedback_store.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "testing/gunit.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__) && TARGET_OS_OSX
#include <unistd.h>
#endif

namespace mozc {
namespace session {
namespace {

#if defined(_WIN32)

std::wstring JoinPath(const std::wstring& lhs, const std::wstring& rhs) {
  if (lhs.empty()) {
    return rhs;
  }
  if (lhs.back() == L'\\') {
    return lhs + rhs;
  }
  return lhs + L"\\" + rhs;
}

bool EnsureDirectory(const std::wstring& path) {
  if (::CreateDirectoryW(path.c_str(), nullptr)) {
    return true;
  }
  return ::GetLastError() == ERROR_ALREADY_EXISTS;
}

class ScopedUserProfileForZenzFeedbackStoreTest {
 public:
  ScopedUserProfileForZenzFeedbackStoreTest() {
    wchar_t old_profile[32767] = {};
    const DWORD old_len =
        ::GetEnvironmentVariableW(L"USERPROFILE", old_profile, 32767);
    if (old_len > 0 && old_len < 32767) {
      has_old_profile_ = true;
      old_profile_.assign(old_profile, old_len);
    }

    wchar_t temp_path[MAX_PATH] = {};
    const DWORD temp_len = ::GetTempPathW(MAX_PATH, temp_path);
    if (temp_len == 0 || temp_len >= MAX_PATH) {
      return;
    }

    profile_dir_ =
        std::wstring(temp_path, temp_len) +
        L"mozc_zenz_feedback_store_test_" +
        std::to_wstring(::GetCurrentProcessId()) + L"_" +
        std::to_wstring(::GetTickCount64());

    const std::wstring app_data_dir = JoinPath(profile_dir_, L"AppData");
    const std::wstring local_low_dir = JoinPath(app_data_dir, L"LocalLow");

    if (!EnsureDirectory(profile_dir_) ||
        !EnsureDirectory(app_data_dir) ||
        !EnsureDirectory(local_low_dir)) {
      return;
    }

    ok_ = ::SetEnvironmentVariableW(L"USERPROFILE", profile_dir_.c_str());
  }

  ~ScopedUserProfileForZenzFeedbackStoreTest() {
    if (has_old_profile_) {
      ::SetEnvironmentVariableW(L"USERPROFILE", old_profile_.c_str());
    } else {
      ::SetEnvironmentVariableW(L"USERPROFILE", nullptr);
    }

    const std::wstring app_data_dir = JoinPath(profile_dir_, L"AppData");
    const std::wstring local_low_dir = JoinPath(app_data_dir, L"LocalLow");
    const std::wstring mozc_dir = JoinPath(local_low_dir, L"Mozc");
    const std::wstring feedback_path = JoinPath(mozc_dir, L"zenz_feedback.tsv");

    ::DeleteFileW(feedback_path.c_str());
    ::RemoveDirectoryW(mozc_dir.c_str());
    ::RemoveDirectoryW(local_low_dir.c_str());
    ::RemoveDirectoryW(app_data_dir.c_str());
    ::RemoveDirectoryW(profile_dir_.c_str());
  }

  bool ok() const { return ok_; }

  std::wstring feedback_path() const {
    const std::wstring app_data_dir = JoinPath(profile_dir_, L"AppData");
    const std::wstring local_low_dir = JoinPath(app_data_dir, L"LocalLow");
    const std::wstring mozc_dir = JoinPath(local_low_dir, L"Mozc");
    return JoinPath(mozc_dir, L"zenz_feedback.tsv");
  }

  std::wstring temp_file_path(const std::wstring& name) const {
    return JoinPath(profile_dir_, name);
  }

 private:
  bool ok_ = false;
  bool has_old_profile_ = false;
  std::wstring old_profile_;
  std::wstring profile_dir_;
};

TEST(ZenzFeedbackStoreTest, GetAcceptedCandidatesAllowsSingleAcceptedAndSorts) {
  ScopedUserProfileForZenzFeedbackStoreTest profile;
  ASSERT_TRUE(profile.ok());

  ZenzFeedbackStore store;

  // One accepted record is enough and should be returned.
  store.RecordAccepted("k", "empty", "弱い");

  // accepted=2, rejected=0, margin=2. Returned.
  store.RecordAccepted("k", "empty", "強い");
  store.RecordAccepted("k", "empty", "強い");

  // accepted=3, rejected=1, margin=2. Returned and sorted before "強い"
  // because accepted_count is larger.
  store.RecordAccepted("k", "empty", "さらに強い");
  store.RecordAccepted("k", "empty", "さらに強い");
  store.RecordAccepted("k", "empty", "さらに強い");
  store.RecordRejected("k", "empty", "さらに強い", "explicit_reject");

  // Ordinary rejected feedback is a ranking signal, not a hard exclusion.
  // accepted=2 and two medium rejections still produce a positive score, so the
  // candidate remains available but is sorted below stronger candidates.
  store.RecordAccepted("k", "empty", "拒否優勢");
  store.RecordAccepted("k", "empty", "拒否優勢");
  store.RecordRejected("k", "empty", "拒否優勢", "explicit_reject");
  store.RecordRejected("k", "empty", "拒否優勢", "explicit_reject");

  // Different key must be ignored.
  store.RecordAccepted("other", "empty", "別キー");
  store.RecordAccepted("other", "empty", "別キー");
  store.RecordAccepted("other", "empty", "別キー");

  // Sensitive-like context must never be used for normal candidate promotion,
  // even if it has many accepted records.
  store.RecordAccepted("k", "sensitive_like", "機密文脈");
  store.RecordAccepted("k", "sensitive_like", "機密文脈");
  store.RecordAccepted("k", "sensitive_like", "機密文脈");

  const std::vector<ZenzFeedbackCandidate> candidates =
      store.GetAcceptedCandidates("k", "empty");

  ASSERT_EQ(candidates.size(), 4);

  EXPECT_EQ(candidates[0].value, "さらに強い");
  EXPECT_EQ(candidates[0].accepted_count, 3);
  EXPECT_EQ(candidates[0].rejected_count, 1);
  EXPECT_EQ(candidates[0].reason, "feedback_preferred");

  EXPECT_EQ(candidates[1].value, "強い");
  EXPECT_EQ(candidates[1].accepted_count, 2);
  EXPECT_EQ(candidates[1].rejected_count, 0);
  EXPECT_EQ(candidates[1].reason, "feedback_preferred");

  EXPECT_EQ(candidates[2].value, "拒否優勢");
  EXPECT_EQ(candidates[2].accepted_count, 2);
  EXPECT_EQ(candidates[2].rejected_count, 2);
  EXPECT_EQ(candidates[2].reason, "feedback_preferred");
  EXPECT_GT(candidates[2].total_score, 0);

  EXPECT_EQ(candidates[3].value, "弱い");
  EXPECT_EQ(candidates[3].accepted_count, 1);
  EXPECT_EQ(candidates[3].rejected_count, 0);
  EXPECT_EQ(candidates[3].reason, "feedback_preferred");
}

TEST(ZenzFeedbackStoreTest, DecideTreatsOrdinaryRejectedAsSoftSignal) {
  ScopedUserProfileForZenzFeedbackStoreTest profile;
  ASSERT_TRUE(profile.ok());

  ZenzFeedbackStore store;

  store.RecordRejected("k", "empty", "v", "space_revert_zenz_to_mozc");

  ZenzFeedbackDecision decision = store.Decide("k", "empty", "v");
  EXPECT_EQ(decision.action, ZenzFeedbackAction::kNeutral);
  EXPECT_EQ(decision.reason, "feedback_downgraded");
  EXPECT_EQ(decision.accepted_count, 0);
  EXPECT_EQ(decision.rejected_count, 1);
  EXPECT_LT(decision.total_score, 0);

  store.RecordAccepted("k", "empty", "v");
  decision = store.Decide("k", "empty", "v");
  EXPECT_EQ(decision.action, ZenzFeedbackAction::kPrefer);
  EXPECT_EQ(decision.reason, "feedback_preferred");
  EXPECT_EQ(decision.accepted_count, 1);
  EXPECT_EQ(decision.rejected_count, 1);
  EXPECT_GT(decision.total_score, 0);
}

TEST(ZenzFeedbackStoreTest,
     DecideAutoBlockPolicySuppressesAfterThresholdDynamically) {
  ScopedUserProfileForZenzFeedbackStoreTest profile;
  ASSERT_TRUE(profile.ok());

  ZenzFeedbackStore store;
  store.RecordAccepted("k", "empty", "v");
  store.RecordAccepted("k", "empty", "v");
  store.RecordAccepted("k", "empty", "v");
  store.RecordRejected("k", "empty", "v", "space_revert_zenz_to_mozc");
  store.RecordRejected("k", "empty", "v", "space_revert_zenz_to_mozc");

  ZenzFeedbackAutoBlockPolicy policy;
  policy.enabled = true;
  policy.reject_threshold = 2;

  ZenzFeedbackDecision decision = store.Decide("k", "empty", "v", policy);
  EXPECT_EQ(decision.action, ZenzFeedbackAction::kReject);
  EXPECT_EQ(decision.reason, "feedback_auto_blocked");
  EXPECT_TRUE(decision.auto_blocked);
  EXPECT_FALSE(decision.hard_rejected);
  EXPECT_EQ(decision.accepted_count, 3);
  EXPECT_EQ(decision.rejected_count, 2);
  EXPECT_EQ(decision.auto_block_reject_count, 2);

  policy.reject_threshold = 3;
  decision = store.Decide("k", "empty", "v", policy);
  EXPECT_EQ(decision.action, ZenzFeedbackAction::kPrefer);
  EXPECT_EQ(decision.reason, "feedback_preferred");
  EXPECT_FALSE(decision.auto_blocked);

  policy.enabled = false;
  policy.reject_threshold = 1;
  decision = store.Decide("k", "empty", "v", policy);
  EXPECT_EQ(decision.action, ZenzFeedbackAction::kPrefer);
  EXPECT_EQ(decision.reason, "feedback_preferred");
  EXPECT_FALSE(decision.auto_blocked);
}

TEST(ZenzFeedbackStoreTest,
     DecideStopsPreferenceWhenOrdinaryRejectCountDominates) {
  ScopedUserProfileForZenzFeedbackStoreTest profile;
  ASSERT_TRUE(profile.ok());

  ZenzFeedbackStore store;
  store.RecordAccepted("k", "empty", "v");
  store.RecordRejected("k", "empty", "v", "space_revert_zenz_to_mozc");
  store.RecordRejected("k", "empty", "v", "space_revert_zenz_to_mozc");

  ZenzFeedbackDecision decision = store.Decide("k", "empty", "v");
  EXPECT_EQ(decision.action, ZenzFeedbackAction::kNeutral);
  EXPECT_EQ(decision.reason, "feedback_reject_count_dominant");
  EXPECT_FALSE(decision.auto_blocked);
  EXPECT_FALSE(decision.hard_rejected);
  EXPECT_EQ(decision.accepted_count, 1);
  EXPECT_EQ(decision.rejected_count, 2);
  EXPECT_EQ(decision.auto_block_reject_count, 2);
  EXPECT_GT(decision.total_score, 0);

  EXPECT_TRUE(store.GetRankedCandidates("k", "empty").empty());

  const std::vector<ZenzFeedbackEntry> entries = store.ListEntries();
  ASSERT_EQ(entries.size(), 1);
  EXPECT_EQ(entries[0].reason, "feedback_reject_count_dominant");
}

TEST(ZenzFeedbackStoreTest,
     DecideUsesCompatibleContextAggregateForRejectSensitivity) {
  ScopedUserProfileForZenzFeedbackStoreTest profile;
  ASSERT_TRUE(profile.ok());

  ZenzFeedbackStore store;
  store.RecordAccepted("k", "japanese_only", "v");
  store.RecordRejected("k", "empty", "v", "space_revert_zenz_to_mozc");

  // A native-context accept followed by one unavailable-context reject must not
  // become weaker merely because the reject lands in the empty coarse bucket.
  ZenzFeedbackDecision decision = store.Decide("k", "empty", "v");
  EXPECT_EQ(decision.action, ZenzFeedbackAction::kPrefer);
  EXPECT_EQ(decision.reason, "feedback_preferred");
  EXPECT_EQ(decision.accepted_count, 1);
  EXPECT_EQ(decision.rejected_count, 1);
  EXPECT_EQ(decision.auto_block_reject_count, 1);
  ASSERT_EQ(store.GetRankedCandidates("k", "empty").size(), 1);

  // A second compatible reject now dominates regardless of which normal
  // context class is available on the request.
  store.RecordRejected("k", "japanese_with_punctuation", "v",
                       "space_revert_zenz_to_mozc");
  decision = store.Decide("k", "empty", "v");
  EXPECT_EQ(decision.action, ZenzFeedbackAction::kNeutral);
  EXPECT_EQ(decision.reason, "feedback_reject_count_dominant");
  EXPECT_EQ(decision.auto_block_reject_count, 2);
  EXPECT_TRUE(store.GetRankedCandidates("k", "empty").empty());

  decision = store.Decide("k", "japanese_only", "v");
  EXPECT_EQ(decision.action, ZenzFeedbackAction::kNeutral);
  EXPECT_EQ(decision.reason, "feedback_reject_count_dominant");
  EXPECT_EQ(decision.auto_block_reject_count, 2);
}


TEST(ZenzFeedbackStoreTest,
     AsciiOnlyUnavailableContextUsesSameNormalFeedbackBucket) {
  ScopedUserProfileForZenzFeedbackStoreTest profile;
  ASSERT_TRUE(profile.ok());

  ZenzFeedbackStore store;
  store.RecordAccepted("k", "ascii_or_digit", "v");
  EXPECT_EQ(store.Decide("k", "empty", "v").action,
            ZenzFeedbackAction::kPrefer);
  EXPECT_EQ(store.Decide("k", "japanese_only", "v").accepted_count, 1);

  store.RecordRejected("k", "empty", "v", "space_revert_zenz_to_mozc");
  const ZenzFeedbackDecision decision =
      store.Decide("k", "ascii_or_digit", "v");
  EXPECT_EQ(decision.action, ZenzFeedbackAction::kPrefer);
  EXPECT_EQ(decision.accepted_count, 1);
  EXPECT_EQ(decision.rejected_count, 1);
}

TEST(ZenzFeedbackStoreTest, ListEntriesUsesRuntimeEffectiveState) {
  ScopedUserProfileForZenzFeedbackStoreTest profile;
  ASSERT_TRUE(profile.ok());

  ZenzFeedbackStore store;
  store.RecordAccepted("k", "japanese_only", "v");
  store.RecordRejected("k", "empty", "v", "space_revert_zenz_to_mozc");
  store.RecordRejected("k", "symbol_or_other", "v",
                       "space_revert_zenz_to_mozc");

  EXPECT_EQ(store.Decide("k", "empty", "v").reason,
            "feedback_reject_count_dominant");
  const auto entries = store.ListEntries();
  ASSERT_EQ(entries.size(), 3);
  for (const auto& entry : entries) {
    EXPECT_EQ(entry.effective_accepted_count, 1);
    EXPECT_EQ(entry.effective_rejected_count, 2);
    EXPECT_EQ(entry.reason, "feedback_reject_count_dominant");
  }
}

TEST(ZenzFeedbackStoreTest, AutoBlockPolicyUsesCompatibleContextAggregate) {
  ScopedUserProfileForZenzFeedbackStoreTest profile;
  ASSERT_TRUE(profile.ok());

  ZenzFeedbackStore store;
  store.RecordAccepted("k", "japanese_only", "v");
  store.RecordRejected("k", "japanese_only", "v",
                       "space_revert_zenz_to_mozc");
  store.RecordRejected("k", "japanese_only", "v",
                       "space_revert_zenz_to_mozc");

  ZenzFeedbackAutoBlockPolicy policy;
  policy.enabled = true;
  policy.reject_threshold = 2;

  ZenzFeedbackDecision decision = store.Decide("k", "empty", "v", policy);
  EXPECT_EQ(decision.action, ZenzFeedbackAction::kReject);
  EXPECT_EQ(decision.reason, "feedback_auto_blocked");
  EXPECT_TRUE(decision.auto_blocked);
  EXPECT_EQ(decision.auto_block_reject_count, 2);
  EXPECT_TRUE(store.GetRankedCandidates("k", "empty", policy).empty());

  decision = store.Decide("k", "japanese_only", "v", policy);
  EXPECT_EQ(decision.action, ZenzFeedbackAction::kReject);
  EXPECT_EQ(decision.reason, "feedback_auto_blocked");
  EXPECT_TRUE(decision.auto_blocked);
  EXPECT_EQ(decision.auto_block_reject_count, 2);
}

TEST(ZenzFeedbackStoreTest,
     CompatibleAggregationKeepsSensitiveContextIsolated) {
  ScopedUserProfileForZenzFeedbackStoreTest profile;
  ASSERT_TRUE(profile.ok());

  ZenzFeedbackStore store;
  store.RecordAccepted("k", "japanese_only", "v");
  store.RecordRejected("k", "sensitive_like", "v",
                       "space_revert_zenz_to_mozc");
  store.RecordRejected("k", "sensitive_like", "v",
                       "space_revert_zenz_to_mozc");

  const ZenzFeedbackDecision normal = store.Decide("k", "empty", "v");
  EXPECT_EQ(normal.action, ZenzFeedbackAction::kPrefer);
  EXPECT_EQ(normal.accepted_count, 1);
  EXPECT_EQ(normal.rejected_count, 0);

  const ZenzFeedbackDecision sensitive =
      store.Decide("k", "sensitive_like", "v");
  EXPECT_EQ(sensitive.action, ZenzFeedbackAction::kNeutral);
  EXPECT_EQ(sensitive.accepted_count, 0);
  EXPECT_EQ(sensitive.rejected_count, 2);
}

TEST(ZenzFeedbackStoreTest, DecideAllowsFutureHardRejectReason) {
  ScopedUserProfileForZenzFeedbackStoreTest profile;
  ASSERT_TRUE(profile.ok());

  ZenzFeedbackStore store;
  store.RecordRejected("k", "empty", "v", "hard_reject");

  ZenzFeedbackDecision decision = store.Decide("k", "empty", "v");
  EXPECT_EQ(decision.action, ZenzFeedbackAction::kReject);
  EXPECT_EQ(decision.reason, "feedback_hard_rejected");
  EXPECT_TRUE(decision.hard_rejected);
}

TEST(ZenzFeedbackStoreTest, DecideHardRejectOverridesAcceptedFeedback) {
  ScopedUserProfileForZenzFeedbackStoreTest profile;
  ASSERT_TRUE(profile.ok());

  ZenzFeedbackStore store;
  store.RecordAccepted("k", "empty", "v");
  store.RecordAccepted("k", "empty", "v");
  store.RecordAccepted("k", "empty", "v");
  store.RecordRejected("k", "empty", "v", "hard_reject");

  const ZenzFeedbackDecision decision = store.Decide("k", "empty", "v");
  EXPECT_EQ(decision.action, ZenzFeedbackAction::kReject);
  EXPECT_EQ(decision.reason, "feedback_hard_rejected");
  EXPECT_TRUE(decision.hard_rejected);
  EXPECT_GT(decision.total_score, 0);
}

TEST(ZenzFeedbackStoreTest, GetRankedCandidatesExcludesHardRejectedValue) {
  ScopedUserProfileForZenzFeedbackStoreTest profile;
  ASSERT_TRUE(profile.ok());

  ZenzFeedbackStore store;
  store.RecordAccepted("k", "empty", "kept");
  store.RecordAccepted("k", "empty", "blocked");
  store.RecordAccepted("k", "empty", "blocked");
  store.RecordAccepted("k", "empty", "blocked");
  store.RecordRejected("k", "empty", "blocked", "hard_reject");

  const std::vector<ZenzFeedbackCandidate> candidates =
      store.GetRankedCandidates("k", "empty");

  ASSERT_EQ(candidates.size(), 1);
  EXPECT_EQ(candidates[0].value, "kept");
  EXPECT_FALSE(candidates[0].hard_rejected);
}

TEST(ZenzFeedbackStoreTest,
     GetRankedCandidatesExcludesAutoBlockedValueOnlyWhenPolicyEnabled) {
  ScopedUserProfileForZenzFeedbackStoreTest profile;
  ASSERT_TRUE(profile.ok());

  ZenzFeedbackStore store;
  store.RecordAccepted("k", "empty", "kept");
  store.RecordAccepted("k", "empty", "blocked");
  store.RecordAccepted("k", "empty", "blocked");
  store.RecordAccepted("k", "empty", "blocked");
  store.RecordRejected("k", "empty", "blocked",
                       "space_revert_zenz_to_mozc");
  store.RecordRejected("k", "empty", "blocked",
                       "space_revert_zenz_to_mozc");

  ZenzFeedbackAutoBlockPolicy policy;
  policy.enabled = true;
  policy.reject_threshold = 2;

  std::vector<ZenzFeedbackCandidate> candidates =
      store.GetRankedCandidates("k", "empty", policy);

  ASSERT_EQ(candidates.size(), 1);
  EXPECT_EQ(candidates[0].value, "kept");

  policy.enabled = false;
  candidates = store.GetRankedCandidates("k", "empty", policy);

  ASSERT_EQ(candidates.size(), 2);
  EXPECT_EQ(candidates[0].value, "blocked");
  EXPECT_EQ(candidates[1].value, "kept");
}

TEST(ZenzFeedbackStoreTest,
     GetRankedCandidatesExcludesRejectDominantValueWithoutAutoBlock) {
  ScopedUserProfileForZenzFeedbackStoreTest profile;
  ASSERT_TRUE(profile.ok());

  ZenzFeedbackStore store;
  store.RecordAccepted("k", "empty", "kept");
  store.RecordAccepted("k", "empty", "dominant");
  store.RecordRejected("k", "empty", "dominant",
                       "space_revert_zenz_to_mozc");
  store.RecordRejected("k", "empty", "dominant",
                       "space_revert_zenz_to_mozc");

  ZenzFeedbackAutoBlockPolicy policy;
  policy.enabled = false;
  policy.reject_threshold = 1;

  const std::vector<ZenzFeedbackCandidate> candidates =
      store.GetRankedCandidates("k", "empty", policy);

  ASSERT_EQ(candidates.size(), 1);
  EXPECT_EQ(candidates[0].value, "kept");
}

TEST(ZenzFeedbackStoreTest,
     GetAcceptedCandidatesSharesSafeContextClassesForPromotion) {
  ScopedUserProfileForZenzFeedbackStoreTest profile;
  ASSERT_TRUE(profile.ok());

  ZenzFeedbackStore store;

  // Feedback learned with a safe Japanese context must still help ordinary
  // conversion where the current lookup context is empty.  Otherwise a useful
  // Zenz correction disappears simply because the later conversion has no
  // preceding text.
  store.RecordAccepted("かれはてんてきです", "japanese_only", "彼は天敵です");

  const std::vector<ZenzFeedbackCandidate> empty_candidates =
      store.GetAcceptedCandidates("かれはてんてきです", "empty");

  ASSERT_EQ(empty_candidates.size(), 1);
  EXPECT_EQ(empty_candidates[0].value, "彼は天敵です");
  EXPECT_EQ(empty_candidates[0].accepted_count, 1);
  EXPECT_EQ(empty_candidates[0].rejected_count, 0);
  EXPECT_EQ(empty_candidates[0].reason, "feedback_preferred");

  const std::vector<ZenzFeedbackCandidate> symbol_candidates =
      store.GetAcceptedCandidates("かれはてんてきです", "symbol_or_other");

  ASSERT_EQ(symbol_candidates.size(), 1);
  EXPECT_EQ(symbol_candidates[0].value, "彼は天敵です");
  EXPECT_EQ(symbol_candidates[0].accepted_count, 1);
  EXPECT_EQ(symbol_candidates[0].rejected_count, 0);
  EXPECT_EQ(symbol_candidates[0].reason, "feedback_preferred");
}

TEST(ZenzFeedbackStoreTest,
     GetAcceptedCandidatesAggregatesSafeContextClassesByValue) {
  ScopedUserProfileForZenzFeedbackStoreTest profile;
  ASSERT_TRUE(profile.ok());

  ZenzFeedbackStore store;

  store.RecordAccepted("かれはてんてきです", "empty", "彼は天敵です");
  store.RecordAccepted("かれはてんてきです", "japanese_only", "彼は天敵です");
  store.RecordAccepted("かれはてんてきです", "japanese_with_punctuation",
                       "彼は天敵です");
  store.RecordRejected("かれはてんてきです", "empty", "彼は天敵です",
                       "explicit_reject");

  const std::vector<ZenzFeedbackCandidate> candidates =
      store.GetAcceptedCandidates("かれはてんてきです", "empty");

  ASSERT_EQ(candidates.size(), 1);
  EXPECT_EQ(candidates[0].value, "彼は天敵です");
  EXPECT_EQ(candidates[0].accepted_count, 3);
  EXPECT_EQ(candidates[0].rejected_count, 1);
  EXPECT_EQ(candidates[0].reason, "feedback_preferred");
}

TEST(ZenzFeedbackStoreTest,
     GetAcceptedCandidatesDoesNotShareSensitiveLikeContext) {
  ScopedUserProfileForZenzFeedbackStoreTest profile;
  ASSERT_TRUE(profile.ok());

  ZenzFeedbackStore store;

  store.RecordAccepted("secret_key", "sensitive_like", "秘密候補");
  store.RecordAccepted("secret_key", "sensitive_like", "秘密候補");
  store.RecordAccepted("secret_key", "sensitive_like", "秘密候補");

  EXPECT_TRUE(
      store.GetAcceptedCandidates("secret_key", "empty").empty());
  EXPECT_TRUE(
      store.GetAcceptedCandidates("secret_key", "japanese_only").empty());
  EXPECT_TRUE(
      store.GetAcceptedCandidates("secret_key", "symbol_or_other").empty());
}

TEST(ZenzFeedbackStoreTest,
     GetAcceptedCandidatesAllowsSensitiveLikeExactMatch) {
  ScopedUserProfileForZenzFeedbackStoreTest profile;
  ASSERT_TRUE(profile.ok());

  ZenzFeedbackStore store;
  store.RecordAccepted(
      "はげんざいこうですが",
      "sensitive_like",
      "は現在こうですが");

  const std::vector<ZenzFeedbackCandidate> candidates =
      store.GetAcceptedCandidates(
          "はげんざいこうですが",
          "sensitive_like");

  ASSERT_EQ(candidates.size(), 1);
  EXPECT_EQ(candidates[0].value, "は現在こうですが");
  EXPECT_EQ(candidates[0].accepted_count, 1);
  EXPECT_EQ(candidates[0].rejected_count, 0);

  const std::vector<ZenzFeedbackCandidate> normal_candidates =
      store.GetAcceptedCandidates(
          "はげんざいこうですが",
          "empty");

  EXPECT_TRUE(normal_candidates.empty());
}

TEST(ZenzFeedbackStoreTest, GetAcceptedCandidatesAcceptsUtf8Bom) {
  ScopedUserProfileForZenzFeedbackStoreTest profile;
  ASSERT_TRUE(profile.ok());

  ZenzFeedbackStore store;

  // Create %USERPROFILE%\AppData\LocalLow\Mozc first.  This test intentionally
  // overwrites the TSV directly to simulate a file saved by tools/editors that
  // write UTF-8 with BOM.
  store.RecordAccepted("__mkdir__", "empty", "__mkdir__");

  {
    std::ofstream file(profile.feedback_path(),
                       std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(file);
    file << "\xEF\xBB\xBF"
         << "v2\taccepted\tかれはてんてきです\tjapanese_only\t彼は天敵です\t\n";
  }

  const std::vector<ZenzFeedbackCandidate> candidates =
      store.GetAcceptedCandidates("かれはてんてきです", "empty");

  ASSERT_EQ(candidates.size(), 1);
  EXPECT_EQ(candidates[0].value, "彼は天敵です");
  EXPECT_EQ(candidates[0].accepted_count, 1);
  EXPECT_EQ(candidates[0].rejected_count, 0);
  EXPECT_EQ(candidates[0].reason, "feedback_preferred");
}

TEST(ZenzFeedbackStoreTest, GetAcceptedCandidatesNormalizesEmptyContextClass) {
  ScopedUserProfileForZenzFeedbackStoreTest profile;
  ASSERT_TRUE(profile.ok());

  ZenzFeedbackStore store;

  store.RecordAccepted("empty_context_key", "", "空文脈候補");
  store.RecordAccepted("empty_context_key", "", "空文脈候補");

  const std::vector<ZenzFeedbackCandidate> candidates_from_empty =
      store.GetAcceptedCandidates("empty_context_key", "");
  const std::vector<ZenzFeedbackCandidate> candidates_from_empty_label =
      store.GetAcceptedCandidates("empty_context_key", "empty");

  ASSERT_EQ(candidates_from_empty.size(), 1);
  EXPECT_EQ(candidates_from_empty[0].value, "空文脈候補");
  EXPECT_EQ(candidates_from_empty[0].accepted_count, 2);
  EXPECT_EQ(candidates_from_empty[0].rejected_count, 0);

  ASSERT_EQ(candidates_from_empty_label.size(), 1);
  EXPECT_EQ(candidates_from_empty_label[0].value, "空文脈候補");
  EXPECT_EQ(candidates_from_empty_label[0].accepted_count, 2);
  EXPECT_EQ(candidates_from_empty_label[0].rejected_count, 0);
}

TEST(ZenzFeedbackStoreTest, ListEntriesAggregatesExactEntries) {
  ScopedUserProfileForZenzFeedbackStoreTest profile;
  ASSERT_TRUE(profile.ok());

  ZenzFeedbackStore store;

  store.RecordAccepted("b", "empty", "B");
  store.RecordRejected("b", "empty", "B", "explicit_reject");
  store.RecordAccepted("a", "japanese_only", "A");

  const std::vector<ZenzFeedbackEntry> entries = store.ListEntries();

  ASSERT_EQ(entries.size(), 2);

  EXPECT_EQ(entries[0].key, "a");
  EXPECT_EQ(entries[0].context_class, "japanese_only");
  EXPECT_EQ(entries[0].value, "A");
  EXPECT_EQ(entries[0].accepted_count, 1);
  EXPECT_EQ(entries[0].rejected_count, 0);
  EXPECT_EQ(entries[0].reason, "feedback_preferred");

  EXPECT_EQ(entries[1].key, "b");
  EXPECT_EQ(entries[1].context_class, "empty");
  EXPECT_EQ(entries[1].value, "B");
  EXPECT_EQ(entries[1].accepted_count, 1);
  EXPECT_EQ(entries[1].rejected_count, 1);
  EXPECT_EQ(entries[1].reason, "feedback_preferred");
  EXPECT_GT(store.Decide("b", "empty", "B").total_score, 0);
}

TEST(ZenzFeedbackStoreTest, LocalPreferenceIsAtomicAndIsolated) {
  ScopedUserProfileForZenzFeedbackStoreTest profile;
  ASSERT_TRUE(profile.ok());

  ZenzFeedbackStore store;
  store.RecordLocalPreference("りせき", "japanese_only", "離席", "離籍");

  const std::vector<ZenzLocalPreference> local =
      store.GetLocalPreferences("ちょっとりせきします", "japanese_only");
  ASSERT_EQ(local.size(), 1);
  EXPECT_EQ(local[0].key, "りせき");
  EXPECT_EQ(local[0].preferred_value, "離席");
  EXPECT_EQ(local[0].disfavored_value, "離籍");
  EXPECT_EQ(local[0].observation_count, 1);

  // Atomic v3 local evidence must never enter ordinary full-sequence lookup.
  EXPECT_TRUE(store.GetRankedCandidates("りせき", "empty").empty());
  EXPECT_EQ(store.Decide("りせき", "empty", "離席").action,
            ZenzFeedbackAction::kNeutral);

  // Repeated reading occurrences are ambiguous and intentionally skipped.
  EXPECT_TRUE(store.GetLocalPreferences(
      "りせきとりせき", "japanese_only").empty());

  std::ifstream file(profile.feedback_path(), std::ios::binary);
  ASSERT_TRUE(file);
  std::string line;
  ASSERT_TRUE(static_cast<bool>(std::getline(file, line)));
  EXPECT_EQ(line,
            "v3\tlocal_preference\tりせき\tjapanese_only\t離席\t離籍\t"
            "rejected_zenz_final_commit");
  EXPECT_TRUE(file.peek() == std::ifstream::traits_type::eof());
}

TEST(ZenzFeedbackStoreTest,
     LocalPreferenceMinimumObservationCountIsDirectional) {
  ScopedUserProfileForZenzFeedbackStoreTest profile;
  ASSERT_TRUE(profile.ok());

  ZenzFeedbackStore store;
  store.RecordLocalPreference("りせき", "japanese_only", "離席", "離籍");
  EXPECT_TRUE(store.GetLocalPreferences(
      "ちょっとりせきします", "japanese_only", 12, 2).empty());

  // An opposite observation must not count as confirmation of this direction.
  store.RecordLocalPreference("りせき", "japanese_only", "離籍", "離席");
  EXPECT_TRUE(store.GetLocalPreferences(
      "ちょっとりせきします", "japanese_only", 12, 2).empty());

  // Only the second same-direction observation matures 離席 > 離籍.
  store.RecordLocalPreference("りせき", "japanese_only", "離席", "離籍");
  const auto mature = store.GetLocalPreferences(
      "ちょっとりせきします", "japanese_only", 12, 2);
  ASSERT_EQ(mature.size(), 1);
  EXPECT_EQ(mature[0].preferred_value, "離席");
  EXPECT_EQ(mature[0].disfavored_value, "離籍");
  EXPECT_EQ(mature[0].observation_count, 2);

  // Raising the setting makes the same persisted evidence dormant again.
  EXPECT_TRUE(store.GetLocalPreferences(
      "ちょっとりせきします", "japanese_only", 12, 3).empty());
}

TEST(ZenzFeedbackStoreTest,
     LocalPreferenceRankingUsesCompatibleTotalNotExactContextCount) {
  ScopedUserProfileForZenzFeedbackStoreTest profile;
  ASSERT_TRUE(profile.ok());

  ZenzFeedbackStore store;
  // Stronger compatible evidence is deliberately recorded in the bucket used
  // when native context is unavailable.
  store.RecordLocalPreference("りせき", "empty", "離席", "離籍");
  store.RecordLocalPreference("りせき", "empty", "離席", "離籍");
  store.RecordLocalPreference("りせき", "empty", "離席", "離籍");

  // The opposite direction has more exact japanese_only evidence, but less
  // total compatible evidence.
  store.RecordLocalPreference("りせき", "japanese_only", "離籍", "離席");
  store.RecordLocalPreference("りせき", "japanese_only", "離籍", "離席");

  auto preferences = store.GetLocalPreferences(
      "ちょっとりせきします", "empty", 1, 2);
  ASSERT_EQ(preferences.size(), 1);
  EXPECT_EQ(preferences[0].preferred_value, "離席");
  EXPECT_EQ(preferences[0].disfavored_value, "離籍");
  EXPECT_EQ(preferences[0].observation_count, 3);

  preferences = store.GetLocalPreferences(
      "ちょっとりせきします", "japanese_only", 1, 2);
  ASSERT_EQ(preferences.size(), 1);
  EXPECT_EQ(preferences[0].preferred_value, "離席");
  EXPECT_EQ(preferences[0].disfavored_value, "離籍");
  EXPECT_EQ(preferences[0].observation_count, 3);
}

TEST(ZenzFeedbackStoreTest, RecordLocalPreferencesBatchesAtomicRows) {
  ScopedUserProfileForZenzFeedbackStoreTest profile;
  ASSERT_TRUE(profile.ok());

  ZenzFeedbackStore store;
  std::vector<ZenzLocalPreference> preferences(2);
  preferences[0].key = "りせき";
  preferences[0].context_class = "japanese_only";
  preferences[0].preferred_value = "離席";
  preferences[0].disfavored_value = "離籍";
  preferences[1].key = "こうせい";
  preferences[1].context_class = "japanese_only";
  preferences[1].preferred_value = "構成";
  preferences[1].disfavored_value = "校正";

  store.RecordLocalPreferences(preferences);

  const auto entries = store.ListLocalPreferenceEntries();
  ASSERT_EQ(entries.size(), 2);

  std::ifstream file(profile.feedback_path(), std::ios::binary);
  ASSERT_TRUE(file);
  std::vector<std::string> lines;
  for (std::string line; std::getline(file, line);) {
    lines.push_back(line);
  }
  ASSERT_EQ(lines.size(), 2);
  EXPECT_EQ(lines[0],
            "v3\tlocal_preference\tりせき\tjapanese_only\t離席\t離籍\t"
            "rejected_zenz_final_commit");
  EXPECT_EQ(lines[1],
            "v3\tlocal_preference\tこうせい\tjapanese_only\t構成\t校正\t"
            "rejected_zenz_final_commit");
}

TEST(ZenzFeedbackStoreTest, OppositeLocalPreferencesCoexistInStore) {
  ScopedUserProfileForZenzFeedbackStoreTest profile;
  ASSERT_TRUE(profile.ok());

  ZenzFeedbackStore store;
  store.RecordLocalPreference("りせき", "japanese_only", "離席", "離籍");
  store.RecordLocalPreference("りせき", "japanese_only", "離籍", "離席");

  // Store deliberately does not inspect current Mozc. Session resolves which
  // direction is applicable after seeing the current Mozc surface.
  const auto preferences =
      store.GetLocalPreferences("ちょっとりせきします", "japanese_only");
  ASSERT_EQ(preferences.size(), 2);
  bool saw_seat = false;
  bool saw_register = false;
  for (const ZenzLocalPreference& preference : preferences) {
    saw_seat = saw_seat ||
        (preference.preferred_value == "離席" &&
         preference.disfavored_value == "離籍");
    saw_register = saw_register ||
        (preference.preferred_value == "離籍" &&
         preference.disfavored_value == "離席");
  }
  EXPECT_TRUE(saw_seat);
  EXPECT_TRUE(saw_register);
}

TEST(ZenzFeedbackStoreTest,
     LocalPreferenceManagementViewAndDeleteAreDirectional) {
  ScopedUserProfileForZenzFeedbackStoreTest profile;
  ASSERT_TRUE(profile.ok());

  ZenzFeedbackStore store;
  store.RecordLocalPreference("りせき", "empty", "離席", "離籍");
  store.RecordLocalPreference("りせき", "japanese_only", "離席", "離籍");
  store.RecordLocalPreference("りせき", "japanese_only", "離籍", "離席");

  const std::vector<ZenzLocalPreferenceEntry> entries =
      store.ListLocalPreferenceEntries();
  ASSERT_EQ(entries.size(), 3);

  bool saw_japanese_preferred = false;
  for (const ZenzLocalPreferenceEntry& entry : entries) {
    if (entry.context_class == "japanese_only" &&
        entry.preferred_value == "離席") {
      saw_japanese_preferred = true;
      EXPECT_EQ(entry.observation_count, 1);
      // empty and japanese_only are promotion-compatible, so runtime sees
      // two same-direction observations for this source context.
      EXPECT_EQ(entry.effective_observation_count, 2);
      EXPECT_EQ(entry.opposite_effective_observation_count, 1);
    }
  }
  EXPECT_TRUE(saw_japanese_preferred);

  ASSERT_TRUE(store.DeleteLocalPreference(
      "りせき", "japanese_only", "離席", "離籍"));
  const auto after = store.ListLocalPreferenceEntries();
  ASSERT_EQ(after.size(), 2);
  EXPECT_EQ(store.GetLocalPreferences("りせき", "japanese_only").size(), 2);

  // Ordinary full-sequence delete remains isolated from v3 local evidence.
  ASSERT_TRUE(store.DeleteEntry("りせき", "empty", "離席"));
  EXPECT_EQ(store.ListLocalPreferenceEntries().size(), 2);
}

TEST(ZenzFeedbackStoreTest, SensitiveLocalPreferenceDoesNotCrossContext) {
  ScopedUserProfileForZenzFeedbackStoreTest profile;
  ASSERT_TRUE(profile.ok());

  ZenzFeedbackStore store;
  store.RecordLocalPreference("りせき", "sensitive_like", "離席", "離籍");
  EXPECT_TRUE(store.GetLocalPreferences("りせき", "japanese_only").empty());
  ASSERT_EQ(store.GetLocalPreferences("りせき", "sensitive_like").size(), 1);
}

TEST(ZenzFeedbackStoreTest,
     FullSequenceFeedbackDoesNotMatchSegmentLocalLookup) {
  ScopedUserProfileForZenzFeedbackStoreTest profile;
  ASSERT_TRUE(profile.ok());

  ZenzFeedbackStore store;

  // ZenzFeedbackStore is scoped to the complete Zenz reading/correction pair.
  // Recording a full-sequence observation must not implicitly create
  // segment-local or lexical-unit feedback.
  store.RecordAccepted("full_sequence_reading", "japanese_only",
                       "full_sequence_correction");
  store.RecordRejected("full_sequence_reading", "japanese_only",
                       "full_sequence_correction",
                       "space_revert_zenz_to_mozc");

  const std::vector<ZenzFeedbackCandidate> full_candidates =
      store.GetRankedCandidates("full_sequence_reading", "empty");
  ASSERT_EQ(full_candidates.size(), 1);
  EXPECT_EQ(full_candidates[0].value, "full_sequence_correction");
  EXPECT_EQ(full_candidates[0].accepted_count, 1);
  EXPECT_EQ(full_candidates[0].rejected_count, 1);
  EXPECT_GT(full_candidates[0].total_score, 0);

  EXPECT_TRUE(store.GetRankedCandidates("segment_reading", "empty").empty());
  EXPECT_EQ(store.Decide("segment_reading", "empty", "segment_correction")
                .action,
            ZenzFeedbackAction::kNeutral);

  const std::vector<ZenzFeedbackEntry> entries = store.ListEntries();
  ASSERT_EQ(entries.size(), 1);
  EXPECT_EQ(entries[0].key, "full_sequence_reading");
  EXPECT_EQ(entries[0].context_class, "japanese_only");
  EXPECT_EQ(entries[0].value, "full_sequence_correction");
  EXPECT_EQ(entries[0].accepted_count, 1);
  EXPECT_EQ(entries[0].rejected_count, 1);
}

TEST(ZenzFeedbackStoreTest, DeleteEntryRemovesOnlyMatchingRawRecords) {
  ScopedUserProfileForZenzFeedbackStoreTest profile;
  ASSERT_TRUE(profile.ok());

  ZenzFeedbackStore store;

  store.RecordAccepted("k", "empty", "削除対象");
  store.RecordAccepted("k", "empty", "削除対象");
  store.RecordRejected("k", "empty", "削除対象", "explicit_reject");
  store.RecordAccepted("k", "empty", "残す");
  store.RecordAccepted("k", "japanese_only", "削除対象");

  EXPECT_TRUE(store.DeleteEntry("k", "empty", "削除対象"));

  const std::vector<ZenzFeedbackEntry> entries = store.ListEntries();

  ASSERT_EQ(entries.size(), 2);

  auto find_entry = [&](const char* key,
                        const char* context_class,
                        const char* value) -> const ZenzFeedbackEntry* {
    for (const ZenzFeedbackEntry& entry : entries) {
      if (entry.key == key &&
          entry.context_class == context_class &&
          entry.value == value) {
        return &entry;
      }
    }
    return nullptr;
  };

  EXPECT_EQ(find_entry("k", "empty", "削除対象"), nullptr);

  const ZenzFeedbackEntry* empty_remaining =
      find_entry("k", "empty", "残す");
  ASSERT_NE(empty_remaining, nullptr);
  EXPECT_EQ(empty_remaining->accepted_count, 1);
  EXPECT_EQ(empty_remaining->rejected_count, 0);
  EXPECT_EQ(empty_remaining->reason, "feedback_preferred");

  const ZenzFeedbackEntry* japanese_only_remaining =
      find_entry("k", "japanese_only", "削除対象");
  ASSERT_NE(japanese_only_remaining, nullptr);
  EXPECT_EQ(japanese_only_remaining->accepted_count, 1);
  EXPECT_EQ(japanese_only_remaining->rejected_count, 0);
  EXPECT_EQ(japanese_only_remaining->reason, "feedback_preferred");
}

TEST(ZenzFeedbackStoreTest, ClearAllRemovesAllEntries) {
  ScopedUserProfileForZenzFeedbackStoreTest profile;
  ASSERT_TRUE(profile.ok());

  ZenzFeedbackStore store;

  store.RecordAccepted("k", "empty", "v");
  ASSERT_FALSE(store.ListEntries().empty());

  EXPECT_TRUE(store.ClearAll());
  EXPECT_TRUE(store.ListEntries().empty());

  std::ifstream file(profile.feedback_path(), std::ios::binary);
  EXPECT_FALSE(file);
}

TEST(ZenzFeedbackStoreTest, ExportAndImportReplacePreservesRecords) {
  ScopedUserProfileForZenzFeedbackStoreTest profile;
  ASSERT_TRUE(profile.ok());

  ZenzFeedbackStore store;

  store.RecordAccepted("k", "empty", "v");
  store.RecordRejected("k", "empty", "v", "explicit_reject");

  const std::wstring export_path = profile.temp_file_path(L"export.tsv");
  ASSERT_TRUE(store.ExportToFile(export_path));

  ASSERT_TRUE(store.ClearAll());
  ASSERT_TRUE(store.ListEntries().empty());

  ASSERT_TRUE(store.ImportFromFile(
      export_path, ZenzFeedbackImportMode::kReplace));

  const std::vector<ZenzFeedbackEntry> entries = store.ListEntries();

  ASSERT_EQ(entries.size(), 1);
  EXPECT_EQ(entries[0].key, "k");
  EXPECT_EQ(entries[0].context_class, "empty");
  EXPECT_EQ(entries[0].value, "v");
  EXPECT_EQ(entries[0].accepted_count, 1);
  EXPECT_EQ(entries[0].rejected_count, 1);
  EXPECT_EQ(entries[0].reason, "feedback_preferred");
  EXPECT_GT(store.Decide("k", "empty", "v").total_score, 0);
}

TEST(ZenzFeedbackStoreTest, ImportAppendKeepsExistingRecords) {
  ScopedUserProfileForZenzFeedbackStoreTest profile;
  ASSERT_TRUE(profile.ok());

  ZenzFeedbackStore store;

  store.RecordAccepted("existing", "empty", "既存");

  const std::wstring import_path = profile.temp_file_path(L"import_append.tsv");
  {
    std::ofstream file(import_path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(file);
    file << "v2\taccepted\timported\tempty\t追加\t\n";
  }

  ASSERT_TRUE(store.ImportFromFile(
      import_path, ZenzFeedbackImportMode::kAppend));

  const std::vector<ZenzFeedbackEntry> entries = store.ListEntries();

  ASSERT_EQ(entries.size(), 2);
  EXPECT_EQ(entries[0].key, "existing");
  EXPECT_EQ(entries[0].value, "既存");
  EXPECT_EQ(entries[1].key, "imported");
  EXPECT_EQ(entries[1].value, "追加");
}

TEST(ZenzFeedbackStoreTest, ImportRejectsInvalidUtf8WithoutChangingExisting) {
  ScopedUserProfileForZenzFeedbackStoreTest profile;
  ASSERT_TRUE(profile.ok());

  ZenzFeedbackStore store;
  store.RecordAccepted("existing", "empty", "既存");

  const std::wstring import_path = profile.temp_file_path(L"import_invalid_utf8.tsv");
  {
    std::ofstream file(import_path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(file);
    file << "v2\taccepted\tbad\tempty\t";
    const char invalid_utf8[] = {static_cast<char>(0xC3), static_cast<char>(0x28)};
    file.write(invalid_utf8, sizeof(invalid_utf8));
    file << "\t\n";
  }

  EXPECT_FALSE(store.ImportFromFile(import_path, ZenzFeedbackImportMode::kAppend));
  const std::vector<ZenzFeedbackEntry> entries = store.ListEntries();
  ASSERT_EQ(entries.size(), 1);
  EXPECT_EQ(entries[0].key, "existing");
}

TEST(ZenzFeedbackStoreTest, ImportRejectsMalformedFileWithoutChangingExisting) {
  ScopedUserProfileForZenzFeedbackStoreTest profile;
  ASSERT_TRUE(profile.ok());

  ZenzFeedbackStore store;

  store.RecordAccepted("existing", "empty", "既存");

  const std::wstring import_path =
      profile.temp_file_path(L"import_malformed.tsv");
  {
    std::ofstream file(import_path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(file);
    file << "v2\tunknown_action\tk\tempty\tv\t\n";
  }

  EXPECT_FALSE(store.ImportFromFile(
      import_path, ZenzFeedbackImportMode::kAppend));

  const std::vector<ZenzFeedbackEntry> entries = store.ListEntries();

  ASSERT_EQ(entries.size(), 1);
  EXPECT_EQ(entries[0].key, "existing");
  EXPECT_EQ(entries[0].context_class, "empty");
  EXPECT_EQ(entries[0].value, "既存");
  EXPECT_EQ(entries[0].accepted_count, 1);
  EXPECT_EQ(entries[0].rejected_count, 0);
}

#elif defined(__APPLE__) && TARGET_OS_OSX

class MacZenzFeedbackStoreTest : public testing::Test {
 protected:
  void SetUp() override {
    if (const char* home = std::getenv("HOME"); home != nullptr) {
      had_old_home_ = true;
      old_home_ = home;
    }
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    root_ = std::filesystem::temp_directory_path() /
            ("mozc_zenz_feedback_store_test_" +
             std::to_string(static_cast<long long>(::getpid())) + "_" +
             std::to_string(static_cast<long long>(nonce)));
    std::error_code ec;
    std::filesystem::create_directories(profile_dir(), ec);
    ASSERT_FALSE(ec);
    ASSERT_EQ(::setenv("HOME", root_.string().c_str(), 1), 0);
  }

  void TearDown() override {
    ZenzFeedbackStore().ClearAll();
    if (had_old_home_) {
      ::setenv("HOME", old_home_.c_str(), 1);
    } else {
      ::unsetenv("HOME");
    }
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  std::filesystem::path profile_dir() const { return root_ / ".mozc"; }
  std::filesystem::path profile_path(const char* filename) const {
    return profile_dir() / filename;
  }
  std::wstring wide_profile_path(const char* filename) const {
    const std::string native = profile_path(filename).string();
    return std::wstring(native.begin(), native.end());
  }

 private:
  bool had_old_home_ = false;
  std::string old_home_;
  std::filesystem::path root_;
};

TEST_F(MacZenzFeedbackStoreTest,
       DecideUsesCompatibleContextAggregateForRejectSensitivity) {
  ZenzFeedbackStore store;
  store.RecordAccepted("k", "japanese_only", "v");
  store.RecordRejected("k", "empty", "v", "space_revert_zenz_to_mozc");

  ZenzFeedbackDecision decision = store.Decide("k", "empty", "v");
  EXPECT_EQ(decision.action, ZenzFeedbackAction::kPrefer);
  EXPECT_EQ(decision.reason, "feedback_preferred");
  EXPECT_EQ(decision.accepted_count, 1);
  EXPECT_EQ(decision.rejected_count, 1);
  EXPECT_EQ(decision.auto_block_reject_count, 1);
  ASSERT_EQ(store.GetRankedCandidates("k", "empty").size(), 1);

  store.RecordRejected("k", "japanese_with_punctuation", "v",
                       "space_revert_zenz_to_mozc");
  decision = store.Decide("k", "empty", "v");
  EXPECT_EQ(decision.action, ZenzFeedbackAction::kNeutral);
  EXPECT_EQ(decision.reason, "feedback_reject_count_dominant");
  EXPECT_EQ(decision.auto_block_reject_count, 2);
  EXPECT_TRUE(store.GetRankedCandidates("k", "empty").empty());

  decision = store.Decide("k", "japanese_only", "v");
  EXPECT_EQ(decision.action, ZenzFeedbackAction::kNeutral);
  EXPECT_EQ(decision.reason, "feedback_reject_count_dominant");
  EXPECT_EQ(decision.auto_block_reject_count, 2);
}


TEST_F(MacZenzFeedbackStoreTest,
       AsciiOnlyUnavailableContextUsesSameNormalFeedbackBucket) {
  ZenzFeedbackStore store;
  store.RecordAccepted("k", "ascii_or_digit", "v");
  EXPECT_EQ(store.Decide("k", "empty", "v").action,
            ZenzFeedbackAction::kPrefer);
  EXPECT_EQ(store.Decide("k", "japanese_only", "v").accepted_count, 1);

  store.RecordRejected("k", "empty", "v", "space_revert_zenz_to_mozc");
  const ZenzFeedbackDecision decision =
      store.Decide("k", "ascii_or_digit", "v");
  EXPECT_EQ(decision.action, ZenzFeedbackAction::kPrefer);
  EXPECT_EQ(decision.accepted_count, 1);
  EXPECT_EQ(decision.rejected_count, 1);
}

TEST_F(MacZenzFeedbackStoreTest, ListEntriesUsesRuntimeEffectiveState) {
  ZenzFeedbackStore store;
  store.RecordAccepted("k", "japanese_only", "v");
  store.RecordRejected("k", "empty", "v", "space_revert_zenz_to_mozc");
  store.RecordRejected("k", "symbol_or_other", "v",
                       "space_revert_zenz_to_mozc");

  EXPECT_EQ(store.Decide("k", "empty", "v").reason,
            "feedback_reject_count_dominant");
  const auto entries = store.ListEntries();
  ASSERT_EQ(entries.size(), 3);
  for (const auto& entry : entries) {
    EXPECT_EQ(entry.effective_accepted_count, 1);
    EXPECT_EQ(entry.effective_rejected_count, 2);
    EXPECT_EQ(entry.reason, "feedback_reject_count_dominant");
  }
}

TEST_F(MacZenzFeedbackStoreTest,
       AutoBlockPolicyUsesCompatibleContextAggregate) {
  ZenzFeedbackStore store;
  store.RecordAccepted("k", "japanese_only", "v");
  store.RecordRejected("k", "japanese_only", "v",
                       "space_revert_zenz_to_mozc");
  store.RecordRejected("k", "japanese_only", "v",
                       "space_revert_zenz_to_mozc");

  ZenzFeedbackAutoBlockPolicy policy;
  policy.enabled = true;
  policy.reject_threshold = 2;

  ZenzFeedbackDecision decision = store.Decide("k", "empty", "v", policy);
  EXPECT_EQ(decision.action, ZenzFeedbackAction::kReject);
  EXPECT_EQ(decision.reason, "feedback_auto_blocked");
  EXPECT_TRUE(decision.auto_blocked);
  EXPECT_EQ(decision.auto_block_reject_count, 2);
  EXPECT_TRUE(store.GetRankedCandidates("k", "empty", policy).empty());

  decision = store.Decide("k", "japanese_only", "v", policy);
  EXPECT_EQ(decision.action, ZenzFeedbackAction::kReject);
  EXPECT_EQ(decision.reason, "feedback_auto_blocked");
  EXPECT_TRUE(decision.auto_blocked);
  EXPECT_EQ(decision.auto_block_reject_count, 2);
}

TEST_F(MacZenzFeedbackStoreTest,
       CompatibleAggregationKeepsSensitiveContextIsolated) {
  ZenzFeedbackStore store;
  store.RecordAccepted("k", "japanese_only", "v");
  store.RecordRejected("k", "sensitive_like", "v",
                       "space_revert_zenz_to_mozc");
  store.RecordRejected("k", "sensitive_like", "v",
                       "space_revert_zenz_to_mozc");

  const ZenzFeedbackDecision normal = store.Decide("k", "empty", "v");
  EXPECT_EQ(normal.action, ZenzFeedbackAction::kPrefer);
  EXPECT_EQ(normal.accepted_count, 1);
  EXPECT_EQ(normal.rejected_count, 0);

  const ZenzFeedbackDecision sensitive =
      store.Decide("k", "sensitive_like", "v");
  EXPECT_EQ(sensitive.action, ZenzFeedbackAction::kNeutral);
  EXPECT_EQ(sensitive.accepted_count, 0);
  EXPECT_EQ(sensitive.rejected_count, 2);
}

TEST_F(MacZenzFeedbackStoreTest, LocalPreferenceIsAtomicAndIsolated) {
  ZenzFeedbackStore store;
  store.RecordLocalPreference("りせき", "japanese_only", "離席", "離籍");

  const std::vector<ZenzLocalPreference> local =
      store.GetLocalPreferences("ちょっとりせきします", "japanese_only");
  ASSERT_EQ(local.size(), 1);
  EXPECT_EQ(local[0].key, "りせき");
  EXPECT_EQ(local[0].preferred_value, "離席");
  EXPECT_EQ(local[0].disfavored_value, "離籍");
  EXPECT_EQ(local[0].observation_count, 1);

  EXPECT_TRUE(store.GetRankedCandidates("りせき", "empty").empty());
  EXPECT_EQ(store.Decide("りせき", "empty", "離席").action,
            ZenzFeedbackAction::kNeutral);
  EXPECT_TRUE(store.GetLocalPreferences(
      "りせきとりせき", "japanese_only").empty());
}

TEST_F(MacZenzFeedbackStoreTest,
       LocalPreferenceMinimumObservationCountIsDirectional) {
  ZenzFeedbackStore store;
  store.RecordLocalPreference("りせき", "japanese_only", "離席", "離籍");
  EXPECT_TRUE(store.GetLocalPreferences(
      "ちょっとりせきします", "japanese_only", 12, 2).empty());

  store.RecordLocalPreference("りせき", "japanese_only", "離籍", "離席");
  EXPECT_TRUE(store.GetLocalPreferences(
      "ちょっとりせきします", "japanese_only", 12, 2).empty());

  store.RecordLocalPreference("りせき", "japanese_only", "離席", "離籍");
  const auto mature = store.GetLocalPreferences(
      "ちょっとりせきします", "japanese_only", 12, 2);
  ASSERT_EQ(mature.size(), 1);
  EXPECT_EQ(mature[0].preferred_value, "離席");
  EXPECT_EQ(mature[0].disfavored_value, "離籍");
  EXPECT_EQ(mature[0].observation_count, 2);
  EXPECT_TRUE(store.GetLocalPreferences(
      "ちょっとりせきします", "japanese_only", 12, 3).empty());
}

TEST_F(MacZenzFeedbackStoreTest,
       LocalPreferenceRankingUsesCompatibleTotalNotExactContextCount) {
  ZenzFeedbackStore store;
  store.RecordLocalPreference("りせき", "empty", "離席", "離籍");
  store.RecordLocalPreference("りせき", "empty", "離席", "離籍");
  store.RecordLocalPreference("りせき", "empty", "離席", "離籍");
  store.RecordLocalPreference("りせき", "japanese_only", "離籍", "離席");
  store.RecordLocalPreference("りせき", "japanese_only", "離籍", "離席");

  auto preferences = store.GetLocalPreferences(
      "ちょっとりせきします", "empty", 1, 2);
  ASSERT_EQ(preferences.size(), 1);
  EXPECT_EQ(preferences[0].preferred_value, "離席");
  EXPECT_EQ(preferences[0].disfavored_value, "離籍");
  EXPECT_EQ(preferences[0].observation_count, 3);

  preferences = store.GetLocalPreferences(
      "ちょっとりせきします", "japanese_only", 1, 2);
  ASSERT_EQ(preferences.size(), 1);
  EXPECT_EQ(preferences[0].preferred_value, "離席");
  EXPECT_EQ(preferences[0].disfavored_value, "離籍");
  EXPECT_EQ(preferences[0].observation_count, 3);
}

TEST_F(MacZenzFeedbackStoreTest, RecordLocalPreferencesBatchesAtomicRows) {
  ZenzFeedbackStore store;
  std::vector<ZenzLocalPreference> preferences(2);
  preferences[0].key = "りせき";
  preferences[0].context_class = "japanese_only";
  preferences[0].preferred_value = "離席";
  preferences[0].disfavored_value = "離籍";
  preferences[1].key = "こうせい";
  preferences[1].context_class = "japanese_only";
  preferences[1].preferred_value = "構成";
  preferences[1].disfavored_value = "校正";

  store.RecordLocalPreferences(preferences);

  const auto entries = store.ListLocalPreferenceEntries();
  ASSERT_EQ(entries.size(), 2);

  std::ifstream file(profile_path("zenz_feedback.tsv"), std::ios::binary);
  ASSERT_TRUE(file);
  std::vector<std::string> lines;
  for (std::string line; std::getline(file, line);) {
    lines.push_back(line);
  }
  ASSERT_EQ(lines.size(), 2);
  EXPECT_EQ(lines[0],
            "v3\tlocal_preference\tりせき\tjapanese_only\t離席\t離籍\t"
            "rejected_zenz_final_commit");
  EXPECT_EQ(lines[1],
            "v3\tlocal_preference\tこうせい\tjapanese_only\t構成\t校正\t"
            "rejected_zenz_final_commit");
}

TEST_F(MacZenzFeedbackStoreTest, OppositeLocalPreferencesCoexistInStore) {
  ZenzFeedbackStore store;
  store.RecordLocalPreference("りせき", "japanese_only", "離席", "離籍");
  store.RecordLocalPreference("りせき", "japanese_only", "離籍", "離席");

  const auto preferences =
      store.GetLocalPreferences("ちょっとりせきします", "japanese_only");
  ASSERT_EQ(preferences.size(), 2);
  bool saw_seat = false;
  bool saw_register = false;
  for (const ZenzLocalPreference& preference : preferences) {
    saw_seat = saw_seat ||
        (preference.preferred_value == "離席" &&
         preference.disfavored_value == "離籍");
    saw_register = saw_register ||
        (preference.preferred_value == "離籍" &&
         preference.disfavored_value == "離席");
  }
  EXPECT_TRUE(saw_seat);
  EXPECT_TRUE(saw_register);
}

TEST_F(MacZenzFeedbackStoreTest,
       LocalPreferenceManagementViewAndDeleteAreDirectional) {
  ZenzFeedbackStore store;
  store.RecordLocalPreference("りせき", "empty", "離席", "離籍");
  store.RecordLocalPreference("りせき", "japanese_only", "離席", "離籍");
  store.RecordLocalPreference("りせき", "japanese_only", "離籍", "離席");

  const std::vector<ZenzLocalPreferenceEntry> entries =
      store.ListLocalPreferenceEntries();
  ASSERT_EQ(entries.size(), 3);

  bool saw_japanese_preferred = false;
  for (const ZenzLocalPreferenceEntry& entry : entries) {
    if (entry.context_class == "japanese_only" &&
        entry.preferred_value == "離席") {
      saw_japanese_preferred = true;
      EXPECT_EQ(entry.observation_count, 1);
      EXPECT_EQ(entry.effective_observation_count, 2);
      EXPECT_EQ(entry.opposite_effective_observation_count, 1);
    }
  }
  EXPECT_TRUE(saw_japanese_preferred);

  ASSERT_TRUE(store.DeleteLocalPreference(
      "りせき", "japanese_only", "離席", "離籍"));
  EXPECT_EQ(store.ListLocalPreferenceEntries().size(), 2);
  EXPECT_EQ(store.GetLocalPreferences("りせき", "japanese_only").size(), 2);
}

TEST_F(MacZenzFeedbackStoreTest, SensitiveLocalPreferenceDoesNotCrossContext) {
  ZenzFeedbackStore store;
  store.RecordLocalPreference("りせき", "sensitive_like", "離席", "離籍");
  EXPECT_TRUE(store.GetLocalPreferences("りせき", "japanese_only").empty());
  ASSERT_EQ(store.GetLocalPreferences("りせき", "sensitive_like").size(), 1);
}


TEST_F(MacZenzFeedbackStoreTest, ExportImportPreservesAtomicV3LocalPreference) {
  ZenzFeedbackStore store;
  store.RecordLocalPreference("りせき", "japanese_only", "離席", "離籍");

  const std::wstring export_path =
      wide_profile_path("local_preference_export.tsv");
  ASSERT_TRUE(store.ExportToFile(export_path));
  ASSERT_TRUE(store.ClearAll());
  ASSERT_TRUE(store.GetLocalPreferences("りせき", "japanese_only").empty());

  ASSERT_TRUE(store.ImportFromFile(
      export_path, ZenzFeedbackImportMode::kReplace));
  const std::vector<ZenzLocalPreference> local =
      store.GetLocalPreferences("りせき", "japanese_only");
  ASSERT_EQ(local.size(), 1);
  EXPECT_EQ(local[0].preferred_value, "離席");
  EXPECT_EQ(local[0].disfavored_value, "離籍");

  // Ordinary delete is intentionally scoped to full-sequence v1/v2 records.
  ASSERT_TRUE(store.DeleteEntry("りせき", "japanese_only", "離席"));
  ASSERT_EQ(store.GetLocalPreferences("りせき", "japanese_only").size(), 1);

  ASSERT_TRUE(store.ClearAll());
  EXPECT_TRUE(store.GetLocalPreferences("りせき", "japanese_only").empty());
}

TEST_F(MacZenzFeedbackStoreTest, ImportsLegacyRev9LocalRevertPair) {
  const std::filesystem::path import_path =
      profile_path("legacy_local_revert.tsv");
  {
    std::ofstream file(import_path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(file);
    file << "v2\taccepted\tりせき\tlocal_revert\t離席\t"
            "local_revert_preferred\n";
    file << "v2\trejected\tりせき\tlocal_revert\t離籍\tlocal_revert\n";
  }

  ZenzFeedbackStore store;
  ASSERT_TRUE(store.ImportFromFile(
      wide_profile_path("legacy_local_revert.tsv"),
      ZenzFeedbackImportMode::kReplace));
  const auto local = store.GetLocalPreferences("りせき", "empty");
  ASSERT_EQ(local.size(), 1);
  EXPECT_EQ(local[0].preferred_value, "離席");
  EXPECT_EQ(local[0].disfavored_value, "離籍");
  EXPECT_TRUE(store.GetRankedCandidates("りせき", "empty").empty());

  const auto managed = store.ListLocalPreferenceEntries();
  ASSERT_EQ(managed.size(), 1);
  EXPECT_EQ(managed[0].context_class, "legacy");
  EXPECT_EQ(managed[0].preferred_value, "離席");
  EXPECT_EQ(managed[0].disfavored_value, "離籍");
  ASSERT_TRUE(store.DeleteLocalPreference("りせき", "legacy", "離席", "離籍"));
  EXPECT_TRUE(store.GetLocalPreferences("りせき", "empty").empty());
}

TEST_F(MacZenzFeedbackStoreTest, PersistsAcceptedFeedbackInUserProfile) {
  ZenzFeedbackStore store;
  store.RecordAccepted("かれはてんてきです", "empty", "彼は天敵です");

  const std::vector<ZenzFeedbackCandidate> candidates =
      store.GetRankedCandidates("かれはてんてきです", "empty");
  ASSERT_EQ(candidates.size(), 1);
  EXPECT_EQ(candidates[0].value, "彼は天敵です");

  ASSERT_TRUE(store.ClearAll());
  EXPECT_TRUE(store.GetRankedCandidates("かれはてんてきです", "empty").empty());
}

TEST_F(MacZenzFeedbackStoreTest, ObservesExternalFeedbackFileChange) {
  ZenzFeedbackStore store;
  store.RecordAccepted("old", "empty", "旧候補");
  ASSERT_EQ(store.GetRankedCandidates("old", "empty").size(), 1);

  {
    std::ofstream file(profile_path("zenz_feedback.tsv"),
                       std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(file);
    file << "v2\taccepted\tnew\tempty\t新候補\t\n";
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(1100));

  EXPECT_TRUE(store.GetRankedCandidates("old", "empty").empty());
  const std::vector<ZenzFeedbackCandidate> candidates =
      store.GetRankedCandidates("new", "empty");
  ASSERT_EQ(candidates.size(), 1);
  EXPECT_EQ(candidates[0].value, "新候補");
}

TEST_F(MacZenzFeedbackStoreTest, ExportAndImportReplacePreservesRecords) {
  ZenzFeedbackStore store;
  store.RecordAccepted("k", "empty", "v");
  store.RecordRejected("k", "empty", "v", "explicit_reject");

  const std::wstring export_path = wide_profile_path("export_japanese.tsv");
  ASSERT_TRUE(store.ExportToFile(export_path));

  ASSERT_TRUE(store.ClearAll());
  ASSERT_TRUE(store.ListEntries().empty());

  ASSERT_TRUE(store.ImportFromFile(
      export_path, ZenzFeedbackImportMode::kReplace));
  const std::vector<ZenzFeedbackEntry> entries = store.ListEntries();
  ASSERT_EQ(entries.size(), 1);
  EXPECT_EQ(entries[0].key, "k");
  EXPECT_EQ(entries[0].value, "v");
  EXPECT_EQ(entries[0].accepted_count, 1);
  EXPECT_EQ(entries[0].rejected_count, 1);
}

TEST_F(MacZenzFeedbackStoreTest,
       ImportRejectsInvalidUtf8WithoutChangingExisting) {
  ZenzFeedbackStore store;
  store.RecordAccepted("existing", "empty", "既存");

  const std::filesystem::path import_path = profile_path("import_invalid_utf8.tsv");
  {
    std::ofstream file(import_path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(file);
    file << "v2\taccepted\tbad\tempty\t";
    const char invalid_utf8[] = {static_cast<char>(0xC3), static_cast<char>(0x28)};
    file.write(invalid_utf8, sizeof(invalid_utf8));
    file << "\t\n";
  }

  EXPECT_FALSE(store.ImportFromFile(
      wide_profile_path("import_invalid_utf8.tsv"),
      ZenzFeedbackImportMode::kAppend));
  const std::vector<ZenzFeedbackEntry> entries = store.ListEntries();
  ASSERT_EQ(entries.size(), 1);
  EXPECT_EQ(entries[0].key, "existing");
}

TEST_F(MacZenzFeedbackStoreTest, ImportAppendKeepsExistingRecords) {
  ZenzFeedbackStore store;
  store.RecordAccepted("existing", "empty", "既存");

  const std::filesystem::path import_path = profile_path("import_append.tsv");
  {
    std::ofstream file(import_path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(file);
    file << "v2\taccepted\timported\tempty\t追加\t\n";
  }

  ASSERT_TRUE(store.ImportFromFile(
      wide_profile_path("import_append.tsv"), ZenzFeedbackImportMode::kAppend));
  const std::vector<ZenzFeedbackEntry> entries = store.ListEntries();
  ASSERT_EQ(entries.size(), 2);
  EXPECT_EQ(entries[0].key, "existing");
  EXPECT_EQ(entries[1].key, "imported");
}

#else  // defined(_WIN32) || macOS

TEST(ZenzFeedbackStoreTest, SkippedOnUnsupportedPlatform) {
  GTEST_SKIP() << "Persistent Zenz feedback is implemented on Windows/macOS.";
}

#endif  // defined(_WIN32) || macOS

}  // namespace
}  // namespace session
}  // namespace mozc
