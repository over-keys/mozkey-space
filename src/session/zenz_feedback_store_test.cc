#include "session/zenz_feedback_store.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
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
  if (lhs.empty()) return rhs;
  return lhs.back() == L'\\' ? lhs + rhs : lhs + L"\\" + rhs;
}

bool EnsureDirectory(const std::wstring& path) {
  if (::CreateDirectoryW(path.c_str(), nullptr)) return true;
  return ::GetLastError() == ERROR_ALREADY_EXISTS;
}

class ScopedFeedbackProfile {
 public:
  ScopedFeedbackProfile() {
    wchar_t old_profile[32767] = {};
    const DWORD old_len =
        ::GetEnvironmentVariableW(L"USERPROFILE", old_profile, 32767);
    if (old_len > 0 && old_len < 32767) {
      had_old_profile_ = true;
      old_profile_.assign(old_profile, old_len);
    }

    wchar_t temp_path[MAX_PATH] = {};
    const DWORD temp_len = ::GetTempPathW(MAX_PATH, temp_path);
    if (temp_len == 0 || temp_len >= MAX_PATH) return;

    profile_dir_ = std::wstring(temp_path, temp_len) +
                   L"mozc_zenz_feedback_v4_test_" +
                   std::to_wstring(::GetCurrentProcessId()) + L"_" +
                   std::to_wstring(::GetTickCount64());
    const std::wstring app_data = JoinPath(profile_dir_, L"AppData");
    const std::wstring local_low = JoinPath(app_data, L"LocalLow");
    if (!EnsureDirectory(profile_dir_) || !EnsureDirectory(app_data) ||
        !EnsureDirectory(local_low)) {
      return;
    }
    ok_ = ::SetEnvironmentVariableW(L"USERPROFILE", profile_dir_.c_str());
  }

  ~ScopedFeedbackProfile() {
    ZenzFeedbackStore().ClearAll();
    if (had_old_profile_) {
      ::SetEnvironmentVariableW(L"USERPROFILE", old_profile_.c_str());
    } else {
      ::SetEnvironmentVariableW(L"USERPROFILE", nullptr);
    }
    std::error_code ec;
    std::filesystem::remove_all(profile_dir_, ec);
  }

  bool ok() const { return ok_; }
  std::wstring feedback_path() const {
    return JoinPath(JoinPath(JoinPath(JoinPath(profile_dir_, L"AppData"),
                                     L"LocalLow"),
                             L"Mozc"),
                    L"zenz_feedback_v4.tsv");
  }
  std::wstring temp_file_path(const std::wstring& name) const {
    return JoinPath(profile_dir_, name);
  }

 private:
  bool ok_ = false;
  bool had_old_profile_ = false;
  std::wstring old_profile_;
  std::wstring profile_dir_;
};

#define V4_TEST(name) TEST(ZenzFeedbackStoreV4Test, name)
#define V4_PROFILE()                           \
  ScopedFeedbackProfile profile;              \
  ASSERT_TRUE(profile.ok())
#define V4_FEEDBACK_PATH() profile.feedback_path()
#define V4_TEMP_PATH(name) profile.temp_file_path(name)

#elif defined(__APPLE__) && TARGET_OS_OSX

class ScopedFeedbackProfile {
 public:
  ScopedFeedbackProfile() {
    if (const char* home = std::getenv("HOME"); home != nullptr) {
      had_old_home_ = true;
      old_home_ = home;
    }
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    root_ = std::filesystem::temp_directory_path() /
            ("mozc_zenz_feedback_v4_test_" +
             std::to_string(static_cast<long long>(::getpid())) + "_" +
             std::to_string(static_cast<long long>(nonce)));
    std::error_code ec;
    std::filesystem::create_directories(root_ / ".mozc", ec);
    if (ec) return;
    ok_ = ::setenv("HOME", root_.string().c_str(), 1) == 0;
  }

  ~ScopedFeedbackProfile() {
    ZenzFeedbackStore().ClearAll();
    if (had_old_home_) {
      ::setenv("HOME", old_home_.c_str(), 1);
    } else {
      ::unsetenv("HOME");
    }
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  bool ok() const { return ok_; }
  std::filesystem::path feedback_path() const {
    return root_ / ".mozc" / "zenz_feedback_v4.tsv";
  }
  std::wstring temp_file_path(const std::wstring& name) const {
    const std::string ascii(name.begin(), name.end());
    const std::string path = (root_ / ascii).string();
    return std::wstring(path.begin(), path.end());
  }

 private:
  bool ok_ = false;
  bool had_old_home_ = false;
  std::string old_home_;
  std::filesystem::path root_;
};

#define V4_TEST(name) TEST(ZenzFeedbackStoreV4Test, name)
#define V4_PROFILE()                           \
  ScopedFeedbackProfile profile;              \
  ASSERT_TRUE(profile.ok())
#define V4_FEEDBACK_PATH() profile.feedback_path()
#define V4_TEMP_PATH(name) profile.temp_file_path(name)

#else

TEST(ZenzFeedbackStoreV4Test, SkippedOnUnsupportedPlatform) {
  GTEST_SKIP() << "Persistent Zenz feedback store is desktop-only";
}

#endif

#if defined(_WIN32) || (defined(__APPLE__) && TARGET_OS_OSX)

V4_TEST(FullSemanticsRemainWeightedAndContextCompatible) {
  V4_PROFILE();
  ZenzFeedbackStore store;

  store.RecordAccepted("k", "japanese_only", "v");
  store.RecordRejected("k", "empty", "v", "space_revert_zenz_to_mozc");

  const ZenzFeedbackDecision decision = store.Decide("k", "empty", "v");
  EXPECT_EQ(decision.action, ZenzFeedbackAction::kPrefer);
  EXPECT_EQ(decision.accepted_count, 1);
  EXPECT_EQ(decision.rejected_count, 1);
  EXPECT_EQ(decision.positive_score, 1000);
  EXPECT_EQ(decision.negative_score, 150);
  EXPECT_EQ(decision.total_score, 850);
}

V4_TEST(FullHardRejectAndAutoBlockRemainUnchanged) {
  V4_PROFILE();
  ZenzFeedbackStore store;

  store.RecordAccepted("hard", "empty", "v");
  store.RecordRejected("hard", "empty", "v", "hard_reject");
  EXPECT_EQ(store.Decide("hard", "empty", "v").action,
            ZenzFeedbackAction::kReject);

  store.RecordAccepted("auto", "empty", "v");
  store.RecordAccepted("auto", "empty", "v");
  store.RecordRejected("auto", "empty", "v", "space_revert_zenz_to_mozc");
  store.RecordRejected("auto", "empty", "v", "space_revert_zenz_to_mozc");
  ZenzFeedbackAutoBlockPolicy policy;
  policy.enabled = true;
  policy.reject_threshold = 2;
  EXPECT_EQ(store.Decide("auto", "empty", "v", policy).action,
            ZenzFeedbackAction::kReject);
}

V4_TEST(LocalCountIsThresholdIndependentAndRejectDecrements) {
  V4_PROFILE();
  ZenzFeedbackStore store;

  store.RecordLocalAccepted("しかい", "empty", "歯科医", "視界");
  store.RecordLocalAccepted("しかい", "empty", "歯科医", "視界");
  store.RecordLocalAccepted("しかい", "empty", "歯科医", "視界");
  store.RecordLocalAccepted("しかい", "empty", "歯科医", "視界");

  auto rules = store.GetLocalPreferences("しかい", "empty", 12, 2);
  ASSERT_EQ(rules.size(), 1);
  EXPECT_EQ(rules[0].observation_count, 4);
  EXPECT_TRUE(store.GetLocalPreferences("しかい", "empty", 12, 5).empty());

  store.RecordLocalRejected("しかい", "empty", "歯科医", "視界");
  rules = store.GetLocalPreferences("しかい", "empty", 12, 2);
  ASSERT_EQ(rules.size(), 1);
  EXPECT_EQ(rules[0].observation_count, 3);

  store.RecordLocalRejected("しかい", "empty", "歯科医", "視界");
  rules = store.GetLocalPreferences("しかい", "empty", 12, 2);
  ASSERT_EQ(rules.size(), 1);
  EXPECT_EQ(rules[0].observation_count, 2);
}

V4_TEST(LocalRejectedFloorsAtZero) {
  V4_PROFILE();
  ZenzFeedbackStore store;
  store.RecordLocalRejected("しかい", "empty", "歯科医", "視界");
  store.RecordLocalRejected("しかい", "empty", "歯科医", "視界");
  EXPECT_TRUE(store.ListLocalPreferenceEntries().empty());
}

V4_TEST(LocalLookupIsContextIndependent) {
  V4_PROFILE();
  ZenzFeedbackStore store;
  store.RecordLocalAccepted("しかい", "japanese_only", "歯科医", "視界");
  store.RecordLocalAccepted("しかい", "japanese_only", "歯科医", "視界");

  const auto other_context =
      store.GetLocalPreferences("しかい", "empty", 12, 2);
  const auto same_context =
      store.GetLocalPreferences("しかい", "japanese_only", 12, 2);
  ASSERT_EQ(other_context.size(), 1);
  ASSERT_EQ(same_context.size(), 1);
  EXPECT_EQ(other_context[0].observation_count, 2);
  EXPECT_EQ(other_context[0].disfavored_value, "歯科医");
  EXPECT_EQ(other_context[0].preferred_value, "視界");
}

V4_TEST(LocalDirectionChangeWeakensOldAndBuildsNew) {
  V4_PROFILE();
  ZenzFeedbackStore store;
  store.RecordLocalAccepted("しかい", "empty", "歯科医", "視界");
  store.RecordLocalAccepted("しかい", "empty", "歯科医", "視界");
  ASSERT_EQ(store.GetLocalPreferences("しかい", "empty", 12, 2).size(), 1);

  // Existing 歯科医->視界 rule intervened, but user committed 司会.
  store.RecordLocalRejected("しかい", "empty", "歯科医", "視界");
  store.RecordLocalAccepted("しかい", "empty", "歯科医", "司会");
  EXPECT_TRUE(store.GetLocalPreferences("しかい", "empty", 12, 2).empty());

  store.RecordLocalAccepted("しかい", "empty", "歯科医", "司会");
  const auto rules = store.GetLocalPreferences("しかい", "empty", 12, 2);
  ASSERT_EQ(rules.size(), 1);
  EXPECT_EQ(rules[0].preferred_value, "司会");
  EXPECT_EQ(rules[0].disfavored_value, "歯科医");
}

V4_TEST(LocalLiteralSentenceShellCanonicalizesToMinimalRule) {
  V4_PROFILE();
  ZenzFeedbackStore store;
  store.RecordLocalAccepted("りせきします", "empty",
                            "離籍します", "離席します");
  const auto entries = store.ListLocalPreferenceEntries();
  ASSERT_EQ(entries.size(), 1);
  EXPECT_EQ(entries[0].key, "りせき");
  EXPECT_EQ(entries[0].disfavored_value, "離籍");
  EXPECT_EQ(entries[0].preferred_value, "離席");
  EXPECT_EQ(entries[0].observation_count, 1);
}

V4_TEST(LocalLiteralPrefixAndSuffixCanonicalizeTogether) {
  V4_PROFILE();
  ZenzFeedbackStore store;
  store.RecordLocalAccepted("ちょっとりせきします", "empty",
                            "ちょっと離籍します", "ちょっと離席します");
  const auto entries = store.ListLocalPreferenceEntries();
  ASSERT_EQ(entries.size(), 1);
  EXPECT_EQ(entries[0].key, "りせき");
  EXPECT_EQ(entries[0].disfavored_value, "離籍");
  EXPECT_EQ(entries[0].preferred_value, "離席");
}

V4_TEST(LocalEvidenceAggregatesAcrossContextClasses) {
  V4_PROFILE();
  ZenzFeedbackStore store;
  store.RecordLocalAccepted("りせき", "empty", "離籍", "離席");
  EXPECT_TRUE(store.GetLocalPreferences("りせきします", "empty", 12, 2).empty());

  store.RecordLocalAccepted("りせき", "japanese_only", "離籍", "離席");
  const auto from_empty =
      store.GetLocalPreferences("りせきします", "empty", 12, 2);
  const auto from_japanese =
      store.GetLocalPreferences("ちょっとりせきします", "japanese_only", 12, 2);
  ASSERT_EQ(from_empty.size(), 1);
  ASSERT_EQ(from_japanese.size(), 1);
  EXPECT_EQ(from_empty[0].observation_count, 2);
  EXPECT_EQ(from_japanese[0].observation_count, 2);
  EXPECT_EQ(from_empty[0].key, "りせき");
  EXPECT_EQ(from_empty[0].disfavored_value, "離籍");
  EXPECT_EQ(from_empty[0].preferred_value, "離席");

  const auto entries = store.ListLocalPreferenceEntries();
  ASSERT_EQ(entries.size(), 1);
  EXPECT_TRUE(entries[0].context_class.empty());
  EXPECT_EQ(entries[0].observation_count, 2);
}

V4_TEST(LocalCrossContextReplaySurvivesMaintenance) {
  V4_PROFILE();
  ZenzFeedbackStore store;
  store.RecordLocalAccepted("りせき", "empty", "離籍", "離席");
  store.RecordLocalAccepted("りせき", "japanese_only", "離籍", "離席");
  store.RecordLocalAccepted("りせき", "mixed_japanese_ascii", "離籍", "離席");
  store.RecordLocalRejected("りせき", "symbol_or_other", "離籍", "離席");
  ASSERT_EQ(store.ListLocalPreferenceEntries()[0].observation_count, 2);

  ASSERT_TRUE(store.Maintenance(1000));
  const auto entries = store.ListLocalPreferenceEntries();
  ASSERT_EQ(entries.size(), 1);
  EXPECT_EQ(entries[0].observation_count, 2);
  EXPECT_EQ(store.GetLocalPreferences("りせきします", "empty", 12, 2).size(), 1);
  EXPECT_EQ(store.GetLocalPreferences("りせきします", "japanese_only", 12, 2).size(), 1);
}

V4_TEST(DeleteLocalPreferenceRemovesAllContextEvents) {
  V4_PROFILE();
  ZenzFeedbackStore store;
  store.RecordLocalAccepted("りせき", "empty", "離籍", "離席");
  store.RecordLocalAccepted("りせき", "japanese_only", "離籍", "離席");
  ASSERT_EQ(store.ListLocalPreferenceEntries().size(), 1);
  ASSERT_TRUE(store.DeleteLocalPreference("りせき", "", "離席", "離籍"));
  EXPECT_TRUE(store.ListLocalPreferenceEntries().empty());
}

V4_TEST(LocalConflictingEqualStrengthCorrectionsFailClosed) {
  V4_PROFILE();
  ZenzFeedbackStore store;
  for (int i = 0; i < 2; ++i) {
    store.RecordLocalAccepted("しかい", "empty", "歯科医", "視界");
    store.RecordLocalAccepted("しかい", "empty", "歯科医", "司会");
  }
  EXPECT_TRUE(store.GetLocalPreferences("しかい", "empty", 12, 2).empty());
}

V4_TEST(LocalMatureInverseDirectionsFailClosed) {
  V4_PROFILE();
  ZenzFeedbackStore store;
  for (int i = 0; i < 2; ++i) {
    store.RecordLocalAccepted("しかい", "empty", "歯科医", "視界");
    store.RecordLocalAccepted("しかい", "empty", "視界", "歯科医");
  }

  EXPECT_TRUE(store.GetLocalPreferences("しかい", "empty", 12, 2).empty());

  store.RecordLocalRejected("しかい", "empty", "視界", "歯科医");
  const auto rules = store.GetLocalPreferences("しかい", "empty", 12, 2);
  ASSERT_EQ(rules.size(), 1);
  EXPECT_EQ(rules[0].disfavored_value, "歯科医");
  EXPECT_EQ(rules[0].preferred_value, "視界");
}

V4_TEST(MaintenancePreservesFullScoreAndLocalReplayState) {
  V4_PROFILE();
  ZenzFeedbackStore store;
  for (int i = 0; i < 4; ++i) store.RecordAccepted("k", "empty", "v");
  for (int i = 0; i < 2; ++i) {
    store.RecordRejected("k", "empty", "v", "space_revert_zenz_to_mozc");
  }
  store.RecordRejected("k", "empty", "v", "explicit_conversion_after_zenz");

  store.RecordLocalAccepted("しかい", "empty", "歯科医", "視界");
  store.RecordLocalAccepted("しかい", "empty", "歯科医", "視界");
  store.RecordLocalAccepted("しかい", "empty", "歯科医", "視界");
  store.RecordLocalRejected("しかい", "empty", "歯科医", "視界");

  const ZenzFeedbackDecision before = store.Decide("k", "empty", "v");
  ASSERT_TRUE(store.Maintenance(1000));
  const ZenzFeedbackDecision after = store.Decide("k", "empty", "v");
  EXPECT_EQ(after.accepted_count, before.accepted_count);
  EXPECT_EQ(after.rejected_count, before.rejected_count);
  EXPECT_EQ(after.positive_score, before.positive_score);
  EXPECT_EQ(after.negative_score, before.negative_score);
  EXPECT_EQ(after.total_score, before.total_score);

  const auto rules = store.GetLocalPreferences("しかい", "empty", 12, 2);
  ASSERT_EQ(rules.size(), 1);
  EXPECT_EQ(rules[0].observation_count, 2);

  // Compaction must not bake threshold 2 into the stored count.
  EXPECT_TRUE(store.GetLocalPreferences("しかい", "empty", 12, 3).empty());
}

V4_TEST(MaintenancePreservesLocalEvidenceAcrossThresholdChanges) {
  V4_PROFILE();
  ZenzFeedbackStore store;
  for (int i = 0; i < 20; ++i) {
    store.RecordLocalAccepted("しかい", "empty", "歯科医", "司会");
  }
  ASSERT_EQ(store.GetLocalPreferences("しかい", "empty", 12, 20).size(), 1);
  ASSERT_TRUE(store.Maintenance(1000));
  const auto entries = store.ListLocalPreferenceEntries();
  ASSERT_EQ(entries.size(), 1);
  EXPECT_EQ(entries[0].observation_count, 20);
  EXPECT_EQ(store.GetLocalPreferences("しかい", "empty", 12, 20).size(), 1);
  EXPECT_TRUE(store.GetLocalPreferences("しかい", "empty", 12, 21).empty());
}

V4_TEST(LocalCountUsesFixed255Cap) {
  V4_PROFILE();
  ZenzFeedbackStore store;
  for (int i = 0; i < 300; ++i) {
    store.RecordLocalAccepted("しかい", "empty", "歯科医", "司会");
  }
  const auto entries = store.ListLocalPreferenceEntries();
  ASSERT_EQ(entries.size(), 1);
  EXPECT_EQ(entries[0].observation_count, 255);
  ASSERT_TRUE(store.Maintenance(1000));
  EXPECT_EQ(store.ListLocalPreferenceEntries()[0].observation_count, 255);
  store.RecordLocalRejected("しかい", "empty", "歯科医", "司会");
  EXPECT_EQ(store.ListLocalPreferenceEntries()[0].observation_count, 254);
}

V4_TEST(MaintenanceWritesOnlyV4AggregateRows) {
  V4_PROFILE();
  ZenzFeedbackStore store;
  store.RecordAccepted("k", "empty", "v");
  store.RecordAccepted("k", "empty", "v");
  store.RecordRejected("k", "empty", "v", "space_revert_zenz_to_mozc");
  store.RecordLocalAccepted("しかい", "empty", "歯科医", "視界");
  store.RecordLocalAccepted("しかい", "empty", "歯科医", "視界");
  ASSERT_TRUE(store.Maintenance(1000));

  std::ifstream file(V4_FEEDBACK_PATH(), std::ios::binary);
  ASSERT_TRUE(file);
  std::string line;
  int full_rows = 0;
  int local_rows = 0;
  while (std::getline(file, line)) {
    EXPECT_EQ(line.rfind("v4\t", 0), 0u);
    if (line.rfind("v4\tfull\t", 0) == 0) ++full_rows;
    if (line.rfind("v4\tlocal\t", 0) == 0) ++local_rows;
  }
  EXPECT_EQ(full_rows, 2);  // accepted aggregate + one rejected reason.
  EXPECT_EQ(local_rows, 1);
}

V4_TEST(ImportRejectsOldFormatsWithoutChangingCurrentData) {
  V4_PROFILE();
  ZenzFeedbackStore store;
  store.RecordAccepted("existing", "empty", "既存");

  const std::wstring old_path = V4_TEMP_PATH(L"legacy.tsv");
  {
    std::ofstream file(std::filesystem::path(old_path),
                       std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(file);
    file << "v2\taccepted\tk\tempty\tv\t\n";
  }
  EXPECT_FALSE(store.ImportFromFile(old_path, ZenzFeedbackImportMode::kAppend));
  const auto entries = store.ListEntries();
  ASSERT_EQ(entries.size(), 1);
  EXPECT_EQ(entries[0].key, "existing");
}

V4_TEST(ExportImportRoundTripPreservesV4Data) {
  V4_PROFILE();
  ZenzFeedbackStore store;
  store.RecordAccepted("k", "empty", "v");
  store.RecordLocalAccepted("しかい", "empty", "歯科医", "視界");
  store.RecordLocalAccepted("しかい", "empty", "歯科医", "視界");

  const std::wstring export_path = V4_TEMP_PATH(L"export_v4.tsv");
  ASSERT_TRUE(store.ExportToFile(export_path));
  ASSERT_TRUE(store.ClearAll());
  ASSERT_TRUE(store.ImportFromFile(export_path, ZenzFeedbackImportMode::kReplace));

  ASSERT_EQ(store.ListEntries().size(), 1);
  ASSERT_EQ(store.GetLocalPreferences("しかい", "empty", 12, 2).size(), 1);
}

V4_TEST(ClearAllRemovesV4Store) {
  V4_PROFILE();
  ZenzFeedbackStore store;
  store.RecordAccepted("k", "empty", "v");
  ASSERT_TRUE(store.ClearAll());
  EXPECT_TRUE(store.ListEntries().empty());
  std::ifstream file(V4_FEEDBACK_PATH(), std::ios::binary);
  EXPECT_FALSE(file);
}

#endif

}  // namespace
}  // namespace session
}  // namespace mozc
