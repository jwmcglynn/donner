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
      await page.evaluate(async () => {
        await document.fonts.ready;
      });
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
      writeFileSync(
        path.join(outputRoot, `${name}.json`),
        JSON.stringify(
          {
            browser: browserName,
            version: browser.version(),
            fontSha256: createHash("sha256").update(Buffer.from(fontBytes, "base64")).digest("hex"),
            source,
            requestedAdjustment: adjustment,
            ...metrics,
          },
          null,
          2,
        ),
      );
      await page.screenshot({ path: path.join(outputRoot, `${name}.png`) });
    });
  }
}

test("native kerning boundaries", async ({ page, browserName, browser }) => {
  const directory = path.dirname(process.env.DONNER_REFERENCE_FONT!);
  const fonts = [
    { family: "BoundaryNoto", weight: "400", file: "NotoSans-Regular.ttf" },
    { family: "BoundaryNoto", weight: "700", file: "NotoSans-Bold.ttf" },
    { family: "BoundaryOther", weight: "400", file: "MPLUS1p-Regular.ttf" },
  ].map((font) => {
    const bytes = readFileSync(path.join(directory, font.file));
    return {
      ...font,
      base64: bytes.toString("base64"),
      sha256: createHash("sha256").update(bytes).digest("hex"),
    };
  });
  const scenarios = [
    { name: "same", first: "A", second: "V", size: 64, weight: 400, child: "" },
    { name: "size-up", first: "T", second: "e", size: 48, weight: 400, child: "font-size='80'" },
    { name: "size-down", first: "x", second: "t", size: 80, weight: 400, child: "font-size='48'" },
    {
      name: "weight-up",
      first: "A",
      second: "V",
      size: 64,
      weight: 400,
      child: "font-weight='700'",
    },
    {
      name: "weight-down",
      first: "V",
      second: "A",
      size: 64,
      weight: 700,
      child: "font-weight='400'",
    },
    {
      name: "family",
      first: "A",
      second: "V",
      size: 64,
      weight: 400,
      child: "font-family='BoundaryOther'",
    },
  ];
  const faces = fonts.map((font) =>
    `@font-face { font-family:${font.family}; font-weight:${font.weight}; src:url(data:font/ttf;base64,${font.base64}); }`
  ).join("\n");
  const texts = scenarios.flatMap((scenario, index) =>
    ["normal", "none"].map((kerning) =>
      `<text id='${scenario.name}-${kerning}' x='${kerning === "normal" ? 10 : 260}' y='${
        70 + index * 75
      }' font-family='BoundaryNoto' font-weight='${scenario.weight}' font-size='${scenario.size}' style='font-kerning:${kerning}'>${scenario.first}<tspan ${scenario.child}>${scenario.second}</tspan></text>`
    )
  ).join("\n");
  await page.setContent(
    `<style>${faces} body { margin:0; }</style><svg width='500' height='500' viewBox='0 0 500 500'>${texts}</svg>`,
  );
  const measurements = await page.evaluate(async () => {
    const loaded = await Promise.all([
      document.fonts.load("400 64px BoundaryNoto", "AVText"),
      document.fonts.load("700 64px BoundaryNoto", "AVText"),
      document.fonts.load("400 64px BoundaryOther", "AVText"),
    ]);
    await document.fonts.ready;
    const styleFields = (element: Element) => {
      const style = getComputedStyle(element);
      return {
        family: style.fontFamily,
        size: style.fontSize,
        weight: style.fontWeight,
        kerning: style.fontKerning,
      };
    };
    return {
      loaded: loaded.map((faces) =>
        faces.map((face) => ({ family: face.family, weight: face.weight, status: face.status }))
      ),
      texts: Array.from(document.querySelectorAll<SVGTextElement>("text"), (text) => ({
        id: text.id,
        root: styleFields(text),
        child: styleFields(text.querySelector("tspan")!),
        count: text.getNumberOfChars(),
        length: text.getComputedTextLength(),
        characters: Array.from({ length: text.getNumberOfChars() }, (_, index) => {
          const start = text.getStartPositionOfChar(index);
          const end = text.getEndPositionOfChar(index);
          return {
            start: { x: start.x, y: start.y },
            end: { x: end.x, y: end.y },
            length: text.getSubStringLength(index, 1),
          };
        }),
      })),
    };
  });
  const name = `${browserName}-kerning-boundaries`;
  writeFileSync(
    path.join(outputRoot, `${name}.json`),
    JSON.stringify(
      {
        browser: browserName,
        version: browser.version(),
        fonts: fonts.map(({ base64, ...font }) => font),
        scenarios,
        ...measurements,
      },
      null,
      2,
    ),
  );
  await page.screenshot({ path: path.join(outputRoot, `${name}.png`) });
  expect(measurements.loaded.every((faces) => faces.length === 1 && faces[0].status === "loaded"))
    .toBe(true);
  expect(measurements.texts).toHaveLength(12);
  expect(measurements.texts.every((text) => text.count === 2)).toBe(true);
  const regular = measurements.texts.filter((text) => text.id.startsWith("same-"));
  const spacing = regular.map((text) => text.characters[1].start.x - text.characters[0].start.x);
  expect(spacing[0]).toBeLessThan(spacing[1]);
});
