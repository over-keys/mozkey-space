// Regression coverage for the same ConfigDialog constructed by RunConfigDialog.
// Never connect to the installed IME or read/write its feedback data.
#include "gui/config_dialog/config_dialog.h"

#include <QApplication>
#include <QByteArray>
#include <QCheckBox>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <functional>
#include <memory>
#include <utility>

#include "client/client.h"
#include "client/client_mock.h"
#include "protocol/config.pb.h"
#include "session/zenz_feedback_store.h"
#include "testing/gmock.h"
#include "testing/gunit.h"

namespace mozc::gui {
namespace {

class TestClientFactory : public client::ClientFactoryInterface {
 public:
  std::unique_ptr<client::ClientInterface> NewClient() override {
    auto mock = std::make_unique<::testing::StrictMock<client::ClientMock>>();
    EXPECT_CALL(*mock, CheckVersionOrRestartServer())
        .WillOnce(::testing::Return(true));
    EXPECT_CALL(*mock, GetConfig(::testing::_))
        .WillOnce([](config::Config* value) {
          value->set_use_live_conversion(true);
          value->set_use_zenz_live_correction(true);
          value->set_use_zenz_feedback_learning(true);
          value->set_use_zenz_local_preference_learning(true);
          value->set_zenz_local_preference_threshold(7);
          return true;
        });
    // Any SetConfig or other call fails the test: opening management must not
    // apply pending settings or communicate with a running server.
    return mock;
  }
};

struct CloseDialog {
  QDialog* dialog;
  ~CloseDialog() {
    if (dialog != nullptr) dialog->reject();
  }
};

// Inspect a modal from its real button connection. The guard closes it even
// when an ASSERT in the callback fails, avoiding a hung test on UI regressions.
void OpenDialog(QWidget& parent, const char* button_name,
                const char* dialog_name,
                const std::function<void(QDialog&)>& inspect) {
  auto* button = parent.findChild<QPushButton*>(QString::fromLatin1(button_name));
  ASSERT_NE(button, nullptr);
  ASSERT_TRUE(button->isEnabled());
  bool visited = false;
  QTimer::singleShot(0, &parent, [&]() {
    auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget());
    CloseDialog close{modal};
    ASSERT_NE(modal, nullptr);
    ASSERT_EQ(modal->objectName(), QString::fromLatin1(dialog_name));
    visited = true;
    inspect(*modal);
  });
  button->click();
  EXPECT_TRUE(visited);
}

class ConfigDialogUiTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(directory_.isValid());
    previous_directory_ = qgetenv("MOZKEY_ZENZ_FEEDBACK_DIRECTORY");
    ASSERT_TRUE(qputenv("MOZKEY_ZENZ_FEEDBACK_DIRECTORY",
                       directory_.path().toUtf8()));
    client::ClientFactory::SetClientFactory(&factory_);
  }
  void TearDown() override {
    client::ClientFactory::SetClientFactory(nullptr);
    if (previous_directory_.isNull()) {
      qunsetenv("MOZKEY_ZENZ_FEEDBACK_DIRECTORY");
    } else {
      qputenv("MOZKEY_ZENZ_FEEDBACK_DIRECTORY", previous_directory_);
    }
  }

  void Manage(ConfigDialog& settings,
              const std::function<void(QDialog&)>& inspect) {
    OpenDialog(settings, "editZenzFeedbackButton",
               "zenzFeedbackManagementDialog", inspect);
  }
  void Manual(QDialog& management,
              const std::function<void(QDialog&)>& inspect) {
    OpenDialog(management, "zenzManualPreferencesButton",
               "zenzManualPreferencesDialog", inspect);
  }

  QTemporaryDir directory_;
  QByteArray previous_directory_;
  TestClientFactory factory_;
};

TEST_F(ConfigDialogUiTest, RealEntryAddsReopensAndRemovesManualPreference) {
  session::ZenzFeedbackStore store;
  // Existing automatic evidence must survive adding/removing a manual marker.
  store.RecordLocalAccepted("よい", "empty", "よい", "良い");
  ConfigDialog settings;
  auto* threshold = settings.findChild<QSpinBox*>(
      QStringLiteral("zenzLocalPreferenceThresholdSpinBox"));
  ASSERT_NE(threshold, nullptr);
  EXPECT_EQ(threshold->value(), 7);

  Manage(settings, [&](QDialog& management) {
    EXPECT_EQ(management.findChildren<QTableWidget*>().size(), 2);
    Manual(management, [&](QDialog& manual) {
      auto* table = manual.findChild<QTableWidget*>();
      ASSERT_NE(table, nullptr);
      EXPECT_EQ(table->rowCount(), 0);
      for (const auto& input : {std::pair{"zenzManualKey", "よい"},
                                std::pair{"zenzManualRaw", "よい"},
                                std::pair{"zenzManualPreferred", "良い"}}) {
        auto* edit = manual.findChild<QLineEdit*>(input.first);
        ASSERT_NE(edit, nullptr);
        edit->setText(QString::fromUtf8(input.second));
      }
      auto* add = manual.findChild<QPushButton*>("zenzManualAdd");
      ASSERT_NE(add, nullptr);
      add->click();
      EXPECT_EQ(table->rowCount(), 1);
    });
    auto* table = management.findChild<QTableWidget*>("zenzLocalPreferencesTable");
    ASSERT_NE(table, nullptr);
    ASSERT_EQ(table->rowCount(), 1);
    EXPECT_EQ(table->item(0, 4)->text(), QString::fromUtf8("手動設定"));
  });
  auto entries = store.ListLocalPreferenceEntries();
  ASSERT_EQ(entries.size(), 1);
  EXPECT_TRUE(entries[0].manual);
  EXPECT_EQ(entries[0].observation_count, 1);

  Manage(settings, [&](QDialog& management) {
    Manual(management, [&](QDialog& manual) {
      auto* table = manual.findChild<QTableWidget*>();
      ASSERT_NE(table, nullptr);
      ASSERT_EQ(table->rowCount(), 1);
      EXPECT_EQ(table->item(0, 2)->text(), QString::fromUtf8("良い"));
      table->selectRow(0);
      auto* remove = manual.findChild<QPushButton*>("zenzManualDelete");
      ASSERT_NE(remove, nullptr);
      QTimer::singleShot(0, &manual, [&]() {
        auto* confirm = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        CloseDialog close{confirm};
        ASSERT_NE(confirm, nullptr);
        for (auto* button : confirm->buttons()) {
          if (confirm->buttonRole(button) == QMessageBox::DestructiveRole) {
            button->click();
            return;
          }
        }
        FAIL() << "Missing delete confirmation";
      });
      remove->click();
      EXPECT_EQ(table->rowCount(), 0);
    });
  });
  entries = store.ListLocalPreferenceEntries();
  ASSERT_EQ(entries.size(), 1);
  EXPECT_FALSE(entries[0].manual);
  EXPECT_EQ(entries[0].observation_count, 1);
}

TEST_F(ConfigDialogUiTest, ManualWarningUsesPendingSettings) {
  ConfigDialog settings;
  auto* feedback = settings.findChild<QCheckBox*>("zenzFeedbackLearningCheckBox");
  auto* local = settings.findChild<QCheckBox*>("zenzLocalPreferenceLearningCheckBox");
  ASSERT_NE(feedback, nullptr);
  ASSERT_NE(local, nullptr);
  for (const auto& gates : {std::pair{true, true}, std::pair{false, true},
                           std::pair{true, false}}) {
    feedback->setChecked(gates.first);
    local->setChecked(gates.second);
    Manage(settings, [&](QDialog& management) {
      Manual(management, [&](QDialog& manual) {
        EXPECT_EQ(manual.findChild<QLabel*>("zenzManualPreferencesDisabled") != nullptr,
                  !gates.first || !gates.second);
      });
    });
  }
}

TEST_F(ConfigDialogUiTest, ZenzDelayAvailabilityDependsOnlyOnZenz) {
  ConfigDialog settings;
  auto* live = settings.findChild<QCheckBox*>("liveConversionCheckBox");
  auto* zenz = settings.findChild<QCheckBox*>("zenzLiveCorrectionCheckBox");
  auto* direct = settings.findChild<QCheckBox*>(
      "zenzDeferredNormalConversionDisplayCheckBox");
  auto* delay = settings.findChild<QSpinBox*>("zenzLiveCorrectionDelaySpinBox");
  ASSERT_NE(live, nullptr);
  ASSERT_NE(zenz, nullptr);
  ASSERT_NE(direct, nullptr);
  ASSERT_NE(delay, nullptr);

  zenz->setChecked(true);
  live->setChecked(false);
  direct->setChecked(false);
  EXPECT_TRUE(delay->isEnabled());

  direct->setChecked(true);
  EXPECT_TRUE(delay->isEnabled());

  live->setChecked(true);
  EXPECT_TRUE(delay->isEnabled());

  zenz->setChecked(false);
  EXPECT_FALSE(delay->isEnabled());
}

TEST_F(ConfigDialogUiTest, InvalidFeedbackOverrideNeverFallsBackToUserData) {
  session::ZenzFeedbackStore store;
  ASSERT_TRUE(store.SetManualLocalPreference("よい", "よい", "良い", true));
  ASSERT_EQ(store.ListLocalPreferenceEntries().size(), 1);
  for (const char* invalid : {"", "relative/path"}) {
    ASSERT_TRUE(qputenv("MOZKEY_ZENZ_FEEDBACK_DIRECTORY", invalid));
    EXPECT_TRUE(store.ListLocalPreferenceEntries().empty());
    EXPECT_FALSE(store.SetManualLocalPreference("よい", "よい", "良い", true));
  }
  ASSERT_TRUE(qputenv("MOZKEY_ZENZ_FEEDBACK_DIRECTORY", directory_.path().toUtf8()));
  ASSERT_EQ(store.ListLocalPreferenceEntries().size(), 1);
  EXPECT_EQ(store.ListLocalPreferenceEntries()[0].observation_count, 0);
}

}  // namespace
}  // namespace mozc::gui

int main(int argc, char** argv) {
  ::testing::InitGoogleMock(&argc, argv);
  QApplication app(argc, argv);
  return RUN_ALL_TESTS();
}
