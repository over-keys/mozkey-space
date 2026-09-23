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

#include "session/zenz_output_validator.h"

#include "testing/gunit.h"

namespace mozc::session {
namespace {

TEST(ZenzOutputValidatorTest, RestoreUserVisibleSymbolStyleFullwidthBrackets) {
  EXPECT_EQ(ZenzOutputValidator::RestoreUserVisibleSymbolStyle(
                "これは（てすと）です", "これは（テスト）です",
                "これは(テスト)だと思います"),
            "これは（テスト）だと思います");
}

TEST(ZenzOutputValidatorTest, RestoreUserVisibleSymbolStyleAsciiBrackets) {
  EXPECT_EQ(ZenzOutputValidator::RestoreUserVisibleSymbolStyle(
                "これは(test)です", "これは(test)です",
                "これは（test）です"),
            "これは(test)です");
}

TEST(ZenzOutputValidatorTest, RestoreUserVisibleSymbolStyleMixedBrackets) {
  EXPECT_EQ(ZenzOutputValidator::RestoreUserVisibleSymbolStyle(
                "（A）と(B)", "（A）と(B)", "(A)と(B)"),
            "（A）と(B)");
}

TEST(ZenzOutputValidatorTest, RestoreUserVisibleSymbolStyleQuestionAndBang) {
  EXPECT_EQ(ZenzOutputValidator::RestoreUserVisibleSymbolStyle(
                "本当ですか？！", "本当ですか？！", "本当ですか?!"),
            "本当ですか？！");

  EXPECT_EQ(ZenzOutputValidator::RestoreUserVisibleSymbolStyle(
                "OK?!", "OK?!", "OK？！"),
            "OK?!");
}

TEST(ZenzOutputValidatorTest, RestoreUserVisibleSymbolStyleKeepsWaveDash) {
  EXPECT_EQ(ZenzOutputValidator::RestoreUserVisibleSymbolStyle(
                "ほ〜", "ほ〜", "ほ~"),
            "ほ〜");

  EXPECT_EQ(ZenzOutputValidator::RestoreUserVisibleSymbolStyle(
                "ほ~", "ほ~", "ほ〜"),
            "ほ~");
}

TEST(ZenzOutputValidatorTest, RestoreUserVisibleSymbolStyleFullwidthAsciiPunct) {
  EXPECT_EQ(ZenzOutputValidator::RestoreUserVisibleSymbolStyle(
                "注：A；B，C．", "注：A；B，C．", "注:A;B,C."),
            "注：A；B，C．");

  EXPECT_EQ(ZenzOutputValidator::RestoreUserVisibleSymbolStyle(
                "note:A;B,C.", "note:A;B,C.", "note：A；B，C．"),
            "note:A;B,C.");
}

TEST(ZenzOutputValidatorTest, RestoreTrailingUserPunctuation) {
  EXPECT_EQ(ZenzOutputValidator::RestoreTrailingUserPunctuation(
                "するな", "するな", "するな！"),
            "するな");
  EXPECT_EQ(ZenzOutputValidator::RestoreTrailingUserPunctuation(
                "本当ですか？", "本当ですか？", "本当ですか"),
            "本当ですか？");
  EXPECT_EQ(ZenzOutputValidator::RestoreTrailingUserPunctuation(
                "3.14", "3.14", "3.14."),
            "3.14");
  EXPECT_EQ(ZenzOutputValidator::RestoreTrailingUserPunctuation(
                "1,000", "1,000", "1,000！"),
            "1,000");
  EXPECT_EQ(ZenzOutputValidator::RestoreTrailingUserPunctuation(
                "今日雨", "今日は雨", "今日は雨。"),
            "今日は雨");
}

TEST(ZenzOutputValidatorTest, RejectsLongLeftContextEcho) {
  ZenzOutputValidator validator;
  ZenzValidationInput input;
  input.key = "きょうはあめがふっています";
  input.mozc_value = "今日は雨が降っています";
  input.zenz_value = "彼は図書館で本を読んでいました。今日は雨";
  input.left_context = "昨日、彼は図書館で本を読んでいました";
  EXPECT_EQ(validator.Validate(input).reason, "left_context_echo");
}

TEST(ZenzOutputValidatorTest, AcceptsLegitimateRepeatedCurrentReading) {
  ZenzOutputValidator validator;
  ZenzValidationInput input;
  input.key = "きょうはあめがふっていますね";
  input.mozc_value = "今日は雨が降っています";
  input.zenz_value = "今日は雨が降っていますね";
  input.left_context = "今日は雨が降っています";
  EXPECT_TRUE(validator.Validate(input).accept);
}

TEST(ZenzOutputValidatorTest, IgnoresShortLeftContextOverlap) {
  ZenzOutputValidator validator;
  ZenzValidationInput input;
  input.key = "とうきょうではあめ";
  input.mozc_value = "東京では雨";
  input.zenz_value = "では雨が続いています";
  input.left_context = "東京では雨";
  EXPECT_TRUE(validator.Validate(input).accept);
}

}  // namespace
}  // namespace mozc::session
