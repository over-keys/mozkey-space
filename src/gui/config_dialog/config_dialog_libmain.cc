// Copyright 2010-2021, Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include <QApplication>
#include <QCheckBox>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLayout>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QWidget>
#include <QtGui>

#include <algorithm>
#include <string>
#include <vector>

#include "base/system_util.h"
#include "gui/base/singleton_window_helper.h"
#include "gui/base/util.h"
#include "gui/config_dialog/config_dialog.h"

namespace {

// Insert one row into an existing QGridLayout without rebuilding the .ui file.
// QGridLayout::takeAt transfers ownership of the QLayoutItem, so the same item
// can be re-added at its shifted position while preserving spans/alignment.
void InsertGridRow(QGridLayout* grid, int row_to_insert) {
  if (grid == nullptr || row_to_insert < 0) {
    return;
  }

  struct MovedItem {
    QLayoutItem* item = nullptr;
    int row = 0;
    int column = 0;
    int row_span = 1;
    int column_span = 1;
    Qt::Alignment alignment;
  };

  std::vector<MovedItem> moved;
  for (int i = grid->count() - 1; i >= 0; --i) {
    int row = 0;
    int column = 0;
    int row_span = 1;
    int column_span = 1;
    grid->getItemPosition(i, &row, &column, &row_span, &column_span);
    const bool starts_at_or_below = row >= row_to_insert;
    const bool spans_inserted_row =
        row < row_to_insert && row + row_span > row_to_insert;
    if (!starts_at_or_below && !spans_inserted_row) {
      continue;
    }
    QLayoutItem* item = grid->takeAt(i);
    if (item != nullptr) {
      moved.push_back({item,
                       starts_at_or_below ? row + 1 : row,
                       column,
                       spans_inserted_row ? row_span + 1 : row_span,
                       column_span,
                       item->alignment()});
    }
  }

  std::reverse(moved.begin(), moved.end());
  for (MovedItem& moved_item : moved) {
    grid->addItem(moved_item.item, moved_item.row, moved_item.column,
                  moved_item.row_span, moved_item.column_span,
                  moved_item.alignment);
  }
}


}  // namespace

void mozc::gui::ConfigDialog::InitializeZenzControls() {
  auto* dialog = this;

  auto* live =
      dialog->findChild<QCheckBox*>(QStringLiteral("liveConversionCheckBox"));
  auto* zenz =
      dialog->findChild<QCheckBox*>(QStringLiteral("zenzLiveCorrectionCheckBox"));
  auto* feedback = dialog->findChild<QCheckBox*>(
      QStringLiteral("zenzFeedbackLearningCheckBox"));
  auto* auto_block = dialog->findChild<QCheckBox*>(
      QStringLiteral("zenzFeedbackAutoBlockCheckBox"));
  auto* full_threshold = dialog->findChild<QSpinBox*>(
      QStringLiteral("zenzFeedbackAutoBlockRejectThresholdSpinBox"));
  auto* full_threshold_label = dialog->findChild<QLabel*>(
      QStringLiteral("zenzFeedbackAutoBlockRejectThresholdLabel"));
  auto* right_context = dialog->findChild<QCheckBox*>(
      QStringLiteral("zenzLiveCorrectionRightContextCheckBox"));
  auto* delay_label = dialog->findChild<QLabel*>(
      QStringLiteral("zenzLiveCorrectionDelayLabel"));
  auto* delay_spin = dialog->findChild<QSpinBox*>(
      QStringLiteral("zenzLiveCorrectionDelaySpinBox"));
  if (zenz == nullptr || feedback == nullptr || auto_block == nullptr ||
      full_threshold == nullptr || full_threshold_label == nullptr ||
      right_context == nullptr || delay_label == nullptr ||
      delay_spin == nullptr) {
    return;
  }

  zenz->setText(QString::fromUtf8("Zenz 補正を有効にする"));
  zenz->setToolTip(QString::fromUtf8(
      "ライブ変換と Space/通常変換の結果に対して、ローカルの Zenz モデルで文脈補正を行います。"));

  feedback->setText(QString::fromUtf8("Zenzの選択結果を学習する"));
  feedback->setToolTip(QString::fromUtf8(
      "raw Zenzがユーザー判断の対象になった場合に採用・却下を記録します。"
      "Local後の無編集確定はFull・Localとも中立です。rawへ明示的に戻した場合はrawの採用になります。"));
  auto_block->setText(
      QString::fromUtf8("繰り返し却下した全文候補を自動ブロック"));
  auto_block->setToolTip(QString::fromUtf8(
      "同じ全文Zenz候補の通常却下が設定回数に達したとき、その候補を動的に抑制します。"));

  // The proto/runtime already supports a bounded 0-128 character left-context
  // length.  Expose it here without forking the upstream .ui file.
  QWidget* zenz_parent = delay_label->parentWidget();
  auto* left_context_label =
      new QLabel(QString::fromUtf8("Zenz 補正の左文脈長"), zenz_parent);
  left_context_label->setObjectName(
      QStringLiteral("zenzLiveCorrectionLeftContextLengthLabel"));
  left_context_label->setToolTip(QString::fromUtf8(
      "Zenz に渡す確定済み左文脈の最大文字数です。0 にすると左文脈を使いません。"
      "既定値は 24 文字です。"));

  auto* left_context_spin = new QSpinBox(zenz_parent);
  left_context_spin->setObjectName(
      QStringLiteral("zenzLiveCorrectionLeftContextLengthSpinBox"));
  left_context_spin->setRange(0, 128);
  left_context_spin->setSuffix(QString::fromUtf8(" 文字"));
  left_context_spin->setSpecialValueText(QString::fromUtf8("使わない"));
  left_context_spin->setValue(static_cast<int>(std::clamp<uint32_t>(
      dialog->zenz_live_correction_left_context_length_for_ui(), 0, 128)));
  left_context_spin->setToolTip(left_context_label->toolTip());

  if (zenz_parent != nullptr) {
    if (auto* grid = qobject_cast<QGridLayout*>(zenz_parent->layout())) {
      const int delay_index = grid->indexOf(delay_label);
      int delay_row = -1;
      int delay_column = -1;
      int delay_row_span = 1;
      int delay_column_span = 1;
      if (delay_index >= 0) {
        grid->getItemPosition(delay_index, &delay_row, &delay_column,
                              &delay_row_span, &delay_column_span);
      }
      if (delay_row >= 0) {
        InsertGridRow(grid, delay_row);
        grid->addWidget(left_context_label, delay_row, 0, 1, 3);
        grid->addWidget(left_context_spin, delay_row, 3, 1, 5);
      }
    } else if (QLayout* layout = zenz_parent->layout()) {
      auto* row_widget = new QWidget(zenz_parent);
      auto* row_layout = new QHBoxLayout(row_widget);
      row_layout->setContentsMargins(0, 0, 0, 0);
      row_layout->addWidget(left_context_label);
      row_layout->addStretch();
      row_layout->addWidget(left_context_spin);
      layout->addWidget(row_widget);
    }
  }

  QObject::connect(
      left_context_spin, qOverload<int>(&QSpinBox::valueChanged), dialog,
      [dialog](int value) {
        dialog->set_zenz_live_correction_left_context_length_for_ui(
            static_cast<uint32_t>(std::clamp(value, 0, 128)));
      });
  QObject::connect(left_context_spin, SIGNAL(valueChanged(int)), dialog,
                   SLOT(EnableApplyButton()));

  full_threshold_label->setText(
      QString::fromUtf8("全文候補を自動ブロックするまでの拒否回数"));
  full_threshold_label->setToolTip(QString::fromUtf8(
      "同じ全文 Zenz 候補を動的に自動ブロックするまでの通常却下回数です。"));
  full_threshold->setToolTip(full_threshold_label->toolTip());

  // Local Preference has a deliberately independent enable switch and threshold.
  // Keep both as dynamic controls so current-main's .ui file and generated uic
  // header do not need to be forked just for the learning policy.
  QWidget* threshold_parent = full_threshold_label->parentWidget();
  auto* local_learning = new QCheckBox(
      QString::fromUtf8("局所表記の選好を学習する"), threshold_parent);
  local_learning->setObjectName(
      QStringLiteral("zenzLocalPreferenceLearningCheckBox"));
  local_learning->setChecked(
      dialog->use_zenz_local_preference_learning_for_ui());
  local_learning->setToolTip(QString::fromUtf8(
      "Zenz補正を別の表記で確定したとき、安全に局所化できた表記の選好を保存し、"
      "同じ読みの今後のZenz補正に利用します。OFFにすると新規記録と保存済み選好の"
      "利用を停止しますが、保存済みデータ自体は削除しません。"));

  auto* local_threshold_label = new QLabel(
      QString::fromUtf8("局所補正を有効にするまでの修正回数"), threshold_parent);
  local_threshold_label->setObjectName(
      QStringLiteral("zenzLocalPreferenceThresholdLabel"));
  local_threshold_label->setToolTip(QString::fromUtf8(
      "同じminimal reading + raw Zenz + 修正後表記の明示修正を何回確認したら、決定論的な局所補正に使うかを指定します。"
      "文脈クラスをまたいで同じruleへ合算します。自動補正をそのまま確定してもcountは増えません。"
      "適用時には現在Mozcが修正後表記を一意に支持することを必須とします。"));

  auto* local_threshold = new QSpinBox(threshold_parent);
  local_threshold->setObjectName(
      QStringLiteral("zenzLocalPreferenceThresholdSpinBox"));
  local_threshold->setRange(1, 255);
  local_threshold->setSuffix(QString::fromUtf8(" 回"));
  local_threshold->setValue(static_cast<int>(std::clamp<uint32_t>(
      dialog->zenz_local_preference_threshold_for_ui(), 1, 255)));
  local_threshold->setToolTip(local_threshold_label->toolTip());

  auto* max_entries_label = new QLabel(
      QString::fromUtf8("Zenz学習の最大保持件数"), threshold_parent);
  max_entries_label->setToolTip(QString::fromUtf8(
      "全文フィードバックと局所表記に、それぞれ独立して適用する最大logical entry数です。"));
  auto* max_entries = new QSpinBox(threshold_parent);
  max_entries->setObjectName(QStringLiteral("zenzFeedbackMaxEntriesSpinBox"));
  max_entries->setRange(100, 20000);
  max_entries->setSingleStep(100);
  max_entries->setSuffix(QString::fromUtf8(" 件"));
  max_entries->setValue(static_cast<int>(std::clamp<uint32_t>(
      dialog->zenz_feedback_max_entries_for_ui(), 100, 20000)));
  max_entries->setToolTip(max_entries_label->toolTip());

  if (threshold_parent != nullptr) {
    if (auto* grid = qobject_cast<QGridLayout*>(threshold_parent->layout())) {
      int local_row = grid->rowCount();

      // Place the local controls immediately after the existing full-sequence
      // threshold.  Shift every row below it, rather than assuming the feedback
      // management row is the only following row.
      const int full_index = grid->indexOf(full_threshold_label);
      if (full_index >= 0) {
        int full_row = -1;
        int full_column = -1;
        int full_row_span = 1;
        int full_column_span = 1;
        grid->getItemPosition(full_index, &full_row, &full_column,
                              &full_row_span, &full_column_span);
        if (full_row >= 0) {
          local_row = full_row + full_row_span;
          InsertGridRow(grid, local_row);
          InsertGridRow(grid, local_row + 1);
          InsertGridRow(grid, local_row + 2);
        }
      }

      grid->addWidget(local_learning, local_row, 0, 1, 8);
      grid->addWidget(local_threshold_label, local_row + 1, 0, 1, 3);
      grid->addWidget(local_threshold, local_row + 1, 3, 1, 5);
      grid->addWidget(max_entries_label, local_row + 2, 0, 1, 3);
      grid->addWidget(max_entries, local_row + 2, 3, 1, 5);
    } else if (QLayout* layout = threshold_parent->layout()) {
      layout->addWidget(local_learning);
      auto* row_widget = new QWidget(threshold_parent);
      auto* row_layout = new QHBoxLayout(row_widget);
      row_layout->setContentsMargins(0, 0, 0, 0);
      row_layout->addWidget(local_threshold_label);
      row_layout->addStretch();
      row_layout->addWidget(local_threshold);
      layout->addWidget(row_widget);
      auto* max_row_widget = new QWidget(threshold_parent);
      auto* max_row_layout = new QHBoxLayout(max_row_widget);
      max_row_layout->setContentsMargins(0, 0, 0, 0);
      max_row_layout->addWidget(max_entries_label);
      max_row_layout->addStretch();
      max_row_layout->addWidget(max_entries);
      layout->addWidget(max_row_widget);
    }
  }

  QObject::connect(
      local_learning, &QCheckBox::stateChanged, dialog,
      [dialog](int state) {
        dialog->set_use_zenz_local_preference_learning_for_ui(
            state == Qt::Checked);
      });
  QObject::connect(local_learning, SIGNAL(stateChanged(int)), dialog,
                   SLOT(EnableApplyButton()));
  QObject::connect(
      local_threshold, qOverload<int>(&QSpinBox::valueChanged), dialog,
      [dialog](int value) {
        dialog->set_zenz_local_preference_threshold_for_ui(
            static_cast<uint32_t>(std::clamp(value, 1, 255)));
      });
  // Reuse ConfigDialog's existing modified-state machinery rather than
  // maintaining a second Apply/OK path for the dynamic controls.
  QObject::connect(local_threshold, SIGNAL(valueChanged(int)), dialog,
                   SLOT(EnableApplyButton()));
  QObject::connect(
      max_entries, qOverload<int>(&QSpinBox::valueChanged), dialog,
      [dialog](int value) {
        dialog->set_zenz_feedback_max_entries_for_ui(
            static_cast<uint32_t>(std::clamp(value, 100, 20000)));
      });
  QObject::connect(max_entries, SIGNAL(valueChanged(int)), dialog,
                   SLOT(EnableApplyButton()));

  delay_label->setText(QString::fromUtf8("Zenz 補正開始の遅延"));
  delay_label->setToolTip(
      QString::fromUtf8("Zenz "
                        "補正を開始するまでの待ち時間です。直接表示が有効な場合"
                        "は使用しません。"));
  delay_spin->setToolTip(QString::fromUtf8(
      "Zenz 補正を開始するまでの待ち時間です。0 ms "
      "で即時開始します。直接表示が有効な場合は使用しません。"));

  auto update = [dialog, live, zenz, feedback, auto_block, full_threshold,
                 full_threshold_label, local_learning, local_threshold,
                 local_threshold_label, max_entries_label, max_entries,
                 left_context_label,
                 left_context_spin, right_context]() {
    (void)live;  // Live conversion intentionally does not gate Zenz anymore.
    zenz->setEnabled(true);
    const bool zenz_enabled = zenz->isChecked();
    // Delay controls are owned by SelectZenzLiveCorrectionSetting(), which
    // also accounts for direct display. Do not override their enabled state.
    const char* zenz_controls[] = {
        "zenzLiveCorrectionMinKeyLengthLabel",
        "zenzLiveCorrectionMinKeyLengthSpinBox",
        "zenzLiveCorrectionProfileLabel",
        "zenzLiveCorrectionProfileLineEdit",
        "zenzLiveCorrectionTopicLabel",
        "zenzLiveCorrectionTopicLineEdit",
        "zenzLiveCorrectionStyleLabel",
        "zenzLiveCorrectionStyleLineEdit",
        "zenzLiveCorrectionSettingsLabel",
        "zenzLiveCorrectionSettingsLineEdit",
    };
    for (const char* name : zenz_controls) {
      if (auto* widget =
              dialog->findChild<QWidget*>(QString::fromLatin1(name))) {
        widget->setEnabled(zenz_enabled);
      }
    }
    left_context_label->setEnabled(zenz_enabled);
    left_context_spin->setEnabled(zenz_enabled);

    right_context->setEnabled(zenz_enabled);
    const bool right_enabled = zenz_enabled && right_context->isChecked();
    if (auto* label = dialog->findChild<QLabel*>(
            QStringLiteral("zenzLiveCorrectionRightContextLengthLabel"))) {
      label->setEnabled(right_enabled);
    }
    if (auto* spin = dialog->findChild<QSpinBox*>(
            QStringLiteral("zenzLiveCorrectionRightContextLengthSpinBox"))) {
      spin->setEnabled(right_enabled);
    }

    feedback->setEnabled(zenz_enabled);
    const bool feedback_enabled = zenz_enabled && feedback->isChecked();
    auto_block->setEnabled(feedback_enabled);

    // Full-sequence auto-block and Local Preference are independent children
    // of the parent feedback switch.  Preserve each child setting while its
    // controls are disabled so toggling the parent never destroys user choices.
    const bool full_block_controls_enabled =
        feedback_enabled && auto_block->isChecked();
    full_threshold_label->setEnabled(full_block_controls_enabled);
    full_threshold->setEnabled(full_block_controls_enabled);
    local_learning->setEnabled(feedback_enabled);
    const bool local_controls_enabled =
        feedback_enabled && local_learning->isChecked();
    local_threshold_label->setEnabled(local_controls_enabled);
    local_threshold->setEnabled(local_controls_enabled);
    max_entries_label->setEnabled(feedback_enabled);
    max_entries->setEnabled(feedback_enabled);
  };

  // ResetToDefaults() updates base_config_ through ConvertFromProto(), but this
  // dynamically-created spin box is outside Ui::ConfigDialog.  Refresh it and
  // the dependent enabled states after the existing reset handler returns.  A
  // cancelled reset leaves base_config_ unchanged, so the previous value is
  // restored without special-case state.
  if (auto* reset = dialog->findChild<QPushButton*>(
          QStringLiteral("resetToDefaultsButton"))) {
    QObject::connect(
        reset, &QPushButton::clicked, dialog,
        [dialog, local_learning, local_threshold, max_entries,
         left_context_spin, update]() {
          QTimer::singleShot(
              0, dialog,
              [dialog, local_learning, local_threshold, max_entries,
               left_context_spin, update]() {
                local_learning->setChecked(
                    dialog->use_zenz_local_preference_learning_for_ui());
                local_threshold->setValue(static_cast<int>(
                    std::clamp<uint32_t>(
                        dialog->zenz_local_preference_threshold_for_ui(), 1,
                        255)));
                max_entries->setValue(static_cast<int>(
                    std::clamp<uint32_t>(
                        dialog->zenz_feedback_max_entries_for_ui(), 100,
                        20000)));
                left_context_spin->setValue(static_cast<int>(
                    std::clamp<uint32_t>(
                        dialog->zenz_live_correction_left_context_length_for_ui(),
                        0, 128)));
                update();
              });
        });
  }

  if (live != nullptr) {
    QObject::connect(live, &QCheckBox::stateChanged, dialog,
                     [update](int) { update(); });
  }
  QObject::connect(zenz, &QCheckBox::stateChanged, dialog,
                   [update](int) { update(); });
  QObject::connect(feedback, &QCheckBox::stateChanged, dialog,
                   [update](int) { update(); });
  QObject::connect(auto_block, &QCheckBox::stateChanged, dialog,
                   [update](int) { update(); });
  QObject::connect(local_learning, &QCheckBox::stateChanged, dialog,
                   [update](int) { update(); });
  QObject::connect(right_context, &QCheckBox::stateChanged, dialog,
                   [update](int) { update(); });

  update();
}

int RunConfigDialog(int argc, char *argv[]) {
  Q_INIT_RESOURCE(qrc_config_dialog);
  auto app = mozc::gui::GuiUtil::InitQt(argc, argv);

  std::string name = "config_dialog.";
  name += mozc::SystemUtil::GetDesktopNameAsString();
  mozc::gui::SingletonWindowHelper window_helper(name);
  if (window_helper.FindPreviousWindow()) {
    window_helper.ActivatePreviousWindow();
    return -1;
  }

  mozc::gui::GuiUtil::InstallTranslator("config_dialog");
  mozc::gui::GuiUtil::InstallTranslator("keymap");
  mozc::gui::ConfigDialog mozc_config;

  mozc_config.show();
  mozc_config.raise();
  return app->exec();
}
