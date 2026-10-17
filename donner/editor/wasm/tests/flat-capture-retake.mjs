/** One capture-validity retake; capture errors propagate and the deadline never resets. */
export async function retakeFlatCapture(
  { capture, isFlat, retain, deadline, now = () => performance.now() },
) {
  const remaining = () => {
    const ms = deadline - now();
    if (ms < 1) throw new Error("animation capture deadline exceeded");
    return ms;
  };
  const first = await capture(remaining());
  remaining();
  const firstIsFlat = isFlat(first);
  remaining();
  if (!firstIsFlat) return first;
  await retain(first, 0);
  const second = await capture(remaining());
  await retain(second, 1);
  remaining();
  const secondIsFlat = isFlat(second);
  remaining();
  if (secondIsFlat) throw new Error("animation capture remained entirely page background");
  return second;
}

/** Exact opaque #101317 from editor.css; a single differing pixel is not eligible. */
export function isEditorBackgroundImage({ data, channels }) {
  if ((channels !== 3 && channels !== 4) || data.length === 0 || data.length % channels !== 0) {
    return false;
  }
  for (let offset = 0; offset < data.length; offset += channels) {
    if (
      data[offset] !== 16 || data[offset + 1] !== 19 || data[offset + 2] !== 23
      || (channels === 4 && data[offset + 3] !== 255)
    ) return false;
  }
  return true;
}
