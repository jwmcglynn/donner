// Override HarfBuzz config to re-enable APIs disabled by HB_LEAN/HB_TINY.
// We need hb_font_draw_glyph() for glyph outline extraction.
#undef HB_NO_DRAW
#undef HB_NO_OUTLINE
// Re-enable CFF outlines (our fallback font Public Sans is OTF/CFF).
#undef HB_NO_CFF
#undef HB_NO_OT_FONT_CFF
// Re-enable file I/O (hb_face_create_from_file_or_fail references
// hb_blob_create_from_file_or_fail which is guarded by HB_NO_OPEN).
#undef HB_NO_OPEN
