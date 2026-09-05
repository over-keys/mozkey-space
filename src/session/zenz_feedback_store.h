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
  int accepted_count = 0;
  int rejected_count = 0;
  int effective_accepted_count = 0;
  int effective_rejected_count = 0;
  int auto_block_reject_count = 0;
  bool hard_rejected = false;
  bool auto_blocked = false;
  std::string reason = "feedback_neutral";
};

// Local v4.1 rule:
//   minimal safe reading + raw Zenz surface + corrected surface
//
// context_class is retained only as event provenance/current-use metadata. It
// is not part of Local rule identity or threshold counting. Contextual safety
// is enforced at application time by unique reading/surface alignment and the
// current Mozc preferred surface.
// preferred_value is the corrected surface.
// disfavored_value is the raw Zenz surface.
// observation_count is the global current evidence count after ordered replay.
// It is independent of the activation threshold and saturates only at the
// fixed implementation cap (255).
struct ZenzLocalPreference {
  std::string key;
  std::string context_class;
  std::string preferred_value;
  std::string disfavored_value;
  int observation_count = 0;

  // Transient application metadata. These fields are never part of Local
  // identity, persistence, threshold counting, import/export, or compaction.
  // They let a rule applied to repeated readings be attributed to the exact
  // reading occurrence if the user later edits or rejects that repair.
  size_t reading_begin = 0;
  bool has_reading_begin = false;
};

struct ZenzLocalPreferenceEntry {
  std::string key;
  std::string context_class;
  std::string preferred_value;
  std::string disfavored_value;
  int observation_count = 0;
  int effective_observation_count = 0;
  int opposite_effective_observation_count = 0;
};

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

  std::vector<ZenzLocalPreference> GetLocalPreferences(
      absl::string_view full_key,
      absl::string_view context_class,
      size_t max_results = 12,
      int min_observation_count = 1,
      // Optional necessary surface filters for the current raw Zenz and Mozc
      // values. They only discard rules that cannot occur in the values; the
      // exact reading-position gate remains the caller's responsibility.
      absl::string_view raw_surface_filter = absl::string_view(),
      absl::string_view preferred_surface_filter = absl::string_view()) const;

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
  // context_class is accepted for source/UI compatibility but Local v4.1
  // deletion removes the context-independent logical rule across all events.
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

  void RecordLocalAccepted(absl::string_view key,
                           absl::string_view context_class,
                           absl::string_view raw_zenz_surface,
                           absl::string_view corrected_surface);

  void RecordLocalRejected(absl::string_view key,
                           absl::string_view context_class,
                           absl::string_view raw_zenz_surface,
                           absl::string_view corrected_surface);

  void RecordLocalAccepteds(
      const std::vector<ZenzLocalPreference>& preferences);

  void RecordLocalRejecteds(
      const std::vector<ZenzLocalPreference>& preferences);

  // Transitional source-level aliases used by call sites while v4 integration
  // is being applied.  They write v4 accepted events only; no legacy format is
  // read or written.
  void RecordLocalPreference(absl::string_view key,
                             absl::string_view context_class,
                             absl::string_view preferred_value,
                             absl::string_view disfavored_value,
                             absl::string_view reason =
                                 "rejected_zenz_final_commit");

  void RecordLocalPreferences(
      const std::vector<ZenzLocalPreference>& preferences,
      absl::string_view reason = "rejected_zenz_final_commit");

  // Heavy operation. Call only at safe lifecycle/management points.
  // Full and Local each get their own independent max_entries budget.
  [[nodiscard]]
  bool Maintenance(size_t max_entries);

  // Lightweight lifecycle gate. Runs Maintenance at most once for the same
  // process, feedback path and N. Management operations should call
  // Maintenance directly.
  bool MaybeMaintenance(size_t max_entries);
};

}  // namespace session
}  // namespace mozc

#endif  // MOZC_SESSION_ZENZ_FEEDBACK_STORE_H_
