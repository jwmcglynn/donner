import type { Page } from "@playwright/test";

export async function dispatchCanvasPointer(page: Page, point: { x: number; y: number }, click = false) {
  // Headed Firefox on macOS can offset native automation moves but not button events.
  await page.evaluate(({ x, y, click }) => {
    const canvas = document.querySelector("canvas#canvas")!;
    for (const type of click ? ["mousemove", "mousedown", "mouseup"] : ["mousemove"]) {
      canvas.dispatchEvent(
        new MouseEvent(type, {
          bubbles: true,
          cancelable: true,
          view: window,
          clientX: x,
          clientY: y,
          button: 0,
          buttons: type === "mousedown" ? 1 : 0,
        }),
      );
    }
  }, { ...point, click });
}
