// Override HarfBuzz config to enable the APIs used by the FreeType-backed shaper.
// Re-enable file I/O (hb_face_create_from_file_or_fail references
// hb_blob_create_from_file_or_fail which is guarded by HB_NO_OPEN).
#undef HB_NO_OPEN
// Preserve font-provided vertical advances and per-glyph origins.
#undef HB_NO_VERTICAL
// Keep variation composite decoding outside the shaping capability set.
#define HB_NO_VAR_COMPOSITES
