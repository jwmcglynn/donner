/**
 * Whether the sample picker has published every finished thumbnail and presentation is idle.
 *
 * The picker publishes each finished sample thumbnail in a later UI frame. Until every rendered
 * thumbnail is published and the frame that published it has completed, another submission is
 * still owed, so a baseline read between two of those frames can catch the last one in flight.
 * `drained` is published only while the picker is visible, which is the state this checks.
 *
 * @param state A responsiveness snapshot with `thumbnails` and `presentation` fields.
 */
export function pickerPresentationSettled(state) {
  const thumbnails = state?.thumbnails;
  const presentation = state?.presentation;
  if (!thumbnails || !presentation) return false;
  return thumbnails.drained === true && thumbnails.pending === false
    && thumbnails.active === false && thumbnails.resultReady === false
    && (thumbnails.ready ?? 0) > 0 && thumbnails.ready === thumbnails.rendered
    && Number.isFinite(thumbnails.publishedAtMs)
    && thumbnails.publishedAtMs <= presentation.completedAtMs
    && presentation.framesInFlight === 0
    && presentation.completedSerial === presentation.submittedSerial;
}
