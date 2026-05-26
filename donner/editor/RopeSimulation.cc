#include "donner/editor/RopeSimulation.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

namespace donner::editor {

namespace {

constexpr double kMinDistance = 1e-6;
constexpr double kMaxCoshArgument = 24.0;
constexpr double kNominalStepSeconds = 1.0 / 60.0;
constexpr double kMaxCatchUpSeconds = 0.25;

double UnitFromSeed(std::uint32_t seed) {
  seed ^= seed >> 16u;
  seed *= 0x7feb352du;
  seed ^= seed >> 15u;
  seed *= 0x846ca68bu;
  seed ^= seed >> 16u;
  return static_cast<double>(seed & 0xffffu) / static_cast<double>(0xffffu);
}

double PolylineLength(std::span<const Vector2d> route) {
  double result = 0.0;
  for (std::size_t i = 1; i < route.size(); ++i) {
    result += route[i - 1].distance(route[i]);
  }
  return result;
}

Vector2d SamplePolyline(std::span<const Vector2d> route, double distance) {
  if (route.empty()) {
    return Vector2d();
  }

  double consumed = 0.0;
  for (std::size_t i = 1; i < route.size(); ++i) {
    const Vector2d segment = route[i] - route[i - 1];
    const double segmentLength = segment.length();
    if (segmentLength <= kMinDistance) {
      continue;
    }

    if (consumed + segmentLength >= distance) {
      const double t = std::clamp((distance - consumed) / segmentLength, 0.0, 1.0);
      return route[i - 1] + segment * t;
    }

    consumed += segmentLength;
  }

  return route.back();
}

double AverageSpeed(std::span<const Vector2d> positions, std::span<const Vector2d> previous) {
  if (positions.size() <= 2 || positions.size() != previous.size()) {
    return 0.0;
  }

  double total = 0.0;
  for (std::size_t i = 1; i + 1 < positions.size(); ++i) {
    total += positions[i].distance(previous[i]);
  }
  return total / static_cast<double>(positions.size() - 2u);
}

double MaxDistance(std::span<const Vector2d> positions, std::span<const Vector2d> reference) {
  if (positions.size() != reference.size()) {
    return 0.0;
  }

  double result = 0.0;
  for (std::size_t i = 0; i < positions.size(); ++i) {
    result = std::max(result, positions[i].distance(reference[i]));
  }
  return result;
}

Vector2d ClampLength(const Vector2d& value, double maxLength) {
  if (maxLength <= 0.0) {
    return Vector2d();
  }

  const double length = value.length();
  return length > maxLength ? value * (maxLength / length) : value;
}

double EffectiveDampingRetention(double activeTimeSeconds, const RopeSimulationOptions& options) {
  const double baseRetention = std::clamp(options.damping, 0.0, 1.0);
  const double targetSeconds = std::max(0.0, options.settleTimeSeconds);
  if (activeTimeSeconds <= targetSeconds) {
    return baseRetention;
  }

  const double overdueRetention = std::clamp(options.overdueDamping, 0.0, 1.0);
  const double rampSeconds = std::max(0.0, options.overdueDampingRampSeconds);
  const double ramp = rampSeconds <= kMinDistance
                          ? 1.0
                          : std::clamp((activeTimeSeconds - targetSeconds) / rampSeconds, 0.0, 1.0);
  return baseRetention + (overdueRetention - baseRetention) * ramp;
}

double SafeCosh(double value) {
  return std::cosh(std::clamp(value, -kMaxCoshArgument, kMaxCoshArgument));
}

std::vector<Vector2d> BuildFallbackCatenaryRoute(const Vector2d& start, const Vector2d& end,
                                                 int particleCount, double sag) {
  std::vector<Vector2d> route;
  route.reserve(static_cast<std::size_t>(particleCount));
  for (int i = 0; i < particleCount; ++i) {
    const double t =
        particleCount <= 1 ? 0.0 : static_cast<double>(i) / static_cast<double>(particleCount - 1);
    const Vector2d chord = start + (end - start) * t;
    route.push_back(Vector2d(chord.x, chord.y + sag * std::sin(std::numbers::pi * t)));
  }
  return route;
}

std::vector<Vector2d> BuildCatenaryRoute(const Vector2d& start, const Vector2d& end,
                                         const RopeSimulationOptions& options) {
  const int segmentCount = std::max(1, options.segmentCount);
  const int particleCount = segmentCount + 1;
  const double chordLength = start.distance(end);
  const double slack =
      std::clamp(std::max(options.catenaryMinSlackPx, chordLength * options.catenarySlackRatio),
                 0.0, std::max(0.0, options.catenaryMaxSlackPx));
  const double sag = std::max(slack * 0.65, 4.0);

  if (std::abs(end.x - start.x) <= kMinDistance || chordLength <= kMinDistance) {
    return BuildFallbackCatenaryRoute(start, end, particleCount, sag);
  }

  const bool reversed = end.x < start.x;
  const Vector2d left = reversed ? end : start;
  const Vector2d right = reversed ? start : end;
  const double horizontalDistance = right.x - left.x;
  const double verticalDistance = right.y - left.y;
  const double arcLength = std::max(chordLength + slack, chordLength + kMinDistance);
  const double horizontalCatenaryDistance = std::sqrt(
      std::max(kMinDistance, arcLength * arcLength - verticalDistance * verticalDistance));
  if (horizontalCatenaryDistance <= horizontalDistance + kMinDistance) {
    return BuildFallbackCatenaryRoute(start, end, particleCount, sag);
  }

  double lo = horizontalDistance / 1000.0;
  double hi = std::max(horizontalDistance, 1.0);
  const auto catenaryDistanceForA = [horizontalDistance](double a) {
    return 2.0 * a * std::sinh(std::clamp(horizontalDistance / (2.0 * a), 0.0, kMaxCoshArgument));
  };

  while (catenaryDistanceForA(hi) > horizontalCatenaryDistance) {
    hi *= 2.0;
  }

  for (int i = 0; i < 64; ++i) {
    const double mid = (lo + hi) * 0.5;
    if (catenaryDistanceForA(mid) > horizontalCatenaryDistance) {
      lo = mid;
    } else {
      hi = mid;
    }
  }

  const double a = (lo + hi) * 0.5;
  const double midX = (left.x + right.x) * 0.5;
  const double x0 = midX + a * std::asinh(verticalDistance / horizontalCatenaryDistance);
  const double c = left.y + a * SafeCosh((left.x - x0) / a);

  std::vector<Vector2d> route;
  route.reserve(static_cast<std::size_t>(particleCount));
  for (int i = 0; i < particleCount; ++i) {
    const double t =
        particleCount <= 1 ? 0.0 : static_cast<double>(i) / static_cast<double>(particleCount - 1);
    const double x = left.x + horizontalDistance * t;
    const double y = c - a * SafeCosh((x - x0) / a);
    route.push_back(Vector2d(x, y));
  }

  if (reversed) {
    std::reverse(route.begin(), route.end());
  }

  return route;
}

}  // namespace

void RopeSimulation::reset(std::span<const Vector2d> route, const RopeSimulationOptions& options) {
  if (route.size() < 2) {
    positions_.clear();
    previousPositions_.clear();
    restPositions_.clear();
    restLengths_.clear();
    catenaryRest_ = false;
    sleep();
    return;
  }

  const int segmentCount = std::max(1, options.segmentCount);
  const int particleCount = segmentCount + 1;
  positions_.assign(static_cast<std::size_t>(particleCount), route.front());

  const double totalLength = PolylineLength(route);
  if (totalLength <= kMinDistance) {
    std::fill(positions_.begin(), positions_.end(), route.front());
  } else {
    for (int i = 0; i < particleCount; ++i) {
      const double distance =
          totalLength * static_cast<double>(i) / static_cast<double>(segmentCount);
      positions_[static_cast<std::size_t>(i)] = SamplePolyline(route, distance);
    }
  }

  previousPositions_ = positions_;
  restPositions_ = positions_;
  hasAnchors_ = true;
  lastStart_ = positions_.front();
  lastEnd_ = positions_.back();
  restLengths_.assign(static_cast<std::size_t>(segmentCount), 0.0);
  for (int i = 0; i < segmentCount; ++i) {
    restLengths_[static_cast<std::size_t>(i)] = positions_[static_cast<std::size_t>(i)].distance(
        positions_[static_cast<std::size_t>(i + 1)]);
  }
  previousStepSeconds_ = std::max(kMinDistance, options.maxDeltaTime);
  catenaryRest_ = false;
  wake();
}

void RopeSimulation::resetCatenary(const Vector2d& start, const Vector2d& end,
                                   const RopeSimulationOptions& options) {
  const std::vector<Vector2d> route = BuildCatenaryRoute(start, end, options);
  reset(route, options);
  positions_.front() = start;
  positions_.back() = end;
  previousPositions_ = positions_;
  restPositions_ = positions_;
  lastStart_ = start;
  lastEnd_ = end;
  hasAnchors_ = true;
  catenaryRest_ = true;
  sleep();
}

void RopeSimulation::applyImpulse(const Vector2d& impulse) {
  if (positions_.size() < 3 || previousPositions_.size() != positions_.size()) {
    return;
  }
  if (impulse.lengthSquared() <= kMinDistance * kMinDistance) {
    return;
  }

  for (std::size_t i = 1; i + 1 < positions_.size(); ++i) {
    const double normalized = static_cast<double>(i) / static_cast<double>(positions_.size() - 1u);
    const double envelope = std::sin(std::numbers::pi * normalized);
    previousPositions_[i] -= impulse * envelope;
  }
  previousStepSeconds_ = kNominalStepSeconds;
  wake();
}

void RopeSimulation::applyBottomImpulse(const Vector2d& impulse, double normalizedWidth) {
  if (positions_.size() < 3 || previousPositions_.size() != positions_.size()) {
    return;
  }
  if (impulse.lengthSquared() <= kMinDistance * kMinDistance) {
    return;
  }

  std::size_t bottomIndex = 1;
  for (std::size_t i = 2; i + 1 < positions_.size(); ++i) {
    if (positions_[i].y > positions_[bottomIndex].y) {
      bottomIndex = i;
    }
  }

  const double center =
      static_cast<double>(bottomIndex) / static_cast<double>(positions_.size() - 1u);
  const double width = std::clamp(normalizedWidth, 0.02, 1.0);
  for (std::size_t i = 1; i + 1 < positions_.size(); ++i) {
    const double normalized = static_cast<double>(i) / static_cast<double>(positions_.size() - 1u);
    const double distance = (normalized - center) / width;
    const double envelope = std::exp(-0.5 * distance * distance);
    previousPositions_[i] -= impulse * envelope;
  }
  previousStepSeconds_ = kNominalStepSeconds;
  wake();
}

void RopeSimulation::update(const Vector2d& start, const Vector2d& end, double deltaTimeSeconds,
                            double scrollDeltaY, double timeSeconds, std::uint32_t phaseSeed,
                            bool frozen, const RopeSimulationOptions& options) {
  if (positions_.size() < 2 || previousPositions_.size() != positions_.size() ||
      restLengths_.size() + 1u != positions_.size()) {
    resetCatenary(start, end, options);
  }

  const bool endpointMoved = applyEndpointMotion(start, end, options, frozen);
  positions_.front() = start;
  positions_.back() = end;
  lastStart_ = start;
  lastEnd_ = end;
  hasAnchors_ = true;
  if (frozen) {
    previousPositions_ = positions_;
    return;
  }

  const bool scrolled = std::abs(scrollDeltaY) > 0.001;
  if (endpointMoved || scrolled) {
    wake();
  }
  if (settled_) {
    previousPositions_ = positions_;
    return;
  }

  const double maxStep = std::max(kMinDistance, options.maxDeltaTime);
  double remaining = std::clamp(deltaTimeSeconds, 0.0, std::max(kMaxCatchUpSeconds, maxStep));
  const double frameStartTime = timeSeconds - remaining;
  double elapsedThisFrame = 0.0;
  bool applyScrollImpulse = scrolled;
  if (remaining <= kMinDistance) {
    solveConstraints(start, end, options);
    return;
  }

  while (remaining > kMinDistance && !settled_) {
    const double deltaTime = std::min(remaining, maxStep);
    const double averageStepMotion = AverageSpeed(positions_, previousPositions_);
    const bool applyIdleSway = !catenaryRest_ &&
                               activeTimeSeconds_ < std::max(0.0, options.settleTimeSeconds) &&
                               averageStepMotion <= options.idleSwayMaxSpeed;
    const double phase = UnitFromSeed(phaseSeed) * 2.0 * std::numbers::pi;
    const double angularFrequency = 2.0 * std::numbers::pi * options.idleSwayFrequencyHz;
    const double stepTimeSeconds = frameStartTime + elapsedThisFrame + deltaTime;
    const double damping = std::pow(EffectiveDampingRetention(activeTimeSeconds_, options),
                                    deltaTime / kNominalStepSeconds);
    const double velocityScale =
        previousStepSeconds_ > kMinDistance ? deltaTime / previousStepSeconds_ : 1.0;

    for (std::size_t i = 1; i + 1 < positions_.size(); ++i) {
      if (applyScrollImpulse) {
        const double scrollImpulse =
            std::clamp(scrollDeltaY * options.scrollResponse, -options.maxScrollImpulsePx,
                       options.maxScrollImpulsePx);
        previousPositions_[i].y -= scrollImpulse;
      }

      Vector2d velocity = (positions_[i] - previousPositions_[i]) * (velocityScale * damping);
      previousPositions_[i] = positions_[i];

      Vector2d acceleration(0.0, catenaryRest_ ? 0.0 : options.gravityPxPerSec2);
      if (catenaryRest_ && restPositions_.size() == positions_.size()) {
        acceleration += (restPositions_[i] - positions_[i]) *
                        std::max(0.0, options.catenaryRestoringForcePerSec2);
      }
      if (applyIdleSway) {
        const double normalized =
            static_cast<double>(i) / static_cast<double>(positions_.size() - 1u);
        const double envelope = std::sin(std::numbers::pi * normalized);
        acceleration.x += options.idleSwayPxPerSec2 * envelope *
                          std::sin(angularFrequency * stepTimeSeconds + phase + normalized * 1.7);
      }

      positions_[i] += velocity + acceleration * (deltaTime * deltaTime);
    }

    const std::vector<Vector2d> beforeConstraints = positions_;
    solveConstraints(start, end, options);
    for (std::size_t i = 1; i + 1 < positions_.size(); ++i) {
      previousPositions_[i] += positions_[i] - beforeConstraints[i];
    }
    previousPositions_.front() = positions_.front();
    previousPositions_.back() = positions_.back();
    const double solvedStepMotion = AverageSpeed(positions_, previousPositions_);
    const double restDistance = catenaryRest_ ? MaxDistance(positions_, restPositions_) : 0.0;
    const bool closeToRest =
        !catenaryRest_ || restDistance <= std::max(0.0, options.settleRestDistanceThresholdPx);
    previousStepSeconds_ = deltaTime;
    activeTimeSeconds_ += deltaTime;
    if (solvedStepMotion <= std::max(0.0, options.settleMotionThresholdPx) && closeToRest) {
      stillTimeSeconds_ += deltaTime;
    } else {
      stillTimeSeconds_ = 0.0;
    }
    elapsedThisFrame += deltaTime;
    remaining -= deltaTime;
    applyScrollImpulse = false;

    if (stillTimeSeconds_ >= std::max(0.0, options.settleStillnessSeconds)) {
      if (catenaryRest_ && restPositions_.size() == positions_.size()) {
        positions_ = restPositions_;
        positions_.front() = start;
        positions_.back() = end;
      }
      sleep();
    }
  }
}

Path RopeSimulation::toPath(const RopeSimulationOptions& options) const {
  PathBuilder builder;
  if (positions_.empty()) {
    return builder.build();
  }

  builder.moveTo(positions_.front());
  if (positions_.size() == 1u) {
    return builder.build();
  }

  if (positions_.size() == 2u) {
    builder.lineTo(positions_.back());
    return builder.build();
  }

  const double tension = std::clamp(options.bezierTension, 0.0, 1.0);
  for (std::size_t i = 1; i + 1 < positions_.size(); ++i) {
    const Vector2d midpoint = (positions_[i] + positions_[i + 1u]) * 0.5;
    const Vector2d relaxedControl = (builder.currentPoint() + midpoint) * 0.5;
    const Vector2d control = relaxedControl + (positions_[i] - relaxedControl) * tension;
    builder.quadTo(control, midpoint);
  }
  builder.quadTo(positions_[positions_.size() - 2u], positions_.back());

  return builder.build();
}

Vector2d RopeSimulation::endTangent() const {
  if (positions_.size() < 2) {
    return Vector2d::XAxis();
  }

  for (std::size_t i = positions_.size() - 1u; i > 0; --i) {
    const Vector2d tangent = positions_[i] - positions_[i - 1u];
    if (tangent.lengthSquared() > kMinDistance * kMinDistance) {
      return tangent;
    }
  }

  return Vector2d::XAxis();
}

void RopeSimulation::retargetCatenaryRest(const Vector2d& start, const Vector2d& end,
                                          const RopeSimulationOptions& options) {
  if (positions_.size() < 2) {
    return;
  }

  const std::vector<Vector2d> route = BuildCatenaryRoute(start, end, options);
  restPositions_.assign(positions_.size(), start);
  const double totalLength = PolylineLength(route);
  if (totalLength <= kMinDistance) {
    std::fill(restPositions_.begin(), restPositions_.end(), start);
  } else {
    for (std::size_t i = 0; i < restPositions_.size(); ++i) {
      const double distance =
          totalLength * static_cast<double>(i) / static_cast<double>(restPositions_.size() - 1u);
      restPositions_[i] = SamplePolyline(route, distance);
    }
  }

  restPositions_.front() = start;
  restPositions_.back() = end;
  restLengths_.assign(positions_.size() - 1u, 0.0);
  for (std::size_t i = 0; i + 1 < restPositions_.size(); ++i) {
    restLengths_[i] = restPositions_[i].distance(restPositions_[i + 1u]);
  }
  catenaryRest_ = true;
}

void RopeSimulation::solveConstraints(const Vector2d& start, const Vector2d& end,
                                      const RopeSimulationOptions& options) {
  if (positions_.size() < 2 || restLengths_.size() + 1u != positions_.size()) {
    return;
  }

  const int iterationCount = std::max(0, options.constraintIterations);
  for (int iteration = 0; iteration < iterationCount; ++iteration) {
    positions_.front() = start;
    positions_.back() = end;

    for (std::size_t i = 0; i + 1 < positions_.size(); ++i) {
      const Vector2d delta = positions_[i + 1u] - positions_[i];
      const double distance = delta.length();
      if (distance <= kMinDistance) {
        continue;
      }

      const double correction = (distance - restLengths_[i]) / distance;
      const Vector2d offset = delta * (0.5 * correction);
      if (i == 0) {
        positions_[i + 1u] -= offset * 2.0;
      } else if (i + 2u == positions_.size()) {
        positions_[i] += offset * 2.0;
      } else {
        positions_[i] += offset;
        positions_[i + 1u] -= offset;
      }
    }
  }

  positions_.front() = start;
  positions_.back() = end;
}

bool RopeSimulation::applyEndpointMotion(const Vector2d& start, const Vector2d& end,
                                         const RopeSimulationOptions& options, bool frozen) {
  if (!hasAnchors_ || positions_.size() < 3 || previousPositions_.size() != positions_.size()) {
    return false;
  }

  const Vector2d startDelta = start - lastStart_;
  const Vector2d endDelta = end - lastEnd_;
  if (startDelta.lengthSquared() + endDelta.lengthSquared() <= kMinDistance * kMinDistance) {
    return false;
  }

  const double follow = frozen ? 1.0 : std::clamp(options.endpointFollow, 0.0, 1.0);
  const double impulseScale = frozen ? 0.0 : std::max(0.0, options.endpointImpulse);
  retargetCatenaryRest(start, end, options);
  const double velocityRetention =
      frozen ? 0.0 : std::clamp(options.endpointMotionVelocityRetention, 0.0, 1.0);
  const double catenaryBlend = frozen ? 0.0 : std::clamp(options.endpointCatenaryBlend, 0.0, 1.0);
  for (std::size_t i = 1; i + 1 < positions_.size(); ++i) {
    const double normalized = static_cast<double>(i) / static_cast<double>(positions_.size() - 1u);
    const Vector2d anchorDelta = startDelta * (1.0 - normalized) + endDelta * normalized;
    const Vector2d carriedDelta = anchorDelta * follow;
    positions_[i] += carriedDelta;
    previousPositions_[i] += carriedDelta;
    if (restPositions_.size() == positions_.size() && catenaryBlend > 0.0) {
      const Vector2d velocity = positions_[i] - previousPositions_[i];
      positions_[i] += (restPositions_[i] - positions_[i]) * catenaryBlend;
      previousPositions_[i] = positions_[i] - velocity * velocityRetention;
    } else {
      const Vector2d velocity = positions_[i] - previousPositions_[i];
      previousPositions_[i] = positions_[i] - velocity * velocityRetention;
    }

    const double envelope = std::sin(std::numbers::pi * normalized);
    const Vector2d impulse =
        ClampLength(anchorDelta * impulseScale, std::max(0.0, options.maxEndpointImpulsePx));
    previousPositions_[i] -= impulse * envelope;
  }
  return !frozen;
}

void RopeSimulation::wake() {
  activeTimeSeconds_ = 0.0;
  stillTimeSeconds_ = 0.0;
  settled_ = false;
}

void RopeSimulation::sleep() {
  previousPositions_ = positions_;
  stillTimeSeconds_ = 0.0;
  settled_ = true;
}

}  // namespace donner::editor
