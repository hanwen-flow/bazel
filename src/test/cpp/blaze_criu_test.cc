// Copyright 2026 The Bazel Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "src/main/cpp/blaze_criu.h"

#include <string>

#include "src/main/cpp/blaze_util_platform.h"
#include "src/main/cpp/util/file.h"
#include "src/main/cpp/util/path.h"
#include "googletest/include/gtest/gtest.h"

namespace blaze {

class CriuCheckpointExistsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    output_base_ = blaze_util::Path(
        blaze_util::JoinPath(GetPathEnv("TEST_TMPDIR"), "output_base"));
    images_ = output_base_.GetRelative("criu");
    ASSERT_TRUE(blaze_util::MakeDirectories(images_, 0755));
  }

  void TearDown() override { blaze_util::RemoveRecursively(output_base_); }

  void WriteNsPid() {
    ASSERT_TRUE(blaze_util::WriteFile("3\n", images_.GetRelative("ns-pid")));
  }

  void WriteInstallKey(const std::string &key) {
    ASSERT_TRUE(blaze_util::WriteFile(key, images_.GetRelative("install-key")));
  }

  blaze_util::Path output_base_;
  blaze_util::Path images_;
};

TEST_F(CriuCheckpointExistsTest, FalseWhenNoNsPid) {
  WriteInstallKey("abc123");
  EXPECT_FALSE(CriuCheckpointExists(output_base_, "abc123"));
}

TEST_F(CriuCheckpointExistsTest, FalseWhenNoInstallKey) {
  WriteNsPid();
  EXPECT_FALSE(CriuCheckpointExists(output_base_, "abc123"));
}

TEST_F(CriuCheckpointExistsTest, FalseWhenInstallKeyDiffers) {
  WriteNsPid();
  WriteInstallKey("other");
  EXPECT_FALSE(CriuCheckpointExists(output_base_, "abc123"));
}

TEST_F(CriuCheckpointExistsTest, TrueWhenNsPidAndKeyMatch) {
  WriteNsPid();
  WriteInstallKey("abc123");
  EXPECT_TRUE(CriuCheckpointExists(output_base_, "abc123"));
}

TEST_F(CriuCheckpointExistsTest, IgnoresSurroundingWhitespaceInKey) {
  WriteNsPid();
  WriteInstallKey("  abc123\n");
  EXPECT_TRUE(CriuCheckpointExists(output_base_, "abc123"));
}

}  // namespace blaze
