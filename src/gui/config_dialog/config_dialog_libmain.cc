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

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QGroupBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLayout>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <QtGui>

#include <algorithm>
#include <string>
#include <vector>

#include "absl/strings/string_view.h"
#include "base/system_util.h"
#include "gui/base/singleton_window_helper.h"
#include "gui/base/util.h"
#include "gui/config_dialog/config_dialog.h"
#include "session/zenz_feedback_store.h"

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

QString ToQString(absl::string_view text) {
  return QString::fromUtf8(text.data(), static_cast<int>(text.size()));
}

void SetReadOnlyTableItem(QTableWidget* table, int row, int column,
                          const QString& text) {
  auto* item = new QTableWidgetItem(text);
  item->setFlags(item->flags() & ~Qt::ItemIsEditable);
  table->setItem(row, column, item);
}

QString FullFeedbackStateLabel(absl::string_view reason) {
  if (reason == "feedback_preferred") {
    return QString::fromUtf8("優先スコアあり");
  }
  if (reason == "feedback_hard_rejected") {
    return QString::fromUtf8("手動ブロック中");
  }
  if (reason == "feedback_auto_blocked") {
    return QString::fromUtf8("自動ブロック中");
  }
  if (reason == "feedback_reject_count_dominant") {
    return QString::fromUtf8("却下数優勢");
  }
  if (reason == "feedback_rejected" || reason == "feedback_downgraded") {
    return QString::fromUtf8("却下スコアあり");
  }
  return QString::fromUtf8("中立");
}

QString LocalPreferenceStateLabel(
    const mozc::session::ZenzLocalPreferenceEntry& entry, int threshold) {
  const int effective = entry.effective_observation_count;
  const int opposite = entry.opposite_effective_observation_count;
  if (opposite > 0 && opposite >= effective) {
    if (opposite == effective) {
      return QString::fromUtf8("競合（同数）");
    }
    return QString::fromUtf8("競合（反対方向が優勢）");
  }
  if (effective < threshold) {
    QString state = QString::fromUtf8("学習中 (%1/%2)")
                        .arg(effective)
                        .arg(threshold);
    if (opposite > 0) {
      state += QString::fromUtf8(" / 競合あり");
    }
    return state;
  }
  QString state = QString::fromUtf8("局所補正有効");
  if (opposite > 0) {
    state += QString::fromUtf8(" / 競合あり");
  }
  return state;
}

void ShowInfo(QWidget* parent, const QString& title, const QString& text) {
  QMessageBox box(parent);
  box.setWindowTitle(title);
  box.setIcon(QMessageBox::Information);
  box.setText(text);
  box.addButton(QString::fromUtf8("OK"), QMessageBox::AcceptRole);
  box.exec();
}

void ShowError(QWidget* parent, const QString& title, const QString& text) {
  QMessageBox box(parent);
  box.setWindowTitle(title);
  box.setIcon(QMessageBox::Critical);
  box.setText(text);
  box.addButton(QString::fromUtf8("OK"), QMessageBox::AcceptRole);
  box.exec();
}

void ShowRev10ZenzFeedbackManagementDialog(
    QWidget* parent, QCheckBox* auto_block_checkbox,
    QSpinBox* full_threshold_spinbox, QSpinBox* local_threshold_spinbox,
    QSpinBox* max_entries_spinbox) {
  QDialog dialog(parent);
  dialog.setWindowTitle(QString::fromUtf8("Zenz 学習データの管理"));
  dialog.resize(920, 650);

  mozc::session::ZenzFeedbackStore store;
  const mozc::config::Config default_config;
  const int full_threshold = std::max(
      1, full_threshold_spinbox == nullptr
             ? static_cast<int>(default_config.zenz_auto_block_reject_threshold())
             : full_threshold_spinbox->value());
  const int local_threshold = std::max(
      1, local_threshold_spinbox == nullptr
             ? static_cast<int>(default_config.zenz_local_preference_threshold())
             : local_threshold_spinbox->value());
  const int max_entries = std::clamp(
      max_entries_spinbox == nullptr
          ? static_cast<int>(default_config.zenz_feedback_max_entries())
          : max_entries_spinbox->value(),
      100, 20000);
  (void)store.Maintenance(static_cast<size_t>(max_entries));
  mozc::session::ZenzFeedbackAutoBlockPolicy auto_block_policy;
  auto_block_policy.enabled =
      auto_block_checkbox != nullptr && auto_block_checkbox->isChecked();
  auto_block_policy.reject_threshold = full_threshold;

  auto* root = new QVBoxLayout(&dialog);
  auto* intro = new QLabel(
      QString::fromUtf8(
          "Zenz 学習を、全文フィードバックと局所表記に分けて表示します。"
          "局所表記は1回目から記録されますが、設定した修正回数に達するまでは"
          "決定論的な出力側局所補正には使われません。"),
      &dialog);
  intro->setWordWrap(true);
  root->addWidget(intro);

  auto* details = new QPushButton(QString::fromUtf8("詳しく..."), &dialog);
  details->setFixedWidth(90);
  root->addWidget(details, 0, Qt::AlignRight);

  auto* search_layout = new QHBoxLayout;
  search_layout->addWidget(new QLabel(QString::fromUtf8("検索:"), &dialog));
  auto* search = new QLineEdit(&dialog);
    search->setPlaceholderText(QString::fromUtf8(
      "読み、raw Zenz/修正後表記で絞り込み"));
  search_layout->addWidget(search);
  root->addLayout(search_layout);

  auto* full_group =
      new QGroupBox(QString::fromUtf8("全文フィードバック"), &dialog);
  auto* full_layout = new QVBoxLayout(full_group);
  auto* full_table = new QTableWidget(full_group);
  full_table->setColumnCount(6);
  full_table->setHorizontalHeaderLabels(
      QStringList() << QString::fromUtf8("読み")
                    << QString::fromUtf8("Zenz候補")
                    << QString::fromUtf8("文脈クラス")
                    << QString::fromUtf8("採用")
                    << QString::fromUtf8("却下")
                    << QString::fromUtf8("状態"));
  full_table->setSelectionBehavior(QAbstractItemView::SelectRows);
  full_table->setSelectionMode(QAbstractItemView::SingleSelection);
  full_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  full_table->horizontalHeader()->setStretchLastSection(true);
  full_table->setMinimumHeight(150);
  full_layout->addWidget(full_table);
  auto* full_buttons = new QHBoxLayout;
  auto* block_full =
      new QPushButton(QString::fromUtf8("この補正をブロック"), full_group);
  auto* delete_full = new QPushButton(
      QString::fromUtf8("選択した全文データを削除"), full_group);
  full_buttons->addWidget(block_full);
  full_buttons->addWidget(delete_full);
  full_buttons->addStretch();
  full_layout->addLayout(full_buttons);
  root->addWidget(full_group, 1);

  auto* local_group =
      new QGroupBox(QString::fromUtf8("局所表記"), &dialog);
  auto* local_layout = new QVBoxLayout(local_group);
  auto* local_table = new QTableWidget(local_group);
  local_table->setColumnCount(5);
  local_table->setHorizontalHeaderLabels(
      QStringList() << QString::fromUtf8("読み")
                    << QString::fromUtf8("修正後表記")
                    << QString::fromUtf8("raw Zenz表記")
                    << QString::fromUtf8("回数")
                    << QString::fromUtf8("状態"));
  local_table->setSelectionBehavior(QAbstractItemView::SelectRows);
  local_table->setSelectionMode(QAbstractItemView::SingleSelection);
  local_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  local_table->horizontalHeader()->setStretchLastSection(true);
  local_table->setMinimumHeight(150);
  local_layout->addWidget(local_table);
  auto* local_buttons = new QHBoxLayout;
  auto* delete_local = new QPushButton(
      QString::fromUtf8("選択した局所表記を削除"), local_group);
  local_buttons->addWidget(delete_local);
  local_buttons->addStretch();
  local_layout->addLayout(local_buttons);
  root->addWidget(local_group, 1);

  auto* status = new QLabel(&dialog);
  status->setWordWrap(true);
  root->addWidget(status);

  auto* bottom = new QHBoxLayout;
  auto* import_button =
      new QPushButton(QString::fromUtf8("インポート..."), &dialog);
  auto* export_button =
      new QPushButton(QString::fromUtf8("エクスポート..."), &dialog);
  auto* clear_button =
      new QPushButton(QString::fromUtf8("すべて削除"), &dialog);
  auto* close_box = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
  if (auto* close = close_box->button(QDialogButtonBox::Close)) {
    close->setText(QString::fromUtf8("閉じる"));
  }
  bottom->addWidget(import_button);
  bottom->addWidget(export_button);
  bottom->addWidget(clear_button);
  bottom->addStretch();
  bottom->addWidget(close_box);
  root->addLayout(bottom);

  auto update_selection_buttons = [&]() {
    delete_full->setEnabled(full_table->currentRow() >= 0);
    delete_local->setEnabled(local_table->currentRow() >= 0);
    const int full_row = full_table->currentRow();
    const bool full_hard_rejected =
        full_row >= 0 && full_table->item(full_row, 5) != nullptr &&
        full_table->item(full_row, 5)->data(Qt::UserRole).toString() ==
            QStringLiteral("feedback_hard_rejected");
    block_full->setEnabled(full_row >= 0 && !full_hard_rejected);
  };

  auto reload = [&]() {
    const QString filter = search->text();
    const auto full_entries = store.ListEntries(auto_block_policy);
    const auto local_entries = store.ListLocalPreferenceEntries();
    full_table->setRowCount(0);
    local_table->setRowCount(0);
    int visible_full = 0;
    int visible_local = 0;

    for (const auto& entry : full_entries) {
      const QString key = ToQString(entry.key);
      const QString value = ToQString(entry.value);
      const QString context = ToQString(entry.context_class);
      if (!filter.isEmpty() && !key.contains(filter, Qt::CaseInsensitive) &&
          !value.contains(filter, Qt::CaseInsensitive) &&
          !context.contains(filter, Qt::CaseInsensitive)) {
        continue;
      }
      const int row = full_table->rowCount();
      full_table->insertRow(row);
      SetReadOnlyTableItem(full_table, row, 0, key);
      SetReadOnlyTableItem(full_table, row, 1, value);
      SetReadOnlyTableItem(full_table, row, 2, context);
      QString accepted = QString::number(entry.accepted_count);
      if (entry.effective_accepted_count != entry.accepted_count) {
        accepted += QString::fromUtf8(" (有効%1)")
                        .arg(entry.effective_accepted_count);
      }
      QString rejected = QString::number(entry.rejected_count);
      if (entry.effective_rejected_count != entry.rejected_count) {
        rejected += QString::fromUtf8(" (有効%1)")
                        .arg(entry.effective_rejected_count);
      }
      SetReadOnlyTableItem(full_table, row, 3, accepted);
      SetReadOnlyTableItem(full_table, row, 4, rejected);
      SetReadOnlyTableItem(full_table, row, 5,
                           FullFeedbackStateLabel(entry.reason));
      full_table->item(row, 0)->setData(Qt::UserRole, key);
      full_table->item(row, 1)->setData(Qt::UserRole, value);
      full_table->item(row, 2)->setData(Qt::UserRole, context);
      full_table->item(row, 5)->setData(Qt::UserRole, ToQString(entry.reason));
      ++visible_full;
    }

    for (const auto& entry : local_entries) {
      const QString key = ToQString(entry.key);
      const QString preferred = ToQString(entry.preferred_value);
      const QString disfavored = ToQString(entry.disfavored_value);
      if (!filter.isEmpty() && !key.contains(filter, Qt::CaseInsensitive) &&
          !preferred.contains(filter, Qt::CaseInsensitive) &&
          !disfavored.contains(filter, Qt::CaseInsensitive)) {
        continue;
      }
      const int row = local_table->rowCount();
      local_table->insertRow(row);
      SetReadOnlyTableItem(local_table, row, 0, key);
      SetReadOnlyTableItem(local_table, row, 1, preferred);
      SetReadOnlyTableItem(local_table, row, 2, disfavored);
      SetReadOnlyTableItem(local_table, row, 3,
                           QString::number(entry.observation_count));
      SetReadOnlyTableItem(local_table, row, 4,
                           LocalPreferenceStateLabel(entry, local_threshold));
      local_table->item(row, 0)->setData(Qt::UserRole, key);
      local_table->item(row, 1)->setData(Qt::UserRole, preferred);
      local_table->item(row, 2)->setData(Qt::UserRole, disfavored);
      ++visible_local;
    }

    full_table->resizeColumnsToContents();
    local_table->resizeColumnsToContents();
    status->setText(
        QString::fromUtf8(
            "全文 %1/%2 件 / 局所 %3/%4 件 / 全文ブロック %5 回 / "
            "局所成立 %6 回%7")
            .arg(visible_full)
            .arg(static_cast<int>(full_entries.size()))
            .arg(visible_local)
            .arg(static_cast<int>(local_entries.size()))
            .arg(full_threshold)
            .arg(local_threshold)
            .arg(auto_block_policy.enabled
                     ? QString::fromUtf8(" / 全文自動ブロック ON")
                     : QString::fromUtf8(" / 全文自動ブロック OFF")));
    const bool has_any = !full_entries.empty() || !local_entries.empty();
    export_button->setEnabled(has_any);
    clear_button->setEnabled(has_any);
    update_selection_buttons();
  };

  QObject::connect(details, &QPushButton::clicked, &dialog, [&]() {
    ShowInfo(
        &dialog, dialog.windowTitle(),
        QString::fromUtf8(
            "【全文フィードバック】\n"
            "raw Zenzがユーザー判断の対象になった場合に限って採用/却下を記録します。"
            "未補正のrawを採用した場合、またはLocal後にrawへ明示的に戻した場合はrawの採用です。"
            "未補正のrawから通常変換へ戻して別の結果を確定するとrawの却下が1回記録されます。"
            "Local後の候補をそのまま確定しても、Fullの採用/却下とLocalの回数は増減しません。"
            "確定した表記の利用履歴はMozc側で学習します。"
            "Local後の候補を編集しただけでは、見えていないrawの却下とは扱いません。"
            "Space 却下1回は弱いマイナス信号で、通常 Mozc 候補を削除しません。"
            "通常の粗い文脈クラス間では実行時と同じ有効回数を合算し、保存行の回数と"
            "異なる場合は「有効N」と併記します。\n\n"
            "【局所表記】\n"
            "raw Zenzと実際の最終確定値を同じ局所readingへ一意に対応できた場合だけ、"
            "raw Zenz表記 > 修正後表記の方向をv4 acceptedとして記録します。"
            "成立済みruleが自動適用され、そのまま確定した場合は自己強化を避けるためcountを増やしません。"
            "介入したruleの表記をユーザーが変更した場合だけ、そのspanをrejectedとして1段弱め、"
            "第三表記ならraw Zenzから新しい最終表記へのacceptedも記録します。"
            "設定した局所成立回数に達するまでは記録だけを保持し、出力には使いません。"
            "到達後もreading/surface alignmentを一意に証明できない場合は補正しません。"
            "Localの成立countは文脈クラスをまたいで同じminimal ruleへ集約します。"
            "適用時には現在Mozcが修正後表記を同じreading intervalで一意に選んでいることを必須とし、"
            "raw Zenz側も同じintervalでraw表記に一致した場合だけ補正します。\n\n"
            "【競合】\n"
            "同じreadingの逆方向観測は消さずに共存します。同数競合なら局所一般化だけで"
            "方向を決めません。\n\n"
            "【プライバシー】\n"
            "生の左右文脈は保存せず、粗い文脈クラスだけを保存します。PASSWORD、"
            "NO_HISTORY、incognitoではpersistent local preferenceを使いません。"
            "READ_ONLYでは参照のみで新規書き込みをしません。\n\n"
            "【Mozc履歴】\n"
            "Zenzを却下してMozcへ戻したことだけを理由にMozc履歴へ人工学習はしません。"
            "実際に選択したMozc候補だけがMozc自身の通常経路で学習されます。"));
  });

  QObject::connect(search, &QLineEdit::textChanged, &dialog,
                   [&](const QString&) { reload(); });
  QObject::connect(full_table, &QTableWidget::itemSelectionChanged, &dialog,
                   [&]() { update_selection_buttons(); });
  QObject::connect(local_table, &QTableWidget::itemSelectionChanged, &dialog,
                   [&]() { update_selection_buttons(); });
  QObject::connect(close_box, &QDialogButtonBox::rejected, &dialog,
                   &QDialog::reject);

  QObject::connect(block_full, &QPushButton::clicked, &dialog, [&]() {
    const int row = full_table->currentRow();
    if (row < 0) return;
    const QString key = full_table->item(row, 0)->data(Qt::UserRole).toString();
    const QString value = full_table->item(row, 1)->data(Qt::UserRole).toString();
    const QString context = full_table->item(row, 2)->data(Qt::UserRole).toString();
    if (QMessageBox::warning(
            &dialog, dialog.windowTitle(),
            QString::fromUtf8("この全文 Zenz 補正を手動ブロックしますか？\n\n読み: %1\n候補: %2")
                .arg(key, value),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Yes) {
      return;
    }
    store.RecordRejected(key.toUtf8().constData(),
                         context.toUtf8().constData(),
                         value.toUtf8().constData(), "hard_reject");
    reload();
  });

  QObject::connect(delete_full, &QPushButton::clicked, &dialog, [&]() {
    const int row = full_table->currentRow();
    if (row < 0) return;
    const QString key = full_table->item(row, 0)->data(Qt::UserRole).toString();
    const QString value = full_table->item(row, 1)->data(Qt::UserRole).toString();
    const QString context = full_table->item(row, 2)->data(Qt::UserRole).toString();
    if (!store.DeleteEntry(key.toUtf8().constData(),
                           context.toUtf8().constData(),
                           value.toUtf8().constData())) {
      ShowError(&dialog, dialog.windowTitle(),
                QString::fromUtf8("全文フィードバックを削除できませんでした。"));
    }
    reload();
  });

  QObject::connect(delete_local, &QPushButton::clicked, &dialog, [&]() {
    const int row = local_table->currentRow();
    if (row < 0) return;
    const QString key = local_table->item(row, 0)->data(Qt::UserRole).toString();
    const QString preferred = local_table->item(row, 1)->data(Qt::UserRole).toString();
    const QString disfavored = local_table->item(row, 2)->data(Qt::UserRole).toString();
    if (!store.DeleteLocalPreference(key.toUtf8().constData(), "",
                                     preferred.toUtf8().constData(),
                                     disfavored.toUtf8().constData())) {
      ShowError(&dialog, dialog.windowTitle(),
                QString::fromUtf8("局所表記を削除できませんでした。"));
    }
    reload();
  });

  QObject::connect(export_button, &QPushButton::clicked, &dialog, [&]() {
    const QString path = QFileDialog::getSaveFileName(
        &dialog, QString::fromUtf8("Zenz 学習データをエクスポート"),
        QStringLiteral("zenz_feedback_v4.tsv"),
        QString::fromUtf8("TSV ファイル (*.tsv);;すべてのファイル (*)"));
    if (path.isEmpty()) return;
    (void)store.Maintenance(static_cast<size_t>(max_entries));
    if (!store.ExportToFile(path.toStdWString())) {
      ShowError(&dialog, dialog.windowTitle(),
                QString::fromUtf8("Zenz 学習データをエクスポートできませんでした。"));
      return;
    }
    ShowInfo(&dialog, dialog.windowTitle(),
             QString::fromUtf8("Zenz 学習データをエクスポートしました。"));
  });

  QObject::connect(import_button, &QPushButton::clicked, &dialog, [&]() {
    const QString path = QFileDialog::getOpenFileName(
        &dialog, QString::fromUtf8("Zenz 学習データをインポート"), QString(),
        QString::fromUtf8("TSV ファイル (*.tsv);;すべてのファイル (*)"));
    if (path.isEmpty()) return;
    QMessageBox box(&dialog);
    box.setWindowTitle(dialog.windowTitle());
    box.setIcon(QMessageBox::Question);
    box.setText(QString::fromUtf8("Zenz 学習データをインポートします。"));
    auto* append = box.addButton(QString::fromUtf8("追加"), QMessageBox::AcceptRole);
    auto* replace = box.addButton(QString::fromUtf8("置き換え"), QMessageBox::DestructiveRole);
    auto* cancel = box.addButton(QString::fromUtf8("キャンセル"), QMessageBox::RejectRole);
    box.setDefaultButton(append);
    box.exec();
    if (box.clickedButton() == cancel) return;
    const auto mode = box.clickedButton() == replace
                          ? mozc::session::ZenzFeedbackImportMode::kReplace
                          : mozc::session::ZenzFeedbackImportMode::kAppend;
    if (!store.ImportFromFile(path.toStdWString(), mode)) {
      ShowError(&dialog, dialog.windowTitle(),
                QString::fromUtf8("Zenz 学習データをインポートできませんでした。"));
      return;
    }
    (void)store.Maintenance(static_cast<size_t>(max_entries));
    reload();
  });

  QObject::connect(clear_button, &QPushButton::clicked, &dialog, [&]() {
    if (QMessageBox::warning(
            &dialog, dialog.windowTitle(),
            QString::fromUtf8("全文・局所を含む Zenz 学習データをすべて削除しますか？"),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Yes) {
      return;
    }
    if (!store.ClearAll()) {
      ShowError(&dialog, dialog.windowTitle(),
                QString::fromUtf8("Zenz 学習データを削除できませんでした。"));
    }
    reload();
  });

  reload();
  dialog.exec();
}

void InstallRev10ConfigDialogIntegration(mozc::gui::ConfigDialog* dialog) {
  if (dialog == nullptr) {
    return;
  }

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
  auto* manage = dialog->findChild<QPushButton*>(
      QStringLiteral("editZenzFeedbackButton"));
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
  // header do not need to be forked just for REV10 learning policy.
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
  delay_label->setToolTip(QString::fromUtf8(
      "Mozc の変換結果が表示された後、Zenz 補正を開始するまでの待ち時間です。"));
  delay_spin->setToolTip(QString::fromUtf8(
      "Mozc の変換結果が表示された後、Zenz 補正を開始するまでの待ち時間です。0 ms は即時です。"));

  auto update = [dialog, live, zenz, feedback, auto_block, full_threshold,
                 full_threshold_label, local_learning, local_threshold,
                 local_threshold_label, max_entries_label, max_entries,
                 left_context_label,
                 left_context_spin, right_context]() {
    (void)live;  // Live conversion intentionally does not gate Zenz anymore.
    zenz->setEnabled(true);
    const bool zenz_enabled = zenz->isChecked();
    const char* zenz_controls[] = {
        "zenzLiveCorrectionDelayLabel",
        "zenzLiveCorrectionDelaySpinBox",
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

  if (manage != nullptr) {
    manage->disconnect(dialog);
    QObject::connect(
        manage, &QPushButton::clicked, dialog,
        [dialog, auto_block, full_threshold, local_threshold, max_entries]() {
          ShowRev10ZenzFeedbackManagementDialog(
              dialog, auto_block, full_threshold, local_threshold, max_entries);
        });
  }

  update();
}

}  // namespace

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
  InstallRev10ConfigDialogIntegration(&mozc_config);

  mozc_config.show();
  mozc_config.raise();
  return app->exec();
}
