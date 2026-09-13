import { expect, test } from "@playwright/test";
import { createHash } from "node:crypto";
import { readFileSync, writeFileSync } from "node:fs";
import path from "node:path";

const fontBytes = readFileSync(process.env.DONNER_REFERENCE_FONT!).toString("base64");
const outputRoot = process.env.TEST_UNDECLARED_OUTPUTS_DIR!;

for (const source of ["attribute", "style"]) {
  for (const adjustment of ["0.3", "none"]) {
    test(`native font-size-adjust ${source} ${adjustment}`, async ({ page, browserName, browser }) => {
      await page.setContent(`<style>
        @font-face { font-family: FixtureNoto; src: url(data:font/ttf;base64,${fontBytes}); }
        body { margin: 0; }
        </style><svg xmlns="http://www.w3.org/2000/svg" width="500" height="500"
          viewBox="0 0 200 200" font-family="FixtureNoto" font-size="64">
          <text id="text" x="100" y="100" text-anchor="middle"
            font-size-adjust="${source === "attribute" ? adjustment : "none"}"
            style="${source === "style" ? `font-size-adjust:${adjustment}` : ""}">Text</text>
          <rect x="1" y="1" width="198" height="198" fill="none" stroke="black"/>
        </svg>`);
      await page.evaluate(async () => { await document.fonts.ready; });
      const metrics = await page.evaluate(() => {
        const text = document.querySelector<SVGTextElement>("#text")!;
        const style = getComputedStyle(text);
        const rect = text.getBBox();
        return {
          supportsCss: CSS.supports("font-size-adjust", "0.3"),
          loaded: document.fonts.check("64px FixtureNoto"),
          size: style.fontSize,
          adjustment: style.fontSizeAdjust,
          kerning: style.fontKerning,
          length: text.getComputedTextLength(),
          bounds: { x: rect.x, y: rect.y, width: rect.width, height: rect.height },
          characters: Array.from({ length: 4 }, (_, i) => {
            const point = text.getStartPositionOfChar(i);
            const extent = text.getExtentOfChar(i);
            return { x: point.x, y: point.y, width: extent.width, height: extent.height };
          }),
        };
      });
      expect(metrics.loaded).toBe(true);
      expect(metrics.size).toBe("64px");
      const name = `${browserName}-font-size-adjust-${source}-${adjustment}`;
      writeFileSync(path.join(outputRoot, `${name}.json`), JSON.stringify({ browser: browserName, version: browser.version(),
        fontSha256: createHash("sha256").update(Buffer.from(fontBytes, "base64")).digest("hex"),
        source, requestedAdjustment: adjustment, ...metrics }, null, 2));
      await page.screenshot({ path: path.join(outputRoot, `${name}.png`) });
    });
  }
}
