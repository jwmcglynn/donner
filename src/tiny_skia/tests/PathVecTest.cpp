#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include "tiny_skia/PathVec.h"

using tiny_skia::F32x2;
using tiny_skia::F32x4;

// ---------- F32x2 construction ----------

TEST(F32x2Test, DefaultIsZero) {
  F32x2 v{};
  EXPECT_EQ(v.x, 0.0f);
  EXPECT_EQ(v.y, 0.0f);
}

TEST(F32x2Test, From) {
  auto v = F32x2::from(1.0f, 2.0f);
  EXPECT_EQ(v.x, 1.0f);
  EXPECT_EQ(v.y, 2.0f);
}

TEST(F32x2Test, Splat) {
  auto v = F32x2::splat(3.0f);
  EXPECT_EQ(v.x, 3.0f);
  EXPECT_EQ(v.y, 3.0f);
}

// ---------- F32x2 arithmetic operators ----------

TEST(F32x2Test, Add) {
  auto a = F32x2::from(1.0f, 2.0f);
  auto b = F32x2::from(3.0f, 4.0f);
  auto r = a + b;
  EXPECT_EQ(r, F32x2::from(4.0f, 6.0f));
}

TEST(F32x2Test, Sub) {
  auto a = F32x2::from(5.0f, 7.0f);
  auto b = F32x2::from(2.0f, 3.0f);
  auto r = a - b;
  EXPECT_EQ(r, F32x2::from(3.0f, 4.0f));
}

TEST(F32x2Test, Mul) {
  auto a = F32x2::from(2.0f, 3.0f);
  auto b = F32x2::from(4.0f, 5.0f);
  auto r = a * b;
  EXPECT_EQ(r, F32x2::from(8.0f, 15.0f));
}

TEST(F32x2Test, Div) {
  auto a = F32x2::from(10.0f, 15.0f);
  auto b = F32x2::from(2.0f, 3.0f);
  auto r = a / b;
  EXPECT_EQ(r, F32x2::from(5.0f, 5.0f));
}

// ---------- F32x2 methods ----------

TEST(F32x2Test, Abs) {
  auto v = F32x2::from(-3.0f, 4.0f);
  auto r = v.abs();
  EXPECT_EQ(r, F32x2::from(3.0f, 4.0f));

  auto v2 = F32x2::from(-1.0f, -2.0f);
  EXPECT_EQ(v2.abs(), F32x2::from(1.0f, 2.0f));
}

TEST(F32x2Test, Min) {
  auto a = F32x2::from(1.0f, 5.0f);
  auto b = F32x2::from(3.0f, 2.0f);
  auto r = a.min(b);
  EXPECT_EQ(r, F32x2::from(1.0f, 2.0f));
}

TEST(F32x2Test, Max) {
  auto a = F32x2::from(1.0f, 5.0f);
  auto b = F32x2::from(3.0f, 2.0f);
  auto r = a.max(b);
  EXPECT_EQ(r, F32x2::from(3.0f, 5.0f));
}

TEST(F32x2Test, MaxComponent) {
  auto v = F32x2::from(3.0f, 7.0f);
  EXPECT_EQ(v.maxComponent(), 7.0f);

  auto v2 = F32x2::from(10.0f, 2.0f);
  EXPECT_EQ(v2.maxComponent(), 10.0f);
}

TEST(F32x2Test, Equality) {
  auto a = F32x2::from(1.0f, 2.0f);
  auto b = F32x2::from(1.0f, 2.0f);
  auto c = F32x2::from(1.0f, 3.0f);
  EXPECT_EQ(a, b);
  EXPECT_NE(a, c);
}

// ---------- F32x4 construction ----------

TEST(F32x4Test, DefaultIsZero) {
  F32x4 v{};
  EXPECT_EQ(v.a, 0.0f);
  EXPECT_EQ(v.b, 0.0f);
  EXPECT_EQ(v.c, 0.0f);
  EXPECT_EQ(v.d, 0.0f);
}

TEST(F32x4Test, From) {
  auto v = F32x4::from(1.0f, 2.0f, 3.0f, 4.0f);
  EXPECT_EQ(v.a, 1.0f);
  EXPECT_EQ(v.b, 2.0f);
  EXPECT_EQ(v.c, 3.0f);
  EXPECT_EQ(v.d, 4.0f);
}

// ---------- F32x4 arithmetic operators ----------

TEST(F32x4Test, Add) {
  auto a = F32x4::from(1.0f, 2.0f, 3.0f, 4.0f);
  auto b = F32x4::from(5.0f, 6.0f, 7.0f, 8.0f);
  auto r = a + b;
  EXPECT_EQ(r, F32x4::from(6.0f, 8.0f, 10.0f, 12.0f));
}

TEST(F32x4Test, Sub) {
  auto a = F32x4::from(10.0f, 9.0f, 8.0f, 7.0f);
  auto b = F32x4::from(1.0f, 2.0f, 3.0f, 4.0f);
  auto r = a - b;
  EXPECT_EQ(r, F32x4::from(9.0f, 7.0f, 5.0f, 3.0f));
}

TEST(F32x4Test, Mul) {
  auto a = F32x4::from(2.0f, 3.0f, 4.0f, 5.0f);
  auto b = F32x4::from(3.0f, 4.0f, 5.0f, 6.0f);
  auto r = a * b;
  EXPECT_EQ(r, F32x4::from(6.0f, 12.0f, 20.0f, 30.0f));
}

TEST(F32x4Test, AddAssign) {
  auto a = F32x4::from(1.0f, 2.0f, 3.0f, 4.0f);
  a += F32x4::from(10.0f, 20.0f, 30.0f, 40.0f);
  EXPECT_EQ(a, F32x4::from(11.0f, 22.0f, 33.0f, 44.0f));
}

TEST(F32x4Test, MulAssign) {
  auto a = F32x4::from(2.0f, 3.0f, 4.0f, 5.0f);
  a *= F32x4::from(2.0f, 2.0f, 2.0f, 2.0f);
  EXPECT_EQ(a, F32x4::from(4.0f, 6.0f, 8.0f, 10.0f));
}

// ---------- F32x4 methods ----------

TEST(F32x4Test, Min) {
  auto a = F32x4::from(1.0f, 5.0f, 3.0f, 8.0f);
  auto b = F32x4::from(4.0f, 2.0f, 6.0f, 1.0f);
  auto r = a.min(b);
  EXPECT_EQ(r, F32x4::from(1.0f, 2.0f, 3.0f, 1.0f));
}

TEST(F32x4Test, Max) {
  auto a = F32x4::from(1.0f, 5.0f, 3.0f, 8.0f);
  auto b = F32x4::from(4.0f, 2.0f, 6.0f, 1.0f);
  auto r = a.max(b);
  EXPECT_EQ(r, F32x4::from(4.0f, 5.0f, 6.0f, 8.0f));
}

TEST(F32x4Test, Equality) {
  auto a = F32x4::from(1.0f, 2.0f, 3.0f, 4.0f);
  auto b = F32x4::from(1.0f, 2.0f, 3.0f, 4.0f);
  auto c = F32x4::from(1.0f, 2.0f, 3.0f, 5.0f);
  EXPECT_EQ(a, b);
  EXPECT_NE(a, c);
}

// ---------- F32x2 negative values ----------

TEST(F32x2Test, NegativeArithmetic) {
  auto a = F32x2::from(-1.0f, -2.0f);
  auto b = F32x2::from(3.0f, -4.0f);
  EXPECT_EQ(a + b, F32x2::from(2.0f, -6.0f));
  EXPECT_EQ(a - b, F32x2::from(-4.0f, 2.0f));
  EXPECT_EQ(a * b, F32x2::from(-3.0f, 8.0f));
}

// ---------- F32x4 negative values ----------

TEST(F32x4Test, NegativeArithmetic) {
  auto a = F32x4::from(-1.0f, 2.0f, -3.0f, 4.0f);
  auto b = F32x4::from(5.0f, -6.0f, 7.0f, -8.0f);
  EXPECT_EQ(a + b, F32x4::from(4.0f, -4.0f, 4.0f, -4.0f));
  EXPECT_EQ(a - b, F32x4::from(-6.0f, 8.0f, -10.0f, 12.0f));
  EXPECT_EQ(a * b, F32x4::from(-5.0f, -12.0f, -21.0f, -32.0f));
}
