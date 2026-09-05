#include "session/zenz_feedback_store.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
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
constexpr int kAcceptedFeedbackWeight = 1000;
constexpr int kSpaceRevertRejectWeight = 150;
constexpr int kPredictAfterZenzRejectWeight = 200;
constexpr int kExplicitConversionRejectWeight = 400;
constexpr int kLegacyRejectWeight = 400;
constexpr int kHardRejectWeight = 2000;

constexpr size_t kMinMaintenanceEntries = 100;
constexpr size_t kMaxMaintenanceEntries = 20000;
constexpr int kMinLocalThreshold = 1;
constexpr int kMaxLocalThreshold = 255;
constexpr int kMaxLocalEvidenceCount = 255;

using FeedbackKey = std::tuple<std::string, std::string, std::string>;
using LocalKey =
    std::tuple<std::string, std::string, std::string>;

enum class RecordKind {
  kFull,
  kLocal,
};

struct Record {
  RecordKind kind = RecordKind::kFull;
  std::string action;
  std::string key;
  std::string context_class;
  // Full: complete Zenz value.
  // Local: raw Zenz surface.
  std::string value;
  // Full: reject reason.
  // Local: corrected surface.
  std::string extra;
  int count = 1;
  size_t sequence = 0;
};

struct Counts {
  int accepted = 0;
  int rejected = 0;
  int auto_block_rejected = 0;
  int positive_score = 0;
  int negative_score = 0;
  bool hard_rejected = false;
};

struct LocalEvent {
  std::string context_class;
  std::string raw_zenz_surface;
  std::string corrected_surface;
  bool accepted = true;
  int count = 1;
  size_t sequence = 0;
};

using FeedbackCounts = std::map<FeedbackKey, Counts>;
struct LocalCounts {
  int count = 0;
  size_t last_sequence = 0;
};

using LocalDirections =
    std::map<std::pair<std::string, std::string>, LocalCounts>;

struct FeedbackData {
  FeedbackCounts counts;
  std::map<std::string, LocalDirections> local_counts_by_reading;
};

std::mutex g_feedback_data_cache_mutex;
std::mutex g_feedback_mutation_mutex;
std::mutex g_lifecycle_maintenance_mutex;
bool g_lifecycle_maintenance_done = false;
std::filesystem::path g_lifecycle_maintenance_path;
size_t g_lifecycle_maintenance_max_entries = 0;
uintmax_t g_lifecycle_maintenance_file_size = 0;

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
        out.push_back('\\');
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

std::vector<std::string> SplitTab(const std::string& line) {
  std::vector<std::string> fields;
  std::string current;
  for (const char c : line) {
    if (c == '\t') {
      fields.push_back(std::move(current));
      current.clear();
    } else {
      current.push_back(c);
    }
  }
  fields.push_back(std::move(current));
  return fields;
}

void StripUtf8Bom(std::vector<std::string>* fields) {
  if (fields == nullptr || fields->empty()) {
    return;
  }
  constexpr absl::string_view kBom = "\xEF\xBB\xBF";
  if (absl::StartsWith((*fields)[0], kBom)) {
    (*fields)[0].erase(0, kBom.size());
  }
}

bool ParsePositiveCount(absl::string_view text, int* value) {
  if (value == nullptr || text.empty()) {
    return false;
  }
  int parsed = 0;
  const char* begin = text.data();
  const char* end = text.data() + text.size();
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc() || result.ptr != end || parsed <= 0) {
    return false;
  }
  *value = parsed;
  return true;
}

bool IsKnownContextClass(absl::string_view context_class) {
  return context_class == "empty" ||
         context_class == "japanese_only" ||
         context_class == "japanese_with_punctuation" ||
         context_class == "mixed_japanese_ascii" ||
         context_class == "symbol_or_other" ||
         context_class == "ascii_or_digit" ||
         context_class == "sensitive_like";
}

std::string NormalizeContextClass(absl::string_view context_class) {
  return context_class.empty() ? std::string("empty")
                               : std::string(context_class);
}

bool ContainsUnsafePersistedChar(absl::string_view s) {
  for (const unsigned char c : s) {
    if (c == 0 || c < 0x20 || c == 0x7f) {
      return true;
    }
  }
  return false;
}

bool IsSafeRecord(const Record& record) {
  if ((record.action != "accepted" && record.action != "rejected") ||
      record.key.empty() || record.value.empty() || record.count <= 0 ||
      !IsKnownContextClass(record.context_class)) {
    return false;
  }

  if (record.kind == RecordKind::kLocal) {
    if (record.extra.empty() || record.value == record.extra) {
      return false;
    }
  }

  constexpr size_t kMaxKeyBytes = 512;
  constexpr size_t kMaxContextClassBytes = 64;
  constexpr size_t kMaxValueBytes = 512;
  constexpr size_t kMaxReasonBytes = 128;
  if (record.key.size() > kMaxKeyBytes ||
      record.context_class.size() > kMaxContextClassBytes ||
      record.value.size() > kMaxValueBytes ||
      record.extra.size() >
          (record.kind == RecordKind::kLocal ? kMaxValueBytes
                                             : kMaxReasonBytes)) {
    return false;
  }

  if (ContainsUnsafePersistedChar(record.key) ||
      ContainsUnsafePersistedChar(record.context_class) ||
      ContainsUnsafePersistedChar(record.value) ||
      ContainsUnsafePersistedChar(record.extra)) {
    return false;
  }

  return Util::IsValidUtf8(record.key) &&
         Util::IsValidUtf8(record.context_class) &&
         Util::IsValidUtf8(record.value) &&
         Util::IsValidUtf8(record.extra);
}

bool ParseRecord(const std::vector<std::string>& fields, Record* record) {
  if (record == nullptr || fields.size() != 8 || fields[0] != "v4") {
    return false;
  }

  Record parsed;
  if (fields[1] == "full") {
    parsed.kind = RecordKind::kFull;
  } else if (fields[1] == "local") {
    parsed.kind = RecordKind::kLocal;
  } else {
    return false;
  }

  parsed.action = UnescapeTsv(fields[2]);
  parsed.key = UnescapeTsv(fields[3]);
  parsed.context_class =
      NormalizeContextClass(UnescapeTsv(fields[4]));
  parsed.value = UnescapeTsv(fields[5]);
  parsed.extra = UnescapeTsv(fields[6]);
  if (!ParsePositiveCount(fields[7], &parsed.count)) {
    return false;
  }
  if (!IsSafeRecord(parsed)) {
    return false;
  }

  *record = std::move(parsed);
  return true;
}

void WriteRecordToStream(const Record& record, std::ostream* output) {
  *output << "v4" << '\t'
          << (record.kind == RecordKind::kFull ? "full" : "local") << '\t'
          << EscapeTsv(record.action) << '\t'
          << EscapeTsv(record.key) << '\t'
          << EscapeTsv(NormalizeContextClass(record.context_class)) << '\t'
          << EscapeTsv(record.value) << '\t'
          << EscapeTsv(record.extra) << '\t'
          << record.count << '\n';
}

bool WriteRecordsToStream(const std::vector<Record>& records,
                          std::ostream* output) {
  if (output == nullptr) {
    return false;
  }
  for (const Record& record : records) {
    if (!IsSafeRecord(record)) {
      return false;
    }
    WriteRecordToStream(record, output);
  }
  output->flush();
  return static_cast<bool>(*output);
}

bool LoadRecordsFromStream(std::istream* input, bool strict,
                           std::vector<Record>* records) {
  if (input == nullptr || records == nullptr) {
    return false;
  }
  records->clear();
  bool all_valid = true;
  std::string line;
  size_t sequence = 0;
  while (std::getline(*input, line)) {
    if (line.empty()) {
      continue;
    }
    std::vector<std::string> fields = SplitTab(line);
    StripUtf8Bom(&fields);
    Record record;
    if (!ParseRecord(fields, &record)) {
      all_valid = false;
      if (strict) {
        return false;
      }
      continue;
    }
    record.sequence = sequence++;
    records->push_back(std::move(record));
  }
  return all_valid || !strict;
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
  return kLegacyRejectWeight;
}

int SaturatingAddNonNegative(int lhs, int rhs) {
  if (rhs <= 0) {
    return lhs;
  }
  if (lhs >= std::numeric_limits<int>::max() - rhs) {
    return std::numeric_limits<int>::max();
  }
  return lhs + rhs;
}

int SaturatingWeightedAdd(int current, int count, int weight) {
  if (count <= 0 || weight <= 0) {
    return current;
  }
  const int64_t value = static_cast<int64_t>(current) +
                        static_cast<int64_t>(count) * weight;
  return static_cast<int>(std::min<int64_t>(
      value, std::numeric_limits<int>::max()));
}

void AddAccepted(int count, Counts* c) {
  c->accepted = SaturatingAddNonNegative(c->accepted, count);
  c->positive_score = SaturatingWeightedAdd(
      c->positive_score, count, kAcceptedFeedbackWeight);
}

void AddRejected(absl::string_view reason, int count, Counts* c) {
  c->rejected = SaturatingAddNonNegative(c->rejected, count);
  c->negative_score = SaturatingWeightedAdd(
      c->negative_score, count, RejectWeightForReason(reason));
  if (!IsHardRejectReason(reason)) {
    c->auto_block_rejected =
        SaturatingAddNonNegative(c->auto_block_rejected, count);
  }
  if (count > 0 && IsHardRejectReason(reason)) {
    c->hard_rejected = true;
  }
}

void MergeCounts(const Counts& src, Counts* dest) {
  dest->accepted = SaturatingAddNonNegative(dest->accepted, src.accepted);
  dest->rejected = SaturatingAddNonNegative(dest->rejected, src.rejected);
  dest->auto_block_rejected = SaturatingAddNonNegative(
      dest->auto_block_rejected, src.auto_block_rejected);
  dest->positive_score = SaturatingAddNonNegative(
      dest->positive_score, src.positive_score);
  dest->negative_score = SaturatingAddNonNegative(
      dest->negative_score, src.negative_score);
  dest->hard_rejected |= src.hard_rejected;
}

int TotalScore(const Counts& counts) {
  const int64_t score = static_cast<int64_t>(counts.positive_score) -
                        static_cast<int64_t>(counts.negative_score);
  return static_cast<int>(std::clamp<int64_t>(
      score, std::numeric_limits<int>::min(),
      std::numeric_limits<int>::max()));
}

bool IsSensitiveFeedbackContextClass(absl::string_view context_class) {
  return context_class == "sensitive_like";
}

bool IsSharedFeedbackContextClass(absl::string_view context_class) {
  return context_class == "empty" ||
         context_class == "japanese_only" ||
         context_class == "japanese_with_punctuation" ||
         context_class == "mixed_japanese_ascii" ||
         context_class == "symbol_or_other" ||
         context_class == "ascii_or_digit";
}

bool IsFeedbackContextCompatible(absl::string_view requested_context_class,
                                 absl::string_view record_context_class) {
  if (requested_context_class == record_context_class) {
    return true;
  }
  if (IsSensitiveFeedbackContextClass(requested_context_class) ||
      IsSensitiveFeedbackContextClass(record_context_class)) {
    return false;
  }
  return IsSharedFeedbackContextClass(requested_context_class) &&
         IsSharedFeedbackContextClass(record_context_class);
}

ZenzFeedbackAutoBlockPolicy NormalizeAutoBlockPolicy(
    ZenzFeedbackAutoBlockPolicy policy) {
  if (!policy.enabled || policy.reject_threshold <= 0) {
    return ZenzFeedbackAutoBlockPolicy();
  }
  return policy;
}

bool IsAutoBlockedByPolicy(
    const Counts& counts,
    const ZenzFeedbackAutoBlockPolicy& policy) {
  const ZenzFeedbackAutoBlockPolicy normalized =
      NormalizeAutoBlockPolicy(policy);
  return normalized.enabled &&
         counts.auto_block_rejected >= normalized.reject_threshold;
}

bool IsRejectCountDominant(const Counts& counts) {
  return counts.auto_block_rejected > counts.accepted;
}

ZenzFeedbackDecision BuildDecisionFromCounts(const Counts& counts) {
  ZenzFeedbackDecision decision;
  decision.accepted_count = counts.accepted;
  decision.rejected_count = counts.rejected;
  decision.auto_block_reject_count = counts.auto_block_rejected;
  decision.positive_score = counts.positive_score;
  decision.negative_score = counts.negative_score;
  decision.total_score = TotalScore(counts);
  decision.hard_rejected = counts.hard_rejected;

  if (counts.hard_rejected) {
    decision.action = ZenzFeedbackAction::kReject;
    decision.reason = "feedback_hard_rejected";
    return decision;
  }
  if (counts.accepted >= kAcceptThreshold && decision.total_score > 0) {
    decision.action = ZenzFeedbackAction::kPrefer;
    decision.reason = "feedback_preferred";
    return decision;
  }
  decision.action = ZenzFeedbackAction::kNeutral;
  decision.reason =
      counts.rejected > 0 ? "feedback_downgraded" : "feedback_neutral";
  return decision;
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
    const ZenzFeedbackAutoBlockPolicy& policy,
    ZenzFeedbackDecision* decision) {
  if (decision == nullptr || decision->hard_rejected) {
    return;
  }
  decision->auto_block_reject_count = counts.auto_block_rejected;
  if (!IsAutoBlockedByPolicy(counts, policy)) {
    return;
  }
  decision->action = ZenzFeedbackAction::kReject;
  decision->reason = "feedback_auto_blocked";
  decision->auto_blocked = true;
}

struct CanonicalLocalIdentity {
  std::string key;
  std::string raw_zenz_surface;
  std::string corrected_surface;
};

std::vector<absl::string_view> LocalUtf8Chars(absl::string_view text) {
  std::vector<absl::string_view> chars;
  for (size_t i = 0; i < text.size();) {
    const unsigned char lead = static_cast<unsigned char>(text[i]);
    size_t n = 1;
    if ((lead & 0x80) == 0) {
      n = 1;
    } else if ((lead & 0xe0) == 0xc0) {
      n = 2;
    } else if ((lead & 0xf0) == 0xe0) {
      n = 3;
    } else if ((lead & 0xf8) == 0xf0) {
      n = 4;
    } else {
      return {};
    }
    if (i + n > text.size()) {
      return {};
    }
    chars.push_back(text.substr(i, n));
    i += n;
  }
  return chars;
}

std::string JoinLocalUtf8Chars(const std::vector<absl::string_view>& chars,
                               size_t begin, size_t end) {
  std::string out;
  for (size_t i = begin; i < end; ++i) {
    out.append(chars[i]);
  }
  return out;
}

CanonicalLocalIdentity CanonicalizeLocalIdentity(
    absl::string_view key, absl::string_view raw_zenz_surface,
    absl::string_view corrected_surface) {
  CanonicalLocalIdentity original{std::string(key),
                                  std::string(raw_zenz_surface),
                                  std::string(corrected_surface)};
  if (key.empty() || raw_zenz_surface.empty() || corrected_surface.empty() ||
      raw_zenz_surface == corrected_surface || !Util::IsValidUtf8(key) ||
      !Util::IsValidUtf8(raw_zenz_surface) ||
      !Util::IsValidUtf8(corrected_surface)) {
    return original;
  }

  const std::vector<absl::string_view> key_chars = LocalUtf8Chars(key);
  const std::vector<absl::string_view> raw_chars =
      LocalUtf8Chars(raw_zenz_surface);
  const std::vector<absl::string_view> corrected_chars =
      LocalUtf8Chars(corrected_surface);
  if (key_chars.empty() || raw_chars.empty() || corrected_chars.empty()) {
    return original;
  }

  // Strip only outer characters that are literally identical in the reading,
  // raw Zenz surface and corrected surface. This is safe without a converter
  // and lets existing rows such as
  //   りせきします / 離籍します -> 離席します
  // migrate in-memory to the same minimal りせき / 離籍 -> 離席 rule.
  size_t prefix = 0;
  while (prefix < key_chars.size() && prefix < raw_chars.size() &&
         prefix < corrected_chars.size() &&
         key_chars[prefix] == raw_chars[prefix] &&
         key_chars[prefix] == corrected_chars[prefix]) {
    ++prefix;
  }

  size_t suffix = 0;
  while (suffix + prefix < key_chars.size() &&
         suffix + prefix < raw_chars.size() &&
         suffix + prefix < corrected_chars.size() &&
         key_chars[key_chars.size() - 1 - suffix] ==
             raw_chars[raw_chars.size() - 1 - suffix] &&
         key_chars[key_chars.size() - 1 - suffix] ==
             corrected_chars[corrected_chars.size() - 1 - suffix]) {
    ++suffix;
  }

  constexpr size_t kMinCanonicalLocalKeyChars = 2;
  const size_t key_remaining = key_chars.size() - prefix - suffix;
  const size_t raw_remaining = raw_chars.size() - prefix - suffix;
  const size_t corrected_remaining = corrected_chars.size() - prefix - suffix;
  if (key_remaining < kMinCanonicalLocalKeyChars || raw_remaining == 0 ||
      corrected_remaining == 0 || (prefix == 0 && suffix == 0)) {
    return original;
  }

  CanonicalLocalIdentity canonical;
  canonical.key = JoinLocalUtf8Chars(
      key_chars, prefix, key_chars.size() - suffix);
  canonical.raw_zenz_surface = JoinLocalUtf8Chars(
      raw_chars, prefix, raw_chars.size() - suffix);
  canonical.corrected_surface = JoinLocalUtf8Chars(
      corrected_chars, prefix, corrected_chars.size() - suffix);
  if (canonical.raw_zenz_surface.empty() || canonical.corrected_surface.empty() ||
      canonical.raw_zenz_surface == canonical.corrected_surface) {
    return original;
  }
  return canonical;
}

std::shared_ptr<const FeedbackData> BuildFeedbackData(
    const std::vector<Record>& records) {
  auto data = std::make_shared<FeedbackData>();
  for (const Record& record : records) {
    if (record.kind == RecordKind::kFull) {
      Counts& counts =
          data->counts[FeedbackKey(record.key, record.context_class,
                                   record.value)];
      if (record.action == "accepted") {
        AddAccepted(record.count, &counts);
      } else {
        AddRejected(record.extra, record.count, &counts);
      }
      continue;
    }

    const CanonicalLocalIdentity local = CanonicalizeLocalIdentity(
        record.key, record.value, record.extra);
    // Replay once in file order when loading the immutable snapshot. Applying
    // the cap/floor per event is essential: a net accepted-minus-rejected sum
    // would change learning after saturation or rejection at zero.
    LocalCounts& counts = data->local_counts_by_reading[local.key]
        [{local.raw_zenz_surface, local.corrected_surface}];
    counts.last_sequence = record.sequence;
    if (record.action == "accepted") {
      const int remaining = kMaxLocalEvidenceCount - counts.count;
      counts.count = record.count >= remaining
                         ? kMaxLocalEvidenceCount
                         : counts.count + record.count;
    } else {
      counts.count = record.count >= counts.count
                         ? 0
                         : counts.count - record.count;
    }
  }
  return data;
}

std::filesystem::path FeedbackDirectory() {
#if defined(_WIN32)
  // Read the process environment through Win32 rather than the CRT copy. Mozc
  // tests and some host processes update USERPROFILE with SetEnvironmentVariableW,
  // which is not guaranteed to refresh the CRT environment block.
  const DWORD required =
      ::GetEnvironmentVariableW(L"USERPROFILE", nullptr, 0);
  if (required <= 1) {
    return {};
  }
  std::wstring profile(required, L'\0');
  const DWORD copied = ::GetEnvironmentVariableW(
      L"USERPROFILE", profile.data(), required);
  if (copied == 0 || copied >= required) {
    return {};
  }
  profile.resize(copied);
  return std::filesystem::path(profile) / L"AppData" / L"LocalLow" / L"Mozc";
#elif defined(__APPLE__) && TARGET_OS_OSX
  const char* home = std::getenv("HOME");
  if (home == nullptr || *home == '\0') {
    return {};
  }
  return std::filesystem::path(home) / ".mozc";
#else
  return {};
#endif
}

std::filesystem::path FeedbackPathForRead() {
  const std::filesystem::path dir = FeedbackDirectory();
  if (dir.empty()) {
    return {};
  }
  return dir / "zenz_feedback_v4.tsv";
}

std::filesystem::path FeedbackPathForWrite() {
  const std::filesystem::path dir = FeedbackDirectory();
  if (dir.empty()) {
    return {};
  }
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    return {};
  }
  return dir / "zenz_feedback_v4.tsv";
}

bool EnsurePrivateInternalFeedbackFile(
    const std::filesystem::path& path) {
#if defined(__APPLE__) && TARGET_OS_OSX
  if (path.empty()) {
    return false;
  }
  return ::chmod(path.c_str(), S_IRUSR | S_IWUSR) == 0;
#else
  (void)path;
  return true;
#endif
}

bool PreparePrivateInternalFeedbackFileForWrite(
    const std::filesystem::path& path) {
#if defined(__APPLE__) && TARGET_OS_OSX
  if (path.empty()) {
    return false;
  }

  // Secure internal atomic-temp storage before any feedback content is
  // written. fchmod also corrects a stale temp file left by an interrupted
  // operation.
  const int fd =
      ::open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
  if (fd < 0) {
    return false;
  }
  const bool ok = ::fchmod(fd, S_IRUSR | S_IWUSR) == 0;
  ::close(fd);
  return ok;
#else
  (void)path;
  return true;
#endif
}

bool NeedsAppendBoundaryNewline(const std::filesystem::path& path,
                                bool* needs_newline) {
  if (needs_newline == nullptr) {
    return false;
  }
  *needs_newline = false;
  if (path.empty()) {
    return false;
  }

  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    return !ec;
  }
  const uintmax_t size = std::filesystem::file_size(path, ec);
  if (ec) {
    return false;
  }
  if (size == 0) {
    return true;
  }

  std::ifstream tail(path, std::ios::binary);
  if (!tail) {
    return false;
  }
  tail.seekg(-1, std::ios::end);
  char last = '\0';
  tail.get(last);
  if (!tail) {
    return false;
  }
  *needs_newline = last != '\n';
  return true;
}

class ScopedFeedbackInterprocessLock {
 public:
  explicit ScopedFeedbackInterprocessLock(
      std::chrono::milliseconds timeout = std::chrono::milliseconds(1000)) {
    (void)timeout;
#if defined(_WIN32)
    handle_ =
        ::CreateMutexW(nullptr, FALSE, L"Local\\MozcZenzFeedbackStoreMutationV4");
    if (handle_ == nullptr) {
      return;
    }
    const int64_t timeout_msec = std::clamp<int64_t>(
        timeout.count(), 0, std::numeric_limits<DWORD>::max());
    const DWORD wait = ::WaitForSingleObject(
        handle_, static_cast<DWORD>(timeout_msec));
    locked_ = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
#elif defined(__APPLE__) && TARGET_OS_OSX
    const std::filesystem::path path = FeedbackPathForWrite();
    if (path.empty()) {
      return;
    }
    const std::string lock_path = path.string() + ".lock";
    fd_ = ::open(lock_path.c_str(), O_CREAT | O_RDWR, 0600);
    if (fd_ < 0) {
      return;
    }
    const int flags = ::fcntl(fd_, F_GETFD, 0);
    if (flags >= 0) {
      (void)::fcntl(fd_, F_SETFD, flags | FD_CLOEXEC);
    }
    const int64_t timeout_msec = std::max<int64_t>(0, timeout.count());
    const int64_t rounded_attempts =
        timeout_msec == 0 ? 1 : (timeout_msec - 1) / 10 + 1;
    const int attempts = static_cast<int>(std::min<int64_t>(
        rounded_attempts, std::numeric_limits<int>::max()));
    for (int attempt = 0; attempt < attempts; ++attempt) {
      if (::flock(fd_, LOCK_EX | LOCK_NB) == 0) {
        locked_ = true;
        return;
      }
      if (errno != EWOULDBLOCK && errno != EAGAIN) {
        return;
      }
      if (attempt + 1 < attempts) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    }
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

  bool ok() const { return locked_; }

 private:
  bool locked_ = false;
#if defined(_WIN32)
  HANDLE handle_ = nullptr;
#elif defined(__APPLE__) && TARGET_OS_OSX
  int fd_ = -1;
#endif
};

struct FileStamp {
  bool exists = false;
  uintmax_t size = 0;
  std::filesystem::file_time_type write_time = {};
};

FileStamp GetFileStamp(const std::filesystem::path& path) {
  FileStamp stamp;
  if (path.empty()) {
    return stamp;
  }
  std::error_code ec;
  stamp.exists = std::filesystem::exists(path, ec);
  if (ec || !stamp.exists) {
    return FileStamp();
  }
  stamp.size = std::filesystem::file_size(path, ec);
  if (ec) {
    return FileStamp();
  }
  stamp.write_time = std::filesystem::last_write_time(path, ec);
  if (ec) {
    return FileStamp();
  }
  return stamp;
}

[[maybe_unused]] bool SameStamp(const FileStamp& lhs, const FileStamp& rhs) {
  return lhs.exists == rhs.exists &&
         lhs.size == rhs.size &&
         lhs.write_time == rhs.write_time;
}

struct FeedbackDataCache {
  bool valid = false;
  std::filesystem::path path;
  FileStamp stamp;
  std::chrono::steady_clock::time_point next_stamp_check;
  std::shared_ptr<const FeedbackData> data;
};

FeedbackDataCache g_feedback_data_cache;

void InvalidateCache() {
  std::lock_guard<std::mutex> lock(g_feedback_data_cache_mutex);
  g_feedback_data_cache = FeedbackDataCache();
}

bool LoadRecordsFromDisk(std::vector<Record>* records) {
  if (records == nullptr) {
    return false;
  }
  records->clear();
  const std::filesystem::path path = FeedbackPathForRead();
  if (path.empty()) {
    return true;
  }
  const FileStamp stamp = GetFileStamp(path);
  if (!stamp.exists) {
    return true;
  }
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  return LoadRecordsFromStream(&file, false, records);
}

std::shared_ptr<const FeedbackData> LoadFeedbackData() {
#if defined(_WIN32) || (defined(__APPLE__) && TARGET_OS_OSX)
  constexpr auto kStampCheckInterval = std::chrono::seconds(1);
  const std::filesystem::path path = FeedbackPathForRead();
  const auto now = std::chrono::steady_clock::now();

  std::lock_guard<std::mutex> lock(g_feedback_data_cache_mutex);
  if (g_feedback_data_cache.valid &&
      g_feedback_data_cache.path == path &&
      g_feedback_data_cache.data != nullptr &&
      now < g_feedback_data_cache.next_stamp_check) {
    return g_feedback_data_cache.data;
  }

  const FileStamp stamp = GetFileStamp(path);
  if (g_feedback_data_cache.valid &&
      g_feedback_data_cache.path == path &&
      g_feedback_data_cache.data != nullptr &&
      SameStamp(g_feedback_data_cache.stamp, stamp)) {
    g_feedback_data_cache.next_stamp_check = now + kStampCheckInterval;
    return g_feedback_data_cache.data;
  }

  std::vector<Record> records;
  if (stamp.exists) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
      if (g_feedback_data_cache.valid &&
          g_feedback_data_cache.path == path &&
          g_feedback_data_cache.data != nullptr) {
        g_feedback_data_cache.next_stamp_check = now + kStampCheckInterval;
        return g_feedback_data_cache.data;
      }
      records.clear();
    } else {
      (void)LoadRecordsFromStream(&file, false, &records);
    }
  }

  g_feedback_data_cache.valid = true;
  g_feedback_data_cache.path = path;
  g_feedback_data_cache.stamp = stamp;
  g_feedback_data_cache.next_stamp_check = now + kStampCheckInterval;
  g_feedback_data_cache.data = BuildFeedbackData(records);
  return g_feedback_data_cache.data;
#else
  static const std::shared_ptr<const FeedbackData> empty =
      BuildFeedbackData({});
  return empty;
#endif
}

[[maybe_unused]] bool WriteRecordsToPath(const std::filesystem::path& path,
                        const std::vector<Record>& records) {
  if (path.empty()) {
    return false;
  }
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  return file && WriteRecordsToStream(records, &file);
}

bool WriteRecordsAtomically(const std::vector<Record>& records) {
#if defined(_WIN32) || (defined(__APPLE__) && TARGET_OS_OSX)
  const std::filesystem::path path = FeedbackPathForWrite();
  if (path.empty()) {
    return false;
  }

  if (records.empty()) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    if (ec) {
      return false;
    }
    InvalidateCache();
    return true;
  }

#if defined(_WIN32)
  const std::filesystem::path tmp =
      path.wstring() + L".tmp." + std::to_wstring(::GetCurrentProcessId());
#else
  const std::filesystem::path tmp =
      path.string() + ".tmp." + std::to_string(::getpid());
#endif

  if (!PreparePrivateInternalFeedbackFileForWrite(tmp) ||
      !WriteRecordsToPath(tmp, records) ||
      !EnsurePrivateInternalFeedbackFile(tmp)) {
    std::error_code ignored;
    std::filesystem::remove(tmp, ignored);
    return false;
  }

#if defined(_WIN32)
  if (!::MoveFileExW(tmp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    std::error_code ignored;
    std::filesystem::remove(tmp, ignored);
    return false;
  }
#else
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    std::error_code ignored;
    std::filesystem::remove(tmp, ignored);
    return false;
  }
#endif

  InvalidateCache();
  return true;
#else
  (void)records;
  return false;
#endif
}

void AppendRecords(const std::vector<Record>& records) {
  if (records.empty()) {
    return;
  }
  for (const Record& record : records) {
    if (!IsSafeRecord(record)) {
      return;
    }
  }

  std::ostringstream payload;
  for (const Record& record : records) {
    WriteRecordToStream(record, &payload);
  }
  const std::string text = payload.str();
  if (text.empty()) {
    return;
  }

  std::unique_lock<std::mutex> mutation_lock(
      g_feedback_mutation_mutex, std::try_to_lock);
  if (!mutation_lock.owns_lock()) {
    return;
  }
  // Feedback recording is on an IME hot path. If another process is running a
  // maintenance/import operation, skip this observation rather than stalling
  // user input for the long management-operation lock timeout.
  ScopedFeedbackInterprocessLock interprocess_lock(
      std::chrono::milliseconds(20));
  if (!interprocess_lock.ok()) {
    return;
  }

  const std::filesystem::path path = FeedbackPathForWrite();
  if (path.empty()) {
    return;
  }

  bool needs_boundary_newline = false;
  if (!NeedsAppendBoundaryNewline(path, &needs_boundary_newline)) {
    return;
  }

  std::ofstream file(path, std::ios::binary | std::ios::app);
  if (!file) {
    return;
  }
  if (!EnsurePrivateInternalFeedbackFile(path)) {
    file.close();
    return;
  }
  if (needs_boundary_newline) {
    file.put('\n');
  }
  file.write(text.data(), static_cast<std::streamsize>(text.size()));
  file.flush();
  if (!file) {
    return;
  }
  InvalidateCache();
}

[[maybe_unused]] std::filesystem::path WidePathToFilesystemPath(const std::wstring& path) {
#if defined(_WIN32)
  return std::filesystem::path(path);
#else
  std::string utf8;
  utf8.reserve(path.size());
  for (const wchar_t wc : path) {
    const uint32_t c = static_cast<uint32_t>(wc);
    if (c <= 0x7f) {
      utf8.push_back(static_cast<char>(c));
    } else if (c <= 0x7ff) {
      utf8.push_back(static_cast<char>(0xc0 | (c >> 6)));
      utf8.push_back(static_cast<char>(0x80 | (c & 0x3f)));
    } else if (c <= 0xffff) {
      if (c >= 0xd800 && c <= 0xdfff) {
        return {};
      }
      utf8.push_back(static_cast<char>(0xe0 | (c >> 12)));
      utf8.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3f)));
      utf8.push_back(static_cast<char>(0x80 | (c & 0x3f)));
    } else if (c <= 0x10ffff) {
      utf8.push_back(static_cast<char>(0xf0 | (c >> 18)));
      utf8.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3f)));
      utf8.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3f)));
      utf8.push_back(static_cast<char>(0x80 | (c & 0x3f)));
    } else {
      return {};
    }
  }
  return std::filesystem::path(utf8);
#endif
}

struct FullCompactAggregate {
  std::string key;
  std::string context_class;
  std::string value;
  int accepted_count = 0;
  std::map<std::string, int> rejected_by_reason;
  Counts counts;
  size_t last_sequence = 0;
};

struct LocalCompactAggregate {
  std::string key;
  std::string context_class;
  std::string raw_zenz_surface;
  std::string corrected_surface;
  int count = 0;
  size_t last_sequence = 0;
};

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
  const std::shared_ptr<const FeedbackData> data = LoadFeedbackData();
  const std::string normalized_key(key);
  const std::string normalized_context =
      NormalizeContextClass(context_class);
  const std::string normalized_value(value);
  Counts aggregated;

  for (auto it =
           data->counts.lower_bound(FeedbackKey(normalized_key, "", ""));
       it != data->counts.end() &&
       std::get<0>(it->first) == normalized_key; ++it) {
    if (std::get<2>(it->first) != normalized_value ||
        !IsFeedbackContextCompatible(normalized_context,
                                     std::get<1>(it->first))) {
      continue;
    }
    MergeCounts(it->second, &aggregated);
  }

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
  const std::shared_ptr<const FeedbackData> data = LoadFeedbackData();
  const std::string normalized_key(key);
  const std::string normalized_context =
      NormalizeContextClass(context_class);
  std::map<std::string, Counts> by_value;

  for (auto it =
           data->counts.lower_bound(FeedbackKey(normalized_key, "", ""));
       it != data->counts.end() &&
       std::get<0>(it->first) == normalized_key; ++it) {
    if (!IsFeedbackContextCompatible(normalized_context,
                                     std::get<1>(it->first))) {
      continue;
    }
    MergeCounts(it->second, &by_value[std::get<2>(it->first)]);
  }

  std::vector<ZenzFeedbackCandidate> result;
  for (const auto& [value, counts] : by_value) {
    if (counts.hard_rejected ||
        IsAutoBlockedByPolicy(counts, auto_block_policy) ||
        IsRejectCountDominant(counts) ||
        counts.accepted < kAcceptThreshold ||
        TotalScore(counts) <= 0) {
      continue;
    }
    ZenzFeedbackCandidate candidate;
    candidate.value = value;
    candidate.accepted_count = counts.accepted;
    candidate.rejected_count = counts.rejected;
    candidate.positive_score = counts.positive_score;
    candidate.negative_score = counts.negative_score;
    candidate.total_score = TotalScore(counts);
    candidate.auto_block_reject_count = counts.auto_block_rejected;
    candidate.hard_rejected = counts.hard_rejected;
    candidate.auto_blocked = false;
    candidate.reason = "feedback_preferred";
    result.push_back(std::move(candidate));
  }

  std::sort(result.begin(), result.end(),
            [](const ZenzFeedbackCandidate& lhs,
               const ZenzFeedbackCandidate& rhs) {
              if (lhs.total_score != rhs.total_score) {
                return lhs.total_score > rhs.total_score;
              }
              if (lhs.positive_score != rhs.positive_score) {
                return lhs.positive_score > rhs.positive_score;
              }
              if (lhs.negative_score != rhs.negative_score) {
                return lhs.negative_score < rhs.negative_score;
              }
              return lhs.value < rhs.value;
            });
  return result;
}

std::vector<ZenzFeedbackCandidate> ZenzFeedbackStore::GetAcceptedCandidates(
    absl::string_view key,
    absl::string_view context_class) const {
  return GetAcceptedCandidates(key, context_class,
                               ZenzFeedbackAutoBlockPolicy());
}

std::vector<ZenzFeedbackCandidate> ZenzFeedbackStore::GetAcceptedCandidates(
    absl::string_view key,
    absl::string_view context_class,
    const ZenzFeedbackAutoBlockPolicy& auto_block_policy) const {
  return GetRankedCandidates(key, context_class, auto_block_policy);
}

std::vector<ZenzLocalPreference> ZenzFeedbackStore::GetLocalPreferences(
    absl::string_view full_key, absl::string_view context_class,
    size_t max_results, int min_observation_count,
    absl::string_view raw_surface_filter,
    absl::string_view preferred_surface_filter) const {
  std::vector<ZenzLocalPreference> result;
  if (full_key.empty() || max_results == 0) {
    return result;
  }
  const int threshold =
      std::clamp(min_observation_count, kMinLocalThreshold, kMaxLocalThreshold);
  const std::string current_context = NormalizeContextClass(context_class);
  const std::shared_ptr<const FeedbackData> data = LoadFeedbackData();

  struct Candidate {
    absl::string_view key;
    absl::string_view raw;
    absl::string_view corrected;
    int count = 0;
    size_t last_sequence = 0;
  };
  std::vector<Candidate> candidates;

  for (const auto& [reading, directions] : data->local_counts_by_reading) {
    if (reading.empty() || full_key.find(reading) == absl::string_view::npos) {
      continue;
    }

    // Local v4.1 evidence is deliberately context-independent. The coarse
    // context class remains in individual TSV events only as provenance; it is
    // not part of the logical rule or threshold count. Contextual safety comes
    // from the current Mozc reading/surface alignment at application time.
    // For the same reading/raw surface, keep every mature corrected direction.
    // The current Mozc surface is the contextual gate at application time, so
    // choosing a global winner here would discard valid homophone-specific
    // rules before ApplyLocalPreferenceRepairs can disambiguate them.
    for (const auto& [direction, counts] : directions) {
      const auto& [raw, corrected] = direction;
      // These are necessary conditions only. Exact reading-position and
      // Mozc-surface correspondence is still checked by Session after this
      // bounded ranking step.
      if ((!raw_surface_filter.empty() &&
           raw_surface_filter.find(absl::string_view(raw)) ==
               absl::string_view::npos) ||
          (!preferred_surface_filter.empty() &&
           preferred_surface_filter.find(absl::string_view(corrected)) ==
               absl::string_view::npos)) {
        continue;
      }
      if (counts.count < threshold) {
        continue;
      }

      // If the exact inverse direction is also mature globally, do not guess.
      const auto inverse = directions.find({corrected, raw});
      if (inverse != directions.end() && inverse->second.count >= threshold) {
        continue;
      }

      candidates.push_back(
          {reading, raw, corrected, counts.count, counts.last_sequence});
    }
  }

  std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& lhs, const Candidate& rhs) {
              if (lhs.key.size() != rhs.key.size()) {
                return lhs.key.size() > rhs.key.size();
              }
              if (lhs.count != rhs.count) {
                return lhs.count > rhs.count;
              }
              if (lhs.last_sequence != rhs.last_sequence) {
                return lhs.last_sequence > rhs.last_sequence;
              }
              if (lhs.key != rhs.key) {
                return lhs.key < rhs.key;
              }
              if (lhs.raw != rhs.raw) {
                return lhs.raw < rhs.raw;
              }
              return lhs.corrected < rhs.corrected;
            });

  for (const Candidate& candidate : candidates) {
    ZenzLocalPreference preference;
    preference.key = candidate.key;
    // Carry the current context only as event provenance if this applied rule
    // is later explicitly rejected. It does not participate in rule identity.
    preference.context_class = current_context;
    preference.preferred_value = candidate.corrected;
    preference.disfavored_value = candidate.raw;
    preference.observation_count = candidate.count;
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

  for (const auto& [reading, directions] : data->local_counts_by_reading) {
    for (const auto& [direction, counts] : directions) {
      const auto& [raw, corrected] = direction;
      const int count = counts.count;
      if (count == 0) {
        continue;
      }
      const auto inverse = directions.find({corrected, raw});
      const int opposite =
          inverse == directions.end() ? 0 : inverse->second.count;

      ZenzLocalPreferenceEntry entry;
      entry.key = reading;
      // Local logical entries are context-independent. Keep the public field
      // empty so management UI does not imply a false context partition.
      entry.context_class.clear();
      entry.preferred_value = corrected;
      entry.disfavored_value = raw;
      entry.observation_count = count;
      entry.effective_observation_count = count;
      entry.opposite_effective_observation_count = opposite;
      entries.push_back(std::move(entry));
    }
  }

  std::sort(entries.begin(), entries.end(),
            [](const ZenzLocalPreferenceEntry& lhs,
               const ZenzLocalPreferenceEntry& rhs) {
              if (lhs.key != rhs.key) {
                return lhs.key < rhs.key;
              }
              if (lhs.disfavored_value != rhs.disfavored_value) {
                return lhs.disfavored_value < rhs.disfavored_value;
              }
              return lhs.preferred_value < rhs.preferred_value;
            });
  return entries;
}

std::vector<ZenzFeedbackEntry> ZenzFeedbackStore::ListEntries() const {
  return ListEntries(ZenzFeedbackAutoBlockPolicy());
}

std::vector<ZenzFeedbackEntry> ZenzFeedbackStore::ListEntries(
    const ZenzFeedbackAutoBlockPolicy& auto_block_policy) const {
  const std::shared_ptr<const FeedbackData> data = LoadFeedbackData();
  std::vector<ZenzFeedbackEntry> entries;

  for (const auto& [key, counts] : data->counts) {
    const auto& [reading, context, value] = key;
    Counts effective;
    for (auto it = data->counts.lower_bound(FeedbackKey(reading, "", ""));
         it != data->counts.end() &&
         std::get<0>(it->first) == reading; ++it) {
      if (std::get<2>(it->first) != value ||
          !IsFeedbackContextCompatible(context, std::get<1>(it->first))) {
        continue;
      }
      MergeCounts(it->second, &effective);
    }

    ZenzFeedbackDecision decision = BuildDecisionFromCounts(effective);
    ApplyRejectDominanceDecision(effective, &decision);
    ApplyAutoBlockDecision(effective, auto_block_policy, &decision);

    ZenzFeedbackEntry entry;
    entry.key = reading;
    entry.context_class = context;
    entry.value = value;
    entry.accepted_count = counts.accepted;
    entry.rejected_count = counts.rejected;
    entry.effective_accepted_count = effective.accepted;
    entry.effective_rejected_count = effective.rejected;
    entry.auto_block_reject_count = effective.auto_block_rejected;
    entry.hard_rejected = effective.hard_rejected;
    entry.auto_blocked = decision.auto_blocked;
    entry.reason = decision.reason;
    entries.push_back(std::move(entry));
  }

  std::sort(entries.begin(), entries.end(),
            [](const ZenzFeedbackEntry& lhs,
               const ZenzFeedbackEntry& rhs) {
              if (lhs.key != rhs.key) {
                return lhs.key < rhs.key;
              }
              if (lhs.context_class != rhs.context_class) {
                return lhs.context_class < rhs.context_class;
              }
              return lhs.value < rhs.value;
            });
  return entries;
}

bool ZenzFeedbackStore::ExportToFile(const std::wstring& path) const {
#if defined(_WIN32) || (defined(__APPLE__) && TARGET_OS_OSX)
  const std::filesystem::path output_path =
      WidePathToFilesystemPath(path);
  if (output_path.empty()) {
    return false;
  }
  std::lock_guard<std::mutex> mutation_lock(g_feedback_mutation_mutex);
  ScopedFeedbackInterprocessLock interprocess_lock;
  if (!interprocess_lock.ok()) {
    return false;
  }
  std::vector<Record> records;
  if (!LoadRecordsFromDisk(&records)) {
    return false;
  }
  return WriteRecordsToPath(output_path, records);
#else
  (void)path;
  return false;
#endif
}

bool ZenzFeedbackStore::ImportFromFile(
    const std::wstring& path, ZenzFeedbackImportMode mode) {
#if defined(_WIN32) || (defined(__APPLE__) && TARGET_OS_OSX)
  const std::filesystem::path input_path =
      WidePathToFilesystemPath(path);
  if (input_path.empty()) {
    return false;
  }
  std::ifstream file(input_path, std::ios::binary);
  if (!file) {
    return false;
  }
  std::vector<Record> imported;
  if (!LoadRecordsFromStream(&file, true, &imported)) {
    return false;
  }
  file.close();

  std::lock_guard<std::mutex> mutation_lock(g_feedback_mutation_mutex);
  ScopedFeedbackInterprocessLock interprocess_lock;
  if (!interprocess_lock.ok()) {
    return false;
  }

  std::vector<Record> records;
  if (mode == ZenzFeedbackImportMode::kAppend &&
      !LoadRecordsFromDisk(&records)) {
    return false;
  }
  records.insert(records.end(), imported.begin(), imported.end());
  return WriteRecordsAtomically(records);
#else
  (void)path;
  (void)mode;
  return false;
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
  std::vector<Record> records;
  if (!LoadRecordsFromDisk(&records)) {
    return false;
  }

  const std::string normalized_context =
      NormalizeContextClass(context_class);
  const auto new_end =
      std::remove_if(records.begin(), records.end(),
                     [&](const Record& record) {
                       return record.kind == RecordKind::kFull &&
                              record.key == key &&
                              record.context_class == normalized_context &&
                              record.value == value;
                     });
  if (new_end == records.end()) {
    return true;
  }
  records.erase(new_end, records.end());
  return WriteRecordsAtomically(records);
}

bool ZenzFeedbackStore::DeleteLocalPreference(
    absl::string_view key, absl::string_view context_class,
    absl::string_view preferred_value, absl::string_view disfavored_value) {
  (void)context_class;
  std::lock_guard<std::mutex> mutation_lock(g_feedback_mutation_mutex);
  ScopedFeedbackInterprocessLock interprocess_lock;
  if (!interprocess_lock.ok()) {
    return false;
  }
  std::vector<Record> records;
  if (!LoadRecordsFromDisk(&records)) {
    return false;
  }

  const auto new_end =
      std::remove_if(records.begin(), records.end(),
                     [&](const Record& record) {
                       if (record.kind != RecordKind::kLocal) {
                         return false;
                       }
                       const CanonicalLocalIdentity local =
                           CanonicalizeLocalIdentity(
                               record.key, record.value, record.extra);
                       return local.key == key &&
                              local.raw_zenz_surface == disfavored_value &&
                              local.corrected_surface == preferred_value;
                     });
  if (new_end == records.end()) {
    return true;
  }
  records.erase(new_end, records.end());
  return WriteRecordsAtomically(records);
}

bool ZenzFeedbackStore::ClearAll() {
  std::lock_guard<std::mutex> mutation_lock(g_feedback_mutation_mutex);
  ScopedFeedbackInterprocessLock interprocess_lock;
  if (!interprocess_lock.ok()) {
    return false;
  }
  return WriteRecordsAtomically({});
}

void ZenzFeedbackStore::RecordAccepted(
    absl::string_view key, absl::string_view context_class,
    absl::string_view value) {
  Record record;
  record.kind = RecordKind::kFull;
  record.action = "accepted";
  record.key = std::string(key);
  record.context_class = NormalizeContextClass(context_class);
  record.value = std::string(value);
  record.extra = "";
  record.count = 1;
  AppendRecords({record});
}

void ZenzFeedbackStore::RecordRejected(
    absl::string_view key, absl::string_view context_class,
    absl::string_view value, absl::string_view reason) {
  Record record;
  record.kind = RecordKind::kFull;
  record.action = "rejected";
  record.key = std::string(key);
  record.context_class = NormalizeContextClass(context_class);
  record.value = std::string(value);
  record.extra = std::string(reason);
  record.count = 1;
  AppendRecords({record});
}

void ZenzFeedbackStore::RecordLocalAccepted(
    absl::string_view key, absl::string_view context_class,
    absl::string_view raw_zenz_surface,
    absl::string_view corrected_surface) {
  ZenzLocalPreference preference;
  preference.key = std::string(key);
  preference.context_class = NormalizeContextClass(context_class);
  preference.preferred_value = std::string(corrected_surface);
  preference.disfavored_value = std::string(raw_zenz_surface);
  RecordLocalAccepteds({preference});
}

void ZenzFeedbackStore::RecordLocalRejected(
    absl::string_view key, absl::string_view context_class,
    absl::string_view raw_zenz_surface,
    absl::string_view corrected_surface) {
  ZenzLocalPreference preference;
  preference.key = std::string(key);
  preference.context_class = NormalizeContextClass(context_class);
  preference.preferred_value = std::string(corrected_surface);
  preference.disfavored_value = std::string(raw_zenz_surface);
  RecordLocalRejecteds({preference});
}

void ZenzFeedbackStore::RecordLocalAccepteds(
    const std::vector<ZenzLocalPreference>& preferences) {
  std::vector<Record> records;
  records.reserve(preferences.size());
  for (const ZenzLocalPreference& preference : preferences) {
    if (preference.key.empty() || preference.preferred_value.empty() ||
        preference.disfavored_value.empty() ||
        preference.preferred_value == preference.disfavored_value) {
      continue;
    }
    const CanonicalLocalIdentity local = CanonicalizeLocalIdentity(
        preference.key, preference.disfavored_value,
        preference.preferred_value);
    Record record;
    record.kind = RecordKind::kLocal;
    record.action = "accepted";
    record.key = local.key;
    record.context_class =
        NormalizeContextClass(preference.context_class);
    record.value = local.raw_zenz_surface;
    record.extra = local.corrected_surface;
    record.count = 1;
    records.push_back(std::move(record));
  }
  AppendRecords(records);
}

void ZenzFeedbackStore::RecordLocalRejecteds(
    const std::vector<ZenzLocalPreference>& preferences) {
  std::vector<Record> records;
  records.reserve(preferences.size());
  for (const ZenzLocalPreference& preference : preferences) {
    if (preference.key.empty() || preference.preferred_value.empty() ||
        preference.disfavored_value.empty() ||
        preference.preferred_value == preference.disfavored_value) {
      continue;
    }
    const CanonicalLocalIdentity local = CanonicalizeLocalIdentity(
        preference.key, preference.disfavored_value,
        preference.preferred_value);
    Record record;
    record.kind = RecordKind::kLocal;
    record.action = "rejected";
    record.key = local.key;
    record.context_class =
        NormalizeContextClass(preference.context_class);
    record.value = local.raw_zenz_surface;
    record.extra = local.corrected_surface;
    record.count = 1;
    records.push_back(std::move(record));
  }
  AppendRecords(records);
}

void ZenzFeedbackStore::RecordLocalPreference(
    absl::string_view key, absl::string_view context_class,
    absl::string_view preferred_value, absl::string_view disfavored_value,
    absl::string_view) {
  RecordLocalAccepted(key, context_class, disfavored_value, preferred_value);
}

void ZenzFeedbackStore::RecordLocalPreferences(
    const std::vector<ZenzLocalPreference>& preferences,
    absl::string_view) {
  RecordLocalAccepteds(preferences);
}

bool ZenzFeedbackStore::Maintenance(size_t max_entries) {
  max_entries =
      std::clamp(max_entries, kMinMaintenanceEntries,
                 kMaxMaintenanceEntries);

  std::lock_guard<std::mutex> mutation_lock(g_feedback_mutation_mutex);
  ScopedFeedbackInterprocessLock interprocess_lock;
  if (!interprocess_lock.ok()) {
    return false;
  }

  std::vector<Record> records;
  if (!LoadRecordsFromDisk(&records)) {
    return false;
  }

  std::map<FeedbackKey, FullCompactAggregate> full;
  std::map<LocalKey, std::vector<LocalEvent>> local_events;

  for (size_t i = 0; i < records.size(); ++i) {
    const Record& record = records[i];
    if (record.kind == RecordKind::kFull) {
      const FeedbackKey key(record.key, record.context_class, record.value);
      FullCompactAggregate& aggregate = full[key];
      aggregate.key = record.key;
      aggregate.context_class = record.context_class;
      aggregate.value = record.value;
      aggregate.last_sequence = i;
      if (record.action == "accepted") {
        aggregate.accepted_count = SaturatingAddNonNegative(
            aggregate.accepted_count, record.count);
        AddAccepted(record.count, &aggregate.counts);
      } else {
        aggregate.rejected_by_reason[record.extra] = SaturatingAddNonNegative(
            aggregate.rejected_by_reason[record.extra], record.count);
        AddRejected(record.extra, record.count, &aggregate.counts);
      }
      continue;
    }

    const CanonicalLocalIdentity local = CanonicalizeLocalIdentity(
        record.key, record.value, record.extra);
    LocalEvent event;
    event.context_class = record.context_class;
    event.raw_zenz_surface = local.raw_zenz_surface;
    event.corrected_surface = local.corrected_surface;
    event.accepted = record.action == "accepted";
    event.count = record.count;
    event.sequence = i;
    local_events[LocalKey(local.key, local.raw_zenz_surface,
                          local.corrected_surface)]
        .push_back(std::move(event));
  }

  std::vector<FullCompactAggregate> full_entries;
  full_entries.reserve(full.size());
  for (auto& [key, aggregate] : full) {
    full_entries.push_back(std::move(aggregate));
  }

  auto full_keep_better =
      [](const FullCompactAggregate& lhs,
         const FullCompactAggregate& rhs) {
        const bool lhs_clear =
            lhs.counts.hard_rejected || TotalScore(lhs.counts) != 0;
        const bool rhs_clear =
            rhs.counts.hard_rejected || TotalScore(rhs.counts) != 0;
        if (lhs_clear != rhs_clear) {
          return lhs_clear > rhs_clear;
        }
        const int64_t lhs_strength =
            static_cast<int64_t>(lhs.counts.positive_score) +
            lhs.counts.negative_score;
        const int64_t rhs_strength =
            static_cast<int64_t>(rhs.counts.positive_score) +
            rhs.counts.negative_score;
        if (lhs_strength != rhs_strength) {
          return lhs_strength > rhs_strength;
        }
        if (lhs.last_sequence != rhs.last_sequence) {
          return lhs.last_sequence > rhs.last_sequence;
        }
        return std::tie(lhs.key, lhs.context_class, lhs.value) <
               std::tie(rhs.key, rhs.context_class, rhs.value);
      };
  std::sort(full_entries.begin(), full_entries.end(), full_keep_better);
  if (full_entries.size() > max_entries) {
    full_entries.resize(max_entries);
  }

  std::vector<LocalCompactAggregate> local_entries;
  local_entries.reserve(local_events.size());
  for (const auto& [key, events] : local_events) {
    const auto& [reading, raw, corrected] = key;
    int count = 0;
    size_t last_sequence = 0;
    std::string last_context = "empty";
    for (const LocalEvent& event : events) {
      last_sequence = event.sequence;
      last_context = event.context_class;
      if (event.accepted) {
        const int remaining = kMaxLocalEvidenceCount - count;
        count = event.count >= remaining
                    ? kMaxLocalEvidenceCount
                    : count + event.count;
      } else {
        count = event.count >= count ? 0 : count - event.count;
      }
    }
    if (count == 0) {
      continue;
    }
    local_entries.push_back(
        {reading, last_context, raw, corrected, count, last_sequence});
  }

  std::sort(local_entries.begin(), local_entries.end(),
            [](const LocalCompactAggregate& lhs,
               const LocalCompactAggregate& rhs) {
              if (lhs.count != rhs.count) {
                return lhs.count > rhs.count;
              }
              if (lhs.last_sequence != rhs.last_sequence) {
                return lhs.last_sequence > rhs.last_sequence;
              }
              return std::tie(lhs.key, lhs.context_class,
                              lhs.raw_zenz_surface,
                              lhs.corrected_surface) <
                     std::tie(rhs.key, rhs.context_class,
                              rhs.raw_zenz_surface,
                              rhs.corrected_surface);
            });
  if (local_entries.size() > max_entries) {
    local_entries.resize(max_entries);
  }

  // Re-establish recency by writing old logical entries first and new entries
  // last. Within one Full logical entry, accepted and reason-specific rejected
  // aggregate rows remain adjacent.
  std::sort(full_entries.begin(), full_entries.end(),
            [](const FullCompactAggregate& lhs,
               const FullCompactAggregate& rhs) {
              return lhs.last_sequence < rhs.last_sequence;
            });
  std::sort(local_entries.begin(), local_entries.end(),
            [](const LocalCompactAggregate& lhs,
               const LocalCompactAggregate& rhs) {
              return lhs.last_sequence < rhs.last_sequence;
            });

  struct OrderedOutput {
    size_t last_sequence = 0;
    bool local = false;
    size_t index = 0;
  };
  std::vector<OrderedOutput> ordered;
  ordered.reserve(full_entries.size() + local_entries.size());
  for (size_t i = 0; i < full_entries.size(); ++i) {
    ordered.push_back({full_entries[i].last_sequence, false, i});
  }
  for (size_t i = 0; i < local_entries.size(); ++i) {
    ordered.push_back({local_entries[i].last_sequence, true, i});
  }
  std::sort(ordered.begin(), ordered.end(),
            [](const OrderedOutput& lhs, const OrderedOutput& rhs) {
              if (lhs.last_sequence != rhs.last_sequence) {
                return lhs.last_sequence < rhs.last_sequence;
              }
              return lhs.local < rhs.local;
            });

  std::vector<Record> compacted;
  for (const OrderedOutput& item : ordered) {
    if (!item.local) {
      const FullCompactAggregate& entry = full_entries[item.index];
      if (entry.accepted_count > 0) {
        Record record;
        record.kind = RecordKind::kFull;
        record.action = "accepted";
        record.key = entry.key;
        record.context_class = entry.context_class;
        record.value = entry.value;
        record.extra = "";
        record.count = entry.accepted_count;
        compacted.push_back(std::move(record));
      }
      for (const auto& [reason, count] : entry.rejected_by_reason) {
        if (count <= 0) {
          continue;
        }
        Record record;
        record.kind = RecordKind::kFull;
        record.action = "rejected";
        record.key = entry.key;
        record.context_class = entry.context_class;
        record.value = entry.value;
        record.extra = reason;
        record.count = count;
        compacted.push_back(std::move(record));
      }
      continue;
    }

    const LocalCompactAggregate& entry = local_entries[item.index];
    Record record;
    record.kind = RecordKind::kLocal;
    record.action = "accepted";
    record.key = entry.key;
    record.context_class = entry.context_class;
    record.value = entry.raw_zenz_surface;
    record.extra = entry.corrected_surface;
    record.count = entry.count;
    compacted.push_back(std::move(record));
  }

  return WriteRecordsAtomically(compacted);
}

bool ZenzFeedbackStore::MaybeMaintenance(size_t max_entries) {
  max_entries =
      std::clamp(max_entries, kMinMaintenanceEntries, kMaxMaintenanceEntries);
  const std::filesystem::path maintenance_path = FeedbackPathForRead();
  const FileStamp current_stamp = GetFileStamp(maintenance_path);
  const uintmax_t growth_threshold = std::max<uintmax_t>(
      256 * 1024, static_cast<uintmax_t>(max_entries) * 512);

  std::lock_guard<std::mutex> lifecycle_lock(g_lifecycle_maintenance_mutex);
  if (g_lifecycle_maintenance_done &&
      g_lifecycle_maintenance_path == maintenance_path &&
      g_lifecycle_maintenance_max_entries == max_entries) {
    const uintmax_t current_size = current_stamp.exists ? current_stamp.size : 0;
    if (current_size <=
        g_lifecycle_maintenance_file_size + growth_threshold) {
      return true;
    }
  }

  if (!Maintenance(max_entries)) {
    return false;
  }
  g_lifecycle_maintenance_done = true;
  g_lifecycle_maintenance_path = maintenance_path;
  g_lifecycle_maintenance_max_entries = max_entries;
  const FileStamp compacted_stamp = GetFileStamp(maintenance_path);
  g_lifecycle_maintenance_file_size =
      compacted_stamp.exists ? compacted_stamp.size : 0;
  return true;
}

}  // namespace session
}  // namespace mozc
