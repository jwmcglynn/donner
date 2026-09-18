/** Install the pixel classifier used by composited browser sampling. */
export function installCompositedPixelClassifier(config) {
  window.__donnerMatchesCompositedContentPixel = (red, green, blue, alpha) => {
    const passesDefaultGates = alpha >= config.minColorAlpha
      && Math.max(red, green, blue) - Math.min(red, green, blue) >= config.minColorSpread;
    if (!passesDefaultGates) {
      return false;
    }
    if (config.colorMask === "yellow-content") {
      return red - blue >= 60 && green - blue >= 60;
    }
    if (config.colorMask === "basic-blue") {
      return alpha >= 180 && blue > 170 && blue > green + 45 && green > red + 25;
    }
    return true;
  };
}
