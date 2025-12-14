import { createGeodeCanvas } from './geode_canvas.js';

function drawDemoGlyph(canvasContext) {
  canvasContext.beginPath();
  canvasContext.moveTo(40, 40);
  canvasContext.lineTo(200, 40);
  canvasContext.lineTo(200, 200);
  canvasContext.lineTo(40, 200);
  canvasContext.closePath();

  canvasContext.moveTo(90, 90);
  canvasContext.quadraticCurveTo(140, 20, 190, 90);
  canvasContext.lineTo(190, 190);
  canvasContext.lineTo(90, 190);
  canvasContext.closePath();
}

async function main() {
  const canvas = document.querySelector('canvas');
  try {
    const geodeCanvas = await createGeodeCanvas(canvas);
    drawDemoGlyph(geodeCanvas);
    await geodeCanvas.fill();
  } catch (err) {
    const pre = document.createElement('pre');
    pre.textContent = `Geode prototype failed: ${err.message}`;
    document.body.appendChild(pre);
  }
}

window.addEventListener('DOMContentLoaded', () => {
  main();
});
