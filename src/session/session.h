// Copyright 2010-2021, Google Inc.
// All rights reserved.
#ifndef MOZC_SESSION_SESSION_H_
#define MOZC_SESSION_SESSION_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "composer/composer.h"
#include "composer/table.h"
#include "engine/engine_interface.h"
#include "protocol/commands.pb.h"
#include "protocol/config.pb.h"
#include "session/ime_context.h"
#include "session/keymap.h"
#include "session/zenz_adoption_policy.h"
#include "session/zenz_context_assembler.h"
#include "session/zenz_context_sanitizer.h"
#include "session/zenz_feedback_store.h"
#include "session/zenz_live_corrector.h"
#include "session/zenz_output_validator.h"
#include "transliteration/transliteration.h"

namespace mozc { namespace session {
struct ZenzProjectedLearningSegment {
  std::string key;
  std::string value;
  bool is_reranked = false;
};

class Session {
 public:
  explicit Session(const EngineInterface& engine);
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;
  bool SendKey(commands::Command*);
  bool TestSendKey(commands::Command*);
  bool SendCommand(commands::Command*);
  bool IMEOn(commands::Command*);
  bool IMEOff(commands::Command*);
  bool MakeSureIMEOn(commands::Command*);
  bool MakeSureIMEOff(commands::Command*);
  bool EchoBack(commands::Command*);
  bool EchoBackAndClearUndoContext(commands::Command*);
  bool DoNothing(commands::Command*);
  bool DeleteCandidateFromHistory(commands::Command*);
  bool Revert(commands::Command*);
  bool ResetContext(commands::Command*);
  bool GetStatus(commands::Command*);
  bool RequestConvertReverse(commands::Command*);
  bool RequestReconvertSelectionOrInsertSpace(commands::Command*);
  bool ConvertReverse(commands::Command*);
  bool RequestUndo(commands::Command*);
  bool Undo(commands::Command*);
  bool InsertSpace(commands::Command*);
  bool InsertSpaceToggled(commands::Command*);
  bool InsertSpaceHalfWidth(commands::Command*);
  bool InsertSpaceFullWidth(commands::Command*);
  bool InsertCharacter(commands::Command*);
  bool UpdateComposition(commands::Command*);
  bool UpdateCompositionInternal(commands::Command*);
  bool Delete(commands::Command*);
  bool Backspace(commands::Command*);
  bool EditCancel(commands::Command*);
  bool EditCancelAndIMEOff(commands::Command*);
  bool MoveCursorRight(commands::Command*);
  bool MoveCursorLeft(commands::Command*);
  bool MoveCursorToEnd(commands::Command*);
  bool MoveCursorToBeginning(commands::Command*);
  bool MoveCursorTo(commands::Command*);
  bool Convert(commands::Command*);
  bool ConvertWithoutHistory(commands::Command*);
  bool ConvertNext(commands::Command*);
  bool ConvertPrev(commands::Command*);
  bool ConvertNextPage(commands::Command*);
  bool ConvertPrevPage(commands::Command*);
  bool ConvertCancel(commands::Command*);
  bool PredictAndConvert(commands::Command*);
  bool Commit(commands::Command*);
  bool CommitNotTriggeringZeroQuerySuggest(commands::Command*);
  bool CommitFirstSuggestion(commands::Command*);
  bool CommitCandidate(commands::Command*);
  bool CommitSegment(commands::Command*);
  bool CommitHead(size_t, commands::Command*);
  bool CommitIfPassword(commands::Command*);
  bool SegmentFocusRight(commands::Command*);
  bool SegmentFocusLeft(commands::Command*);
  bool SegmentFocusLast(commands::Command*);
  bool SegmentFocusLeftEdge(commands::Command*);
  bool SegmentWidthExpand(commands::Command*);
  bool SegmentWidthShrink(commands::Command*);
  bool ConvertToHiragana(commands::Command*);
  bool ConvertToFullKatakana(commands::Command*);
  bool ConvertToHalfKatakana(commands::Command*);
  bool ConvertToFullASCII(commands::Command*);
  bool ConvertToHalfASCII(commands::Command*);
  bool ConvertToHalfWidth(commands::Command*);
  bool SwitchKanaType(commands::Command*);
  bool DisplayAsHiragana(commands::Command*);
  bool DisplayAsFullKatakana(commands::Command*);
  bool DisplayAsHalfKatakana(commands::Command*);
  bool TranslateFullASCII(commands::Command*);
  bool TranslateHalfASCII(commands::Command*);
  bool TranslateHalfWidth(commands::Command*);
  bool ToggleAlphanumericMode(commands::Command*);
  bool CompositionModeHiragana(commands::Command*);
  bool CompositionModeFullKatakana(commands::Command*);
  bool CompositionModeHalfKatakana(commands::Command*);
  bool CompositionModeFullASCII(commands::Command*);
  bool CompositionModeHalfASCII(commands::Command*);
  bool CompositionModeSwitchKanaType(commands::Command*);
  bool SwitchInputFieldType(commands::Command*);
  bool LaunchConfigDialog(commands::Command*);
  bool LaunchDictionaryTool(commands::Command*);
  bool LaunchWordRegisterDialog(commands::Command*);
  bool UndoOrRewind(commands::Command*);
  bool StopKeyToggling(commands::Command*);
  bool ImeAction(commands::Command*);
  bool ReportBug(commands::Command*);
  void SetConfig(std::shared_ptr<const config::Config>);
  void SetKeyMapManager(std::shared_ptr<const keymap::KeyMapManager>);
  void SetRequest(std::shared_ptr<const commands::Request>);
  void SetConfig(config::Config config) {
    SetConfig(std::make_shared<const config::Config>(std::move(config)));
  }
  void SetRequest(commands::Request request) {
    SetRequest(std::make_shared<const commands::Request>(std::move(request)));
  }
  void SetTable(std::shared_ptr<const composer::Table>);
  void set_client_capability(commands::Capability);
  void set_application_info(commands::ApplicationInfo);
  const commands::ApplicationInfo& application_info() const;
  absl::Time create_session_time() const;
  absl::Time last_command_time() const;
  composer::Composer* get_internal_composer_only_for_unittest();
  const ImeContext& context() const;

 private:
  friend class SessionTestPeer;
  std::unique_ptr<ImeContext> context_;

  bool live_conversion_active_ = false;
  bool live_conversion_pending_ = false;
  uint32_t live_conversion_generation_ = 0;
  uint32_t pending_live_conversion_generation_ = 0;
  std::string pending_live_conversion_key_;
  commands::Input pending_live_conversion_input_;
  commands::CandidateWindow pending_live_conversion_suggestion_candidate_window_;
  commands::CandidateWindow live_conversion_suggestion_candidate_window_;
  std::string live_conversion_key_;
  std::string live_conversion_preedit_;
  std::string live_conversion_value_;
  commands::Preedit live_conversion_preedit_output_;
  std::vector<ProtectedConversionSpan> live_conversion_protected_spans_;

  bool pending_reranked_preedit_commit_after_convert_cancel_ = false;
  std::string pending_reranked_preedit_commit_key_;
  std::string pending_reranked_preedit_commit_value_;
  std::vector<std::string> pending_reranked_preedit_commit_segment_keys_;

  // Explicit/Space conversion uses the same async Zenz pipeline as live
  // conversion without changing the underlying Mozc conversion state.
  bool normal_conversion_zenz_active_ = false;

  struct PendingZenzLiveCorrection {
    uint32_t generation = 0;
    std::string key;
    std::string left_context;
    std::string right_context;
    std::string context_class;
    std::string mozc_value;
    std::string symbol_style_source;
    std::string prompt;
    std::vector<ProtectedConversionSpan> protected_spans;
    commands::Preedit mozc_preedit_output;
    absl::Time issued_at;
    bool pending = false;
    bool submitted = false;
    bool from_live_conversion = true;
    // Whether this conversion is allowed to consult persistent history.
    // CONVERT_WITHOUT_HISTORY sets this false while still allowing the
    // committed result to be learned afterward.
    bool use_conversion_history = false;
    uint32_t poll_count = 0;
  };
  uint32_t zenz_live_generation_ = 0;
  PendingZenzLiveCorrection pending_zenz_live_;

  struct PendingZenzFeedback {
    enum class Action { kNone, kAccepted, kRejected };
    bool pending = false;
    Action action = Action::kNone;
    std::string key;
    std::string context_class;
    // Keep the displayed candidate separate from the model output: accepting
    // an automatically repaired display is not Full evidence for either text.
    std::string value;
    std::string raw_value;
    std::vector<ZenzLocalPreference> applied_local_preferences;
    std::string reason;
    bool has_final_committed_value = false;
    std::string final_committed_value;
    std::vector<std::pair<std::string,std::string>> reverse_learning_segments;
    std::vector<ZenzProjectedLearningSegment> reverse_projected_learning_segments;
  };
  PendingZenzFeedback pending_zenz_feedback_;

  struct PendingDirectCommitLearning {
    bool pending = false;
    std::string key;
    std::string value;
    std::string reason;
    std::unique_ptr<ImeContext> revert_context;
  };
  PendingDirectCommitLearning pending_direct_commit_learning_;

  uint32_t zenz_live_visible_generation_ = 0;
  std::string zenz_live_key_;
  std::string zenz_live_display_key_;
  std::string zenz_live_value_;
  std::string zenz_live_raw_value_;
  std::vector<ZenzLocalPreference> zenz_live_applied_local_preferences_;
  std::string zenz_live_mozc_value_;
  std::string zenz_live_context_class_;
  std::string zenz_live_left_context_;
  commands::Preedit zenz_live_preedit_output_;
  commands::Preedit zenz_live_mozc_preedit_output_;
  bool zenz_live_from_live_conversion_ = true;

  // Volatile Zenz-only snapshot of the immediate left context. Native
  // authoritative context re-anchors it; committed result strings extend it;
  // it is used only while native preceding context is unavailable and input
  // continuity has not been broken. Never persisted or logged.
  std::string zenz_continuation_left_context_;
  std::optional<int32_t> zenz_continuation_revision_;

  ZenzContextAssembler zenz_context_assembler_;
  ZenzContextSanitizer zenz_context_sanitizer_;
  ZenzOutputValidator zenz_output_validator_;
  ZenzAdoptionPolicy zenz_adoption_policy_;
  ZenzFeedbackStore zenz_feedback_store_;
  std::unique_ptr<ZenzLiveCorrector> zenz_live_corrector_;

  struct PendingLiveConversionUndoState {
    std::string pending_key;
    commands::Input pending_input;
    commands::CandidateWindow pending_suggestion_candidate_window;
    commands::CandidateWindow live_suggestion_candidate_window;
    std::string live_key;
    std::string live_preedit;
    std::string live_value;
    commands::Preedit live_preedit_output;
  };
  struct UndoEntry {
    std::unique_ptr<ImeContext> context;
    std::optional<PendingLiveConversionUndoState> pending_live_conversion;
    bool revert_converter_on_undo = true;
  };
  std::deque<UndoEntry> undo_contexts_;

  std::unique_ptr<ImeContext> CreateContext(const EngineInterface&) const;
  void PushUndoContext();
  void PushDirectCommitUndoContext();
  void PopUndoContext();
  bool ShouldRevertConverterOnUndo() const;
  void ClearUndoContext();
  bool HasUndoContext() const;
  bool IsCancelKeyForCompositionOrConversion(const commands::KeyEvent&) const;
  void MaybeSetUndoStatus(commands::Command*) const;
  bool IsFullWidthInsertSpace(const commands::Input&) const;
  bool EditCancelOnPasswordField(commands::Command*);
  void MaybeSetPendingRerankedPreeditCommitAfterConvertCancel();
  void ClearPendingRerankedPreeditCommitAfterConvertCancel();
  bool ShouldMarkPreeditCommitAsRerankedAfterConvertCancel() const;
  bool ShouldMarkPreeditCommitAsRerankedAfterConvertCancel(
      const composer::Composer&) const;
  bool CommitPendingRerankedPreeditAfterConvertCancelForDirectCommit(
      const composer::Composer&, const commands::Context&, absl::string_view);
  bool ConvertToTransliteration(commands::Command*,
      transliteration::TransliterationType);
  bool SelectCandidate(commands::Command*);
  void CommitHeadToFocusedSegmentsInternal(const commands::Context&);
  void CommitCompositionDirectly(commands::Command*);
  std::pair<std::string,std::string>
  GetDirectCommitStringsWithDirectCommitSuffixFallback(
      const composer::Composer&, const commands::KeyEvent&) const;
  void CommitSourceTextDirectly(commands::Command*);
  void CommitRawTextDirectly(commands::Command*);
  void CommitStringDirectly(absl::string_view,absl::string_view,commands::Command*);
  bool CommitInternal(commands::Command*,bool);
  bool Suggest(const commands::Input&);
  bool TryCancelConvertReverse(commands::Command*);
  bool HighlightCandidate(commands::Command*);
  bool SelectCandidateInternal(commands::Command*);
  bool MaybeSelectCandidate(commands::Command*);

  bool MaybeStartLiveConversion(commands::Command*);
  bool MaybeScheduleLiveConversion(commands::Command*);
  bool ApplyDelayedLiveConversion(commands::Command*);
  bool IgnoreStaleDelayedLiveConversion(commands::Command*);
  void CancelPendingLiveConversion();
  void ClearLiveConversionState();
  void CancelLiveConversionForEditing();
  bool PredictAndConvertFromLiveConversion(commands::Command*);
  bool AttachLiveConversionSuggestionCandidateWindow(
      const commands::Input&,commands::Output*);
  bool AttachCachedLiveConversionSuggestionCandidateWindow(commands::Output*);
  bool CommitLiveConversionResult(commands::Command*);
  std::pair<std::string,std::string> GetPendingLiveConversionDisplayCommitStrings() const;
  bool CommitPendingLiveConversionDisplayDirectly(commands::Command*);
  bool CommitPendingLiveConversionDisplayForSubmit(commands::Command*);
  bool OutputPendingLiveConversion(commands::Command*) const;
  void AttachDelayedLiveConversionCallback(commands::Command*) const;

  // Zenz correction shared by live and explicit/Space conversion.
  bool HasActiveZenzCorrectionSource() const;
  bool MaybeStartZenzCorrectionForNormalConversion(
      absl::string_view composition, absl::string_view preedit,
      bool use_conversion_history, commands::Command* command);
  bool CanUseZenzContinuationContextCache() const;
  bool HasZenzContextFeature(const commands::Context&,
                             absl::string_view) const;
  void SetZenzContinuationLeftContext(absl::string_view);
  void PrepareZenzContinuationPrecedingContext(commands::Context*);
  void UpdateZenzContinuationContextCacheFromOutput(
      const commands::Command&);
  void InvalidateZenzContinuationContextCacheForSessionCommand(
      commands::SessionCommand::CommandType);
  bool MaybeScheduleZenzCorrection(commands::Command*,
                                   bool use_conversion_history);
  void AttachZenzLiveCorrectionStartCallback(
      commands::Command* command, const uint32_t delay_msec) const;
  void AttachZenzLiveCorrectionPollCallback(commands::Command*) const;
  bool ApplyZenzLiveCorrection(commands::Command*);
  bool AdvancePendingZenzLiveCorrection(commands::Command*,bool);
  bool IsCurrentZenzLiveCorrectionCallback(const commands::Command&) const;
  bool OutputCurrentLiveConversionWithZenzPending(commands::Command*);
  bool OutputCurrentLiveConversionAfterZenzStop(commands::Command*,absl::string_view);
  ZenzLiveCorrector* EnsureZenzLiveCorrector();
  bool ApplyZenzLiveCorrectionResult(const ZenzLiveResponse&,commands::Command*);
  bool OutputZenzLiveCorrection(absl::string_view,commands::Command*);
  bool RevertZenzLiveCorrectionToNormalConversion(commands::Command*);
  bool CommitZenzLiveCorrectionResult(commands::Command*);

  std::string BuildZenzFeedbackContextClass(absl::string_view) const;
  bool MaybeLearnZenzCandidateToMozcHistory(absl::string_view,absl::string_view);
  int MaybeLearnZenzReverseSegmentsToMozcHistory(
      const std::vector<std::pair<std::string,std::string>>&);
  int MaybeLearnZenzProjectedSegmentsToMozcHistory(
      const std::vector<ZenzProjectedLearningSegment>&);

  bool SetPendingDirectCommitLearning(absl::string_view,absl::string_view,
                                      absl::string_view);
  bool SetPendingDirectCommitLearningFromCommittedResult(
      const commands::Command&,absl::string_view);
  void ConfirmPendingDirectCommitLearning(absl::string_view);
  void DiscardPendingDirectCommitLearning(absl::string_view);
  void HandlePendingDirectCommitLearningForKeyEvent(const commands::KeyEvent&);
  void HandlePendingDirectCommitLearningForSessionCommand(
      commands::SessionCommand::CommandType);

  bool HasVisibleZenzLiveCorrection() const;
  void SetPendingZenzFeedbackAccepted(absl::string_view,absl::string_view,
                                      absl::string_view);
  void SetPendingZenzFeedbackRejected(absl::string_view);
  void ObservePendingZenzFeedbackCommittedResult(const commands::Command&,
                                                  absl::string_view);
  void ConfirmPendingZenzFeedback();
  void DiscardPendingZenzFeedback(absl::string_view);
  void HandlePendingZenzFeedbackForKeyEvent(const commands::KeyEvent&);
  void HandlePendingZenzFeedbackForSessionCommand(
      commands::SessionCommand::CommandType);
  void CancelPendingZenzLiveCorrection();
  void ClearZenzLiveCorrectionState();

  void OutputFromState(commands::Command*);
  void Output(commands::Command*);
  void OutputMode(commands::Command*) const;
  void OutputComposition(commands::Command*) const;
  void OutputKey(commands::Command*) const;
  bool ExecuteCommandSequence(const keymap::CommandSequence&,commands::Command*);
  bool ExecuteCommandSequenceWithInitialOutput(
      const keymap::CommandSequence&,const commands::Output*,commands::Command*);
  bool ExecuteDirectInputCommand(keymap::DirectInputState::Commands,commands::Command*);
  bool ExecutePrecompositionCommand(keymap::PrecompositionState::Commands,commands::Command*);
  bool ExecuteCompositionCommand(keymap::CompositionState::Commands,commands::Command*);
  bool ExecuteConversionCommand(keymap::ConversionState::Commands,commands::Command*);
  bool ExecuteCommandName(const std::string&,commands::Command*);
  bool SendKeyDirectInputState(commands::Command*);
  bool SendKeyPrecompositionState(commands::Command*);
  bool SendKeyCompositionState(commands::Command*);
  bool SendKeyConversionState(commands::Command*);
  bool MoveCursorToEndInternal(commands::Command*,bool);
  void UpdateTime();
  void UpdatePreferences(commands::Command*);
  void TransformInput(commands::Input*);
  void EnsureIMEIsOn();
  bool CanStartAutoConversion(const commands::KeyEvent&) const;
  bool CanDirectCommitPendingLiveConversionBeforeInsert(const commands::KeyEvent&) const;
  bool CanDirectCommitAfterPunctuation(const commands::KeyEvent&) const;
  bool HandleIndirectImeOnOff(commands::Command*);
  bool CommitRawText(commands::Command*);
};
} }  // namespace mozc::session
#endif  // MOZC_SESSION_SESSION_H_
