import type { Page } from "@playwright/test";

export async function dispatchCanvasPointer(page: Page, point: { x: number; y: number }, click = false) {
  // Headed Firefox on macOS can offset native automation moves but not button events.
  return page.evaluate(({ x, y, click }) => {
    const canvas = document.querySelector("canvas#canvas");
    if (!canvas) throw new Error("missing editor canvas");
    const inputType = click ? "mousedown" : "mousemove";
    let dispatchedAt = Number.NaN;
    for (const type of click ? ["mousemove", "mousedown", "mouseup"] : ["mousemove"]) {
      const event = new MouseEvent(type, {
        bubbles: true,
        cancelable: true,
        view: window,
        clientX: x,
        clientY: y,
        button: 0,
        buttons: type === "mousedown" ? 1 : 0,
      });
      // Include every synchronous handler, including earlier capture listeners.
      if (type === inputType) dispatchedAt = performance.now();
      canvas.dispatchEvent(event);
    }
    if (!Number.isFinite(dispatchedAt)) throw new Error("input timestamp was not observed");
    return dispatchedAt;
  }, { ...point, click });
}
