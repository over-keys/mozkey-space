#include "session/zenz_feedback_store.h"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <istream>
#include <map>
#include <memory>
#include <mutex>
#include <ostream>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "base/util.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__) && TARGET_OS_OSX
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace mozc {
namespace session {
namespace {

constexpr int kAcceptThreshold = 1;

// Ordinary v2 records are full-sequence observations for one Zenz
// request/response pair. REV10 stores conservative Local Zenz Preference
// observations as atomic v3 records in the same file.  Legacy REV9
// "local_revert" v2 pairs remain parseable for import compatibility, but REV10
// never writes them and they never participate in ordinary feedback ranking.
//
// Feedback is interpreted as a ranking signal.  Accepted feedback is a strong
// positive observation.  Rejected feedback is usually a weak or medium negative
// observation: Space after a visible Zenz result often means "show me the
// normal candidates now", not "never show this candidate again".
constexpr int kAcceptedFeedbackWeight = 1000;
constexpr int kSpaceRevertRejectWeight = 150;
constexpr int kPredictAfterZenzRejectWeight = 200;
constexpr int kExplicitConversionRejectWeight = 400;
constexpr int kLegacyRejectWeight = 400;
constexpr int kHardRejectWeight = 2000;

#if defined(_WIN32)

std::wstring Utf8ToWide(absl::string_view s) {
  if (s.empty()) {
    return std::wstring();
  }

  const int input_size = static_cast<int>(s.size());
  const int wide_size =
      ::MultiByteToWideChar(CP_UTF8, 0, s.data(), input_size, nullptr, 0);
  if (wide_size <= 0) {
    return L"<invalid utf8>";
  }

  std::wstring w(wide_size, L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s.data(), input_size, w.data(), wide_size);
  return w;
}

std::string WideToUtf8(const std::wstring& w) {
  if (w.empty()) {
    return "";
  }

  const int utf8_size = ::WideCharToMultiByte(
      CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr,
      nullptr);
  if (utf8_size <= 0) {
    return "";
  }

  std::string s(utf8_size, '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        s.data(), utf8_size, nullptr, nullptr);
  return s;
}

void StoreDebugOutputWide(const std::wstring& message) {
  std::wstring line = L"[zenz-feedback-store] ";
  line.append(message);
  line.push_back(L'\n');
  ::OutputDebugStringW(line.c_str());
}

void StoreDebugOutput(absl::string_view message) {
  StoreDebugOutputWide(Utf8ToWide(message));
}

std::wstring RedactedWidePathStats(const wchar_t* label,
                                   const std::wstring& path) {
  std::wstring output(label);
  output.append(L"_chars=");
  output.append(std::to_wstring(path.size()));
  return output;
}

bool EnsureDirectoryExists(const std::wstring& dir);

std::wstring GetUserProfileDir() {
  wchar_t buffer[MAX_PATH] = {};
  const DWORD n = ::GetEnvironmentVariableW(L"USERPROFILE", buffer, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) {
    return L"";
  }
  return std::wstring(buffer, n);
}

std::wstring GetLocalLowAppDataDir() {
  const std::wstring user_profile = GetUserProfileDir();
  if (user_profile.empty()) {
    return L"";
  }
  return user_profile + L"\\AppData\\LocalLow";
}

std::wstring GetFeedbackDirWideForRead() {
  const std::wstring local_low = GetLocalLowAppDataDir();
  if (local_low.empty()) {
    return L"";
  }
  return local_low + L"\\Mozc";
}

std::wstring GetFeedbackDirWideForWrite() {
  const std::wstring dir = GetFeedbackDirWideForRead();
  if (dir.empty()) {
    StoreDebugOutputWide(L"LocalLow path unavailable");
    return L"";
  }

  if (!EnsureDirectoryExists(dir)) {
    StoreDebugOutputWide(
      std::wstring(L"LocalLow Mozc dir cannot be created ")
          .append(RedactedWidePathStats(L"dir", dir)));
    return L"";
  }

  return dir;
}

std::wstring GetFeedbackPathWideFromDir(const std::wstring& dir) {
  if (dir.empty()) {
    return L"";
  }

  return dir + L"\\zenz_feedback.tsv";
}

std::wstring GetFeedbackPathWide() {
  return GetFeedbackPathWideFromDir(GetFeedbackDirWideForRead());
}

bool EnsureDirectoryExists(const std::wstring& dir) {
  if (dir.empty()) {
    StoreDebugOutputWide(L"directory path is empty");
    return false;
  }

  const DWORD attr = ::GetFileAttributesW(dir.c_str());
  if (attr != INVALID_FILE_ATTRIBUTES) {
    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
      StoreDebugOutputWide(
        std::wstring(L"directory already exists ")
            .append(RedactedWidePathStats(L"dir", dir)));
      return true;
    }
    StoreDebugOutputWide(
      std::wstring(L"path exists but is not directory ")
          .append(RedactedWidePathStats(L"dir", dir)));
    return false;
  }

  if (::CreateDirectoryW(dir.c_str(), nullptr)) {
    StoreDebugOutputWide(
      std::wstring(L"CreateDirectoryW ok ")
          .append(RedactedWidePathStats(L"dir", dir)));
    return true;
  }

  const DWORD error = ::GetLastError();
  if (error == ERROR_ALREADY_EXISTS) {
    StoreDebugOutputWide(
      std::wstring(L"CreateDirectoryW already exists ")
          .append(RedactedWidePathStats(L"dir", dir)));
    return true;
  }

  StoreDebugOutputWide(
    std::wstring(L"CreateDirectoryW failed error=")
        .append(std::to_wstring(error))
        .append(L" ")
        .append(RedactedWidePathStats(L"dir", dir)));
  return false;
}

#elif defined(__APPLE__) && TARGET_OS_OSX

void StoreDebugOutput(absl::string_view) {}

std::string GetFeedbackDirectoryUtf8() {
  const char* home = std::getenv("HOME");
  if (home == nullptr || *home == '\0') {
    return std::string();
  }
  return (std::filesystem::path(home) / ".mozc").string();
}

std::string GetFeedbackPathUtf8() {
  const std::string profile_dir = GetFeedbackDirectoryUtf8();
  if (profile_dir.empty()) {
    return std::string();
  }
  return (std::filesystem::path(profile_dir) / "zenz_feedback.tsv").string();
}

std::string GetFeedbackPathUtf8ForWrite() {
  const std::string profile_dir = GetFeedbackDirectoryUtf8();
  if (profile_dir.empty()) {
    return std::string();
  }
  std::error_code ec;
  std::filesystem::create_directories(profile_dir, ec);
  if (ec) {
    return std::string();
  }
  return (std::filesystem::path(profile_dir) / "zenz_feedback.tsv").string();
}

void AppendUtf8CodePoint(uint32_t c, std::string* output) {
  if (c <= 0x7F) {
    output->push_back(static_cast<char>(c));
  } else if (c <= 0x7FF) {
    output->push_back(static_cast<char>(0xC0 | (c >> 6)));
    output->push_back(static_cast<char>(0x80 | (c & 0x3F)));
  } else if (c <= 0xFFFF) {
    output->push_back(static_cast<char>(0xE0 | (c >> 12)));
    output->push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
    output->push_back(static_cast<char>(0x80 | (c & 0x3F)));
  } else if (c <= 0x10FFFF) {
    output->push_back(static_cast<char>(0xF0 | (c >> 18)));
    output->push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
    output->push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
    output->push_back(static_cast<char>(0x80 | (c & 0x3F)));
  }
}

std::string WidePathToUtf8(const std::wstring& path) {
  std::string output;
  output.reserve(path.size());
  for (const wchar_t c : path) {
    const uint32_t codepoint = static_cast<uint32_t>(c);
    if (codepoint >= 0xD800 && codepoint <= 0xDFFF) {
      return std::string();
    }
    AppendUtf8CodePoint(codepoint, &output);
  }
  return output;
}

#else

void StoreDebugOutput(absl::string_view) {}

#endif

// Configuration UI and IME run in separate processes.  Every mutation uses
// this OS-level lock in addition to the in-process mutex below so a management
// read-modify-rewrite cannot race with an IME append and silently lose the
// newest feedback row.  The lock contains no input text and is automatically
// released by the OS if a process exits unexpectedly.
class ScopedFeedbackInterprocessLock {
 private:
  static constexpr int kLockWaitMsec = 1000;

 public:
  ScopedFeedbackInterprocessLock() {
#if defined(_WIN32)
    handle_ = ::CreateMutexW(
        nullptr, FALSE, L"Local\\MozcZenzFeedbackStoreMutationV1");
    if (handle_ == nullptr) {
      StoreDebugOutput("cross-process lock create failed");
      return;
    }
    const DWORD wait = ::WaitForSingleObject(handle_, kLockWaitMsec);
    if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
      locked_ = true;
      return;
    }
    StoreDebugOutput("cross-process lock wait failed");
#elif defined(__APPLE__) && TARGET_OS_OSX
    const std::string feedback_path = GetFeedbackPathUtf8ForWrite();
    if (feedback_path.empty()) {
      return;
    }
    const std::string lock_path = feedback_path + ".lock";
    fd_ = ::open(lock_path.c_str(), O_CREAT | O_RDWR, 0600);
    if (fd_ < 0) {
      return;
    }
    const int flags = ::fcntl(fd_, F_GETFD, 0);
    if (flags >= 0) {
      (void)::fcntl(fd_, F_SETFD, flags | FD_CLOEXEC);
    }
    constexpr int kRetryStepMsec = 10;
    const int attempts = kLockWaitMsec / kRetryStepMsec;
    for (int attempt = 0; attempt < attempts; ++attempt) {
      if (::flock(fd_, LOCK_EX | LOCK_NB) == 0) {
        locked_ = true;
        return;
      }
      if (errno != EWOULDBLOCK && errno != EAGAIN) {
        return;
      }
      std::this_thread::sleep_for(
          std::chrono::milliseconds(kRetryStepMsec));
    }
#else
    return;
#endif
  }

  ~ScopedFeedbackInterprocessLock() {
#if defined(_WIN32)
    if (locked_ && handle_ != nullptr) {
      ::ReleaseMutex(handle_);
    }
    if (handle_ != nullptr) {
      ::CloseHandle(handle_);
    }
#elif defined(__APPLE__) && TARGET_OS_OSX
    if (fd_ >= 0) {
      if (locked_) {
        (void)::flock(fd_, LOCK_UN);
      }
      ::close(fd_);
    }
#endif
  }

  ScopedFeedbackInterprocessLock(const ScopedFeedbackInterprocessLock&) =
      delete;
  ScopedFeedbackInterprocessLock& operator=(
      const ScopedFeedbackInterprocessLock&) = delete;

  bool ok() const { return locked_; }

 private:
  bool locked_ = false;
#if defined(_WIN32)
  HANDLE handle_ = nullptr;
#elif defined(__APPLE__) && TARGET_OS_OSX
  int fd_ = -1;
#endif
};

std::string EscapeTsv(absl::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (const char c : s) {
    switch (c) {
      case '\t':
        out.append("\\t");
        break;
      case '\r':
        out.append("\\r");
        break;
      case '\n':
        out.append("\\n");
        break;
      case '\\':
        out.append("\\\\");
        break;
      default:
        out.push_back(c);
        break;
    }
  }
  return out;
}

std::vector<std::string> SplitTab(const std::string& line) {
  std::vector<std::string> fields;
  std::string current;

  for (char c : line) {
    if (c == '\t') {
      fields.push_back(current);
      current.clear();
    } else {
      current.push_back(c);
    }
  }

  fields.push_back(current);
  return fields;
}

void StripUtf8BomFromFirstField(std::vector<std::string>* fields) {
  if (fields == nullptr || fields->empty()) {
    return;
  }

  std::string& first = (*fields)[0];
  constexpr absl::string_view kUtf8Bom = "\xEF\xBB\xBF";
  if (absl::StartsWith(first, kUtf8Bom)) {
    first.erase(0, kUtf8Bom.size());
  }
}

std::string RedactedStats(absl::string_view label, absl::string_view text) {
  return absl::StrCat(label, "_bytes=", text.size());
}

std::string UnescapeTsv(absl::string_view s) {
  std::string out;
  out.reserve(s.size());

  bool escaping = false;
  for (const char c : s) {
    if (!escaping) {
      if (c == '\\') {
        escaping = true;
      } else {
        out.push_back(c);
      }
      continue;
    }

    switch (c) {
      case 't':
        out.push_back('\t');
        break;
      case 'r':
        out.push_back('\r');
        break;
      case 'n':
        out.push_back('\n');
        break;
      case '\\':
        out.push_back('\\');
        break;
      default:
        out.push_back(c);
        break;
    }
    escaping = false;
  }

  if (escaping) {
    out.push_back('\\');
  }

  return out;
}

bool ContainsUnsafeTsvTextChar(absl::string_view s) {
  for (const char c : s) {
    if (c == '\t' || c == '\r' || c == '\n') {
      return true;
    }
  }
  return false;
}

bool IsValidUtf8ForFeedback(absl::string_view s) {
  return Util::IsValidUtf8(s);
}

bool IsKnownContextClass(absl::string_view context_class) {
  return context_class == "empty" ||
         context_class == "japanese_only" ||
         context_class == "japanese_with_punctuation" ||
         context_class == "mixed_japanese_ascii" ||
         context_class == "symbol_or_other" ||
         context_class == "ascii_or_digit" ||
         context_class == "sensitive_like" ||
         context_class == "legacy" ||
         context_class == "local_revert";
}

std::string NormalizeContextClass(absl::string_view context_class) {
  return context_class.empty() ? std::string("empty")
                               : std::string(context_class);
}

enum class ParsedFeedbackRecordKind {
  kFullSequence,
  kLocalPreference,
};

struct ParsedFeedbackRecord {
  ParsedFeedbackRecordKind kind = ParsedFeedbackRecordKind::kFullSequence;
  std::string action;
  std::string key;
  std::string context_class;
  std::string value;
  std::string disfavored_value;
  std::string reason;
};

bool IsSafeFeedbackRecord(const ParsedFeedbackRecord& record) {
  if (record.key.empty() || record.value.empty()) {
    return false;
  }

  if (!IsKnownContextClass(record.context_class)) {
    return false;
  }

  if (record.kind == ParsedFeedbackRecordKind::kLocalPreference) {
    if (record.context_class == "local_revert" ||
        record.disfavored_value.empty() ||
        record.value == record.disfavored_value ||
        record.reason != "rejected_zenz_final_commit") {
      return false;
    }
  } else {
    if (record.action != "accepted" && record.action != "rejected") {
      return false;
    }

    // Legacy REV9 local_revert is a dedicated paired-evidence namespace.
    // Accept only the two canonical row shapes so imported or hand-edited TSV
    // cannot silently turn arbitrary local rows into runtime preferences.
    if (record.context_class == "local_revert") {
      const bool valid_local_preferred =
          record.action == "accepted" &&
          record.reason == "local_revert_preferred";
      const bool valid_local_rejected =
          record.action == "rejected" && record.reason == "local_revert";
      if (!valid_local_preferred && !valid_local_rejected) {
        return false;
      }
    }
  }

  constexpr size_t kMaxKeyBytes = 512;
  constexpr size_t kMaxContextClassBytes = 64;
  constexpr size_t kMaxValueBytes = 512;
  constexpr size_t kMaxReasonBytes = 128;

  if (record.key.size() > kMaxKeyBytes ||
      record.context_class.size() > kMaxContextClassBytes ||
      record.value.size() > kMaxValueBytes ||
      record.disfavored_value.size() > kMaxValueBytes ||
      record.reason.size() > kMaxReasonBytes) {
    return false;
  }

  if (ContainsUnsafeTsvTextChar(record.action) ||
      ContainsUnsafeTsvTextChar(record.key) ||
      ContainsUnsafeTsvTextChar(record.context_class) ||
      ContainsUnsafeTsvTextChar(record.value) ||
      ContainsUnsafeTsvTextChar(record.disfavored_value) ||
      ContainsUnsafeTsvTextChar(record.reason)) {
    return false;
  }

  return IsValidUtf8ForFeedback(record.key) &&
         IsValidUtf8ForFeedback(record.context_class) &&
         IsValidUtf8ForFeedback(record.value) &&
         IsValidUtf8ForFeedback(record.disfavored_value) &&
         IsValidUtf8ForFeedback(record.reason);
}

bool ParseFeedbackRecord(const std::vector<std::string>& fields,
                         ParsedFeedbackRecord* record) {
  if (record == nullptr) {
    return false;
  }

  // v3 Local Zenz Preference:
  //   v3  local_preference  key  source_context_class
  //       preferred  disfavored  reason
  //
  // One line is one observation; preferred/disfavored can never be split by
  // concurrent append or import/export.
  if (fields.size() == 7 && fields[0] == "v3" &&
      fields[1] == "local_preference") {
    ParsedFeedbackRecord parsed;
    parsed.kind = ParsedFeedbackRecordKind::kLocalPreference;
    parsed.key = UnescapeTsv(fields[2]);
    parsed.context_class =
        NormalizeContextClass(UnescapeTsv(fields[3]));
    parsed.value = UnescapeTsv(fields[4]);
    parsed.disfavored_value = UnescapeTsv(fields[5]);
    parsed.reason = UnescapeTsv(fields[6]);

    if (!IsSafeFeedbackRecord(parsed)) {
      return false;
    }

    *record = std::move(parsed);
    return true;
  }

  // v2:
  //   v2  accepted|rejected  key  context_class  value  reason
  //
  // Ordinary key/value fields are full-sequence reading/correction pairs.
  // The dedicated local_revert class is the only local exception and stores
  // already-proven local reading/surface evidence using the same v2 columns.
  // Raw left/right context is never persisted.
  if (fields.size() >= 5 && fields[0] == "v2") {
    ParsedFeedbackRecord parsed;
    parsed.action = UnescapeTsv(fields[1]);
    parsed.key = UnescapeTsv(fields[2]);
    parsed.context_class =
        NormalizeContextClass(UnescapeTsv(fields[3]));
    parsed.value = UnescapeTsv(fields[4]);
    parsed.reason = fields.size() >= 6 ? UnescapeTsv(fields[5]) : "";

    if (!IsSafeFeedbackRecord(parsed)) {
      return false;
    }

    *record = std::move(parsed);
    return true;
  }

  // v1 legacy:
  //   accepted|rejected  key  context  value  reason
  //
  // The v1 context field may contain raw or reversible left context.  Never use
  // it as a lookup key after the privacy migration.  Keep only a coarse legacy
  // bucket so old feedback can still influence non-contextual decisions without
  // preserving or comparing raw context.
  if (fields.size() >= 4 &&
      (fields[0] == "accepted" || fields[0] == "rejected")) {
    ParsedFeedbackRecord parsed;
    parsed.action = UnescapeTsv(fields[0]);
    parsed.key = UnescapeTsv(fields[1]);
    parsed.context_class = "legacy";
    parsed.value = UnescapeTsv(fields[3]);
    parsed.reason = fields.size() >= 5 ? UnescapeTsv(fields[4]) : "";

    if (!IsSafeFeedbackRecord(parsed)) {
      return false;
    }

    *record = std::move(parsed);
    return true;
  }

  return false;
}

struct Counts {
  int accepted = 0;
  int rejected = 0;
  int auto_block_rejected = 0;
  int positive_score = 0;
  int negative_score = 0;
  bool hard_rejected = false;
};

int TotalScore(const Counts& c) {
  return c.positive_score - c.negative_score;
}

bool IsHardRejectReason(absl::string_view reason) {
  return reason == "hard_reject" ||
         reason == "user_hard_reject" ||
         reason == "manual_hard_reject" ||
         reason == "explicit_hard_reject";
}

int RejectWeightForReason(absl::string_view reason) {
  if (IsHardRejectReason(reason)) {
    return kHardRejectWeight;
  }
  if (reason == "space_revert_zenz_to_mozc") {
    return kSpaceRevertRejectWeight;
  }
  if (reason == "predict_after_zenz") {
    return kPredictAfterZenzRejectWeight;
  }
  if (reason == "explicit_conversion_after_zenz") {
    return kExplicitConversionRejectWeight;
  }
  if (reason == "local_revert") {
    return kAcceptedFeedbackWeight;
  }
  return kLegacyRejectWeight;
}

void AddAccepted(Counts* c) {
  ++c->accepted;
  c->positive_score += kAcceptedFeedbackWeight;
}

void AddRejected(absl::string_view reason, Counts* c) {
  ++c->rejected;
  c->negative_score += RejectWeightForReason(reason);
  if (!IsHardRejectReason(reason) && reason != "local_revert") {
    ++c->auto_block_rejected;
  }
  c->hard_rejected |= IsHardRejectReason(reason);
}

void MergeCounts(const Counts& src, Counts* dest) {
  dest->accepted += src.accepted;
  dest->rejected += src.rejected;
  dest->auto_block_rejected += src.auto_block_rejected;
  dest->positive_score += src.positive_score;
  dest->negative_score += src.negative_score;
  dest->hard_rejected |= src.hard_rejected;
}

ZenzFeedbackAutoBlockPolicy NormalizeAutoBlockPolicy(
    ZenzFeedbackAutoBlockPolicy policy) {
  if (!policy.enabled || policy.reject_threshold <= 0) {
    return ZenzFeedbackAutoBlockPolicy();
  }
  return policy;
}

bool IsAutoBlockedByPolicy(
    const Counts& c,
    const ZenzFeedbackAutoBlockPolicy& auto_block_policy) {
  const ZenzFeedbackAutoBlockPolicy policy =
      NormalizeAutoBlockPolicy(auto_block_policy);
  return policy.enabled &&
         c.auto_block_rejected >= policy.reject_threshold;
}

bool IsRejectCountDominant(const Counts& c) {
  return c.auto_block_rejected > c.accepted;
}

void ApplyRejectDominanceDecision(const Counts& counts,
                                  ZenzFeedbackDecision* decision) {
  if (decision == nullptr || decision->hard_rejected ||
      decision->auto_blocked ||
      decision->action != ZenzFeedbackAction::kPrefer) {
    return;
  }

  decision->auto_block_reject_count = counts.auto_block_rejected;
  if (!IsRejectCountDominant(counts)) {
    return;
  }

  decision->action = ZenzFeedbackAction::kNeutral;
  decision->reason = "feedback_reject_count_dominant";
}

void ApplyAutoBlockDecision(
    const Counts& counts,
    const ZenzFeedbackAutoBlockPolicy& auto_block_policy,
    ZenzFeedbackDecision* decision) {
  if (decision == nullptr || decision->hard_rejected) {
    return;
  }

  decision->auto_block_reject_count = counts.auto_block_rejected;
  if (!IsAutoBlockedByPolicy(counts, auto_block_policy)) {
    return;
  }

  decision->action = ZenzFeedbackAction::kReject;
  decision->reason = "feedback_auto_blocked";
  decision->auto_blocked = true;
}

using FeedbackKey = std::tuple<std::string, std::string, std::string>;

#if defined(_WIN32)
struct FeedbackFileStamp {
  bool exists = false;
  FILETIME last_write_time = {};
  uint64_t file_size = 0;
};

bool SameFileTime(const FILETIME& lhs, const FILETIME& rhs) {
  return ::CompareFileTime(&lhs, &rhs) == 0;
}

bool SameFeedbackFileStamp(const FeedbackFileStamp& lhs,
                           const FeedbackFileStamp& rhs) {
  return lhs.exists == rhs.exists &&
         lhs.file_size == rhs.file_size &&
         SameFileTime(lhs.last_write_time, rhs.last_write_time);
}

FeedbackFileStamp GetFeedbackFileStamp(const std::wstring& path) {
  FeedbackFileStamp stamp;

  if (path.empty()) {
    return stamp;
  }

  WIN32_FILE_ATTRIBUTE_DATA data = {};
  if (!::GetFileAttributesExW(
          path.c_str(), GetFileExInfoStandard, &data)) {
    return stamp;
  }

  stamp.exists = true;
  stamp.last_write_time = data.ftLastWriteTime;
  stamp.file_size =
      (static_cast<uint64_t>(data.nFileSizeHigh) << 32) |
      static_cast<uint64_t>(data.nFileSizeLow);
  return stamp;
}
#elif defined(__APPLE__) && TARGET_OS_OSX
struct FeedbackFileStamp {
  bool exists = false;
  timespec last_write_time = {};
  uint64_t file_size = 0;
};

bool SameFeedbackFileStamp(const FeedbackFileStamp& lhs,
                           const FeedbackFileStamp& rhs) {
  return lhs.exists == rhs.exists &&
         lhs.file_size == rhs.file_size &&
         lhs.last_write_time.tv_sec == rhs.last_write_time.tv_sec &&
         lhs.last_write_time.tv_nsec == rhs.last_write_time.tv_nsec;
}

FeedbackFileStamp GetFeedbackFileStamp(const std::string& path) {
  FeedbackFileStamp stamp;
  if (path.empty()) {
    return stamp;
  }

  struct stat data = {};
  if (::stat(path.c_str(), &data) != 0) {
    return stamp;
  }

  stamp.exists = true;
  stamp.last_write_time = data.st_mtimespec;
  stamp.file_size = static_cast<uint64_t>(data.st_size);
  return stamp;
}
#endif

size_t CountOccurrencesUpToTwo(absl::string_view text,
                              absl::string_view needle) {
  if (text.empty() || needle.empty()) {
    return 0;
  }

  size_t count = 0;
  size_t pos = 0;
  while (pos <= text.size()) {
    const size_t found = text.find(needle, pos);
    if (found == absl::string_view::npos) {
      break;
    }
    if (++count >= 2) {
      return count;
    }
    // Advance by one byte so overlapping occurrences are also considered.
    // UTF-8 continuation bytes cannot spuriously match the first byte of a
    // valid UTF-8 |needle|, so this remains safe for Japanese text.
    pos = found + 1;
  }
  return count;
}

bool IsSensitiveFeedbackContextClass(absl::string_view context_class) {
  return context_class == "sensitive_like";
}

bool IsSharedFeedbackContextClass(absl::string_view context_class) {
  // These buckets do not contain raw left context. They are coarse classes
  // only, so feedback learned in one safe text context can be reused in normal
  // conversion. ascii_or_digit belongs here because the sanitizer does not pass
  // a pure ASCII/digit left context to Zenz; from the model's perspective that
  // is the same empty prompt context as native-context-unavailable fallback.
  // This is important for cases such as:
  //
  //   learned: key + japanese_only
  //   lookup:  key + empty / symbol_or_other
  //
  // The correction itself is key/value feedback and should not be lost merely
  // because the later conversion has no preceding text or because the preceding
  // text was rejected as non-Japanese context.
  return context_class == "empty" ||
         context_class == "japanese_only" ||
         context_class == "japanese_with_punctuation" ||
         context_class == "mixed_japanese_ascii" ||
         context_class == "symbol_or_other" ||
         context_class == "ascii_or_digit" ||
         context_class == "legacy";
}

bool IsFeedbackContextCompatible(
    absl::string_view requested_context_class,
    absl::string_view record_context_class) {
  // Exact bucket reuse is allowed even for sensitive_like.
  //
  // This does not expose or compare raw left context.  It only reuses the
  // non-reversible context class already stored in feedback TSV.
  //
  // Important:
  //   sensitive_like -> sensitive_like is allowed for exact feedback lookup.
  //   sensitive_like -> normal context is still forbidden.
  //   normal context -> sensitive_like is still forbidden.
  if (requested_context_class == record_context_class) {
    return true;
  }

  if (IsSensitiveFeedbackContextClass(requested_context_class) ||
      IsSensitiveFeedbackContextClass(record_context_class)) {
    return false;
  }

  if (IsSharedFeedbackContextClass(requested_context_class) &&
      IsSharedFeedbackContextClass(record_context_class)) {
    return true;
  }

  return false;
}

ZenzFeedbackDecision BuildDecisionFromCounts(const Counts& c) {
  ZenzFeedbackDecision decision;
  decision.accepted_count = c.accepted;
  decision.rejected_count = c.rejected;
  decision.auto_block_reject_count = c.auto_block_rejected;
  decision.positive_score = c.positive_score;
  decision.negative_score = c.negative_score;
  decision.total_score = TotalScore(c);
  decision.hard_rejected = c.hard_rejected;

  if (c.hard_rejected) {
    decision.action = ZenzFeedbackAction::kReject;
    decision.reason = "feedback_hard_rejected";
    return decision;
  }

  if (c.accepted >= kAcceptThreshold && decision.total_score > 0) {
    decision.action = ZenzFeedbackAction::kPrefer;
    decision.reason = "feedback_preferred";
    return decision;
  }

  decision.action = ZenzFeedbackAction::kNeutral;
  decision.reason = c.rejected > 0 ? "feedback_downgraded"
                                   : "feedback_neutral";
  return decision;
}

void WriteRecordToStream(const ParsedFeedbackRecord& record,
                         std::ostream* output) {
  if (record.kind == ParsedFeedbackRecordKind::kLocalPreference) {
    *output << "v3" << '\t'
            << "local_preference" << '\t'
            << EscapeTsv(record.key) << '\t'
            << EscapeTsv(NormalizeContextClass(record.context_class)) << '\t'
            << EscapeTsv(record.value) << '\t'
            << EscapeTsv(record.disfavored_value) << '\t'
            << EscapeTsv(record.reason) << '\n';
    return;
  }

  *output << "v2" << '\t'
          << EscapeTsv(record.action) << '\t'
          << EscapeTsv(record.key) << '\t'
          << EscapeTsv(NormalizeContextClass(record.context_class)) << '\t'
          << EscapeTsv(record.value) << '\t'
          << EscapeTsv(record.reason) << '\n';
}

bool WriteRecordsToStream(const std::vector<ParsedFeedbackRecord>& records,
                          std::ostream* output) {
  if (output == nullptr) {
    return false;
  }

  for (const ParsedFeedbackRecord& record : records) {
    if (!IsSafeFeedbackRecord(record)) {
      return false;
    }
    WriteRecordToStream(record, output);
  }

  output->flush();
  return static_cast<bool>(*output);
}

bool LoadRecordsFromStream(std::istream* input,
                           bool strict,
                           std::vector<ParsedFeedbackRecord>* records) {
  if (input == nullptr || records == nullptr) {
    return false;
  }

  records->clear();

  bool ok = true;
  std::string line;
  while (std::getline(*input, line)) {
    if (line.empty()) {
      continue;
    }

    std::vector<std::string> fields = SplitTab(line);
    StripUtf8BomFromFirstField(&fields);

    ParsedFeedbackRecord record;
    if (!ParseFeedbackRecord(fields, &record)) {
      ok = false;
      if (strict) {
        return false;
      }
      continue;
    }

    records->push_back(std::move(record));
  }

  return ok || !strict;
}

bool LoadFeedbackRecordsFromDisk(
    std::vector<ParsedFeedbackRecord>* records) {
  if (records == nullptr) {
    return false;
  }
  records->clear();

#if defined(_WIN32)
  const std::wstring path = GetFeedbackPathWide();
  if (path.empty()) {
    return false;
  }
  const FeedbackFileStamp stamp = GetFeedbackFileStamp(path);
  if (!stamp.exists) {
    return true;
  }
  std::ifstream file(path, std::ios::binary);
#elif defined(__APPLE__) && TARGET_OS_OSX
  const std::string path = GetFeedbackPathUtf8();
  if (path.empty()) {
    return false;
  }
  const FeedbackFileStamp stamp = GetFeedbackFileStamp(path);
  if (!stamp.exists) {
    return true;
  }
  std::ifstream file(path, std::ios::binary);
#else
  return true;
#endif

#if defined(_WIN32) || (defined(__APPLE__) && TARGET_OS_OSX)
  if (!file) {
    return false;
  }
  return LoadRecordsFromStream(&file, false, records);
#endif
}

using FeedbackCounts = std::map<FeedbackKey, Counts>;

struct LocalPreferenceAggregate {
  std::string context_class;
  std::string preferred_value;
  std::string disfavored_value;
  int observation_count = 0;
};

using LocalPreferencesByReading =
    std::map<std::string, std::vector<LocalPreferenceAggregate>>;

struct FeedbackData {
  FeedbackCounts counts;
  LocalPreferencesByReading local_preferences_by_reading;
};

std::shared_ptr<const FeedbackData> BuildFeedbackData(
    std::vector<ParsedFeedbackRecord> records) {
  auto data = std::make_shared<FeedbackData>();

  using LocalKey =
      std::tuple<std::string, std::string, std::string, std::string>;
  std::map<LocalKey, int> local_counts;

  for (size_t i = 0; i < records.size(); ++i) {
    const ParsedFeedbackRecord& record = records[i];
    if (record.kind == ParsedFeedbackRecordKind::kLocalPreference) {
      ++local_counts[LocalKey(record.key, record.context_class, record.value,
                             record.disfavored_value)];
      continue;
    }

    // Preserve REV9 local_revert import compatibility.  Those v2 rows did not
    // retain their original coarse source context, so treat a canonical pair as
    // legacy non-sensitive evidence. REV10 never writes this form.
    if (record.context_class == "local_revert") {
      if (i + 1 < records.size()) {
        const ParsedFeedbackRecord& rejected = records[i + 1];
        if (record.action == "accepted" &&
            record.reason == "local_revert_preferred" &&
            rejected.kind == ParsedFeedbackRecordKind::kFullSequence &&
            rejected.action == "rejected" &&
            rejected.context_class == "local_revert" &&
            rejected.reason == "local_revert" &&
            record.key == rejected.key &&
            record.value != rejected.value) {
          ++local_counts[LocalKey(record.key, "legacy", record.value,
                                 rejected.value)];
          ++i;
        }
      }
      continue;
    }

    Counts& c = data->counts[FeedbackKey(
        record.key, record.context_class, record.value)];
    if (record.action == "accepted") {
      AddAccepted(&c);
    } else if (record.action == "rejected") {
      AddRejected(record.reason, &c);
    }
  }

  for (const auto& [local_key, count] : local_counts) {
    const auto& [key, context_class, preferred, disfavored] = local_key;
    data->local_preferences_by_reading[key].push_back(
        {context_class, preferred, disfavored, count});
  }

  return data;
}

struct FeedbackDataCache {
  bool valid = false;
#if defined(_WIN32)
  std::wstring path;
#elif defined(__APPLE__) && TARGET_OS_OSX
  std::string path;
#endif
#if defined(_WIN32) || (defined(__APPLE__) && TARGET_OS_OSX)
  FeedbackFileStamp stamp;
  std::chrono::steady_clock::time_point next_stamp_check;
#endif
  std::shared_ptr<const FeedbackData> data;
};

std::mutex g_feedback_data_cache_mutex;
FeedbackDataCache g_feedback_data_cache;

// Serializes every mutation within one process.  The OS-level lock above then
// extends the same critical section across the IME and configuration process.
std::mutex g_feedback_mutation_mutex;

void InvalidateFeedbackRecordsCache() {
  std::lock_guard<std::mutex> lock(g_feedback_data_cache_mutex);
  g_feedback_data_cache = FeedbackDataCache();
}

std::shared_ptr<const FeedbackData> LoadFeedbackData() {
#if defined(_WIN32)
  // The feedback file can be changed by the separate configuration process.
  // Check metadata at most once per second; cache hits in between are memory-only.
  // Directory creation and writability checks stay on the write path only.
  constexpr auto kStampCheckInterval = std::chrono::seconds(1);
  const std::wstring path = GetFeedbackPathWide();
  const auto now = std::chrono::steady_clock::now();

  std::lock_guard<std::mutex> lock(g_feedback_data_cache_mutex);
  if (g_feedback_data_cache.valid &&
      g_feedback_data_cache.path == path &&
      g_feedback_data_cache.data != nullptr &&
      now < g_feedback_data_cache.next_stamp_check) {
    return g_feedback_data_cache.data;
  }

  const FeedbackFileStamp stamp = GetFeedbackFileStamp(path);
  if (g_feedback_data_cache.valid &&
      g_feedback_data_cache.path == path &&
      g_feedback_data_cache.data != nullptr &&
      SameFeedbackFileStamp(g_feedback_data_cache.stamp, stamp)) {
    g_feedback_data_cache.next_stamp_check = now + kStampCheckInterval;
    return g_feedback_data_cache.data;
  }

  std::vector<ParsedFeedbackRecord> records;
  if (stamp.exists) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
      // Do not turn a transient open failure into a valid empty cache.
      // Preserve the last known-good data when possible and retry after the
      // normal stamp interval.
      if (g_feedback_data_cache.valid &&
          g_feedback_data_cache.path == path &&
          g_feedback_data_cache.data != nullptr) {
        g_feedback_data_cache.next_stamp_check = now + kStampCheckInterval;
        return g_feedback_data_cache.data;
      }
      g_feedback_data_cache.valid = true;
      g_feedback_data_cache.path = path;
      g_feedback_data_cache.stamp = FeedbackFileStamp();
      g_feedback_data_cache.next_stamp_check = now + kStampCheckInterval;
      g_feedback_data_cache.data = BuildFeedbackData({});
      return g_feedback_data_cache.data;
    }
    LoadRecordsFromStream(&file, false, &records);
  }

  g_feedback_data_cache.valid = true;
  g_feedback_data_cache.path = path;
  g_feedback_data_cache.stamp = stamp;
  g_feedback_data_cache.next_stamp_check = now + kStampCheckInterval;
  g_feedback_data_cache.data = BuildFeedbackData(std::move(records));
  return g_feedback_data_cache.data;
#elif defined(__APPLE__) && TARGET_OS_OSX
  // ConfigDialog is a separate process on macOS too, so use the same bounded
  // metadata polling policy as Windows.  This keeps normal conversions
  // memory-only while making import/delete/clear visible without restarting IME.
  constexpr auto kStampCheckInterval = std::chrono::seconds(1);
  const std::string path = GetFeedbackPathUtf8();
  const auto now = std::chrono::steady_clock::now();

  std::lock_guard<std::mutex> lock(g_feedback_data_cache_mutex);
  if (g_feedback_data_cache.valid &&
      g_feedback_data_cache.path == path &&
      g_feedback_data_cache.data != nullptr &&
      now < g_feedback_data_cache.next_stamp_check) {
    return g_feedback_data_cache.data;
  }

  const FeedbackFileStamp stamp = GetFeedbackFileStamp(path);
  if (g_feedback_data_cache.valid &&
      g_feedback_data_cache.path == path &&
      g_feedback_data_cache.data != nullptr &&
      SameFeedbackFileStamp(g_feedback_data_cache.stamp, stamp)) {
    g_feedback_data_cache.next_stamp_check = now + kStampCheckInterval;
    return g_feedback_data_cache.data;
  }

  std::vector<ParsedFeedbackRecord> records;
  if (stamp.exists) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
      if (g_feedback_data_cache.valid &&
          g_feedback_data_cache.path == path &&
          g_feedback_data_cache.data != nullptr) {
        g_feedback_data_cache.next_stamp_check = now + kStampCheckInterval;
        return g_feedback_data_cache.data;
      }
      g_feedback_data_cache.valid = true;
      g_feedback_data_cache.path = path;
      g_feedback_data_cache.stamp = FeedbackFileStamp();
      g_feedback_data_cache.next_stamp_check = now + kStampCheckInterval;
      g_feedback_data_cache.data = BuildFeedbackData({});
      return g_feedback_data_cache.data;
    }
    LoadRecordsFromStream(&file, false, &records);
  }

  g_feedback_data_cache.valid = true;
  g_feedback_data_cache.path = path;
  g_feedback_data_cache.stamp = stamp;
  g_feedback_data_cache.next_stamp_check = now + kStampCheckInterval;
  g_feedback_data_cache.data = BuildFeedbackData(std::move(records));
  return g_feedback_data_cache.data;
#else
  static const std::shared_ptr<const FeedbackData> empty_data =
      BuildFeedbackData({});
  return empty_data;
#endif
}

std::shared_ptr<const FeedbackCounts> LoadCounts() {
  const std::shared_ptr<const FeedbackData> data = LoadFeedbackData();
  return std::shared_ptr<const FeedbackCounts>(data, &data->counts);
}

#if defined(_WIN32)
bool WriteRecordsToPath(const std::wstring& path,
                        const std::vector<ParsedFeedbackRecord>& records) {
  if (path.empty()) {
    return false;
  }

  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    StoreDebugOutputWide(
        std::wstring(L"write open failed ")
            .append(RedactedWidePathStats(L"path", path)));
    return false;
  }

  return WriteRecordsToStream(records, &file);
}

bool WriteFeedbackRecordsAtomically(
    const std::vector<ParsedFeedbackRecord>& records) {
  const std::wstring dir_w = GetFeedbackDirWideForWrite();
  const std::wstring path_w = GetFeedbackPathWideFromDir(dir_w);

  if (dir_w.empty() || path_w.empty()) {
    StoreDebugOutput("atomic write failed: empty path");
    return false;
  }

  if (records.empty()) {
    if (::DeleteFileW(path_w.c_str())) {
      StoreDebugOutput("clear ok: file removed");
      InvalidateFeedbackRecordsCache();
      return true;
    }

    const DWORD error = ::GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
      StoreDebugOutput("clear ok: file already absent");
      InvalidateFeedbackRecordsCache();
      return true;
    }

    StoreDebugOutputWide(
        std::wstring(L"clear failed error=")
            .append(std::to_wstring(error))
            .append(L" ")
            .append(RedactedWidePathStats(L"path", path_w)));
    return false;
  }

  const std::wstring tmp_path_w =
      path_w + L".tmp." + std::to_wstring(::GetCurrentProcessId());

  if (!WriteRecordsToPath(tmp_path_w, records)) {
    ::DeleteFileW(tmp_path_w.c_str());
    return false;
  }

  if (!::MoveFileExW(tmp_path_w.c_str(),
                     path_w.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    const DWORD error = ::GetLastError();
    StoreDebugOutputWide(
        std::wstring(L"atomic replace failed error=")
            .append(std::to_wstring(error))
            .append(L" ")
            .append(RedactedWidePathStats(L"path", path_w)));
    ::DeleteFileW(tmp_path_w.c_str());
    return false;
  }

  StoreDebugOutputWide(
      std::wstring(L"atomic write ok ")
          .append(RedactedWidePathStats(L"path", path_w)));
  InvalidateFeedbackRecordsCache();
  return true;
}
#elif defined(__APPLE__) && TARGET_OS_OSX
bool WriteRecordsToPathUtf8(
    absl::string_view path,
    const std::vector<ParsedFeedbackRecord>& records) {
  if (path.empty()) {
    return false;
  }
  std::ofstream file(std::string(path), std::ios::binary | std::ios::trunc);
  return file && WriteRecordsToStream(records, &file);
}

bool WriteFeedbackRecordsAtomically(
    const std::vector<ParsedFeedbackRecord>& records) {
  const std::string path = GetFeedbackPathUtf8ForWrite();
  if (path.empty()) {
    return false;
  }
  if (records.empty()) {
    std::error_code ec;
    const bool existed = std::filesystem::exists(path, ec);
    if (ec) {
      return false;
    }
    if (existed) {
      std::filesystem::remove(path, ec);
      if (ec) {
        return false;
      }
    }
    InvalidateFeedbackRecordsCache();
    return true;
  }

  const std::string tmp_path = path + ".tmp";
  if (!WriteRecordsToPathUtf8(tmp_path, records)) {
    std::error_code ignored;
    std::filesystem::remove(tmp_path, ignored);
    return false;
  }
  std::error_code ec;
  std::filesystem::rename(tmp_path, path, ec);
  if (ec) {
    std::error_code ignored;
    std::filesystem::remove(tmp_path, ignored);
    return false;
  }
  InvalidateFeedbackRecordsCache();
  return true;
}
#else
bool WriteFeedbackRecordsAtomically(
    const std::vector<ParsedFeedbackRecord>&) {
  StoreDebugOutput("atomic write failed: zenz feedback store is unsupported");
  return false;
}
#endif

void AppendRecords(const std::vector<ParsedFeedbackRecord>& records) {
  if (records.empty()) {
    return;
  }
  for (const ParsedFeedbackRecord& record : records) {
    if (!IsSafeFeedbackRecord(record)) {
      StoreDebugOutput(absl::StrCat(
          "append rejected invalid record action=", record.action,
          " ", RedactedStats("key", record.key),
          " context_class=", record.context_class,
          " ", RedactedStats("value", record.value),
          " reason=", record.reason));
      return;
    }
  }

  // Build the complete append payload first and serialize appends within this
  // IME process.  A REV10 local preference is one v3 row, so one observation
  // can no longer be split into preferred/rejected halves as in REV9.
  std::ostringstream payload_stream;
  for (const ParsedFeedbackRecord& record : records) {
    WriteRecordToStream(record, &payload_stream);
  }
  const std::string payload = payload_stream.str();
  if (payload.empty()) {
    return;
  }

  std::lock_guard<std::mutex> mutation_lock(g_feedback_mutation_mutex);
  ScopedFeedbackInterprocessLock interprocess_lock;
  if (!interprocess_lock.ok()) {
    StoreDebugOutput("append failed: cross-process lock unavailable");
    return;
  }

#if defined(_WIN32)
  const std::wstring dir_w = GetFeedbackDirWideForWrite();
  const std::wstring path_w = GetFeedbackPathWideFromDir(dir_w);

  StoreDebugOutputWide(
      std::wstring(L"append feedback dir ")
          .append(RedactedWidePathStats(L"dir", dir_w)));
  StoreDebugOutputWide(
      std::wstring(L"append feedback path ")
          .append(RedactedWidePathStats(L"path", path_w)));

  if (dir_w.empty() || path_w.empty()) {
    StoreDebugOutput("append failed: empty path");
    return;
  }

  std::ofstream file(path_w, std::ios::binary | std::ios::app);
#elif defined(__APPLE__) && TARGET_OS_OSX
  const std::string path = GetFeedbackPathUtf8ForWrite();
  if (path.empty()) {
    return;
  }
  std::ofstream file(path, std::ios::binary | std::ios::app);
#else
  std::ofstream file;
#endif

  if (!file) {
#if defined(_WIN32)
    StoreDebugOutputWide(
        std::wstring(L"append open failed ")
            .append(RedactedWidePathStats(L"path", path_w)));
#else
    StoreDebugOutput("append open failed");
#endif
    return;
  }

  file.write(payload.data(), static_cast<std::streamsize>(payload.size()));
  file.flush();

  if (!file) {
    StoreDebugOutput("append write/flush failed");
    return;
  }

  InvalidateFeedbackRecordsCache();

  for (const ParsedFeedbackRecord& record : records) {
    StoreDebugOutput(absl::StrCat(
        "append ok action=", record.action,
        " ", RedactedStats("key", record.key),
        " context_class=", record.context_class,
        " ", RedactedStats("value", record.value),
        " reason=", record.reason));
  }
}

void AppendRecord(absl::string_view action,
                  absl::string_view key,
                  absl::string_view context_class,
                  absl::string_view value,
                  absl::string_view reason) {
  ParsedFeedbackRecord record;
  record.action = std::string(action);
  record.key = std::string(key);
  record.context_class = NormalizeContextClass(context_class);
  record.value = std::string(value);
  record.reason = std::string(reason);
  AppendRecords({record});
}

}  // namespace

ZenzFeedbackDecision ZenzFeedbackStore::Decide(
    absl::string_view key,
    absl::string_view context_class,
    absl::string_view value) const {
  return Decide(key, context_class, value, ZenzFeedbackAutoBlockPolicy());
}

ZenzFeedbackDecision ZenzFeedbackStore::Decide(
    absl::string_view key,
    absl::string_view context_class,
    absl::string_view value,
    const ZenzFeedbackAutoBlockPolicy& auto_block_policy) const {
  const std::shared_ptr<const FeedbackCounts> counts = LoadCounts();

  const std::string normalized_key(key);
  const std::string normalized_context_class =
      NormalizeContextClass(context_class);
  const std::string normalized_value(value);

  Counts aggregated;

  for (auto it = counts->lower_bound(FeedbackKey(normalized_key, "", ""));
       it != counts->end() && std::get<0>(it->first) == normalized_key; ++it) {
    const FeedbackKey& feedback_key = it->first;
    const Counts& c = it->second;

    const std::string& record_context_class = std::get<1>(feedback_key);
    const std::string& record_value = std::get<2>(feedback_key);

    if (record_value != normalized_value) {
      continue;
    }

    if (!IsFeedbackContextCompatible(
            normalized_context_class, record_context_class)) {
      continue;
    }

    MergeCounts(c, &aggregated);
  }

  // All ordinary coarse context classes are intentionally promotion-compatible.
  // Use the same compatible aggregate for every part of the decision so native
  // context availability cannot change learning sensitivity merely by moving an
  // observation between e.g. japanese_only and empty.  Privacy-separated
  // classes remain isolated by IsFeedbackContextCompatible().
  ZenzFeedbackDecision decision = BuildDecisionFromCounts(aggregated);
  ApplyRejectDominanceDecision(aggregated, &decision);
  ApplyAutoBlockDecision(aggregated, auto_block_policy, &decision);
  return decision;
}

std::vector<ZenzFeedbackCandidate> ZenzFeedbackStore::GetRankedCandidates(
    absl::string_view key,
    absl::string_view context_class) const {
  return GetRankedCandidates(key, context_class,
                             ZenzFeedbackAutoBlockPolicy());
}

std::vector<ZenzFeedbackCandidate> ZenzFeedbackStore::GetRankedCandidates(
    absl::string_view key,
    absl::string_view context_class,
    const ZenzFeedbackAutoBlockPolicy& auto_block_policy) const {
  const std::shared_ptr<const FeedbackCounts> counts = LoadCounts();

  const std::string normalized_key(key);
  const std::string normalized_context_class =
      NormalizeContextClass(context_class);

  std::map<std::string, Counts> value_counts;

  for (auto it = counts->lower_bound(FeedbackKey(normalized_key, "", ""));
       it != counts->end() && std::get<0>(it->first) == normalized_key; ++it) {
    const FeedbackKey& feedback_key = it->first;
    const Counts& c = it->second;

    const std::string& record_context_class = std::get<1>(feedback_key);
    const std::string& record_value = std::get<2>(feedback_key);

    if (!IsFeedbackContextCompatible(
            normalized_context_class, record_context_class)) {
      continue;
    }

    Counts& aggregated = value_counts[record_value];
    MergeCounts(c, &aggregated);
  }

  std::vector<ZenzFeedbackCandidate> candidates;

  for (const auto& item : value_counts) {
    const std::string& value = item.first;
    const Counts& c = item.second;

    if (c.hard_rejected ||
        IsAutoBlockedByPolicy(c, auto_block_policy) ||
        IsRejectCountDominant(c) ||
        c.accepted < kAcceptThreshold ||
        TotalScore(c) <= 0) {
      continue;
    }

    ZenzFeedbackCandidate candidate;
    candidate.value = value;
    candidate.accepted_count = c.accepted;
    candidate.rejected_count = c.rejected;
    candidate.positive_score = c.positive_score;
    candidate.negative_score = c.negative_score;
    candidate.total_score = TotalScore(c);
    candidate.auto_block_reject_count = c.auto_block_rejected;
    candidate.hard_rejected = c.hard_rejected;
    candidate.auto_blocked = false;
    candidate.reason = "feedback_preferred";
    candidates.push_back(std::move(candidate));
  }

  std::sort(candidates.begin(), candidates.end(),
            [](const ZenzFeedbackCandidate& a,
               const ZenzFeedbackCandidate& b) {
              if (a.total_score != b.total_score) {
                return a.total_score > b.total_score;
              }
              if (a.positive_score != b.positive_score) {
                return a.positive_score > b.positive_score;
              }
              if (a.negative_score != b.negative_score) {
                return a.negative_score < b.negative_score;
              }
              return a.value < b.value;
            });

  return candidates;
}

std::vector<ZenzLocalPreference> ZenzFeedbackStore::GetLocalPreferences(
    absl::string_view full_key, absl::string_view context_class,
    size_t max_results, int min_observation_count) const {
  std::vector<ZenzLocalPreference> result;
  if (full_key.empty() || max_results == 0) {
    return result;
  }
  min_observation_count = std::max(1, min_observation_count);

  const std::shared_ptr<const FeedbackData> data = LoadFeedbackData();
  if (data->local_preferences_by_reading.empty()) {
    return result;
  }

  // Enumerate only readings that are substrings of the current full reading,
  // then probe the in-memory index. With the normal 64-character Zenz key cap
  // this is bounded and avoids scanning a growing history on every request.
  std::vector<size_t> boundaries;
  boundaries.reserve(full_key.size() + 1);
  boundaries.push_back(0);
  for (size_t i = 1; i < full_key.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(full_key[i]);
    if ((c & 0xC0) != 0x80) {
      boundaries.push_back(i);
    }
  }
  boundaries.push_back(full_key.size());

  std::set<std::string> matching_readings;
  for (size_t begin_index = 0; begin_index + 1 < boundaries.size();
       ++begin_index) {
    for (size_t end_index = begin_index + 1; end_index < boundaries.size();
         ++end_index) {
      const size_t begin = boundaries[begin_index];
      const size_t end = boundaries[end_index];
      const std::string reading(full_key.substr(begin, end - begin));
      if (data->local_preferences_by_reading.find(reading) !=
          data->local_preferences_by_reading.end()) {
        matching_readings.insert(reading);
      }
    }
  }

  struct Candidate {
    std::string key;
    std::string preferred_value;
    std::string disfavored_value;
    int observation_count = 0;
  };
  std::vector<Candidate> candidates;

  const std::string requested_context_class =
      NormalizeContextClass(context_class);
  for (const std::string& key : matching_readings) {
    if (CountOccurrencesUpToTwo(full_key, key) != 1) {
      continue;
    }

    const auto reading_it = data->local_preferences_by_reading.find(key);
    if (reading_it == data->local_preferences_by_reading.end()) {
      continue;
    }

    using DirectionKey = std::pair<std::string, std::string>;
    std::map<DirectionKey, int> direction_counts;
    for (const LocalPreferenceAggregate& aggregate : reading_it->second) {
      if (!IsFeedbackContextCompatible(
              requested_context_class, aggregate.context_class)) {
        continue;
      }
      direction_counts[DirectionKey(
          aggregate.preferred_value, aggregate.disfavored_value)] +=
          aggregate.observation_count;
    }

    for (const auto& [direction, count] : direction_counts) {
      if (count < min_observation_count || direction.first.empty() ||
          direction.second.empty() || direction.first == direction.second) {
        continue;
      }
      candidates.push_back(
          {key, direction.first, direction.second, count});
    }
  }

  std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& a, const Candidate& b) {
              if (a.key.size() != b.key.size()) {
                return a.key.size() > b.key.size();
              }
              // Compatible normal context classes represent the same local
              // preference evidence.  Rank by total evidence, not by which
              // coarse class happened to be available on this request.
              if (a.observation_count != b.observation_count) {
                return a.observation_count > b.observation_count;
              }
              if (a.preferred_value != b.preferred_value) {
                return a.preferred_value < b.preferred_value;
              }
              return a.disfavored_value < b.disfavored_value;
            });

  for (const Candidate& candidate : candidates) {
    ZenzLocalPreference preference;
    preference.key = candidate.key;
    preference.context_class = requested_context_class;
    preference.preferred_value = candidate.preferred_value;
    preference.disfavored_value = candidate.disfavored_value;
    preference.observation_count = candidate.observation_count;
    result.push_back(std::move(preference));
    if (result.size() >= max_results) {
      break;
    }
  }

  return result;
}

std::vector<ZenzLocalPreferenceEntry>
ZenzFeedbackStore::ListLocalPreferenceEntries() const {
  const std::shared_ptr<const FeedbackData> data = LoadFeedbackData();
  std::vector<ZenzLocalPreferenceEntry> entries;

  for (const auto& [key, aggregates] : data->local_preferences_by_reading) {
    for (const LocalPreferenceAggregate& aggregate : aggregates) {
      int effective_count = 0;
      int opposite_effective_count = 0;
      for (const LocalPreferenceAggregate& other : aggregates) {
        if (!IsFeedbackContextCompatible(
                aggregate.context_class, other.context_class)) {
          continue;
        }
        if (other.preferred_value == aggregate.preferred_value &&
            other.disfavored_value == aggregate.disfavored_value) {
          effective_count += other.observation_count;
        }
        if (other.preferred_value == aggregate.disfavored_value &&
            other.disfavored_value == aggregate.preferred_value) {
          opposite_effective_count += other.observation_count;
        }
      }

      ZenzLocalPreferenceEntry entry;
      entry.key = key;
      entry.context_class = aggregate.context_class;
      entry.preferred_value = aggregate.preferred_value;
      entry.disfavored_value = aggregate.disfavored_value;
      entry.observation_count = aggregate.observation_count;
      entry.effective_observation_count = effective_count;
      entry.opposite_effective_observation_count = opposite_effective_count;
      entries.push_back(std::move(entry));
    }
  }

  std::sort(entries.begin(), entries.end(),
            [](const ZenzLocalPreferenceEntry& a,
               const ZenzLocalPreferenceEntry& b) {
              if (a.key != b.key) {
                return a.key < b.key;
              }
              if (a.context_class != b.context_class) {
                return a.context_class < b.context_class;
              }
              if (a.preferred_value != b.preferred_value) {
                return a.preferred_value < b.preferred_value;
              }
              return a.disfavored_value < b.disfavored_value;
            });
  return entries;
}

std::vector<ZenzFeedbackCandidate> ZenzFeedbackStore::GetAcceptedCandidates(
    absl::string_view key,
    absl::string_view context_class) const {
  return GetRankedCandidates(key, context_class);
}

std::vector<ZenzFeedbackCandidate> ZenzFeedbackStore::GetAcceptedCandidates(
    absl::string_view key,
    absl::string_view context_class,
    const ZenzFeedbackAutoBlockPolicy& auto_block_policy) const {
  return GetRankedCandidates(key, context_class, auto_block_policy);
}

std::vector<ZenzFeedbackEntry> ZenzFeedbackStore::ListEntries() const {
  return ListEntries(ZenzFeedbackAutoBlockPolicy());
}

std::vector<ZenzFeedbackEntry> ZenzFeedbackStore::ListEntries(
    const ZenzFeedbackAutoBlockPolicy& auto_block_policy) const {
  const std::shared_ptr<const FeedbackCounts> counts = LoadCounts();

  std::vector<ZenzFeedbackEntry> entries;
  entries.reserve(counts->size());

  for (const auto& item : *counts) {
    const FeedbackKey& feedback_key = item.first;
    const Counts& exact = item.second;
    const std::string& key = std::get<0>(feedback_key);
    const std::string& context_class = std::get<1>(feedback_key);
    const std::string& value = std::get<2>(feedback_key);

    Counts effective;
    for (auto it = counts->lower_bound(FeedbackKey(key, "", ""));
         it != counts->end() && std::get<0>(it->first) == key; ++it) {
      if (std::get<2>(it->first) != value ||
          !IsFeedbackContextCompatible(
              context_class, std::get<1>(it->first))) {
        continue;
      }
      MergeCounts(it->second, &effective);
    }

    ZenzFeedbackDecision decision = BuildDecisionFromCounts(effective);
    ApplyRejectDominanceDecision(effective, &decision);
    ApplyAutoBlockDecision(effective, auto_block_policy, &decision);

    ZenzFeedbackEntry entry;
    entry.key = key;
    entry.context_class = context_class;
    entry.value = value;
    entry.accepted_count = exact.accepted;
    entry.rejected_count = exact.rejected;
    entry.effective_accepted_count = effective.accepted;
    entry.effective_rejected_count = effective.rejected;
    entry.auto_block_reject_count = exact.auto_block_rejected;
    entry.hard_rejected = decision.hard_rejected;
    entry.auto_blocked = decision.auto_blocked;
    entry.reason = decision.reason;
    entries.push_back(std::move(entry));
  }

  std::sort(entries.begin(), entries.end(),
            [](const ZenzFeedbackEntry& a,
               const ZenzFeedbackEntry& b) {
              if (a.key != b.key) {
                return a.key < b.key;
              }
              if (a.value != b.value) {
                return a.value < b.value;
              }
              return a.context_class < b.context_class;
            });

  return entries;
}

bool ZenzFeedbackStore::ExportToFile(const std::wstring& path) const {
  std::lock_guard<std::mutex> mutation_lock(g_feedback_mutation_mutex);
  ScopedFeedbackInterprocessLock interprocess_lock;
  if (!interprocess_lock.ok()) {
    return false;
  }
#if defined(_WIN32)
  std::vector<ParsedFeedbackRecord> records;
  if (!LoadFeedbackRecordsFromDisk(&records)) {
    return false;
  }
  return WriteRecordsToPath(path, records);
#elif defined(__APPLE__) && TARGET_OS_OSX
  const std::string path_utf8 = WidePathToUtf8(path);
  if (path_utf8.empty()) {
    return false;
  }
  std::vector<ParsedFeedbackRecord> records;
  if (!LoadFeedbackRecordsFromDisk(&records)) {
    return false;
  }
  return WriteRecordsToPathUtf8(path_utf8, records);
#else
  StoreDebugOutput("export failed: zenz feedback store is unsupported");
  return false;
#endif
}

bool ZenzFeedbackStore::ImportFromFile(
    const std::wstring& path,
    ZenzFeedbackImportMode mode) {
#if defined(_WIN32)
  if (path.empty()) {
    return false;
  }

  std::ifstream file(path, std::ios::binary);
  if (!file) {
    StoreDebugOutputWide(
        std::wstring(L"import open failed ")
            .append(RedactedWidePathStats(L"path", path)));
    return false;
  }
#elif defined(__APPLE__) && TARGET_OS_OSX
  const std::string path_utf8 = WidePathToUtf8(path);
  if (path_utf8.empty()) {
    return false;
  }
  std::ifstream file(path_utf8, std::ios::binary);
  if (!file) {
    StoreDebugOutput("import open failed");
    return false;
  }
#else
  StoreDebugOutput("import failed: zenz feedback store is unsupported");
  return false;
#endif

#if defined(_WIN32) || (defined(__APPLE__) && TARGET_OS_OSX)
  std::vector<ParsedFeedbackRecord> imported_records;
  if (!LoadRecordsFromStream(&file, true, &imported_records)) {
#if defined(_WIN32)
    StoreDebugOutputWide(
        std::wstring(L"import parse failed ")
            .append(RedactedWidePathStats(L"path", path)));
#else
    StoreDebugOutput("import parse failed");
#endif
    return false;
  }
  // Do not keep the import source handle open while replacing the feedback
  // store. This also makes importing from the store's own path fail-safe on
  // Windows, where an open handle can otherwise block MoveFileExW.
  file.close();

  std::lock_guard<std::mutex> mutation_lock(g_feedback_mutation_mutex);
  ScopedFeedbackInterprocessLock interprocess_lock;
  if (!interprocess_lock.ok()) {
    return false;
  }

  std::vector<ParsedFeedbackRecord> new_records;
  if (mode == ZenzFeedbackImportMode::kAppend &&
      !LoadFeedbackRecordsFromDisk(&new_records)) {
    return false;
  }

  new_records.insert(new_records.end(),
                     imported_records.begin(),
                     imported_records.end());

  return WriteFeedbackRecordsAtomically(new_records);
#endif
}

bool ZenzFeedbackStore::DeleteEntry(absl::string_view key,
                                    absl::string_view context_class,
                                    absl::string_view value) {
  std::lock_guard<std::mutex> mutation_lock(g_feedback_mutation_mutex);
  ScopedFeedbackInterprocessLock interprocess_lock;
  if (!interprocess_lock.ok()) {
    return false;
  }

  std::vector<ParsedFeedbackRecord> records;
  if (!LoadFeedbackRecordsFromDisk(&records)) {
    return false;
  }

  const std::string normalized_key(key);
  const std::string normalized_context_class =
      NormalizeContextClass(context_class);
  const std::string normalized_value(value);

  const auto new_end =
      std::remove_if(records.begin(), records.end(),
                     [&](const ParsedFeedbackRecord& record) {
                       return record.kind ==
                                  ParsedFeedbackRecordKind::kFullSequence &&
                              record.key == normalized_key &&
                              record.context_class == normalized_context_class &&
                              record.value == normalized_value;
                     });

  if (new_end == records.end()) {
    return true;
  }

  records.erase(new_end, records.end());
  return WriteFeedbackRecordsAtomically(records);
}

bool ZenzFeedbackStore::DeleteLocalPreference(
    absl::string_view key, absl::string_view context_class,
    absl::string_view preferred_value, absl::string_view disfavored_value) {
  std::lock_guard<std::mutex> mutation_lock(g_feedback_mutation_mutex);
  ScopedFeedbackInterprocessLock interprocess_lock;
  if (!interprocess_lock.ok()) {
    return false;
  }

  std::vector<ParsedFeedbackRecord> records;
  if (!LoadFeedbackRecordsFromDisk(&records)) {
    return false;
  }

  const std::string normalized_key(key);
  const std::string normalized_context_class =
      NormalizeContextClass(context_class);
  const std::string normalized_preferred(preferred_value);
  const std::string normalized_disfavored(disfavored_value);

  std::vector<ParsedFeedbackRecord> kept;
  kept.reserve(records.size());
  bool changed = false;

  for (size_t i = 0; i < records.size(); ++i) {
    const ParsedFeedbackRecord& record = records[i];
    if (record.kind == ParsedFeedbackRecordKind::kLocalPreference &&
        record.key == normalized_key &&
        record.context_class == normalized_context_class &&
        record.value == normalized_preferred &&
        record.disfavored_value == normalized_disfavored) {
      changed = true;
      continue;
    }

    // Imported REV9 local_revert data is exposed as context "legacy" by the
    // aggregate view.  Delete the canonical accepted/rejected pair together.
    if (normalized_context_class == "legacy" &&
        i + 1 < records.size() &&
        record.kind == ParsedFeedbackRecordKind::kFullSequence &&
        record.action == "accepted" &&
        record.context_class == "local_revert" &&
        record.reason == "local_revert_preferred" &&
        record.key == normalized_key &&
        record.value == normalized_preferred) {
      const ParsedFeedbackRecord& rejected = records[i + 1];
      if (rejected.kind == ParsedFeedbackRecordKind::kFullSequence &&
          rejected.action == "rejected" &&
          rejected.context_class == "local_revert" &&
          rejected.reason == "local_revert" &&
          rejected.key == normalized_key &&
          rejected.value == normalized_disfavored) {
        changed = true;
        ++i;
        continue;
      }
    }

    kept.push_back(record);
  }

  if (!changed) {
    return true;
  }
  return WriteFeedbackRecordsAtomically(kept);
}

bool ZenzFeedbackStore::ClearAll() {
  std::lock_guard<std::mutex> mutation_lock(g_feedback_mutation_mutex);
  ScopedFeedbackInterprocessLock interprocess_lock;
  if (!interprocess_lock.ok()) {
    return false;
  }
  return WriteFeedbackRecordsAtomically({});
}

void ZenzFeedbackStore::RecordAccepted(
    absl::string_view key,
    absl::string_view context_class,
    absl::string_view value) {
  // The caller is responsible for passing the complete ordinary Zenz
  // reading/correction pair. Local evidence uses the v3 local-preference APIs only.
  AppendRecord("accepted", key, context_class, value, "");
}

void ZenzFeedbackStore::RecordRejected(
    absl::string_view key,
    absl::string_view context_class,
    absl::string_view value,
    absl::string_view reason) {
  // An ordinary rejected record is full-sequence scoped. Local evidence uses
  // the v3 local-preference APIs only.
  AppendRecord("rejected", key, context_class, value, reason);
}

void ZenzFeedbackStore::RecordLocalPreference(
    absl::string_view key, absl::string_view context_class,
    absl::string_view preferred_value, absl::string_view disfavored_value,
    absl::string_view reason) {
  ZenzLocalPreference preference;
  preference.key = std::string(key);
  preference.context_class = std::string(context_class);
  preference.preferred_value = std::string(preferred_value);
  preference.disfavored_value = std::string(disfavored_value);
  RecordLocalPreferences({preference}, reason);
}

void ZenzFeedbackStore::RecordLocalPreferences(
    const std::vector<ZenzLocalPreference>& preferences,
    absl::string_view reason) {
  std::vector<ParsedFeedbackRecord> records;
  records.reserve(preferences.size());
  for (const ZenzLocalPreference& preference : preferences) {
    if (preference.key.empty() || preference.preferred_value.empty() ||
        preference.disfavored_value.empty() ||
        preference.preferred_value == preference.disfavored_value) {
      continue;
    }

    ParsedFeedbackRecord record;
    record.kind = ParsedFeedbackRecordKind::kLocalPreference;
    record.key = preference.key;
    record.context_class = NormalizeContextClass(preference.context_class);
    record.value = preference.preferred_value;
    record.disfavored_value = preference.disfavored_value;
    record.reason = std::string(reason);
    records.push_back(std::move(record));
  }
  AppendRecords(records);
}


}  // namespace session
}  // namespace mozc
