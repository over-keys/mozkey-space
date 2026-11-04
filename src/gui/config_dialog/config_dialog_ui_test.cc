// Copyright 2026
// Licensed under the same terms as Mozc.

// Exercise the real config dialog without contacting the installed IME.
#include "gui/config_dialog/config_dialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QTranslator>
#include <memory>

#include "client/client.h"
#include "client/client_mock.h"
#include "protocol/config.pb.h"
#include "testing/gmock.h"
#include "testing/gunit.h"

namespace mozc::gui {
namespace {

class TestClientFactory : public client::ClientFactoryInterface {
 public:
  bool live = false;
  bool zenz = true;

  std::unique_ptr<client::ClientInterface> NewClient() override {
    auto mock = std::make_unique<::testing::StrictMock<client::ClientMock>>();
    EXPECT_CALL(*mock, CheckVersionOrRestartServer())
        .WillOnce(::testing::Return(true));
    EXPECT_CALL(*mock, GetConfig(::testing::_))
        .WillOnce([this](config::Config* value) {
          value->set_use_live_conversion(live);
          value->set_use_zenz_live_correction(zenz);
          value->set_use_zenz_live_correction_right_context(true);
          value->set_defer_live_conversion_display_until_zenz_result(true);
          return true;
        });
    // Unexpected SetConfig calls fail: these tests must never save preferences.
    return mock;
  }
};

class ConfigDialogUiTest : public ::testing::Test {
 protected:
  void SetUp() override {
    client::ClientFactory::SetClientFactory(&factory_);
  }
  void TearDown() override {
    client::ClientFactory::SetClientFactory(nullptr);
  }

  TestClientFactory factory_;
};

TEST_F(ConfigDialogUiTest, LoadsZenzIndependentlyOfLiveConversion) {
  QTranslator translator;
  ASSERT_TRUE(translator.load(":/config_dialog_en.qm"));
  ASSERT_TRUE(QApplication::installTranslator(&translator));
  for (const bool live : {false, true}) {
    for (const bool zenz : {false, true}) {
      SCOPED_TRACE(::testing::Message() << "live=" << live << " zenz=" << zenz);
      factory_.live = live;
      factory_.zenz = zenz;
      ConfigDialog settings;
      auto* check =
          settings.findChild<QCheckBox*>("zenzLiveCorrectionCheckBox");
      ASSERT_NE(check, nullptr);
      EXPECT_TRUE(check->isEnabled());
      EXPECT_EQ(check->isChecked(), zenz);
      EXPECT_EQ(check->text(), "Enable Zenz correction");
      EXPECT_TRUE(check->toolTip().contains("Space"));
    }
  }
}

TEST_F(ConfigDialogUiTest, TogglesZenzForSpaceWithoutEnablingLiveConversion) {
  ConfigDialog settings;
  auto* live = settings.findChild<QCheckBox*>("liveConversionCheckBox");
  auto* zenz = settings.findChild<QCheckBox*>("zenzLiveCorrectionCheckBox");
  auto* profile =
      settings.findChild<QLineEdit*>("zenzLiveCorrectionProfileLineEdit");
  auto* right =
      settings.findChild<QCheckBox*>("zenzLiveCorrectionRightContextCheckBox");
  auto* right_length = settings.findChild<QSpinBox*>(
      "zenzLiveCorrectionRightContextLengthSpinBox");
  auto* delay = settings.findChild<QSpinBox*>("zenzLiveCorrectionDelaySpinBox");
  auto* min_length =
      settings.findChild<QSpinBox*>("zenzLiveCorrectionMinKeyLengthSpinBox");
  auto* deferred =
      settings.findChild<QCheckBox*>("zenzDeferLiveConversionDisplayCheckBox");
  auto* timeout =
      settings.findChild<QSpinBox*>("zenzDeferredPresentationTimeoutSpinBox");
  ASSERT_NE(live, nullptr);
  ASSERT_NE(zenz, nullptr);
  ASSERT_NE(profile, nullptr);
  ASSERT_NE(right, nullptr);
  ASSERT_NE(right_length, nullptr);
  ASSERT_NE(delay, nullptr);
  ASSERT_NE(min_length, nullptr);
  ASSERT_NE(deferred, nullptr);
  ASSERT_NE(timeout, nullptr);

  EXPECT_FALSE(live->isChecked());
  EXPECT_TRUE(zenz->isChecked());
  EXPECT_TRUE(profile->isEnabled());
  EXPECT_TRUE(right->isEnabled());
  EXPECT_TRUE(right_length->isEnabled());
  EXPECT_FALSE(delay->isEnabled());
  EXPECT_FALSE(min_length->isEnabled());
  EXPECT_FALSE(deferred->isEnabled());
  EXPECT_FALSE(timeout->isEnabled());

  zenz->setChecked(false);
  EXPECT_FALSE(live->isChecked());
  EXPECT_FALSE(profile->isEnabled());
  EXPECT_FALSE(right->isEnabled());
  EXPECT_FALSE(right_length->isEnabled());

  zenz->setChecked(true);
  EXPECT_FALSE(live->isChecked());
  EXPECT_TRUE(profile->isEnabled());
  EXPECT_TRUE(right_length->isEnabled());

  live->setChecked(true);
  EXPECT_TRUE(delay->isEnabled());
  EXPECT_TRUE(min_length->isEnabled());
  EXPECT_TRUE(deferred->isEnabled());
  EXPECT_TRUE(timeout->isEnabled());

  live->setChecked(false);
  EXPECT_TRUE(zenz->isEnabled());
  EXPECT_TRUE(zenz->isChecked());
  EXPECT_FALSE(delay->isEnabled());
  EXPECT_FALSE(timeout->isEnabled());
  right->setChecked(false);
  EXPECT_FALSE(right_length->isEnabled());
  right->setChecked(true);
  EXPECT_TRUE(right_length->isEnabled());
}

}  // namespace
}  // namespace mozc::gui

int main(int argc, char** argv) {
  ::testing::InitGoogleMock(&argc, argv);
  Q_INIT_RESOURCE(qrc_config_dialog);
  QApplication app(argc, argv);
  return RUN_ALL_TESTS();
}
