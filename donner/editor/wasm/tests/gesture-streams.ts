import { type Page } from "@playwright/test";

/**
 * Realistic trackpad gesture streams for the editor's browser suites.
 *
 * WHY THIS FILE EXISTS
 *
 * The presentation regressions that escaped the board this week were all
 * *timing* defects: a wrong-scale flicker and a backing-store-clear black
 * frame that only appear while a gesture is still arriving. The suites that
 * were supposed to catch them synthesized zoom as a handful of coarse notches
 * delivered in a single task (six wheels of `deltaY = +/-250`, then a 200 ms
 * settle). That shape is not what a trackpad produces, and it is precisely the
 * shape that hides these defects:
 *
 *  - One task per burst means the editor observes the entire zoom delta before
 *    it can render, so it never runs a frame *inside* the gesture. The defects
 *    live in those frames.
 *  - A 200 ms settle after every burst lets the worker catch up, which is
 *    exactly the state in which pixels and CSS agree again.
 *  - Integer `+/-250` notches are ~12x the magnitude of a real trackpad event,
 *    so the run traverses a scale range in six samples instead of a few
 *    hundred, and never lands on the intermediate scales that expose a
 *    mismatched epoch.
 *
 * Real macOS/Chromium trackpad pinch delivers ctrl-flagged wheels at roughly
 * the display refresh rate (~60-120 Hz; 90 Hz is a good midpoint for a
 * ProMotion-class panel), with *fractional* `deltaY`, interleaved
 * `pointermove` jitter from the fingers shifting on the pad, and a decaying
 * momentum tail after lift-off. Measured against the same build, the coarse
 * `6 x 250` shape missed the flicker on every attempt while a 90 Hz fractional
 * stream reproduced it 3/3.
 *
 * These generators dispatch that shape into the live page. They deliberately
 * do the pacing *inside* one `page.evaluate` so the events carry realistic
 * inter-event timing instead of being spaced by the CDP round trip, and they
 * return dispatch counts and measured timings so a test can prove the stream
 * actually ran (a storm the editor ignored proves nothing).
 *
 * All streams target `#canvas`, the editor's ImGui canvas, which is where the
 * input bridge listens. The render pane only classifies wheel input while it
 * is the hovered window, so a caller must `page.mouse.move()` over the pane
 * before running a stream.
 */

/** Viewport-relative point, in CSS pixels. */
export interface GesturePoint {
  x: number;
  y: number;
}

/**
 * What a stream actually delivered.
 *
 * Timings are measured in-page with `performance.now()`, so they reflect the
 * pacing the editor saw rather than the driver's intent. `meanIntervalMs` well
 * above `1000 / hz` means the page was too busy to keep up, which is itself a
 * useful signal: assertions about per-frame behavior are only meaningful when
 * the stream ran at something close to frame cadence.
 */
export interface GestureStreamResult {
  /** Wheel events dispatched during the active (finger-down) phase. */
  activeWheelEvents: number;
  /** Wheel events dispatched during the decaying momentum tail. */
  momentumWheelEvents: number;
  /** `pointermove` events interleaved with the wheels. */
  pointerEvents: number;
  /** Wall-clock duration of the whole stream, measured in-page. */
  elapsedMs: number;
  /** Mean gap between consecutive dispatched events. */
  meanIntervalMs: number;
  /** Largest gap between consecutive dispatched events. */
  maxIntervalMs: number;
  /** Requested cadence, for comparison against `meanIntervalMs`. */
  requestedHz: number;
  /** Sum of dispatched `deltaX`, active phase plus tail. */
  totalDeltaX: number;
  /** Sum of dispatched `deltaY`, active phase plus tail. */
  totalDeltaY: number;
  /**
   * Product of the per-event scales the wheel deltas encode, active phase plus
   * tail. For a pinch this is the gesture's cumulative scale; a caller can
   * compare it against the presented content scale to measure gain.
   */
  encodedScale: number;
}

/** Which engine's wheel encoding to emulate. */
export type PinchDeltaShape = "chromium" | "gecko-linear";

export interface PinchStreamOptions {
  /**
   * Cumulative scale over the whole active phase (`1.25` zooms in 25%).
   * Mutually exclusive with `scalePerSecond`.
   */
  totalScale?: number;
  /**
   * Cumulative scale per second of active phase. Mutually exclusive with
   * `totalScale`. A steady two-finger spread runs around 1.5-3x/second.
   */
  scalePerSecond?: number;
  /** Active (finger-down) phase length. */
  durationMs: number;
  /** Event cadence. Defaults to 90 Hz, a ProMotion-class trackpad. */
  hz?: number;
  /**
   * Wheel encoding.
   *
   * `chromium` (default) is the shape Chromium synthesizes for a trackpad
   * pinch: a ctrl-flagged `DOM_DELTA_PIXEL` wheel whose
   * `deltaY = -100 * ln(perEventScale)`, so `exp(-deltaY / 100)` recovers the
   * gesture scale exactly. The editor's input bridge discriminates that shape
   * from a real ctrl+scroll and applies the desktop zoom calibration to it.
   *
   * `gecko-linear` approximates Gecko, which reports pinch magnitude linearly
   * rather than logarithmically. Included because a bridge that recovers scale
   * with the wrong inverse still looks correct at small deltas and diverges
   * badly at large ones, and only a cross-engine stream shows that.
   */
  shape?: PinchDeltaShape;
  /**
   * Momentum tail length after the active phase. Chromium keeps emitting
   * decaying ctrl-wheels for roughly 200 ms after the fingers lift; a
   * presenter that treats "input stopped" as "safe to resize" only shows the
   * defect once the tail arrives. Set to 0 to disable.
   */
  momentumMs?: number;
  /**
   * Sub-pixel `pointermove` jitter amplitude. Real fingers drift while
   * pinching, and those moves land on the same input queue as the wheels; the
   * interleaving is what makes the editor rerun hit-testing mid-gesture. Set
   * to 0 to disable.
   */
  jitterPx?: number;
}

export interface PanStreamOptions {
  /** Horizontal pan rate, CSS px per second of active phase. */
  dxPerSec: number;
  /** Vertical pan rate, CSS px per second of active phase. */
  dyPerSec: number;
  /** Active (finger-down) phase length. */
  durationMs: number;
  /** Event cadence. Defaults to 90 Hz. */
  hz?: number;
  /** Momentum tail length. Defaults to 200 ms. */
  momentumMs?: number;
  /** Sub-pixel `pointermove` jitter amplitude. */
  jitterPx?: number;
}

export interface BurstZoomStormOptions {
  /** Number of alternating zoom-out / zoom-in bursts. */
  bursts: number;
  /** Coarse notches per burst. */
  notchesPerBurst: number;
  /** Settle time between bursts. */
  settleMs: number;
  /** Notch magnitude in `deltaY` pixels. Defaults to 250. */
  notchDeltaY?: number;
}

/** Shared in-page dispatcher, stringified into `page.evaluate`. */
interface StreamPlanEvent {
  ctrlKey: boolean;
  deltaX: number;
  deltaY: number;
  jitterX: number;
  jitterY: number;
  momentum: boolean;
  pointerMove: boolean;
}

interface StreamPlan {
  events: StreamPlanEvent[];
  intervalMs: number;
  x: number;
  y: number;
}

/**
 * Dispatch a precomputed event plan into the page at a fixed cadence.
 *
 * The whole loop runs in one `page.evaluate` so the inter-event gaps are the
 * page's own timer resolution rather than a CDP round trip per event. A driver
 * that dispatches one event per `page.evaluate` cannot go faster than ~1-2 ms
 * per call under load and jitters unpredictably, which quietly turns a 90 Hz
 * stream into a 30 Hz one and hides exactly the frames under test.
 */
async function runStreamPlan(page: Page, plan: StreamPlan): Promise<GestureStreamResult> {
  return page.evaluate(async (plan: StreamPlan) => {
    const target = document.getElementById("canvas");
    if (target === null) {
      throw new Error("gesture stream: canvas#canvas not found");
    }

    let activeWheelEvents = 0;
    let momentumWheelEvents = 0;
    let pointerEvents = 0;
    let totalDeltaX = 0;
    let totalDeltaY = 0;
    let encodedScale = 1;
    const intervals: number[] = [];

    const start = performance.now();
    let previous = start;
    for (const event of plan.events) {
      const clientX = plan.x + event.jitterX;
      const clientY = plan.y + event.jitterY;
      if (event.pointerMove) {
        target.dispatchEvent(
          new PointerEvent("pointermove", {
            bubbles: true,
            cancelable: true,
            clientX,
            clientY,
            isPrimary: true,
            pointerId: 1,
            pointerType: "mouse",
          }),
        );
        pointerEvents += 1;
      }
      target.dispatchEvent(
        new WheelEvent("wheel", {
          bubbles: true,
          cancelable: true,
          clientX,
          clientY,
          ctrlKey: event.ctrlKey,
          deltaMode: WheelEvent.DOM_DELTA_PIXEL,
          deltaX: event.deltaX,
          deltaY: event.deltaY,
        }),
      );
      if (event.momentum) {
        momentumWheelEvents += 1;
      } else {
        activeWheelEvents += 1;
      }
      totalDeltaX += event.deltaX;
      totalDeltaY += event.deltaY;
      if (event.ctrlKey) {
        encodedScale *= Math.exp(-event.deltaY / 100);
      }

      // Pace against the stream's own start rather than accumulating a fresh
      // timeout per event, so a slow frame does not permanently stretch the
      // cadence.
      const now = performance.now();
      intervals.push(now - previous);
      previous = now;
      const nextAt = start + intervals.length * plan.intervalMs;
      const waitMs = nextAt - now;
      if (waitMs > 0) {
        await new Promise((resolve) => setTimeout(resolve, waitMs));
      }
    }
    const elapsedMs = performance.now() - start;

    return {
      activeWheelEvents,
      momentumWheelEvents,
      pointerEvents,
      elapsedMs,
      meanIntervalMs: intervals.length === 0
        ? 0
        : intervals.reduce((sum, value) => sum + value, 0) / intervals.length,
      maxIntervalMs: intervals.length === 0 ? 0 : Math.max(...intervals),
      requestedHz: plan.intervalMs === 0 ? 0 : 1000 / plan.intervalMs,
      totalDeltaX,
      totalDeltaY,
      encodedScale,
    };
  }, plan);
}

/** Deterministic sub-pixel jitter, so a failing run replays identically. */
function jitter(index: number, amplitudePx: number): { jitterX: number; jitterY: number } {
  if (amplitudePx <= 0) {
    return { jitterX: 0, jitterY: 0 };
  }
  return {
    jitterX: Math.sin(index * 1.7) * amplitudePx,
    jitterY: Math.cos(index * 2.3) * amplitudePx,
  };
}

/**
 * A trackpad pinch, delivered as a paced stream of fractional ctrl-wheels.
 *
 * WHY THE SHAPE MATTERS
 *
 * This is the generator that reproduces the wrong-scale flicker. A pinch is
 * not a few large steps: it is a hundred-plus small ones, each of which asks
 * the editor for a new scale while the worker is still rasterizing the
 * previous one. Every event in that stream is a chance to present epoch N+1
 * pixels under epoch N geometry, and the sampled window is long enough that a
 * one-frame mismatch is observable rather than a coin flip.
 *
 * Concretely, on the same build: the coarse `6 x 250` in-one-task shape the
 * previous suites used never reproduced the flicker, while a 90 Hz fractional
 * stream of the same cumulative scale reproduced it on 3 of 3 runs.
 *
 * The `deltaY = -100 * ln(perEventScale)` encoding is Chromium's, and it is
 * load-bearing: the editor's input bridge recognizes a ctrl-wheel with a
 * fractional delta as a synthesized pinch and applies the desktop zoom
 * calibration, so `encodedScale` in the result is directly comparable to the
 * scale change the presented content should show. A gain gap (the 10.49x one
 * this week, for instance) is then a single division rather than an inference.
 *
 * The momentum tail matters for a different defect class: code that keys
 * "gesture over, safe to commit a resize" off input going quiet commits during
 * the tail, and the resize clears the backing store under a visible canvas.
 */
export async function pinchStream(
  page: Page,
  at: GesturePoint,
  options: PinchStreamOptions,
): Promise<GestureStreamResult> {
  const hz = options.hz ?? 90;
  const shape = options.shape ?? "chromium";
  const momentumMs = options.momentumMs ?? 200;
  const jitterPx = options.jitterPx ?? 0.5;
  if ((options.totalScale === undefined) === (options.scalePerSecond === undefined)) {
    throw new Error("pinchStream: pass exactly one of totalScale or scalePerSecond");
  }

  const intervalMs = 1000 / hz;
  const activeCount = Math.max(1, Math.round((options.durationMs * hz) / 1000));
  const perEventScale = options.totalScale !== undefined
    ? Math.pow(options.totalScale, 1 / activeCount)
    : Math.pow(options.scalePerSecond as number, 1 / hz);

  // Chromium: logarithmic. Gecko: linear in the scale delta, which agrees with
  // the logarithmic form to first order and diverges as the per-event scale
  // grows, so a bridge with the wrong inverse passes at 90 Hz and fails when
  // the same gesture arrives at 30 Hz in larger steps.
  const encode = (scale: number): number =>
    shape === "chromium" ? -100 * Math.log(scale) : -100 * (scale - 1);

  const events: StreamPlanEvent[] = [];
  for (let index = 0; index < activeCount; ++index) {
    events.push({
      ctrlKey: true,
      deltaX: 0,
      deltaY: encode(perEventScale),
      momentum: false,
      // Every third event, matching the ~30 Hz pointer cadence a trackpad
      // interleaves with a 90 Hz wheel stream.
      pointerMove: index % 3 === 0,
      ...jitter(index, jitterPx),
    });
  }

  const momentumCount = Math.max(0, Math.round((momentumMs * hz) / 1000));
  for (let index = 0; index < momentumCount; ++index) {
    // Exponential decay to ~5% of the active per-event magnitude by the end of
    // the tail, which is the envelope Chromium's fling curve produces.
    const decay = Math.exp((-3 * index) / Math.max(1, momentumCount - 1));
    const tailScale = Math.pow(perEventScale, decay);
    events.push({
      ctrlKey: true,
      deltaX: 0,
      deltaY: encode(tailScale),
      momentum: true,
      pointerMove: false,
      ...jitter(activeCount + index, jitterPx),
    });
  }

  return runStreamPlan(page, { events, intervalMs, x: at.x, y: at.y });
}

/**
 * A two-finger scroll (pan), delivered as a paced stream of fractional plain
 * wheels.
 *
 * WHY THE SHAPE MATTERS
 *
 * Pan shipped completely broken with a green board: it never requested a
 * worker epoch, and placement was pinned to the epoch viewport so the surface
 * could not move between epochs. The suite that should have caught it
 * dispatched six identical `deltaY = 40` wheels in one task and then polled
 * the surface's bounding box once, which cannot distinguish "moves smoothly"
 * from "jumps once when the worker finally catches up" from "does not move at
 * all until a later unrelated epoch".
 *
 * A paced fractional stream makes motion a per-frame observable: sample the
 * surface rect every rAF during the stream and a working pan moves in most
 * frames, while an epoch-pinned pan moves in a handful. Fractional deltas
 * matter because integer-only deltas can be absorbed by rounding in a
 * placement path that quantizes to whole device pixels, which is one of the
 * ways a broken pan still looks alive.
 */
export async function panStream(
  page: Page,
  at: GesturePoint,
  options: PanStreamOptions,
): Promise<GestureStreamResult> {
  const hz = options.hz ?? 90;
  const momentumMs = options.momentumMs ?? 200;
  const jitterPx = options.jitterPx ?? 0.5;
  const intervalMs = 1000 / hz;
  const activeCount = Math.max(1, Math.round((options.durationMs * hz) / 1000));

  // Per-event deltas are the per-second rate divided by the cadence, so they
  // are fractional for any realistic pan rate.
  const perEventDeltaX = options.dxPerSec / hz;
  const perEventDeltaY = options.dyPerSec / hz;

  const events: StreamPlanEvent[] = [];
  for (let index = 0; index < activeCount; ++index) {
    events.push({
      ctrlKey: false,
      deltaX: perEventDeltaX,
      deltaY: perEventDeltaY,
      momentum: false,
      pointerMove: index % 3 === 0,
      ...jitter(index, jitterPx),
    });
  }

  const momentumCount = Math.max(0, Math.round((momentumMs * hz) / 1000));
  for (let index = 0; index < momentumCount; ++index) {
    const decay = Math.exp((-3 * index) / Math.max(1, momentumCount - 1));
    events.push({
      ctrlKey: false,
      deltaX: perEventDeltaX * decay,
      deltaY: perEventDeltaY * decay,
      momentum: true,
      pointerMove: false,
      ...jitter(activeCount + index, jitterPx),
    });
  }

  return runStreamPlan(page, { events, intervalMs, x: at.x, y: at.y });
}

/**
 * The coarse burst shape: `notchesPerBurst` large notches delivered in one
 * task, a settle, then the same burst in the opposite direction.
 *
 * WHY THE SHAPE MATTERS
 *
 * This is trackpad reality too, just a different part of it: a fast flick on
 * the pad coalesces into a handful of large wheels that the browser delivers
 * back to back, and the editor sees the entire delta before it can render one
 * frame. That is the worst case for anything that resizes a backing store per
 * epoch, and it is the shape used in this week's black-frame diagnosis: the
 * backing-store clear was visible on ~18% of sampled frames under a burst
 * storm.
 *
 * Keep both shapes. The burst finds "one huge step breaks the invariant"; the
 * paced streams find "a hundred small steps each break it a little". Neither
 * subsumes the other, and this week both classes escaped at once.
 *
 * Unlike the paced streams, the settle here is real: the point of the burst is
 * the transition into and out of quiescence, so the sampled window has to
 * include the quiet part.
 */
export async function burstZoomStorm(
  page: Page,
  at: GesturePoint,
  options: BurstZoomStormOptions,
): Promise<GestureStreamResult> {
  const notchDeltaY = options.notchDeltaY ?? 250;
  const totals: GestureStreamResult = {
    activeWheelEvents: 0,
    momentumWheelEvents: 0,
    pointerEvents: 0,
    elapsedMs: 0,
    meanIntervalMs: 0,
    maxIntervalMs: 0,
    requestedHz: 0,
    totalDeltaX: 0,
    totalDeltaY: 0,
    encodedScale: 1,
  };

  const started = Date.now();
  for (let burst = 0; burst < options.bursts; ++burst) {
    for (const sign of [1, -1]) {
      // One task per direction: `intervalMs = 0` makes `runStreamPlan` dispatch
      // without yielding, which is what "delivered in one task" means.
      const events: StreamPlanEvent[] = [];
      for (let notch = 0; notch < options.notchesPerBurst; ++notch) {
        events.push({
          ctrlKey: true,
          deltaX: 0,
          deltaY: sign * notchDeltaY,
          jitterX: 0,
          jitterY: 0,
          momentum: false,
          pointerMove: notch === 0,
        });
      }
      const result = await runStreamPlan(page, { events, intervalMs: 0, x: at.x, y: at.y });
      totals.activeWheelEvents += result.activeWheelEvents;
      totals.pointerEvents += result.pointerEvents;
      totals.totalDeltaX += result.totalDeltaX;
      totals.totalDeltaY += result.totalDeltaY;
      totals.encodedScale *= result.encodedScale;
      totals.maxIntervalMs = Math.max(totals.maxIntervalMs, result.maxIntervalMs);
      await page.waitForTimeout(options.settleMs);
    }
  }
  totals.elapsedMs = Date.now() - started;
  const dispatched = totals.activeWheelEvents + totals.momentumWheelEvents;
  totals.meanIntervalMs = dispatched === 0 ? 0 : totals.elapsedMs / dispatched;
  return totals;
}
