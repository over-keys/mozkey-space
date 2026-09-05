#include "session/zenz_feedback_store.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
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
#include <sys/stat.h>
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
    if (!ok_) return;  // Never clear the real profile if isolation failed.
    (void)ZenzFeedbackStore().ClearAll();
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
    if (!ok_) return;
    (void)ZenzFeedbackStore().ClearAll();
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

V4_TEST(LocalMatureSameRawCorrectionsRemainAvailableForMozcGate) {
  V4_PROFILE();
  ZenzFeedbackStore store;
  for (int i = 0; i < 2; ++i) {
    store.RecordLocalAccepted("しかい", "empty", "歯科医", "視界");
  }
  for (int i = 0; i < 3; ++i) {
    store.RecordLocalAccepted("しかい", "empty", "歯科医", "司会");
  }

  const auto rules = store.GetLocalPreferences("しかい", "empty", 12, 2);
  ASSERT_EQ(rules.size(), 2);
  bool saw_shikai = false;
  bool saw_shikai_alt = false;
  for (const ZenzLocalPreference& rule : rules) {
    EXPECT_EQ(rule.disfavored_value, "歯科医");
    if (rule.preferred_value == "視界") {
      saw_shikai = true;
      EXPECT_EQ(rule.observation_count, 2);
    } else if (rule.preferred_value == "司会") {
      saw_shikai_alt = true;
      EXPECT_EQ(rule.observation_count, 3);
    }
  }
  EXPECT_TRUE(saw_shikai);
  EXPECT_TRUE(saw_shikai_alt);
}

V4_TEST(LocalSurfaceFilterRunsBeforeBoundedRanking) {
  V4_PROFILE();
  ZenzFeedbackStore store;
  store.RecordLocalAccepted("しかい", "empty", "歯科医", "視界");
  store.RecordLocalAccepted("しかい", "empty", "歯科医", "視界");
  store.RecordLocalAccepted("りせき", "empty", "離籍", "離席");
  store.RecordLocalAccepted("りせき", "empty", "離籍", "離席");

  // Without the surface gate, the equal-length "しかい" rule can occupy the
  // only returned slot before Session checks the current raw/Mozc surfaces.
  const auto rules = store.GetLocalPreferences(
      "りせきしかい", "empty", 1, 2, "離籍", "離席");
  ASSERT_EQ(rules.size(), 1);
  EXPECT_EQ(rules[0].key, "りせき");
  EXPECT_EQ(rules[0].disfavored_value, "離籍");
  EXPECT_EQ(rules[0].preferred_value, "離席");
}

V4_TEST(LocalRepeatedReadingRemainsAvailableForAlignmentGate) {
  V4_PROFILE();
  ZenzFeedbackStore store;
  store.RecordLocalAccepted("しかい", "empty", "歯科医", "司会");
  store.RecordLocalAccepted("しかい", "japanese_only", "歯科医", "司会");

  const auto rules =
      store.GetLocalPreferences("しかいとしかい", "empty", 12, 2);
  ASSERT_EQ(rules.size(), 1);
  EXPECT_EQ(rules[0].key, "しかい");
  EXPECT_EQ(rules[0].disfavored_value, "歯科医");
  EXPECT_EQ(rules[0].preferred_value, "司会");
  EXPECT_EQ(rules[0].observation_count, 2);
}

V4_TEST(LocalBoundedRankingPreservesLengthCountAndRecency) {
  V4_PROFILE();
  ZenzFeedbackStore store;
  const auto path = V4_TEMP_PATH(L"ranking.tsv");
  {
    std::ofstream file{std::filesystem::path(path), std::ios::binary};
    ASSERT_TRUE(file);
    for (int i = 0; i < 80; ++i) {
      // All rules match, with conflicting length, evidence and recency orders.
      file << "v4\tlocal\taccepted\t" << (i % 2 ? "しかい" : "かい")
           << "\tempty\t歯科医\t表記" << i << '\t' << (2 + i % 3) << '\n';
    }
  }
  ASSERT_TRUE(store.ImportFromFile(path, ZenzFeedbackImportMode::kReplace));
  for (const size_t limit : {size_t{0}, size_t{1}, size_t{12}, size_t{79},
                             size_t{80}, size_t{100}}) {
    std::vector<int> expected;
    for (int length = 1; length >= 0; --length) {
      for (int count = 4; count >= 2; --count) {
        for (int i = 79; i >= 0; --i) {
          if (i % 2 == length && 2 + i % 3 == count) expected.push_back(i);
        }
      }
    }
    if (expected.size() > limit) expected.resize(limit);
    const auto actual = store.GetLocalPreferences("しかい", "empty", limit, 2);
    ASSERT_EQ(actual.size(), expected.size());
    for (size_t i = 0; i < actual.size(); ++i) {
      EXPECT_EQ(actual[i].preferred_value, "表記" + std::to_string(expected[i]));
      EXPECT_EQ(actual[i].context_class, "empty");
      EXPECT_FALSE(actual[i].has_reading_begin);
    }
  }
}

#if defined(_WIN32)
V4_TEST(ContendedRecordingSkipsObservationAndRecovers) {
  V4_PROFILE();
  ZenzFeedbackStore store;
  const HANDLE handle = ::CreateMutexW(
      nullptr, FALSE, L"Local\\MozcZenzFeedbackStoreMutationV4");
  ASSERT_NE(handle, nullptr);
  const std::unique_ptr<void, decltype(&::CloseHandle)> mutex(handle,
                                                            &::CloseHandle);
  const DWORD wait = ::WaitForSingleObject(handle, 1000);
  ASSERT_TRUE(wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED);
  // A separate thread is needed because Win32 mutex ownership is recursive.
  std::thread writer([&] { store.RecordAccepted("k", "empty", "v"); });
  writer.join();
  EXPECT_TRUE(::ReleaseMutex(handle));
  EXPECT_EQ(store.Decide("k", "empty", "v").accepted_count, 0);
  store.RecordAccepted("k", "empty", "v");
  EXPECT_EQ(store.Decide("k", "empty", "v").accepted_count, 1);
}

V4_TEST(CacheRetriesFailedInitialReadWithoutFileChange) {
  V4_PROFILE();
  ZenzFeedbackStore store;
  store.RecordAccepted("k", "empty", "v");  // Invalidates the cache.
  const HANDLE handle = ::CreateFileW(
      V4_FEEDBACK_PATH().c_str(), GENERIC_WRITE,
      FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(handle, INVALID_HANDLE_VALUE);
  {
    const std::unique_ptr<void, decltype(&::CloseHandle)> locked(handle,
                                                               &::CloseHandle);
    EXPECT_EQ(store.Decide("k", "empty", "v").accepted_count, 0);
  }
  // Merely releasing the sharing lock does not change the file stamp.
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  EXPECT_EQ(store.Decide("k", "empty", "v").accepted_count, 1);
}
#endif

V4_TEST(ImportAcceptsCrLfAndUnterminatedFinalLine) {
  V4_PROFILE();
  ZenzFeedbackStore store;
  const auto path = V4_TEMP_PATH(L"crlf.tsv");
  {
    std::ofstream file{std::filesystem::path(path), std::ios::binary};
    ASSERT_TRUE(file);
    file << "v4\tfull\taccepted\tk\tempty\tv\t\t1\r\n"
         << "v4\tlocal\taccepted\tしかい\tempty\t歯科医\t視界\t2\r\n"
         << "v4\tfull\taccepted\tk\tempty\tv\t\t1";
  }
  ASSERT_TRUE(store.ImportFromFile(path, ZenzFeedbackImportMode::kReplace));
  EXPECT_EQ(store.Decide("k", "empty", "v").accepted_count, 2);
  const auto rules = store.GetLocalPreferences("しかい", "empty", 12, 2);
  ASSERT_EQ(rules.size(), 1);
  EXPECT_EQ(rules[0].preferred_value, "視界");
}

V4_TEST(CachedAppendDetectsExternalChanges) {
  V4_PROFILE();
  ZenzFeedbackStore store;
  store.RecordAccepted("k", "empty", "v");
  ASSERT_EQ(store.Decide("k", "empty", "v").accepted_count, 1);
  {
    // Simulate another process changing the file within the stamp-cache window.
    std::ofstream file(V4_FEEDBACK_PATH(), std::ios::binary | std::ios::app);
    ASSERT_TRUE(file);
    file << "v4\tfull\taccepted\tk\tempty\tv\t\t3\n";
  }
  store.RecordAccepted("k", "empty", "v");
  EXPECT_EQ(store.Decide("k", "empty", "v").accepted_count, 5);
}

V4_TEST(CachedLocalReplayMatchesReloadAtCapAndFloor) {
  V4_PROFILE();
  ZenzFeedbackStore store;
  auto expect_count = [&](int count) {
    const auto entries = store.ListLocalPreferenceEntries();
    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].observation_count, count);
  };
  const auto path = V4_TEMP_PATH(L"cap.tsv");
  {
    std::ofstream file{std::filesystem::path(path), std::ios::binary};
    ASSERT_TRUE(file);
    file << "v4\tlocal\taccepted\tりせき\tempty\t離籍\t離席\t255\n";
  }
  ASSERT_TRUE(store.ImportFromFile(path, ZenzFeedbackImportMode::kReplace));
  expect_count(255);
  store.RecordLocalAccepted("りせきします", "empty", "離籍します", "離席します");
  store.RecordLocalRejected("りせき", "empty", "離籍", "離席");
  expect_count(254);
  // Maintenance invalidates the snapshot and forces replay from disk.
  ASSERT_TRUE(store.Maintenance(1000));
  expect_count(254);
  ASSERT_TRUE(store.ClearAll());
  ASSERT_TRUE(store.ListLocalPreferenceEntries().empty());
  store.RecordLocalRejected("りせき", "empty", "離籍", "離席");
  store.RecordLocalAccepted("りせき", "empty", "離籍", "離席");
  expect_count(1);
  ASSERT_TRUE(store.Maintenance(1000));
  expect_count(1);
}

V4_TEST(MetadataFailureIsNotTreatedAsMissingStore) {
  V4_PROFILE();
  ZenzFeedbackStore store;
  store.RecordAccepted("k", "empty", "v");
  ASSERT_EQ(store.Decide("k", "empty", "v").accepted_count, 1);
  const auto backup = std::filesystem::path(V4_TEMP_PATH(L"backup.tsv"));
  std::filesystem::rename(V4_FEEDBACK_PATH(), backup);
  ASSERT_TRUE(std::filesystem::create_directory(V4_FEEDBACK_PATH()));
  // file_size fails for directories. This must not become a successful empty
  // load that maintenance/export can use to overwrite previously learned data.
  EXPECT_FALSE(store.Maintenance(1000));
  EXPECT_FALSE(store.ExportToFile(V4_TEMP_PATH(L"export.tsv")));
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  EXPECT_EQ(store.Decide("k", "empty", "v").accepted_count, 1);
  ASSERT_TRUE(std::filesystem::remove(V4_FEEDBACK_PATH()));
  std::filesystem::rename(backup, V4_FEEDBACK_PATH());
}

V4_TEST(TornAppendKeepsFollowingRecordParseable) {
  V4_PROFILE();
  const std::filesystem::path path(V4_FEEDBACK_PATH());
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  ASSERT_FALSE(ec);

  {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(file);
    file << "v4\tfull\taccepted\tbroken";
    file.flush();
    ASSERT_TRUE(file);
  }

  ZenzFeedbackStore store;
  store.RecordAccepted("りせき", "empty", "離席");
  const ZenzFeedbackDecision decision =
      store.Decide("りせき", "empty", "離席");
  EXPECT_EQ(decision.action, ZenzFeedbackAction::kPrefer);
  EXPECT_EQ(decision.accepted_count, 1);
}

#if defined(__APPLE__) && TARGET_OS_OSX
V4_TEST(MacInternalFeedbackFileIsPrivate0600) {
  V4_PROFILE();
  ZenzFeedbackStore store;
  store.RecordAccepted("りせき", "empty", "離席");

  struct stat st = {};
  const std::filesystem::path path(V4_FEEDBACK_PATH());
  ASSERT_EQ(::stat(path.c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0600);

  ASSERT_TRUE(store.Maintenance(1000));
  ASSERT_EQ(::stat(path.c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0600);
}
#endif

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
