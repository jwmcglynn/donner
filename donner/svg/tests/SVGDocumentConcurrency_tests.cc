#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <optional>
#include <thread>
#include <vector>

#include "donner/base/RcString.h"
#include "donner/base/RcStringOrRef.h"
#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"
#include "donner/css/Color.h"
#include "donner/svg/SVGClipPathElement.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGFEGaussianBlurElement.h"
#include "donner/svg/SVGFEOffsetElement.h"
#include "donner/svg/SVGFilterElement.h"
#include "donner/svg/SVGImageElement.h"
#include "donner/svg/SVGLineElement.h"
#include "donner/svg/SVGLinearGradientElement.h"
#include "donner/svg/SVGMarkerElement.h"
#include "donner/svg/SVGMaskElement.h"
#include "donner/svg/SVGPathElement.h"
#include "donner/svg/SVGPatternElement.h"
#include "donner/svg/SVGPolygonElement.h"
#include "donner/svg/SVGPolylineElement.h"
#include "donner/svg/SVGRadialGradientElement.h"
#include "donner/svg/SVGRectElement.h"
#include "donner/svg/SVGSVGElement.h"
#include "donner/svg/SVGStopElement.h"
#include "donner/svg/SVGStyleElement.h"
#include "donner/svg/SVGSymbolElement.h"
#include "donner/svg/SVGTSpanElement.h"
#include "donner/svg/SVGTextElement.h"
#include "donner/svg/SVGTextPathElement.h"
#include "donner/svg/SVGUseElement.h"

namespace donner::svg {
namespace {

template <typename Element, typename Callback>
auto ReadElement(Element element, Callback callback) {
  return element.withReadAccess(
      [&](DocumentReadAccess&, EntityHandle) { return callback(element); });
}

TEST(SVGDocumentConcurrencyTests, ConcurrentDomSerializesDocumentLevelWrites) {
  SVGDocument document;
  document.setThreadingMode(ThreadingMode::ConcurrentDom);

  constexpr int kThreadCount = 8;
  constexpr int kIterations = 25;
  const std::uint64_t initialRevision = document.handle()->revision();

  std::vector<std::thread> threads;
  threads.reserve(kThreadCount);
  for (int threadIndex = 0; threadIndex < kThreadCount; ++threadIndex) {
    threads.emplace_back([document, threadIndex]() mutable {
      for (int iteration = 0; iteration < kIterations; ++iteration) {
        const int size = 100 + threadIndex * kIterations + iteration;
        document.setCanvasSize(size, size);
      }
    });
  }

  for (std::thread& thread : threads) {
    thread.join();
  }

  EXPECT_GE(document.handle()->revision(),
            initialRevision + static_cast<std::uint64_t>(kThreadCount * kIterations));
  EXPECT_GT(document.canvasSize().x, 0);
  EXPECT_GT(document.canvasSize().y, 0);
}

TEST(SVGDocumentConcurrencyTests, AccessGuardsExposeRegistry) {
  SVGDocument document;
  DocumentReadAccess readAccess = document.readAccess();

  EXPECT_EQ(&readAccess.registry(), &document.registry());
}

TEST(SVGDocumentConcurrencyTests, AccessDiagnosticsTrackConcurrentDomLocks) {
  SVGDocument document;
  document.setThreadingMode(ThreadingMode::ConcurrentDom);

  {
    DocumentReadAccess readAccess = document.readAccess();
    DocumentAccessDiagnostics diagnostics = document.handle()->accessDiagnostics();
    EXPECT_EQ(diagnostics.readAccesses, 1u);
    EXPECT_EQ(diagnostics.readLocksAcquired, 1u);
    EXPECT_EQ(diagnostics.reentrantReadAccesses, 0u);
    EXPECT_EQ(diagnostics.activeReadLocks, 1u);
    EXPECT_FALSE(diagnostics.writeLockHeld);
  }

  DocumentAccessDiagnostics diagnostics = document.handle()->accessDiagnostics();
  EXPECT_EQ(diagnostics.activeReadLocks, 0u);
  EXPECT_GT(diagnostics.totalReadLockHeldNs, 0u);
  EXPECT_GT(diagnostics.maxReadLockHeldNs, 0u);
  EXPECT_GE(diagnostics.totalReadLockHeldNs, diagnostics.maxReadLockHeldNs);

  document.setCanvasSize(16, 16);
  diagnostics = document.handle()->accessDiagnostics();
  EXPECT_EQ(diagnostics.writeAccesses, 1u);
  EXPECT_EQ(diagnostics.writeLocksAcquired, 1u);
  EXPECT_EQ(diagnostics.reentrantWriteAccesses, 0u);
  EXPECT_FALSE(diagnostics.writeLockHeld);
  EXPECT_GT(diagnostics.totalWriteLockHeldNs, 0u);
  EXPECT_GT(diagnostics.maxWriteLockHeldNs, 0u);
  EXPECT_GE(diagnostics.totalWriteLockHeldNs, diagnostics.maxWriteLockHeldNs);

  document.withWriteAccess([&document](DocumentWriteAccess&) {
    DocumentAccessDiagnostics duringWrite = document.handle()->accessDiagnostics();
    EXPECT_EQ(duringWrite.writeAccesses, 2u);
    EXPECT_EQ(duringWrite.writeLocksAcquired, 2u);
    EXPECT_TRUE(duringWrite.writeLockHeld);

    DocumentReadAccess nestedRead = document.readAccess();
    DocumentAccessDiagnostics duringNestedRead = document.handle()->accessDiagnostics();
    EXPECT_EQ(duringNestedRead.readAccesses, 2u);
    EXPECT_EQ(duringNestedRead.readLocksAcquired, 1u);
    EXPECT_EQ(duringNestedRead.reentrantReadAccesses, 1u);
    EXPECT_EQ(duringNestedRead.activeReadLocks, 0u);

    document.setCanvasSize(32, 32);
    DocumentAccessDiagnostics afterNestedWrite = document.handle()->accessDiagnostics();
    EXPECT_EQ(afterNestedWrite.writeAccesses, 3u);
    EXPECT_EQ(afterNestedWrite.writeLocksAcquired, 2u);
    EXPECT_EQ(afterNestedWrite.reentrantWriteAccesses, 1u);
    EXPECT_TRUE(afterNestedWrite.writeLockHeld);
  });

  diagnostics = document.handle()->accessDiagnostics();
  EXPECT_EQ(diagnostics.activeReadLocks, 0u);
  EXPECT_FALSE(diagnostics.writeLockHeld);
  EXPECT_GE(diagnostics.totalWriteLockHeldNs, diagnostics.maxWriteLockHeldNs);
  EXPECT_GE(diagnostics.totalReadLockHeldNs, diagnostics.maxReadLockHeldNs);

  document.handle()->resetAccessDiagnostics();
  diagnostics = document.handle()->accessDiagnostics();
  EXPECT_EQ(diagnostics.readAccesses, 0u);
  EXPECT_EQ(diagnostics.writeAccesses, 0u);
  EXPECT_EQ(diagnostics.readLocksAcquired, 0u);
  EXPECT_EQ(diagnostics.writeLocksAcquired, 0u);
  EXPECT_EQ(diagnostics.totalReadLockHeldNs, 0u);
  EXPECT_EQ(diagnostics.totalWriteLockHeldNs, 0u);
  EXPECT_EQ(diagnostics.maxReadLockHeldNs, 0u);
  EXPECT_EQ(diagnostics.maxWriteLockHeldNs, 0u);
  EXPECT_EQ(diagnostics.activeReadLocks, 0u);
  EXPECT_FALSE(diagnostics.writeLockHeld);
}

TEST(SVGDocumentConcurrencyTests, WithAccessHelpersScopeRegistryAccessAndBatchRevisions) {
  SVGDocument document;
  SVGRectElement rect = SVGRectElement::Create(document);
  document.setThreadingMode(ThreadingMode::ConcurrentDom);

  const Registry* registry =
      document.withReadAccess([&document](DocumentReadAccess& access) -> const Registry* {
        EXPECT_EQ(&access.registry(), &document.registry());
        return &document.registry();
      });
  EXPECT_EQ(registry, &document.unsafeRegistry());

  const std::uint64_t initialRevision = document.handle()->revision();
  document.withWriteAccess([](DocumentWriteAccess&) {});
  EXPECT_EQ(document.handle()->revision(), initialRevision);

  document.withWriteAccess([&document, rect](DocumentWriteAccess& access) mutable {
    EXPECT_EQ(&access.registry(), &document.registry());
    rect.setX(Lengthd(10));
    rect.setY(Lengthd(20));
    document.setCanvasSize(32, 32);
  });

  EXPECT_EQ(document.handle()->revision(), initialRevision + 1);
  EXPECT_EQ(document.canvasSize(), Vector2i(32, 32));
  EXPECT_EQ(ReadElement(rect, [](auto rect) { return rect.x(); }), Lengthd(10));

  const std::uint64_t typedMutationRevision = document.handle()->revision();
  document.withWriteAccess([&document, rect](SVGDocumentMutation& mutation) mutable {
    EXPECT_EQ(&mutation.access().registry(), &document.registry());
    mutation.setAttribute(rect, "data-x", "30");
    mutation.setAttribute(rect, "data-y", "40");
    mutation.removeAttribute(rect, "data-y");
    mutation.setCanvasSize(64, 64);
  });

  EXPECT_EQ(document.handle()->revision(), typedMutationRevision + 1);
  EXPECT_EQ(document.canvasSize(), Vector2i(64, 64));
  EXPECT_EQ(ReadElement(rect, [](auto rect) { return rect.getAttribute("data-x"); }),
            RcString("30"));
  EXPECT_FALSE(ReadElement(rect, [](auto rect) { return rect.hasAttribute("data-y"); }));
}

TEST(SVGDocumentConcurrencyTests, MutationLogRecordsCommittedRevisions) {
  SVGDocument document;
  SVGRectElement rect = SVGRectElement::Create(document);
  document.setThreadingMode(ThreadingMode::ConcurrentDom);

  const std::uint64_t initialRevision = document.handle()->revision();
  const std::uint64_t initialSequence = document.handle()->mutationSequence();

  document.withWriteAccess([](DocumentWriteAccess&) {});
  DocumentMutationLogSnapshot snapshot = document.handle()->mutationRecordsSince(initialSequence);
  EXPECT_TRUE(snapshot.records.empty());
  EXPECT_EQ(snapshot.latestSequence, initialSequence);
  EXPECT_FALSE(snapshot.missedRecords);

  document.withWriteAccess([rect](SVGDocumentMutation& mutation) mutable {
    mutation.setAttribute(rect, "data-a", "1");
    mutation.setAttribute(rect, "data-b", "2");
  });

  snapshot = document.handle()->mutationRecordsSince(initialSequence);
  ASSERT_EQ(snapshot.records.size(), 1u);
  EXPECT_EQ(snapshot.records.front().sequence, initialSequence + 1);
  EXPECT_EQ(snapshot.records.front().revision, initialRevision + 1);
  EXPECT_EQ(snapshot.latestSequence, initialSequence + 1);
  EXPECT_FALSE(snapshot.missedRecords);

  rect.setX(Lengthd(12));

  DocumentMutationLogSnapshot secondSnapshot =
      document.handle()->mutationRecordsSince(snapshot.latestSequence);
  ASSERT_EQ(secondSnapshot.records.size(), 1u);
  EXPECT_EQ(secondSnapshot.records.front().sequence, initialSequence + 2);
  EXPECT_EQ(secondSnapshot.records.front().revision, initialRevision + 2);
  EXPECT_EQ(secondSnapshot.latestSequence, initialSequence + 2);
  EXPECT_FALSE(secondSnapshot.missedRecords);
}

TEST(SVGDocumentConcurrencyTests, ConcurrentDomAllowsLegacyRawAccessInsideGuards) {
  SVGDocument document;
  SVGRectElement rect = SVGRectElement::Create(document);
  document.setThreadingMode(ThreadingMode::ConcurrentDom);

  const Registry* registry = document.withReadAccess(
      [&document](DocumentReadAccess&) -> const Registry* { return &document.registry(); });
  EXPECT_EQ(registry, &document.unsafeRegistry());

  const bool legacyEntityHandleMatchesScopedHandle =
      rect.withReadAccess([rect](DocumentReadAccess&, EntityHandle scopedHandle) mutable {
        EntityHandle legacyHandle = rect.entityHandle();
        return legacyHandle.valid() && legacyHandle.registry() == scopedHandle.registry() &&
               legacyHandle.entity() == scopedHandle.entity();
      });
  EXPECT_TRUE(legacyEntityHandleMatchesScopedHandle);

  EXPECT_EQ(rect.unsafeEntityHandle().registry(), &document.unsafeRegistry());
  EXPECT_TRUE(rect.unsafeEntityHandle().valid());
}

TEST(SVGDocumentConcurrencyTests, ConcurrentDomSerializesElementHandleCopies) {
  SVGDocument document;
  SVGRectElement rect = SVGRectElement::Create(document);
  document.setThreadingMode(ThreadingMode::ConcurrentDom);

  constexpr int kThreadCount = 8;
  constexpr int kIterations = 100;
  std::atomic<bool> sawInvalidHandle = false;

  std::vector<std::thread> threads;
  threads.reserve(kThreadCount);
  for (int threadIndex = 0; threadIndex < kThreadCount; ++threadIndex) {
    threads.emplace_back([rect, &sawInvalidHandle]() {
      for (int iteration = 0; iteration < kIterations; ++iteration) {
        SVGElement copy = rect;
        const bool isValid = copy.withReadAccess(
            [](DocumentReadAccess&, EntityHandle handle) { return handle.valid(); });
        if (!isValid) {
          sawInvalidHandle.store(true, std::memory_order_relaxed);
        }
      }
    });
  }

  for (std::thread& thread : threads) {
    thread.join();
  }

  EXPECT_FALSE(sawInvalidHandle.load(std::memory_order_relaxed));
  EXPECT_TRUE(
      rect.withReadAccess([](DocumentReadAccess&, EntityHandle handle) { return handle.valid(); }));
}

TEST(SVGDocumentConcurrencyTests, ConcurrentDomSerializesBaseTreeMutations) {
  SVGDocument document;
  SVGSVGElement root = document.svgElement();

  constexpr int kChildCount = 16;
  std::vector<SVGRectElement> children;
  children.reserve(kChildCount);
  for (int index = 0; index < kChildCount; ++index) {
    children.push_back(SVGRectElement::Create(document));
  }

  document.setThreadingMode(ThreadingMode::ConcurrentDom);

  std::vector<std::thread> threads;
  threads.reserve(kChildCount);
  for (int index = 0; index < kChildCount; ++index) {
    threads.emplace_back([root, child = children[index]]() mutable { root.appendChild(child); });
  }

  for (std::thread& thread : threads) {
    thread.join();
  }

  const int observedChildren = ReadElement(root, [](auto root) {
    int result = 0;
    for (std::optional<SVGElement> child = root.firstChild(); child; child = child->nextSibling()) {
      ++result;
    }
    return result;
  });

  EXPECT_EQ(observedChildren, kChildCount);
}

TEST(SVGDocumentConcurrencyTests, ConcurrentDomSerializesDerivedGeometrySetters) {
  SVGDocument document;
  SVGRectElement rect = SVGRectElement::Create(document);
  document.setThreadingMode(ThreadingMode::ConcurrentDom);

  constexpr int kThreadCount = 8;
  constexpr int kIterations = 100;
  const std::uint64_t initialRevision = document.handle()->revision();

  std::vector<std::thread> threads;
  threads.reserve(kThreadCount);
  for (int threadIndex = 0; threadIndex < kThreadCount; ++threadIndex) {
    threads.emplace_back([rect, threadIndex]() mutable {
      for (int iteration = 0; iteration < kIterations; ++iteration) {
        rect.setX(Lengthd(threadIndex * kIterations + iteration));
        (void)ReadElement(rect, [](auto rect) { return rect.x(); });
      }
    });
  }

  for (std::thread& thread : threads) {
    thread.join();
  }

  EXPECT_GE(document.handle()->revision(),
            initialRevision + static_cast<std::uint64_t>(kThreadCount * kIterations));
  EXPECT_TRUE(
      rect.withReadAccess([](DocumentReadAccess&, EntityHandle handle) { return handle.valid(); }));
}

TEST(SVGDocumentConcurrencyTests, ConcurrentDomSerializesPathLineAndTransformSetters) {
  SVGDocument document;
  SVGPathElement path = SVGPathElement::Create(document);
  SVGLineElement line = SVGLineElement::Create(document);
  SVGRectElement rect = SVGRectElement::Create(document);
  document.setThreadingMode(ThreadingMode::ConcurrentDom);

  constexpr int kIterations = 100;
  const std::uint64_t initialRevision = document.handle()->revision();

  std::thread pathThread([path]() mutable {
    for (int iteration = 0; iteration < kIterations; ++iteration) {
      path.setD(RcString("M 0 0 L 1 1"));
      (void)ReadElement(path, [](auto path) { return path.d(); });
    }
  });

  std::thread lineThread([line]() mutable {
    for (int iteration = 0; iteration < kIterations; ++iteration) {
      line.setX1(Lengthd(iteration));
      (void)ReadElement(line, [](auto line) { return line.x1(); });
    }
  });

  std::thread transformThread([rect]() mutable {
    for (int iteration = 0; iteration < kIterations; ++iteration) {
      rect.setTransform(Transform2d::Translate(Vector2d(iteration, iteration)));
      (void)rect.transform();
    }
  });

  pathThread.join();
  lineThread.join();
  transformThread.join();

  EXPECT_GE(document.handle()->revision(), initialRevision + 3u * kIterations);
}

TEST(SVGDocumentConcurrencyTests, ConcurrentDomSerializesStyleLineAndPolySetters) {
  SVGDocument document;
  SVGStyleElement style = SVGStyleElement::Create(document);
  SVGLineElement line = SVGLineElement::Create(document);
  SVGPolygonElement polygon = SVGPolygonElement::Create(document);
  SVGPolylineElement polyline = SVGPolylineElement::Create(document);
  document.setThreadingMode(ThreadingMode::ConcurrentDom);

  constexpr int kIterations = 100;
  const std::uint64_t initialRevision = document.handle()->revision();

  std::thread styleThread([style]() mutable {
    for (int iteration = 0; iteration < kIterations; ++iteration) {
      style.setType("text/css");
      style.setContents("rect { fill: red; }");
      (void)ReadElement(style, [](auto style) { return style.isCssType(); });
    }
  });

  std::thread lineThread([line]() mutable {
    for (int iteration = 0; iteration < kIterations; ++iteration) {
      line.setX1(Lengthd(iteration));
      line.setY2(Lengthd(iteration + 1));
      ReadElement(line, [](auto line) {
        (void)line.x1();
        (void)line.y2();
      });
    }
  });

  std::thread polyThread([polygon, polyline]() mutable {
    for (int iteration = 0; iteration < kIterations; ++iteration) {
      polygon.setPoints({Vector2d(iteration, 0.0), Vector2d(1.0, iteration), Vector2d(2.0, 2.0)});
      polyline.setPoints({Vector2d(0.0, iteration), Vector2d(iteration, 1.0)});
      (void)ReadElement(polygon, [](auto polygon) { return polygon.points().size(); });
      (void)ReadElement(polyline, [](auto polyline) { return polyline.points().size(); });
    }
  });

  styleThread.join();
  lineThread.join();
  polyThread.join();

  EXPECT_GE(document.handle()->revision(), initialRevision + 6u * kIterations);
  EXPECT_TRUE(ReadElement(style, [](auto style) { return style.isCssType(); }));
  EXPECT_EQ(ReadElement(polygon, [](auto polygon) { return polygon.points().size(); }), 3u);
  EXPECT_EQ(ReadElement(polyline, [](auto polyline) { return polyline.points().size(); }), 2u);
}

TEST(SVGDocumentConcurrencyTests, ConcurrentDomSerializesGradientAndStopSetters) {
  SVGDocument document;
  SVGLinearGradientElement linear = SVGLinearGradientElement::Create(document);
  SVGRadialGradientElement radial = SVGRadialGradientElement::Create(document);
  SVGStopElement stop = SVGStopElement::Create(document);
  document.setThreadingMode(ThreadingMode::ConcurrentDom);

  constexpr int kIterations = 100;
  const std::uint64_t initialRevision = document.handle()->revision();

  std::thread linearThread([linear]() mutable {
    for (int iteration = 0; iteration < kIterations; ++iteration) {
      linear.setX1(Lengthd(iteration));
      linear.setGradientTransform(Transform2d::Translate(Vector2d(iteration, iteration)));
      (void)ReadElement(linear, [](auto linear) { return linear.x1(); });
      (void)linear.gradientTransform();
    }
  });

  std::thread radialThread([radial]() mutable {
    for (int iteration = 0; iteration < kIterations; ++iteration) {
      radial.setR(Lengthd(iteration + 1));
      radial.setFr(Lengthd(iteration));
      ReadElement(radial, [](auto radial) {
        (void)radial.r();
        (void)radial.fr();
      });
    }
  });

  std::thread stopThread([stop]() mutable {
    for (int iteration = 0; iteration < kIterations; ++iteration) {
      stop.setOffset(static_cast<float>(iteration % 100) / 100.0f);
      stop.setStopColor(css::Color(css::RGBA(iteration % 255, 0, 255, 255)));
      stop.setStopOpacity(static_cast<double>(iteration % 100) / 100.0);
      (void)stop.computedStopOpacity();
    }
  });

  linearThread.join();
  radialThread.join();
  stopThread.join();

  EXPECT_GE(document.handle()->revision(), initialRevision + 7u * kIterations);
  EXPECT_TRUE(ReadElement(linear, [](auto linear) { return linear.x1().has_value(); }));
  EXPECT_TRUE(ReadElement(radial, [](auto radial) { return radial.r().has_value(); }));
  EXPECT_GE(ReadElement(stop, [](auto stop) { return stop.stopOpacity(); }), 0.0);
}

TEST(SVGDocumentConcurrencyTests, ConcurrentDomSerializesFilterAndPrimitiveSetters) {
  SVGDocument document;
  SVGFilterElement filter = SVGFilterElement::Create(document);
  SVGFEGaussianBlurElement blur = SVGFEGaussianBlurElement::Create(document);
  SVGFEOffsetElement offset = SVGFEOffsetElement::Create(document);
  filter.appendChild(blur);
  filter.appendChild(offset);
  document.setThreadingMode(ThreadingMode::ConcurrentDom);

  constexpr int kIterations = 100;
  const std::uint64_t initialRevision = document.handle()->revision();

  std::thread filterThread([filter]() mutable {
    for (int iteration = 0; iteration < kIterations; ++iteration) {
      filter.setX(Lengthd(iteration));
      filter.setFilterUnits(iteration % 2 == 0 ? FilterUnits::UserSpaceOnUse
                                               : FilterUnits::ObjectBoundingBox);
      filter.setPrimitiveUnits(iteration % 2 == 0 ? PrimitiveUnits::UserSpaceOnUse
                                                  : PrimitiveUnits::ObjectBoundingBox);
      ReadElement(filter, [](auto filter) {
        (void)filter.x();
        (void)filter.filterUnits();
        (void)filter.primitiveUnits();
      });
    }
  });

  std::thread primitiveRegionThread([offset]() mutable {
    for (int iteration = 0; iteration < kIterations; ++iteration) {
      offset.setX(Lengthd(iteration));
      offset.setResult(RcStringOrRef("offsetOut"));
      ReadElement(offset, [](auto offset) {
        (void)offset.x();
        (void)offset.result();
      });
    }
  });

  std::thread blurThread([blur]() mutable {
    for (int iteration = 0; iteration < kIterations; ++iteration) {
      blur.setStdDeviation(iteration, iteration + 1);
      ReadElement(blur, [](auto blur) {
        (void)blur.stdDeviationX();
        (void)blur.stdDeviationY();
      });
    }
  });

  std::thread offsetThread([offset]() mutable {
    for (int iteration = 0; iteration < kIterations; ++iteration) {
      offset.setOffset(iteration, -iteration);
      ReadElement(offset, [](auto offset) {
        (void)offset.dx();
        (void)offset.dy();
      });
    }
  });

  filterThread.join();
  primitiveRegionThread.join();
  blurThread.join();
  offsetThread.join();

  EXPECT_GE(document.handle()->revision(), initialRevision + 7u * kIterations);
  EXPECT_TRUE(ReadElement(offset, [](auto offset) { return offset.result().has_value(); }));
  EXPECT_GE(ReadElement(blur, [](auto blur) { return blur.stdDeviationY(); }), 0.0);
}

TEST(SVGDocumentConcurrencyTests, ConcurrentDomSerializesResourceElementSetters) {
  SVGDocument document;
  SVGPatternElement pattern = SVGPatternElement::Create(document);
  SVGMarkerElement marker = SVGMarkerElement::Create(document);
  SVGMaskElement mask = SVGMaskElement::Create(document);
  SVGSymbolElement symbol = SVGSymbolElement::Create(document);
  SVGImageElement image = SVGImageElement::Create(document);
  SVGUseElement use = SVGUseElement::Create(document);
  SVGClipPathElement clipPath = SVGClipPathElement::Create(document);
  document.setThreadingMode(ThreadingMode::ConcurrentDom);

  constexpr int kIterations = 100;
  const std::uint64_t initialRevision = document.handle()->revision();

  std::thread patternThread([pattern]() mutable {
    for (int iteration = 0; iteration < kIterations; ++iteration) {
      pattern.setX(Lengthd(iteration));
      pattern.setPatternUnits(iteration % 2 == 0 ? PatternUnits::UserSpaceOnUse
                                                 : PatternUnits::ObjectBoundingBox);
      pattern.setPatternTransform(Transform2d::Translate(Vector2d(iteration, iteration)));
      pattern.setHref(RcStringOrRef("#basePattern"));
      ReadElement(pattern, [](auto pattern) {
        (void)pattern.x();
        (void)pattern.patternUnits();
      });
      (void)pattern.patternTransform();
    }
  });

  std::thread markerMaskThread([marker, mask, clipPath]() mutable {
    for (int iteration = 0; iteration < kIterations; ++iteration) {
      marker.setMarkerWidth(Lengthd(iteration + 1.0));
      marker.setRefX(Lengthd(iteration));
      marker.setOrient(MarkerOrient::AngleDegrees(iteration));
      mask.setX(Lengthd(iteration));
      mask.setMaskUnits(iteration % 2 == 0 ? MaskUnits::UserSpaceOnUse
                                           : MaskUnits::ObjectBoundingBox);
      clipPath.setClipPathUnits(iteration % 2 == 0 ? ClipPathUnits::UserSpaceOnUse
                                                   : ClipPathUnits::ObjectBoundingBox);
      (void)ReadElement(marker, [](auto marker) { return marker.markerWidth(); });
      (void)ReadElement(mask, [](auto mask) { return mask.x(); });
      (void)ReadElement(clipPath, [](auto clipPath) { return clipPath.clipPathUnits(); });
    }
  });

  std::thread imageUseSymbolThread([image, use, symbol]() mutable {
    for (int iteration = 0; iteration < kIterations; ++iteration) {
      image.setHref(RcStringOrRef("texture.png"));
      image.setX(Lengthd(iteration));
      use.setHref(RcString("#symbol"));
      use.setX(Lengthd(iteration));
      symbol.setX(Lengthd(iteration));
      symbol.setRefX(iteration);
      (void)ReadElement(image, [](auto image) { return image.href(); });
      (void)ReadElement(use, [](auto use) { return use.href(); });
      (void)ReadElement(symbol, [](auto symbol) { return symbol.refX(); });
    }
  });

  patternThread.join();
  markerMaskThread.join();
  imageUseSymbolThread.join();

  EXPECT_GE(document.handle()->revision(), initialRevision + 16u * kIterations);
  std::optional<RcString> patternHref =
      ReadElement(pattern, [](auto pattern) { return pattern.href(); });
  ASSERT_TRUE(patternHref.has_value());
  EXPECT_EQ(patternHref.value(), "#basePattern");
  EXPECT_EQ(ReadElement(image, [](auto image) { return image.href(); }), "texture.png");
  EXPECT_EQ(ReadElement(use, [](auto use) { return use.href(); }), "#symbol");
}

TEST(SVGDocumentConcurrencyTests, ConcurrentDomSerializesTextSetters) {
  SVGDocument document;
  SVGTextElement text = SVGTextElement::Create(document);
  SVGTSpanElement tspan = SVGTSpanElement::Create(document);
  SVGTextPathElement textPath = SVGTextPathElement::Create(document);
  text.appendChild(tspan);
  text.appendChild(textPath);
  document.setThreadingMode(ThreadingMode::ConcurrentDom);

  constexpr int kIterations = 100;
  const std::uint64_t initialRevision = document.handle()->revision();

  std::thread textThread([text]() mutable {
    for (int iteration = 0; iteration < kIterations; ++iteration) {
      text.setX(Lengthd(iteration));
      text.setTextLength(Lengthd(iteration + 1));
      text.setLengthAdjust(iteration % 2 == 0 ? LengthAdjust::Spacing
                                              : LengthAdjust::SpacingAndGlyphs);
      text.appendText("x");
      ReadElement(text, [](auto text) {
        (void)text.x();
        (void)text.textLength();
        (void)text.textContent();
      });
    }
  });

  std::thread tspanThread([tspan]() mutable {
    for (int iteration = 0; iteration < kIterations; ++iteration) {
      tspan.setDx(Lengthd(iteration));
      tspan.setDy(Lengthd(iteration + 1));
      tspan.setRotate(iteration);
      ReadElement(tspan, [](auto tspan) {
        (void)tspan.dx();
        (void)tspan.dy();
        (void)tspan.rotateList();
      });
    }
  });

  std::thread textPathThread([textPath]() mutable {
    for (int iteration = 0; iteration < kIterations; ++iteration) {
      textPath.setHref(RcStringOrRef("#path"));
      textPath.setStartOffset(Lengthd(iteration));
      textPath.appendText("p");
      ReadElement(textPath, [](auto textPath) {
        (void)textPath.href();
        (void)textPath.startOffset();
        (void)textPath.textContent();
      });
    }
  });

  textThread.join();
  tspanThread.join();
  textPathThread.join();

  EXPECT_GE(document.handle()->revision(), initialRevision + 10u * kIterations);
  EXPECT_FALSE(ReadElement(text, [](auto text) { return text.textContent().empty(); }));
  EXPECT_FALSE(ReadElement(textPath, [](auto textPath) { return textPath.textContent().empty(); }));
}

TEST(SVGDocumentConcurrencyTests, ConcurrentDomStressHasDeterministicFinalState) {
  SVGDocument document;
  SVGSVGElement root = document.svgElement();

  constexpr int kThreadCount = 8;
  constexpr int kIterations = 75;
  std::vector<SVGRectElement> rects;
  rects.reserve(kThreadCount);
  for (int index = 0; index < kThreadCount; ++index) {
    rects.push_back(SVGRectElement::Create(document));
    root.appendChild(rects.back());
  }

  document.setThreadingMode(ThreadingMode::ConcurrentDom);

  std::atomic<bool> start = false;
  std::atomic<int> completedWriters = 0;
  std::atomic<bool> sawInvalidRead = false;
  const std::uint64_t initialRevision = document.handle()->revision();

  std::vector<std::thread> writers;
  writers.reserve(kThreadCount);
  for (int index = 0; index < kThreadCount; ++index) {
    writers.emplace_back([rect = rects[index], index, &start, &completedWriters]() mutable {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }

      for (int iteration = 0; iteration < kIterations; ++iteration) {
        rect.setX(Lengthd(index * 1000 + iteration));
        rect.setY(Lengthd(iteration));
        ReadElement(rect, [](auto rect) {
          (void)rect.x();
          (void)rect.y();
        });
      }

      completedWriters.fetch_add(1, std::memory_order_release);
    });
  }

  std::thread reader([rects, &start, &completedWriters, &sawInvalidRead]() mutable {
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }

    while (completedWriters.load(std::memory_order_acquire) < kThreadCount) {
      for (SVGRectElement& rect : rects) {
        const bool valid = rect.withReadAccess(
            [](DocumentReadAccess&, EntityHandle handle) { return handle.valid(); });
        if (!valid) {
          sawInvalidRead.store(true, std::memory_order_relaxed);
        }
        ReadElement(rect, [](auto rect) {
          (void)rect.x();
          (void)rect.y();
        });
      }
    }
  });

  start.store(true, std::memory_order_release);

  for (std::thread& writer : writers) {
    writer.join();
  }
  reader.join();

  EXPECT_FALSE(sawInvalidRead.load(std::memory_order_relaxed));
  EXPECT_GE(document.handle()->revision(),
            initialRevision + static_cast<std::uint64_t>(kThreadCount * kIterations * 2));

  for (int index = 0; index < kThreadCount; ++index) {
    EXPECT_EQ(ReadElement(rects[index], [](auto rect) { return rect.x(); }),
              Lengthd(index * 1000 + kIterations - 1));
    EXPECT_EQ(ReadElement(rects[index], [](auto rect) { return rect.y(); }),
              Lengthd(kIterations - 1));
  }
}

// Regression test for the codex-flagged P1 self-deadlock in `ElementAnchor::release()`. When the
// last public handle to a *detached* `SVGElement` destructs *inside* a `withReadAccess` callback
// (a perfectly-normal API pattern: a detached element local going out of scope while the calling
// thread holds a read guard), the destructor's release() path used to unconditionally acquire
// `documentHandle_->write()`. In ConcurrentDom the writer drains all readers without supporting a
// read→write upgrade, so it would wait on the calling thread's own held read — hanging forever.
// The fix bails when the thread holds read-but-not-write (the opportunistic detached-node Collect
// happens on the next periodic Collect pass instead). If the deadlock regresses, this test hangs
// the entire `donner_svg_concurrency_tests` target until its bazel timeout fires.
TEST(SVGDocumentConcurrencyTests, ReleaseOfDetachedHandleInsideReadAccessDoesNotSelfDeadlock) {
  SVGDocument document;
  SVGRectElement rect = SVGRectElement::Create(document);
  rect.remove();  // mark the element detached so release() takes the cleanup path
  document.setThreadingMode(ThreadingMode::ConcurrentDom);

  document.withReadAccess([&rect](DocumentReadAccess&) {
    // The last public handle is moved into the read-access scope; when `moved` destructs at the end
    // of this lambda the calling thread still holds the read guard. Without the fix this hangs.
    SVGRectElement moved = std::move(rect);
    (void)moved;
  });

  SUCCEED();  // reaching this line means release() did not self-deadlock
}

}  // namespace
}  // namespace donner::svg
