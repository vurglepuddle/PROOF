// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include "geom-kernel/status.h"

TEST(GKScaffold, ExplicitResults) {
    using namespace Proof::GK;
    auto r=Result<int>::success(7);
    ASSERT_TRUE(r); EXPECT_EQ(*r.value,7);
    auto f=Result<int>::failure(Status::NumericalFailure,"certificate failed");
    EXPECT_FALSE(f); EXPECT_FALSE(f.value); EXPECT_EQ(f.detail,"certificate failed");
    EXPECT_NE(Status::OkEmpty,Status::Ok);
}
