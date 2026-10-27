// The welcome carousel's sample tiles and where to click each one on the smoke suite's 1600x900
// layout: a fraction of the canvas width and a CSS pixel offset from its top. Shared by the smoke
// carousel cases and the performance lane's handoff measurement, which must click the same tiles.

export const kCarouselSamples = [
  { id: "donner-splash", name: "Donner Splash", xFraction: 0.24, y: 282 },
  { id: "geode-splash", name: "Geode Splash", xFraction: 0.5, y: 282 },
  { id: "basic-shapes", name: "Basic Shapes", xFraction: 0.76, y: 282 },
  { id: "text-style", name: "Text and Style", xFraction: 0.24, y: 390 },
  { id: "gradients-clip", name: "Gradients and Clip", xFraction: 0.5, y: 390 },
  { id: "animation", name: "Animation", xFraction: 0.76, y: 390 },
];

/** The carousel layout's viewport, which the sample positions above are measured on. */
export const kCarouselViewport = { width: 1600, height: 900 };
