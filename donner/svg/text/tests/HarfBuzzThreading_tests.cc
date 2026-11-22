#include <gtest/gtest.h>
#include <hb.h>

#include <hb-config.hh>
#include <latch>
#include <thread>

namespace donner::svg {
namespace {

TEST(HarfBuzzThreading, SeparateBuffersShareDefaultUnicodeFunctions) {
#ifdef HB_NO_MT
  FAIL() << "Concurrent editor and raster shaping require HarfBuzz thread support";
#endif

  std::latch start(2);
  auto guessScript = [&](const char* text) {
    start.arrive_and_wait();
    hb_buffer_t* buffer = hb_buffer_create();
    hb_buffer_add_utf8(buffer, text, -1, 0, -1);
    hb_buffer_guess_segment_properties(buffer);
    const hb_script_t script = hb_buffer_get_script(buffer);
    hb_buffer_destroy(buffer);
    return script;
  };

  hb_script_t otherScript = HB_SCRIPT_INVALID;
  std::thread other([&] { otherScript = guessScript("مرحبا"); });
  const hb_script_t script = guessScript("Hello");
  other.join();

  EXPECT_EQ(script, HB_SCRIPT_LATIN);
  EXPECT_EQ(otherScript, HB_SCRIPT_ARABIC);
}

}  // namespace
}  // namespace donner::svg
