#pragma once
/// @file
/// Test helpers that skip a test case at run time when the configured text tier lacks a feature.

#include <gtest/gtest.h>

#include "donner/svg/text/TextTier.h"

/**
 * Skips the current test unless text rendering is built in (\ref donner::svg::TextTier::Basic or
 * \ref donner::svg::TextTier::Full). Use as the first statement of a test body; a reason may be
 * streamed after it.
 */
#define DONNER_SKIP_UNLESS_TEXT_BUILT_IN()                        \
  if (::donner::svg::IsTextBuiltIn()) {                           \
  } else                                                          \
    GTEST_SKIP() << "Text is not built into this configuration (" \
                 << ::donner::svg::BuiltTextTier() << ") "

/**
 * Skips the current test unless full text (FreeType and HarfBuzz) is built in. Use as the
 * first statement of a test body; a reason may be streamed after it.
 */
#define DONNER_SKIP_UNLESS_FULL_TEXT_BUILT_IN()                        \
  if (::donner::svg::IsFullTextBuiltIn()) {                            \
  } else                                                               \
    GTEST_SKIP() << "Full text is not built into this configuration (" \
                 << ::donner::svg::BuiltTextTier() << ") "

/**
 * Skips the current test unless the font manager loads WOFF2 fonts. Use as the first statement of
 * a test body; a reason may be streamed after it.
 */
#define DONNER_SKIP_UNLESS_WOFF2_BUILT_IN()                        \
  if (::donner::svg::IsWoff2BuiltIn()) {                           \
  } else                                                           \
    GTEST_SKIP() << "WOFF2 is not built into this configuration (" \
                 << ::donner::svg::BuiltTextTier() << ") "
