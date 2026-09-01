#ifndef MOZC_SESSION_ZENZ_FEEDBACK_STORE_H_
#define MOZC_SESSION_ZENZ_FEEDBACK_STORE_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "absl/strings/string_view.h"

namespace mozc {
namespace session {

enum class ZenzFeedbackAction {
  kNeutral = 0,
  kPrefer = 1,
  kReject = 2,
};

enum class ZenzFeedbackImportMode {
  kAppend = 0,
  kReplace = 1,
};

struct ZenzFeedbackAutoBlockPolicy {
  bool enabled = false;
  int reject_threshold = 0;
};

struct ZenzFeedbackDecision {
  ZenzFeedbackAction action = ZenzFeedbackAction::kNeutral;
  std::string reason = "feedback_neutral";
  int accepted_count = 0;
  int rejected_count = 0;
  int positive_score = 0;
  int negative_score = 0;
  int total_score = 0;
  int auto_block_reject_count = 0;
  bool hard_rejected = false;
  bool auto_blocked = false;
};

struct ZenzFeedbackCandidate {
  std::string value;
  int accepted_count = 0;
  int rejected_count = 0;
  int positive_score = 0;
  int negative_score = 0;
  int total_score = 0;
  int auto_block_reject_count = 0;
  bool hard_rejected = false;
  bool auto_blocked = false;
  std::string reason = "feedback_neutral";
};

struct ZenzFeedbackEntry {
  std::string key;
  std::string context_class;
  std::string value;
  // Counts in this exact persisted coarse-context row.
  int accepted_count = 0;
  int rejected_count = 0;
  // Counts after the same compatible-context aggregation used by runtime.
  int effective_accepted_count = 0;
  int effective_rejected_count = 0;
  int auto_block_reject_count = 0;
  bool hard_rejected = false;
  bool auto_blocked = false;
  std::string reason = "feedback_neutral";
};

// One direction of Local Zenz Preference evidence.  The store does not inspect
// the current Mozc surface; opposite directions for the same reading may coexist.
// Session code resolves each observation against the current Mozc result.
struct ZenzLocalPreference {
  std::string key;
  std::string context_class;
  std::string preferred_value;
  std::string disfavored_value;
  int observation_count = 0;
};

// Exact persisted Local Zenz Preference aggregate for management UI.  Unlike
// GetLocalPreferences(), this view does not perform cross-context promotion or
// current-reading filtering; it reflects what is actually stored.
struct ZenzLocalPreferenceEntry {
  std::string key;
  std::string context_class;
  std::string preferred_value;
  std::string disfavored_value;
  // Exact observations written with this source context class.
  int observation_count = 0;
  // Effective observations after the same coarse-context compatibility rule
  // used by runtime lookup.  Management UI uses this for the threshold/status.
  int effective_observation_count = 0;
  int opposite_effective_observation_count = 0;
};

// Persistent local feedback for Zenz live correction.
//
// Ordinary accepted/rejected feedback remains full-sequence scoped:
//   key   = the complete reading submitted to Zenz
//   value = the complete Zenz correction shown to or accepted by the user
//
// REV10 additionally stores conservative local preference observations as
// atomic v3 records in the same TSV.  Those records never participate in
// ordinary full-sequence promotion/decision lookup.  Raw left/right context is
// never persisted; only the existing coarse non-reversible context class is
// stored with a local observation.
class ZenzFeedbackStore {
 public:
  ZenzFeedbackDecision Decide(absl::string_view key,
                              absl::string_view context_class,
                              absl::string_view value) const;

  ZenzFeedbackDecision Decide(
      absl::string_view key,
      absl::string_view context_class,
      absl::string_view value,
      const ZenzFeedbackAutoBlockPolicy& auto_block_policy) const;

  std::vector<ZenzFeedbackCandidate> GetRankedCandidates(
      absl::string_view key,
      absl::string_view context_class) const;

  std::vector<ZenzFeedbackCandidate> GetRankedCandidates(
      absl::string_view key,
      absl::string_view context_class,
      const ZenzFeedbackAutoBlockPolicy& auto_block_policy) const;

  std::vector<ZenzFeedbackCandidate> GetAcceptedCandidates(
      absl::string_view key,
      absl::string_view context_class) const;

  std::vector<ZenzFeedbackCandidate> GetAcceptedCandidates(
      absl::string_view key,
      absl::string_view context_class,
      const ZenzFeedbackAutoBlockPolicy& auto_block_policy) const;

  // Returns local preference observations whose reading occurs uniquely in the
  // current full reading and whose coarse source context is compatible.  The
  // store deliberately does not filter by the current Mozc surface; Session is
  // responsible for resolving preferred/disfavored/neither semantics so
  // opposite context-dependent directions can coexist.  A direction is returned
  // only after its compatible-context observation count reaches
  // min_observation_count; callers can therefore keep immature observations
  // persisted without letting them affect conversion behavior.
  std::vector<ZenzLocalPreference> GetLocalPreferences(
      absl::string_view full_key,
      absl::string_view context_class,
      size_t max_results = 12,
      int min_observation_count = 1) const;

  // Lists exact persisted local-preference aggregates for management UI.
  std::vector<ZenzLocalPreferenceEntry> ListLocalPreferenceEntries() const;

  std::vector<ZenzFeedbackEntry> ListEntries() const;

  std::vector<ZenzFeedbackEntry> ListEntries(
      const ZenzFeedbackAutoBlockPolicy& auto_block_policy) const;

  [[nodiscard]]
  bool ExportToFile(const std::wstring& path) const;

  [[nodiscard]]
  bool ImportFromFile(const std::wstring& path,
                      ZenzFeedbackImportMode mode);

  [[nodiscard]]
  bool DeleteEntry(absl::string_view key,
                   absl::string_view context_class,
                   absl::string_view value);

  [[nodiscard]]
  bool DeleteLocalPreference(absl::string_view key,
                             absl::string_view context_class,
                             absl::string_view preferred_value,
                             absl::string_view disfavored_value);

  [[nodiscard]]
  bool ClearAll();

  void RecordAccepted(absl::string_view key,
                      absl::string_view context_class,
                      absl::string_view value);

  void RecordRejected(absl::string_view key,
                      absl::string_view context_class,
                      absl::string_view value,
                      absl::string_view reason);

  // Records one atomic v3 Local Zenz Preference observation.  Ordinary v2
  // feedback remains full-sequence scoped.  REV9 local_revert pairs remain
  // import-compatible but are never written by REV10.
  void RecordLocalPreference(absl::string_view key,
                             absl::string_view context_class,
                             absl::string_view preferred_value,
                             absl::string_view disfavored_value,
                             absl::string_view reason =
                                 "rejected_zenz_final_commit");

  // Batches multiple independently validated local observations into one file
  // append/flush.  This is used when one final commit safely localizes several
  // disjoint spans, avoiding one disk open per span.
  void RecordLocalPreferences(
      const std::vector<ZenzLocalPreference>& preferences,
      absl::string_view reason = "rejected_zenz_final_commit");
};

}  // namespace session
}  // namespace mozc

#endif  // MOZC_SESSION_ZENZ_FEEDBACK_STORE_H_
