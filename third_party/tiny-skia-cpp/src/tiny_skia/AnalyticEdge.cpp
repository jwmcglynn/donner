// Ported from Skia's SkAnalyticEdge.cpp for bit-exact Analytic Anti-Aliasing (AAA).
// Original copyright: Copyright 2006 The Android Open Source Project (BSD license).

#include "tiny_skia/AnalyticEdge.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <limits>

#include "tiny_skia/Math.h"

namespace tiny_skia {

namespace {

constexpr int kInverseTableSize = 1024;
constexpr int kMaxCoeffShift = 6;

// Precomputed table of -65536/x for x in 1..1024, used by quickInverse.
// Matches Skia's table in SkAnalyticEdge.cpp exactly.
// NOLINTBEGIN(readability-magic-numbers)
static const std::int32_t kInverseTable[] = {
    -4096,    -4100,    -4104,    -4108,    -4112,    -4116,    -4120,    -4124,    -4128,
    -4132,    -4136,    -4140,    -4144,    -4148,    -4152,    -4156,    -4161,    -4165,
    -4169,    -4173,    -4177,    -4181,    -4185,    -4190,    -4194,    -4198,    -4202,
    -4206,    -4211,    -4215,    -4219,    -4223,    -4228,    -4232,    -4236,    -4240,
    -4245,    -4249,    -4253,    -4258,    -4262,    -4266,    -4271,    -4275,    -4279,
    -4284,    -4288,    -4293,    -4297,    -4301,    -4306,    -4310,    -4315,    -4319,
    -4324,    -4328,    -4332,    -4337,    -4341,    -4346,    -4350,    -4355,    -4359,
    -4364,    -4369,    -4373,    -4378,    -4382,    -4387,    -4391,    -4396,    -4401,
    -4405,    -4410,    -4415,    -4419,    -4424,    -4429,    -4433,    -4438,    -4443,
    -4447,    -4452,    -4457,    -4462,    -4466,    -4471,    -4476,    -4481,    -4485,
    -4490,    -4495,    -4500,    -4505,    -4510,    -4514,    -4519,    -4524,    -4529,
    -4534,    -4539,    -4544,    -4549,    -4554,    -4559,    -4563,    -4568,    -4573,
    -4578,    -4583,    -4588,    -4593,    -4599,    -4604,    -4609,    -4614,    -4619,
    -4624,    -4629,    -4634,    -4639,    -4644,    -4650,    -4655,    -4660,    -4665,
    -4670,    -4675,    -4681,    -4686,    -4691,    -4696,    -4702,    -4707,    -4712,
    -4718,    -4723,    -4728,    -4733,    -4739,    -4744,    -4750,    -4755,    -4760,
    -4766,    -4771,    -4777,    -4782,    -4788,    -4793,    -4798,    -4804,    -4809,
    -4815,    -4821,    -4826,    -4832,    -4837,    -4843,    -4848,    -4854,    -4860,
    -4865,    -4871,    -4877,    -4882,    -4888,    -4894,    -4899,    -4905,    -4911,
    -4917,    -4922,    -4928,    -4934,    -4940,    -4946,    -4951,    -4957,    -4963,
    -4969,    -4975,    -4981,    -4987,    -4993,    -4999,    -5005,    -5011,    -5017,
    -5023,    -5029,    -5035,    -5041,    -5047,    -5053,    -5059,    -5065,    -5071,
    -5077,    -5084,    -5090,    -5096,    -5102,    -5108,    -5115,    -5121,    -5127,
    -5133,    -5140,    -5146,    -5152,    -5159,    -5165,    -5171,    -5178,    -5184,
    -5190,    -5197,    -5203,    -5210,    -5216,    -5223,    -5229,    -5236,    -5242,
    -5249,    -5256,    -5262,    -5269,    -5275,    -5282,    -5289,    -5295,    -5302,
    -5309,    -5315,    -5322,    -5329,    -5336,    -5343,    -5349,    -5356,    -5363,
    -5370,    -5377,    -5384,    -5391,    -5398,    -5405,    -5412,    -5418,    -5426,
    -5433,    -5440,    -5447,    -5454,    -5461,    -5468,    -5475,    -5482,    -5489,
    -5497,    -5504,    -5511,    -5518,    -5526,    -5533,    -5540,    -5548,    -5555,
    -5562,    -5570,    -5577,    -5584,    -5592,    -5599,    -5607,    -5614,    -5622,
    -5629,    -5637,    -5645,    -5652,    -5660,    -5667,    -5675,    -5683,    -5691,
    -5698,    -5706,    -5714,    -5722,    -5729,    -5737,    -5745,    -5753,    -5761,
    -5769,    -5777,    -5785,    -5793,    -5801,    -5809,    -5817,    -5825,    -5833,
    -5841,    -5849,    -5857,    -5866,    -5874,    -5882,    -5890,    -5899,    -5907,
    -5915,    -5924,    -5932,    -5940,    -5949,    -5957,    -5966,    -5974,    -5983,
    -5991,    -6000,    -6009,    -6017,    -6026,    -6034,    -6043,    -6052,    -6061,
    -6069,    -6078,    -6087,    -6096,    -6105,    -6114,    -6123,    -6132,    -6141,
    -6150,    -6159,    -6168,    -6177,    -6186,    -6195,    -6204,    -6213,    -6223,
    -6232,    -6241,    -6250,    -6260,    -6269,    -6278,    -6288,    -6297,    -6307,
    -6316,    -6326,    -6335,    -6345,    -6355,    -6364,    -6374,    -6384,    -6393,
    -6403,    -6413,    -6423,    -6432,    -6442,    -6452,    -6462,    -6472,    -6482,
    -6492,    -6502,    -6512,    -6523,    -6533,    -6543,    -6553,    -6563,    -6574,
    -6584,    -6594,    -6605,    -6615,    -6626,    -6636,    -6647,    -6657,    -6668,
    -6678,    -6689,    -6700,    -6710,    -6721,    -6732,    -6743,    -6754,    -6765,
    -6775,    -6786,    -6797,    -6808,    -6820,    -6831,    -6842,    -6853,    -6864,
    -6875,    -6887,    -6898,    -6909,    -6921,    -6932,    -6944,    -6955,    -6967,
    -6978,    -6990,    -7002,    -7013,    -7025,    -7037,    -7049,    -7061,    -7073,
    -7084,    -7096,    -7108,    -7121,    -7133,    -7145,    -7157,    -7169,    -7182,
    -7194,    -7206,    -7219,    -7231,    -7244,    -7256,    -7269,    -7281,    -7294,
    -7307,    -7319,    -7332,    -7345,    -7358,    -7371,    -7384,    -7397,    -7410,
    -7423,    -7436,    -7449,    -7463,    -7476,    -7489,    -7503,    -7516,    -7530,
    -7543,    -7557,    -7570,    -7584,    -7598,    -7612,    -7626,    -7639,    -7653,
    -7667,    -7681,    -7695,    -7710,    -7724,    -7738,    -7752,    -7767,    -7781,
    -7796,    -7810,    -7825,    -7839,    -7854,    -7869,    -7884,    -7898,    -7913,
    -7928,    -7943,    -7958,    -7973,    -7989,    -8004,    -8019,    -8035,    -8050,
    -8065,    -8081,    -8097,    -8112,    -8128,    -8144,    -8160,    -8176,    -8192,
    -8208,    -8224,    -8240,    -8256,    -8272,    -8289,    -8305,    -8322,    -8338,
    -8355,    -8371,    -8388,    -8405,    -8422,    -8439,    -8456,    -8473,    -8490,
    -8507,    -8525,    -8542,    -8559,    -8577,    -8594,    -8612,    -8630,    -8648,
    -8665,    -8683,    -8701,    -8719,    -8738,    -8756,    -8774,    -8793,    -8811,
    -8830,    -8848,    -8867,    -8886,    -8905,    -8924,    -8943,    -8962,    -8981,
    -9000,    -9020,    -9039,    -9058,    -9078,    -9098,    -9118,    -9137,    -9157,
    -9177,    -9198,    -9218,    -9238,    -9258,    -9279,    -9300,    -9320,    -9341,
    -9362,    -9383,    -9404,    -9425,    -9446,    -9467,    -9489,    -9510,    -9532,
    -9554,    -9576,    -9597,    -9619,    -9642,    -9664,    -9686,    -9709,    -9731,
    -9754,    -9776,    -9799,    -9822,    -9845,    -9868,    -9892,    -9915,    -9939,
    -9962,    -9986,    -10010,   -10034,   -10058,   -10082,   -10106,   -10131,   -10155,
    -10180,   -10205,   -10230,   -10255,   -10280,   -10305,   -10330,   -10356,   -10381,
    -10407,   -10433,   -10459,   -10485,   -10512,   -10538,   -10564,   -10591,   -10618,
    -10645,   -10672,   -10699,   -10727,   -10754,   -10782,   -10810,   -10837,   -10866,
    -10894,   -10922,   -10951,   -10979,   -11008,   -11037,   -11066,   -11096,   -11125,
    -11155,   -11184,   -11214,   -11244,   -11275,   -11305,   -11335,   -11366,   -11397,
    -11428,   -11459,   -11491,   -11522,   -11554,   -11586,   -11618,   -11650,   -11683,
    -11715,   -11748,   -11781,   -11814,   -11848,   -11881,   -11915,   -11949,   -11983,
    -12018,   -12052,   -12087,   -12122,   -12157,   -12192,   -12228,   -12264,   -12300,
    -12336,   -12372,   -12409,   -12446,   -12483,   -12520,   -12557,   -12595,   -12633,
    -12671,   -12710,   -12748,   -12787,   -12826,   -12865,   -12905,   -12945,   -12985,
    -13025,   -13066,   -13107,   -13148,   -13189,   -13231,   -13273,   -13315,   -13357,
    -13400,   -13443,   -13486,   -13530,   -13573,   -13617,   -13662,   -13706,   -13751,
    -13797,   -13842,   -13888,   -13934,   -13981,   -14027,   -14074,   -14122,   -14169,
    -14217,   -14266,   -14315,   -14364,   -14413,   -14463,   -14513,   -14563,   -14614,
    -14665,   -14716,   -14768,   -14820,   -14873,   -14926,   -14979,   -15033,   -15087,
    -15141,   -15196,   -15252,   -15307,   -15363,   -15420,   -15477,   -15534,   -15592,
    -15650,   -15709,   -15768,   -15827,   -15887,   -15947,   -16008,   -16070,   -16131,
    -16194,   -16256,   -16320,   -16384,   -16448,   -16513,   -16578,   -16644,   -16710,
    -16777,   -16844,   -16912,   -16980,   -17050,   -17119,   -17189,   -17260,   -17331,
    -17403,   -17476,   -17549,   -17623,   -17697,   -17772,   -17848,   -17924,   -18001,
    -18078,   -18157,   -18236,   -18315,   -18396,   -18477,   -18558,   -18641,   -18724,
    -18808,   -18893,   -18978,   -19065,   -19152,   -19239,   -19328,   -19418,   -19508,
    -19599,   -19691,   -19784,   -19878,   -19972,   -20068,   -20164,   -20262,   -20360,
    -20460,   -20560,   -20661,   -20763,   -20867,   -20971,   -21076,   -21183,   -21290,
    -21399,   -21509,   -21620,   -21732,   -21845,   -21959,   -22075,   -22192,   -22310,
    -22429,   -22550,   -22671,   -22795,   -22919,   -23045,   -23172,   -23301,   -23431,
    -23563,   -23696,   -23831,   -23967,   -24105,   -24244,   -24385,   -24528,   -24672,
    -24818,   -24966,   -25115,   -25266,   -25420,   -25575,   -25731,   -25890,   -26051,
    -26214,   -26379,   -26546,   -26715,   -26886,   -27060,   -27235,   -27413,   -27594,
    -27776,   -27962,   -28149,   -28339,   -28532,   -28728,   -28926,   -29127,   -29330,
    -29537,   -29746,   -29959,   -30174,   -30393,   -30615,   -30840,   -31068,   -31300,
    -31536,   -31775,   -32017,   -32263,   -32513,   -32768,   -33026,   -33288,   -33554,
    -33825,   -34100,   -34379,   -34663,   -34952,   -35246,   -35544,   -35848,   -36157,
    -36472,   -36792,   -37117,   -37449,   -37786,   -38130,   -38479,   -38836,   -39199,
    -39568,   -39945,   -40329,   -40721,   -41120,   -41527,   -41943,   -42366,   -42799,
    -43240,   -43690,   -44150,   -44620,   -45100,   -45590,   -46091,   -46603,   -47127,
    -47662,   -48210,   -48770,   -49344,   -49932,   -50533,   -51150,   -51781,   -52428,
    -53092,   -53773,   -54471,   -55188,   -55924,   -56679,   -57456,   -58254,   -59074,
    -59918,   -60787,   -61680,   -62601,   -63550,   -64527,   -65536,   -66576,   -67650,
    -68759,   -69905,   -71089,   -72315,   -73584,   -74898,   -76260,   -77672,   -79137,
    -80659,   -82241,   -83886,   -85598,   -87381,   -89240,   -91180,   -93206,   -95325,
    -97541,   -99864,   -102300,  -104857,  -107546,  -110376,  -113359,  -116508,  -119837,
    -123361,  -127100,  -131072,  -135300,  -139810,  -144631,  -149796,  -155344,  -161319,
    -167772,  -174762,  -182361,  -190650,  -199728,  -209715,  -220752,  -233016,  -246723,
    -262144,  -279620,  -299593,  -322638,  -349525,  -381300,  -419430,  -466033,  -524288,
    -599186,  -699050,  -838860,  -1048576, -1398101, -2097152, -4194304, 0};
// NOLINTEND(readability-magic-numbers)

FDot16 quickInverse(FDot6 x) {
  constexpr auto kLastEntry = static_cast<std::size_t>(kInverseTableSize);
  assert(std::abs(x) <= static_cast<std::int32_t>(kLastEntry));

  if (x > 0) {
    return -kInverseTable[kLastEntry - static_cast<std::size_t>(x)];
  }
  return kInverseTable[kLastEntry + static_cast<std::size_t>(x)];
}

FDot6 fixedToFDot6(FDot16 x) { return x >> 10; }
FDot16 fdot6ToFixed(FDot6 x) { return leftShift(x, 10); }
FDot16 fdot6ToFixedDiv2(FDot6 value) { return leftShift(value, 9); }

FDot16 fixedRoundToFixed(FDot16 x) { return (x + fdot16::half) & ~0xFFFF; }

FDot6 fdot6UpShift(FDot6 x, std::int32_t upShift) {
  assert((leftShift(x, upShift) >> upShift) == x);
  return leftShift(x, upShift);
}

FDot6 cheapDistance(FDot6 dx, FDot6 dy) {
  dx = std::abs(dx);
  dy = std::abs(dy);
  return dx > dy ? dx + (dy >> 1) : dy + (dx >> 1);
}

std::int32_t diffToShift(FDot6 dx, FDot6 dy, std::int32_t shiftAA) {
  auto dist = cheapDistance(dx, dy);
  dist = (dist + (1 << (2 + shiftAA))) >> (3 + shiftAA);
  if (dist == 0) {
    return 0;
  }
  return static_cast<std::int32_t>(32 - std::countl_zero(static_cast<std::uint32_t>(dist))) >> 1;
}

FDot6 cubicDeltaFromLine(FDot6 a, FDot6 b, FDot6 c, FDot6 d) {
  const auto oneThird = ((a * 8 - b * 15 + c * 6 + d) * 19) >> 9;
  const auto twoThird = ((a + b * 6 - c * 15 + d * 8) * 19) >> 9;
  return std::max(std::abs(oneThird), std::abs(twoThird));
}


}  // namespace

FDot16 quickDiv(FDot6 a, FDot6 b) {
  constexpr int kMinBits = 3;
  constexpr int kMaxBits = 31;
  constexpr int kMaxAbsA = 1 << (kMaxBits - (22 - kMinBits));
  FDot6 absA = std::abs(a);
  FDot6 absB = std::abs(b);
  if (absB >= (1 << kMinBits) && absB < kInverseTableSize && absA < kMaxAbsA) {
    return (a * quickInverse(b)) >> 6;
  }
  return fdot6::div(a, b);
}

FDot16 AnalyticEdge::snapY(FDot16 y) {
  constexpr int accuracy = kDefaultAccuracy;
  return static_cast<FDot16>(
      (static_cast<std::uint32_t>(y) + (fdot16::one >> (accuracy + 1))) >> (16 - accuracy)
      << (16 - accuracy));
}

void AnalyticEdge::goY(FDot16 targetY) {
  if (targetY == y + fdot16::one) {
    x = x + dx;
    y = targetY;
  } else if (targetY != y) {
    x = upperX + fdot16::mul(dx, targetY - upperY);
    y = targetY;
  }
}

void AnalyticEdge::goY(FDot16 targetY, int yShift) {
  assert(yShift >= 0 && yShift <= kDefaultAccuracy);
  y = targetY;
  x += dx >> yShift;
}

bool AnalyticEdge::setLine(Point p0, Point p1) {
  constexpr int accuracy = kDefaultAccuracy;
  constexpr int multiplier = (1 << kDefaultAccuracy);

  // Convert to FDot6 at 4x scale, then to Fixed, then downshift by accuracy.
  // This matches Skia's coordinate conversion exactly.
  auto x0 = static_cast<FDot16>(fdot6ToFixed(static_cast<FDot6>(p0.x * multiplier * 64.0f)) >>
                                 accuracy);
  auto y0 = snapY(
      static_cast<FDot16>(fdot6ToFixed(static_cast<FDot6>(p0.y * multiplier * 64.0f)) >> accuracy));
  auto x1 = static_cast<FDot16>(fdot6ToFixed(static_cast<FDot6>(p1.x * multiplier * 64.0f)) >>
                                 accuracy);
  auto y1 = snapY(
      static_cast<FDot16>(fdot6ToFixed(static_cast<FDot6>(p1.y * multiplier * 64.0f)) >> accuracy));

  std::int8_t w = 1;
  if (y0 > y1) {
    std::swap(x0, x1);
    std::swap(y0, y1);
    w = -1;
  }

  FDot6 dxFDot6 = fixedToFDot6(y1 - y0);
  if (dxFDot6 == 0) {
    return false;
  }
  FDot6 dyFDot6 = fixedToFDot6(x1 - x0);
  FDot16 slope = quickDiv(dyFDot6, dxFDot6);
  FDot16 absSlope = std::abs(slope);

  this->x = x0;
  this->dx = slope;
  this->upperX = x0;
  this->y = y0;
  this->upperY = y0;
  this->lowerY = y1;
  this->dy = (dyFDot6 == 0 || slope == 0)
                 ? std::numeric_limits<FDot16>::max()
                 : absSlope < kInverseTableSize ? quickInverse(absSlope)
                                                : std::abs(quickDiv(dxFDot6, dyFDot6));
  this->curveCount = 0;
  this->winding = w;
  this->curveShift = 0;

  return true;
}

bool AnalyticEdge::updateLine(FDot16 x0, FDot16 y0, FDot16 x1, FDot16 y1, FDot16 slope) {
  assert(winding == 1 || winding == -1);
  assert(curveCount != 0);

  if (y0 > y1) {
    std::swap(x0, x1);
    std::swap(y0, y1);
    winding = static_cast<std::int8_t>(-winding);
  }

  assert(y0 <= y1);

  FDot6 dxFDot6 = fixedToFDot6(x1 - x0);
  FDot6 dyFDot6 = fixedToFDot6(y1 - y0);

  if (dyFDot6 == 0) {
    return false;
  }

  assert(slope < std::numeric_limits<FDot16>::max());

  FDot6 absSlope = std::abs(fixedToFDot6(slope));
  this->x = x0;
  this->dx = slope;
  this->upperX = x0;
  this->y = y0;
  this->upperY = y0;
  this->lowerY = y1;
  this->dy = (dxFDot6 == 0 || slope == 0)
                 ? std::numeric_limits<FDot16>::max()
                 : absSlope < kInverseTableSize ? quickInverse(absSlope)
                                                : std::abs(quickDiv(dyFDot6, dxFDot6));

  return true;
}

bool AnalyticEdge::update(FDot16 lastY) {
  assert(lastY >= lowerY);
  if (curveCount < 0) {
    return updateCubic();
  }
  if (curveCount > 0) {
    return updateQuadratic();
  }
  return false;
}

bool AnalyticEdge::setQuadratic(const Point pts[3]) {
  constexpr int shift = kDefaultAccuracy;
  FDot6 x0, y0, x1, y1, x2, y2;

  {
    float scale = static_cast<float>(1 << (shift + 6));
    x0 = static_cast<FDot6>(pts[0].x * scale);
    y0 = static_cast<FDot6>(pts[0].y * scale);
    x1 = static_cast<FDot6>(pts[1].x * scale);
    y1 = static_cast<FDot6>(pts[1].y * scale);
    x2 = static_cast<FDot6>(pts[2].x * scale);
    y2 = static_cast<FDot6>(pts[2].y * scale);
  }

  std::int8_t w = 1;
  if (y0 > y2) {
    std::swap(x0, x2);
    std::swap(y0, y2);
    w = -1;
  }
  assert(y0 <= y1 && y1 <= y2);

  int top = fdot6::round(y0);
  int bot = fdot6::round(y2);
  if (top == bot) {
    return false;
  }

  int curveShiftVal;
  {
    FDot6 dx = (leftShift(x1, 1) - x0 - x2) >> 2;
    FDot6 dy = (leftShift(y1, 1) - y0 - y2) >> 2;
    curveShiftVal = diffToShift(dx, dy, shift);
    assert(curveShiftVal >= 0);
  }
  if (curveShiftVal == 0) {
    curveShiftVal = 1;
  } else if (curveShiftVal > kMaxCoeffShift) {
    curveShiftVal = kMaxCoeffShift;
  }

  this->winding = w;
  this->curveCount = static_cast<std::int8_t>(1 << curveShiftVal);
  this->curveShift = static_cast<std::uint8_t>(curveShiftVal - 1);

  FDot16 A = fdot6ToFixedDiv2(x0 - x1 - x1 + x2);
  FDot16 B = fdot6ToFixed(x1 - x0);

  this->qx = fdot6ToFixed(x0);
  this->qdx = B + (A >> curveShiftVal);
  this->qddx = A >> (curveShiftVal - 1);

  A = fdot6ToFixedDiv2(y0 - y1 - y1 + y2);
  B = fdot6ToFixed(y1 - y0);

  this->qy = fdot6ToFixed(y0);
  this->qdy = B + (A >> curveShiftVal);
  this->qddy = A >> (curveShiftVal - 1);

  this->qLastX = fdot6ToFixed(x2);
  this->qLastY = fdot6ToFixed(y2);

  // Apply kDefaultAccuracy downshift.
  this->qx >>= shift;
  this->qy >>= shift;
  this->qdx >>= shift;
  this->qdy >>= shift;
  this->qddx >>= shift;
  this->qddy >>= shift;
  this->qLastX >>= shift;
  this->qLastY >>= shift;
  this->qy = snapY(this->qy);
  this->qLastY = snapY(this->qLastY);

  this->snappedX = this->qx;
  this->snappedY = this->qy;

  return this->updateQuadratic();
}

bool AnalyticEdge::updateQuadratic() {
  int count = curveCount;
  FDot16 oldx = qx;
  FDot16 oldy = qy;
  FDot16 dx = qdx;
  FDot16 dy = qdy;
  FDot16 newx, newy, newSnappedX, newSnappedY;
  int shift = curveShift;
  bool success = false;

  assert(count > 0);

  do {
    FDot16 slope;
    if (--count > 0) {
      newx = oldx + (dx >> shift);
      newy = oldy + (dy >> shift);
      if (std::abs(dy >> shift) >= fdot16::one * 2 &&
          leftShift(static_cast<std::int64_t>(std::abs(dy)), 6) > std::abs(dx)) {
        FDot6 diffY = fixedToFDot6(newy - snappedY);
        slope = diffY ? quickDiv(fixedToFDot6(newx - snappedX), diffY)
                      : std::numeric_limits<FDot16>::max();
        newSnappedY = std::min(qLastY, fixedRoundToFixed(newy));
        newSnappedX = newx - fdot16::mul(slope, newy - newSnappedY);
      } else {
        newSnappedY = std::min(qLastY, snapY(newy));
        newSnappedX = newx;
        FDot6 diffY = fixedToFDot6(newSnappedY - snappedY);
        slope = diffY ? quickDiv(fixedToFDot6(newx - snappedX), diffY)
                      : std::numeric_limits<FDot16>::max();
      }
      dx += qddx;
      dy += qddy;
    } else {
      newx = qLastX;
      newy = qLastY;
      newSnappedY = newy;
      newSnappedX = newx;
      FDot6 diffY = fixedToFDot6(newy - snappedY);
      slope = diffY ? quickDiv(fixedToFDot6(newx - snappedX), diffY)
                    : std::numeric_limits<FDot16>::max();
    }
    if (slope < std::numeric_limits<FDot16>::max()) {
      success = this->updateLine(snappedX, snappedY, newSnappedX, newSnappedY, slope);
    }
    oldx = newx;
    oldy = newy;
  } while (count > 0 && !success);

  assert(newSnappedY <= qLastY);

  qx = newx;
  qy = newy;
  qdx = dx;
  qdy = dy;
  snappedX = newSnappedX;
  snappedY = newSnappedY;
  curveCount = static_cast<std::int8_t>(count);
  return success;
}

void AnalyticEdge::keepContinuousQuad() {
  snappedX = x;
  snappedY = y;
}

bool AnalyticEdge::setCubic(const Point pts[4]) {
  constexpr int shift = kDefaultAccuracy;
  FDot6 x0, y0, x1, y1, x2, y2, x3, y3;

  {
    float scale = static_cast<float>(1 << (shift + 6));
    x0 = static_cast<FDot6>(pts[0].x * scale);
    y0 = static_cast<FDot6>(pts[0].y * scale);
    x1 = static_cast<FDot6>(pts[1].x * scale);
    y1 = static_cast<FDot6>(pts[1].y * scale);
    x2 = static_cast<FDot6>(pts[2].x * scale);
    y2 = static_cast<FDot6>(pts[2].y * scale);
    x3 = static_cast<FDot6>(pts[3].x * scale);
    y3 = static_cast<FDot6>(pts[3].y * scale);
  }

  std::int8_t w = 1;
  if (y0 > y3) {
    std::swap(x0, x3);
    std::swap(x1, x2);
    std::swap(y0, y3);
    std::swap(y1, y2);
    w = -1;
  }

  int top = fdot6::round(y0);
  int bot = fdot6::round(y3);
  if (top == bot) {
    return false;
  }

  int curveShiftVal;
  {
    FDot6 dx = cubicDeltaFromLine(x0, x1, x2, x3);
    FDot6 dy = cubicDeltaFromLine(y0, y1, y2, y3);
    curveShiftVal = diffToShift(dx, dy, shift) + 1;
  }
  assert(curveShiftVal > 0);
  if (curveShiftVal > kMaxCoeffShift) {
    curveShiftVal = kMaxCoeffShift;
  }

  int upShift = 6;
  int downShift = curveShiftVal + upShift - 10;
  if (downShift < 0) {
    downShift = 0;
    upShift = 10 - curveShiftVal;
  }

  this->winding = w;
  this->curveCount = static_cast<std::int8_t>(leftShift(-1, curveShiftVal));
  this->curveShift = static_cast<std::uint8_t>(curveShiftVal);
  this->cubicDShift = static_cast<std::uint8_t>(downShift);

  FDot16 B = fdot6UpShift(3 * (x1 - x0), upShift);
  FDot16 C = fdot6UpShift(3 * (x0 - x1 - x1 + x2), upShift);
  FDot16 D = fdot6UpShift(x3 + 3 * (x1 - x2) - x0, upShift);

  this->cx = fdot6ToFixed(x0);
  this->cdx = B + (C >> curveShiftVal) + (D >> (2 * curveShiftVal));
  this->cddx = 2 * C + ((3 * D) >> (curveShiftVal - 1));
  this->cdddx = (3 * D) >> (curveShiftVal - 1);

  B = fdot6UpShift(3 * (y1 - y0), upShift);
  C = fdot6UpShift(3 * (y0 - y1 - y1 + y2), upShift);
  D = fdot6UpShift(y3 + 3 * (y1 - y2) - y0, upShift);

  this->cy = fdot6ToFixed(y0);
  this->cdy = B + (C >> curveShiftVal) + (D >> (2 * curveShiftVal));
  this->cddy = 2 * C + ((3 * D) >> (curveShiftVal - 1));
  this->cdddy = (3 * D) >> (curveShiftVal - 1);

  this->cLastX = fdot6ToFixed(x3);
  this->cLastY = fdot6ToFixed(y3);

  // Apply kDefaultAccuracy downshift.
  this->cx >>= shift;
  this->cy >>= shift;
  this->cdx >>= shift;
  this->cdy >>= shift;
  this->cddx >>= shift;
  this->cddy >>= shift;
  this->cdddx >>= shift;
  this->cdddy >>= shift;
  this->cLastX >>= shift;
  this->cLastY >>= shift;
  this->cy = snapY(this->cy);
  this->snappedY = this->cy;
  this->cLastY = snapY(this->cLastY);

  return this->updateCubic();
}

bool AnalyticEdge::updateCubic() {
  int count = curveCount;
  FDot16 oldx = cx;
  FDot16 oldy = cy;
  FDot16 newx, newy;
  const int ddshift = curveShift;
  const int dshift = cubicDShift;

  assert(count < 0);

  bool success = false;
  do {
    if (++count < 0) {
      newx = oldx + (cdx >> dshift);
      cdx += cddx >> ddshift;
      cddx += cdddx;

      newy = oldy + (cdy >> dshift);
      cdy += cddy >> ddshift;
      cddy += cdddy;
    } else {
      newx = cLastX;
      newy = cLastY;
    }

    if (newy < oldy) {
      newy = oldy;
    }

    FDot16 newSnappedY = snapY(newy);
    if (cLastY < newSnappedY) {
      newSnappedY = cLastY;
      count = 0;
    }

    FDot16 slope = fixedToFDot6(newSnappedY - snappedY) == 0
                       ? std::numeric_limits<FDot16>::max()
                       : fdot6::div(fixedToFDot6(newx - oldx), fixedToFDot6(newSnappedY - snappedY));

    success = this->updateLine(oldx, snappedY, newx, newSnappedY, slope);

    oldx = newx;
    oldy = newy;
    snappedY = newSnappedY;
  } while (count < 0 && !success);

  cx = newx;
  cy = newy;
  curveCount = static_cast<std::int8_t>(count);
  return success;
}

void AnalyticEdge::keepContinuousCubic() {
  cx = x;
  snappedY = y;
}

}  // namespace tiny_skia
