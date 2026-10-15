#pragma once
/// @file
/// Exact bundled reference font bytes admitted to the basic text test backend.
/// Update only with a reviewed corpus/font change; caller-supplied digests do not grant trust.

#include <array>
#include <string_view>

namespace donner::svg {

inline constexpr std::array<std::string_view, 17> kSvgSuiteReferenceFontHashes = {
    "f3615b08cc3d78409474c28ed7c9e0fb5cc42435e12080a385e1a4163ab46708",  // Amiri-Regular.ttf
    "c6a5f15dcbce870497c580de7fa578a75eb8846d2b5603a65eb8fb69ab949892",  // MPLUS1p-Regular.ttf
    "21fc478e003fc871200ba3bcf4215295a80e06a5c1b96f5368adc2183956f01a",  // NotoColorEmoji.ttf
    "415dc6290378574135b64c808dc640c1df7531973290c4970c51fdeb849cb0c5",  // NotoEmoji-Regular.ttf
    "41fd7ccc82375e2a1e47f0cc2c941c14d7c99ba2f57cf69c9f738d07fb257686",  // NotoMono-Regular.ttf
    "d82477c7cf4f3de0b9b946639ccf5695b8f150b5ed4c5f0f14301738f7eefff0",  // NotoSans-Black.ttf
    "7c15ac396d2ce6bc33a3b4efacdbd322c9e46376599a725c6f790d8036052cab",  // NotoSans-Bold.ttf
    "9202021ad9e40edb215f91e2c3cf7c7f2fb80b141698222f6955b66d73d6ac11",  // NotoSans-ExtraCondensed.ttf
    "35600501daadd3e9030ee6ed8098fd3c7f31bfd898ca58da8edf793a3231c48b",  // NotoSans-Italic.ttf
    "539961bfcb5dd4257bfd62edfd754e5a9b303f6c535984c63d1d15e39e8f57a5",  // NotoSans-Light.ttf
    "6b04c8dd65af6b73eb4279472ed1580b29102d6496a377340e80a40cdb3b22c9",  // NotoSans-Regular.ttf
    "7aa7005d6d306c572da543a1eb99e13b209ff9ac0989d6e952df2fcb3b1f5077",  // NotoSans-Thin.ttf
    "84cda635b5395e22f428c5802b6c36eaeec66750c8b387279424a48545ac1d74",  // NotoSansDevanagari-Regular.ttf
    "0fa6d56e4d826f72c1bc6c287b43fa1ae85b4369339f989cfa5bdf1595f22673",  // NotoSerif-Regular.ttf
    "3938a6df923b0a560b385014903f8ed64317bd456dc0cc8af287216f08b59696",  // SedgwickAveDisplay-Regular.ttf
    "30e35dda7a179dadec11c58e89835b5006ee69d3bd43b55a91dd52f1f02a8cac",  // SourceSansPro-Regular.ttf
    "ae76ccb0d3371de6bcf794a63dd5806e75e9f418aa23d8543d118577988e0703",  // Yellowtail-Regular.ttf
};

}  // namespace donner::svg
