/// @file
/// Wall-clock and per-frame submission cost of a sprite-driven scene through Geode: a 1206x2622
/// phone-sized document that draws about 150 sprites with external `<use>` references into three
/// sprite sheets, some of them glowing through a filter. Reports pipelined and serialized frame
/// times, GPU time, upload bytes, submissions and command buffers for a static and a moving scene.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "donner/svg/SVGDocument.h"
#include "donner/svg/renderer/RendererGeode.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/tests/SpriteSceneFixture.h"

namespace donner::svg {
namespace {

using tests::SpriteScene;

constexpr int kWarmupFrames = 10;
constexpr int kMeasuredFrames = 60;

double Median(std::vector<double> values) {
  std::nth_element(values.begin(), values.begin() + values.size() / 2, values.end());
  return values[values.size() / 2];
}

struct SceneSample {
  double pipelinedMs = 0.0;     //!< Median wall time per frame, frames queued back to back.
  double cpuMs = 0.0;           //!< Median CPU time to record and submit one frame.
  double gpuTailMs = 0.0;       //!< Median wait for the GPU after a frame was submitted.
  double uploadBytes = 0.0;     //!< Mean buffer upload bytes per warm frame.
  double uploads = 0.0;         //!< Mean writeBuffer calls per warm frame.
  double submits = 0.0;         //!< Mean queue submissions per warm frame.
  double commandBuffers = 0.0;  //!< Mean command buffers per warm frame.
  double pathEncodes = 0.0;     //!< Mean CPU path encodes per warm frame.
  double drawCalls = 0.0;       //!< Mean draw calls per warm frame.
};

SceneSample MeasureScene(bool moving) {
  SceneSample sample;
  std::shared_ptr<geode::GeodeDevice> device = geode::GeodeDevice::CreateHeadless();
  EXPECT_NE(device, nullptr);
  if (!device) {
    return sample;
  }
  RendererGeode renderer(device);
  SpriteScene scene;
  SVGDocument& document = scene.document();

  // Pipelined: frames are submitted back to back, the CPU only waiting when the renderer paces it.
  std::vector<double> pipelined;
  uint64_t uploadBytes = 0;
  uint64_t uploads = 0;
  uint64_t submits = 0;
  uint64_t commandBuffers = 0;
  uint64_t pathEncodes = 0;
  uint64_t drawCalls = 0;
  for (int frame = 0; frame < kWarmupFrames + kMeasuredFrames; ++frame) {
    const auto start = std::chrono::steady_clock::now();
    if (moving) {
      scene.advance(frame);
    }
    renderer.draw(document);
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (frame >= kWarmupFrames) {
      pipelined.push_back(ms);
      const geode::GeodeCounters& counters = renderer.lastFrameTimings().counters;
      uploadBytes += counters.bufferWriteBytes;
      uploads += counters.bufferWrites;
      submits += counters.submits;
      commandBuffers += counters.commandBuffers;
      pathEncodes += counters.pathEncodes;
      drawCalls += counters.drawCalls;
    }
  }
  EXPECT_EQ(device->waitForQueueIdle(), geode::GpuWaitResult::Complete);

  // Serialized: each frame's GPU work is waited for, splitting CPU recording from the GPU tail.
  std::vector<double> cpu;
  std::vector<double> gpu;
  for (int frame = 0; frame < kMeasuredFrames; ++frame) {
    const auto start = std::chrono::steady_clock::now();
    if (moving) {
      scene.advance(kWarmupFrames + kMeasuredFrames + frame);
    }
    renderer.draw(document);
    const auto recorded = std::chrono::steady_clock::now();
    EXPECT_EQ(device->waitForQueueIdle(), geode::GpuWaitResult::Complete);
    const auto done = std::chrono::steady_clock::now();
    cpu.push_back(std::chrono::duration<double, std::milli>(recorded - start).count());
    gpu.push_back(std::chrono::duration<double, std::milli>(done - recorded).count());
  }

  sample.pipelinedMs = Median(pipelined);
  sample.cpuMs = Median(cpu);
  sample.gpuTailMs = Median(gpu);
  sample.uploadBytes = static_cast<double>(uploadBytes) / kMeasuredFrames;
  sample.uploads = static_cast<double>(uploads) / kMeasuredFrames;
  sample.submits = static_cast<double>(submits) / kMeasuredFrames;
  sample.commandBuffers = static_cast<double>(commandBuffers) / kMeasuredFrames;
  sample.pathEncodes = static_cast<double>(pathEncodes) / kMeasuredFrames;
  sample.drawCalls = static_cast<double>(drawCalls) / kMeasuredFrames;
  return sample;
}

void Report(const char* label, const SceneSample& sample) {
  std::fprintf(stderr,
               "[GeodeSpriteScene] %-7s pipelined=%.3fms cpu=%.3fms gpuTail=%.3fms "
               "uploadKiB=%.1f uploads=%.1f submits=%.2f commandBuffers=%.2f pathEncodes=%.1f "
               "drawCalls=%.1f\n",
               label, sample.pipelinedMs, sample.cpuMs, sample.gpuTailMs,
               sample.uploadBytes / 1024.0, sample.uploads, sample.submits, sample.commandBuffers,
               sample.pathEncodes, sample.drawCalls);
}

TEST(GeodeSpriteSceneWallclockTest, StaticSpriteScene) {
  const SceneSample sample = MeasureScene(/*moving=*/false);
  Report("static", sample);
  RecordProperty("pipelinedMs", std::to_string(sample.pipelinedMs));
  RecordProperty("uploadBytes", std::to_string(sample.uploadBytes));
}

TEST(GeodeSpriteSceneWallclockTest, MovingSpriteScene) {
  const SceneSample sample = MeasureScene(/*moving=*/true);
  Report("moving", sample);
  RecordProperty("pipelinedMs", std::to_string(sample.pipelinedMs));
  RecordProperty("uploadBytes", std::to_string(sample.uploadBytes));
}

}  // namespace
}  // namespace donner::svg
