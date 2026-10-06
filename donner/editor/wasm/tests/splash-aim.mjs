/**
 * Aiming at the Donner splash's "D" from the editor's published viewport.
 *
 * The aim maps a fixed document point through the published viewport, so it is right only once
 * that viewport describes the splash. Opening the splash sample is observable before it is
 * displayed: the sample attribute changes and the editor goes idle while the viewport the editor
 * publishes still describes the previous document. An aim taken then lands elsewhere (#1683).
 *
 * Plain JavaScript without `import.meta` or top-level `await`, so Playwright can load it from a
 * TypeScript spec.
 */

/** The Donner splash document's size in document units. */
export const kSplashDocumentSize = Object.freeze({ width: 892, height: 512 });

/** The document point the cases aim at: the D's left stem. */
export const kSplashDAim = Object.freeze({ x: 282, y: 390 });

/**
 * Whether `viewport` describes the splash: its document rectangle is the splash's size at the
 * viewport's zoom, to half a document unit.
 *
 * @param {{documentWidth: number, documentHeight: number, zoom: number} | null | undefined}
 *   viewport
 * @returns {boolean}
 */
export function showsSplashDocument(viewport) {
  if (!viewport || !(viewport.zoom > 0)) return false;
  return Math.abs(viewport.documentWidth / viewport.zoom - kSplashDocumentSize.width) < 0.5
    && Math.abs(viewport.documentHeight / viewport.zoom - kSplashDocumentSize.height) < 0.5;
}

/**
 * The screen point of the D aim in `viewport`.
 *
 * @param {{documentX: number, documentY: number, documentWidth: number,
 *   documentHeight: number}} viewport
 * @returns {{x: number, y: number}}
 */
export function splashDPoint(viewport) {
  return {
    x: Math.round(
      viewport.documentX + viewport.documentWidth * kSplashDAim.x / kSplashDocumentSize.width,
    ),
    y: Math.round(
      viewport.documentY + viewport.documentHeight * kSplashDAim.y / kSplashDocumentSize.height,
    ),
  };
}
