#pragma once
/// @file

#include <cstdint>
#include <span>
#include <vector>

#include "donner/base/Path.h"
#include "donner/base/Vector2.h"

namespace donner::editor {

/// Tunable parameters for text-view reference rope simulation.
struct RopeSimulationOptions {
  int segmentCount = 18;              ///< Number of simulated rope segments.
  int constraintIterations = 7;       ///< Distance-constraint solve iterations per frame.
  double gravityPxPerSec2 = 55.0;     ///< Downward acceleration in screen pixels per second.
  double damping = 0.985;             ///< Verlet velocity retained per 60 Hz simulation step.
  double scrollResponse = 0.35;       ///< Extra inertia impulse from source-pane scrolling.
  double maxScrollImpulsePx = 4.0;    ///< Maximum per-frame scroll impulse in screen pixels.
  double idleSwayPxPerSec2 = 18.0;    ///< Low-speed horizontal idle sway acceleration.
  double idleSwayFrequencyHz = 0.45;  ///< Idle sway frequency.
  double idleSwayMaxSpeed = 6.0;      ///< Average substep particle motion under which sway applies.
  double maxDeltaTime = 1.0 / 60.0;   ///< Largest fixed simulation substep.
  double settleTimeSeconds = 5.0;     ///< Target damping window after a disturbance.
  double settleMotionThresholdPx = 0.01;         ///< Average substep motion treated as still.
  double settleStillnessSeconds = 0.25;          ///< Stillness duration required before sleeping.
  double settleRestDistanceThresholdPx = 0.5;    ///< Max distance from rest shape before sleep.
  double overdueDamping = 0.45;                  ///< Velocity retained after missed settle target.
  double overdueDampingRampSeconds = 1.5;        ///< Seconds to ramp into overdue damping.
  double catenaryRestoringForcePerSec2 = 540.0;  ///< Restoring force toward catenary rest shape.
  double bezierTension = 1.0;                    ///< Quadratic Bézier smoothing tension in [0, 1].
  double catenarySlackRatio = 0.10;   ///< Extra catenary length as a ratio of endpoint distance.
  double catenaryMinSlackPx = 18.0;   ///< Minimum extra catenary length in screen pixels.
  double catenaryMaxSlackPx = 70.0;   ///< Maximum extra catenary length in screen pixels.
  double initialImpulsePx = 0.2;      ///< Subtle initial velocity displacement in screen pixels.
  double endpointFollow = 0.82;       ///< Fraction of endpoint movement carried into the body.
  double endpointImpulse = 0.02;      ///< Extra body velocity from endpoint movement.
  double maxEndpointImpulsePx = 1.0;  ///< Clamp for per-frame endpoint-movement impulse.
  double endpointMotionVelocityRetention = 0.35;  ///< Velocity retained after endpoints move.
  double endpointCatenaryBlend = 0.10;            ///< Body blend toward new catenary rest shape.
};

/// Small fixed-endpoint Verlet rope simulation used by source reference connectors.
class RopeSimulation {
public:
  /// Construct an empty rope simulation.
  RopeSimulation() = default;

  /**
   * Reset this rope by sampling \p route into evenly spaced simulated particles.
   *
   * @param route Polyline route from fixed start to fixed end.
   * @param options Simulation options controlling segment count and smoothing.
   */
  void reset(std::span<const Vector2d> route, const RopeSimulationOptions& options);

  /**
   * Reset this rope to a catenary between fixed endpoints.
   *
   * The resulting rest lengths come from the sampled catenary, so gravity and
   * distance constraints settle toward the same hanging-curve family instead
   * of a pre-routed polyline. The rope starts asleep with zero velocity;
   * callers may wake it with \ref applyBottomImpulse or endpoint motion.
   *
   * @param start Fixed rope start point in screen coordinates.
   * @param end Fixed rope end point in screen coordinates.
   * @param options Simulation options controlling slack and segment count.
   */
  void resetCatenary(const Vector2d& start, const Vector2d& end,
                     const RopeSimulationOptions& options);

  /**
   * Add an instantaneous velocity impulse to interior rope particles.
   *
   * @param impulse Screen-space velocity displacement applied with a sine envelope.
   */
  void applyImpulse(const Vector2d& impulse);

  /**
   * Add a localized velocity impulse centered on the lowest interior particle.
   *
   * @param impulse Screen-space velocity displacement at the catenary bottom.
   * @param normalizedWidth Sine-normalized falloff width in [0, 1].
   */
  void applyBottomImpulse(const Vector2d& impulse, double normalizedWidth);

  /**
   * Advance the rope by one frame.
   *
   * @param start Fixed rope start point in screen coordinates.
   * @param end Fixed rope end point in screen coordinates.
   * @param deltaTimeSeconds Frame delta time in seconds.
   * @param scrollDeltaY Source-pane vertical scroll delta since the previous frame.
   * @param timeSeconds Monotonic UI time in seconds, used for idle sway phase.
   * @param phaseSeed Stable per-rope seed used to desynchronize idle sway.
   * @param frozen If true, skip integration and keep the rope body stable for interaction.
   * @param options Simulation options.
   */
  void update(const Vector2d& start, const Vector2d& end, double deltaTimeSeconds,
              double scrollDeltaY, double timeSeconds, std::uint32_t phaseSeed, bool frozen,
              const RopeSimulationOptions& options);

  /// Return the simulated particle positions.
  [[nodiscard]] std::span<const Vector2d> points() const { return positions_; }

  /// Return true when this rope has no particles.
  [[nodiscard]] bool empty() const { return positions_.empty(); }

  /// Return true when this rope still needs timed animation frames.
  [[nodiscard]] bool needsAnimation() const { return !settled_ && positions_.size() >= 2u; }

  /// Convert the current rope body into a Bézier path.
  [[nodiscard]] Path toPath(const RopeSimulationOptions& options) const;

  /// Return the final segment tangent for orienting an arrowhead.
  [[nodiscard]] Vector2d endTangent() const;

private:
  void retargetCatenaryRest(const Vector2d& start, const Vector2d& end,
                            const RopeSimulationOptions& options);
  void solveConstraints(const Vector2d& start, const Vector2d& end,
                        const RopeSimulationOptions& options);
  [[nodiscard]] bool applyEndpointMotion(const Vector2d& start, const Vector2d& end,
                                         const RopeSimulationOptions& options, bool frozen);
  void wake();
  void sleep();

  std::vector<Vector2d> positions_;
  std::vector<Vector2d> previousPositions_;
  std::vector<Vector2d> restPositions_;
  std::vector<double> restLengths_;
  Vector2d lastStart_;
  Vector2d lastEnd_;
  double activeTimeSeconds_ = 0.0;
  double stillTimeSeconds_ = 0.0;
  double previousStepSeconds_ = 1.0 / 60.0;
  bool hasAnchors_ = false;
  bool catenaryRest_ = false;
  bool settled_ = true;
};

}  // namespace donner::editor
