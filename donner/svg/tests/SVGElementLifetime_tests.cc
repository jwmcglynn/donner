#include <gtest/gtest.h>

#include <algorithm>
#include <optional>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/xml/XMLNode.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGGElement.h"
#include "donner/svg/SVGRectElement.h"
#include "donner/svg/components/NodeLifetimeCollector.h"
#include "donner/svg/components/NodeLifetimeComponent.h"
#include "donner/svg/graph/Reference.h"
#include "donner/svg/parser/SVGParser.h"

namespace donner::svg {
namespace {

using components::NodeLifetimeComponent;

bool DetachedQueueContains(const SVGDocument& document, Entity entity) {
  const auto& roots = document.handle()->detachedNodeState().detachedRoots;
  return std::find(roots.begin(), roots.end(), entity) != roots.end();
}

const NodeLifetimeComponent& LifetimeOf(const SVGElement& element) {
  return element.entityHandle().get<NodeLifetimeComponent>();
}

TEST(SVGElementLifetimeTests, CopiesAndMovesTrackExternalReferences) {
  SVGDocument document;
  SVGRectElement rect = SVGRectElement::Create(document);
  EXPECT_EQ(LifetimeOf(rect).externalRefCount(), 1u);

  {
    SVGElement copy = rect;
    EXPECT_EQ(LifetimeOf(rect).externalRefCount(), 2u);

    {
      SVGElement moved = std::move(copy);
      EXPECT_EQ(LifetimeOf(rect).externalRefCount(), 2u);
      EXPECT_EQ(moved, rect);
    }

    EXPECT_EQ(LifetimeOf(rect).externalRefCount(), 1u);
  }

  EXPECT_EQ(LifetimeOf(rect).externalRefCount(), 1u);
}

TEST(SVGElementLifetimeTests, AssignmentReleasesPreviousReference) {
  SVGDocument document;
  SVGRectElement first = SVGRectElement::Create(document);
  SVGRectElement second = SVGRectElement::Create(document);
  EXPECT_EQ(LifetimeOf(first).externalRefCount(), 1u);
  EXPECT_EQ(LifetimeOf(second).externalRefCount(), 1u);

  SVGElement assigned = first;
  EXPECT_EQ(LifetimeOf(first).externalRefCount(), 2u);
  EXPECT_EQ(LifetimeOf(second).externalRefCount(), 1u);

  assigned = second;

  EXPECT_EQ(LifetimeOf(first).externalRefCount(), 1u);
  EXPECT_EQ(LifetimeOf(second).externalRefCount(), 2u);
}

TEST(SVGElementLifetimeTests, HeldElementKeepsDocumentStorageAlive) {
  std::optional<SVGRectElement> retained;

  {
    SVGDocument document;
    retained.emplace(SVGRectElement::Create(document));
    EXPECT_EQ(LifetimeOf(*retained).externalRefCount(), 1u);
  }

  ASSERT_TRUE(retained.has_value());
  EXPECT_TRUE(retained->entityHandle().valid());
  EXPECT_EQ(LifetimeOf(*retained).externalRefCount(), 1u);
}

TEST(SVGElementLifetimeTests, DetachedElementStillTracksExternalReferences) {
  SVGDocument document;
  SVGSVGElement root = document.svgElement();
  SVGRectElement rect = SVGRectElement::Create(document);
  root.appendChild(rect);

  root.removeChild(rect);

  EXPECT_FALSE(LifetimeOf(rect).isAttached());
  EXPECT_EQ(LifetimeOf(rect).externalRefCount(), 1u);
  rect.setAttribute("data-state", "detached");
  EXPECT_TRUE(rect.hasAttribute("data-state"));

  {
    SVGElement copy = rect;
    EXPECT_EQ(LifetimeOf(rect).externalRefCount(), 2u);
  }

  EXPECT_EQ(LifetimeOf(rect).externalRefCount(), 1u);
}

TEST(SVGElementLifetimeTests, DetachedElementCollectedAfterLastHandleIsReleased) {
  SVGDocument document;
  SVGSVGElement root = document.svgElement();
  std::optional<SVGRectElement> rect;
  rect.emplace(SVGRectElement::Create(document));
  const Entity rectEntity = rect->entityHandle().entity();
  root.appendChild(*rect);

  root.removeChild(*rect);

  EXPECT_TRUE(document.registry().valid(rectEntity));

  rect.reset();

  EXPECT_FALSE(document.registry().valid(rectEntity));
}

TEST(SVGElementLifetimeTests, DetachedRootsAreQueuedInDocumentState) {
  SVGDocument document;
  SVGSVGElement root = document.svgElement();
  std::optional<SVGRectElement> rect;
  rect.emplace(SVGRectElement::Create(document));
  const Entity rectEntity = rect->entityHandle().entity();
  root.appendChild(*rect);

  root.removeChild(*rect);

  EXPECT_TRUE(DetachedQueueContains(document, rectEntity));

  rect.reset();

  EXPECT_FALSE(DetachedQueueContains(document, rectEntity));
  EXPECT_FALSE(document.registry().valid(rectEntity));
}

TEST(SVGElementLifetimeTests, DetachedNodeDiagnosticsTrackRetainedAndCollectedRoots) {
  SVGDocument document;
  SVGSVGElement root = document.svgElement();
  std::optional<SVGRectElement> rect;
  rect.emplace(SVGRectElement::Create(document));
  std::optional<SVGElement> rectCopy;
  rectCopy.emplace(*rect);
  const Entity rectEntity = rect->entityHandle().entity();
  root.appendChild(*rect);

  root.removeChild(*rect);

  DetachedNodeDiagnostics diagnostics =
      components::NodeLifetimeCollector::Diagnostics(document.registry());
  EXPECT_EQ(diagnostics.queuedDetachedRoots, 1u);
  EXPECT_EQ(diagnostics.retainedByPublicHandles, 1u);
  EXPECT_EQ(diagnostics.retainedByPublicHandlesInLastPass, 1u);
  EXPECT_EQ(diagnostics.maxPublicHandlesOnRetainedRoot, 2u);
  EXPECT_EQ(diagnostics.retainedBySnapshotOrObserverEpochs, 0u);
  EXPECT_EQ(diagnostics.maxRetainedSnapshotOrObserverEpoch, 0u);
  EXPECT_EQ(diagnostics.collectedInLastPass, 0u);
  EXPECT_FALSE(diagnostics.isCollecting);
  EXPECT_TRUE(document.registry().valid(rectEntity));

  rectCopy.reset();

  diagnostics = components::NodeLifetimeCollector::Diagnostics(document.registry());
  EXPECT_EQ(diagnostics.queuedDetachedRoots, 1u);
  EXPECT_EQ(diagnostics.retainedByPublicHandles, 1u);
  EXPECT_EQ(diagnostics.retainedByPublicHandlesInLastPass, 1u);
  EXPECT_EQ(diagnostics.maxPublicHandlesOnRetainedRoot, 1u);
  EXPECT_EQ(diagnostics.collectedInLastPass, 0u);
  EXPECT_TRUE(document.registry().valid(rectEntity));

  rect.reset();

  diagnostics = components::NodeLifetimeCollector::Diagnostics(document.registry());
  EXPECT_EQ(diagnostics.queuedDetachedRoots, 0u);
  EXPECT_EQ(diagnostics.retainedByPublicHandles, 0u);
  EXPECT_EQ(diagnostics.retainedByPublicHandlesInLastPass, 0u);
  EXPECT_EQ(diagnostics.maxPublicHandlesOnRetainedRoot, 0u);
  EXPECT_EQ(diagnostics.collectedInLastPass, 1u);
  EXPECT_FALSE(diagnostics.isCollecting);
  EXPECT_FALSE(document.registry().valid(rectEntity));
}

TEST(SVGElementLifetimeTests, CollectionDeferralRetainsDetachedRootsUntilGuardExits) {
  SVGDocument document;
  SVGSVGElement root = document.svgElement();
  std::optional<SVGRectElement> rect;
  rect.emplace(SVGRectElement::Create(document));
  const Entity rectEntity = rect->entityHandle().entity();
  root.appendChild(*rect);

  {
    DetachedNodeCollectionDeferral deferral = document.handle()->deferDetachedNodeCollection();

    root.removeChild(*rect);
    rect.reset();

    DetachedNodeDiagnostics diagnostics =
        components::NodeLifetimeCollector::Diagnostics(document.registry());
    EXPECT_EQ(diagnostics.queuedDetachedRoots, 1u);
    EXPECT_EQ(diagnostics.retainedByPublicHandles, 0u);
    EXPECT_EQ(diagnostics.retainedByPublicHandlesInLastPass, 0u);
    EXPECT_EQ(diagnostics.retainedBySnapshotOrObserverEpochs, 1u);
    EXPECT_EQ(diagnostics.maxRetainedSnapshotOrObserverEpoch, deferral.epoch());
    EXPECT_EQ(diagnostics.collectedInLastPass, 0u);
    EXPECT_TRUE(document.registry().valid(rectEntity));
  }

  components::NodeLifetimeCollector::Collect(document.registry());

  DetachedNodeDiagnostics diagnostics =
      components::NodeLifetimeCollector::Diagnostics(document.registry());
  EXPECT_EQ(diagnostics.queuedDetachedRoots, 0u);
  EXPECT_EQ(diagnostics.retainedBySnapshotOrObserverEpochs, 0u);
  EXPECT_EQ(diagnostics.maxRetainedSnapshotOrObserverEpoch, 0u);
  EXPECT_EQ(diagnostics.collectedInLastPass, 1u);
  EXPECT_FALSE(document.registry().valid(rectEntity));
}

TEST(SVGElementLifetimeTests, RepeatedCreateRemoveReinsertCyclesCollectDetachedNodes) {
  SVGDocument document;
  SVGSVGElement root = document.svgElement();

  for (int cycle = 0; cycle < 64; ++cycle) {
    std::optional<SVGRectElement> rect;
    rect.emplace(SVGRectElement::Create(document));
    const Entity rectEntity = rect->entityHandle().entity();

    root.appendChild(*rect);
    root.removeChild(*rect);
    EXPECT_TRUE(DetachedQueueContains(document, rectEntity));
    EXPECT_TRUE(document.registry().valid(rectEntity));

    root.appendChild(*rect);
    EXPECT_FALSE(DetachedQueueContains(document, rectEntity));
    EXPECT_TRUE(document.registry().valid(rectEntity));

    root.removeChild(*rect);
    rect.reset();

    DetachedNodeDiagnostics diagnostics =
        components::NodeLifetimeCollector::Diagnostics(document.registry());
    EXPECT_EQ(diagnostics.queuedDetachedRoots, 0u);
    EXPECT_EQ(diagnostics.retainedByPublicHandles, 0u);
    EXPECT_EQ(diagnostics.collectedInLastPass, 1u);
    EXPECT_FALSE(document.registry().valid(rectEntity));
  }
}

TEST(SVGElementLifetimeTests, DescendantHandleKeepsDetachedSubtreeAlive) {
  SVGDocument document;
  SVGSVGElement root = document.svgElement();
  std::optional<SVGGElement> group;
  std::optional<SVGRectElement> rect;
  group.emplace(SVGGElement::Create(document));
  rect.emplace(SVGRectElement::Create(document));
  const Entity groupEntity = group->entityHandle().entity();
  const Entity rectEntity = rect->entityHandle().entity();
  root.appendChild(*group);
  group->appendChild(*rect);

  root.removeChild(*group);
  group.reset();

  DetachedNodeDiagnostics diagnostics =
      components::NodeLifetimeCollector::Diagnostics(document.registry());
  EXPECT_EQ(diagnostics.queuedDetachedRoots, 1u);
  EXPECT_EQ(diagnostics.retainedByPublicHandles, 1u);
  EXPECT_EQ(diagnostics.retainedByPublicHandlesInLastPass, 1u);
  EXPECT_EQ(diagnostics.maxPublicHandlesOnRetainedRoot, 1u);
  EXPECT_TRUE(document.registry().valid(groupEntity));
  EXPECT_TRUE(document.registry().valid(rectEntity));

  rect.reset();

  diagnostics = components::NodeLifetimeCollector::Diagnostics(document.registry());
  EXPECT_EQ(diagnostics.queuedDetachedRoots, 0u);
  EXPECT_EQ(diagnostics.retainedByPublicHandles, 0u);
  EXPECT_EQ(diagnostics.retainedByPublicHandlesInLastPass, 0u);
  EXPECT_EQ(diagnostics.maxPublicHandlesOnRetainedRoot, 0u);
  EXPECT_EQ(diagnostics.collectedInLastPass, 1u);
  EXPECT_FALSE(document.registry().valid(groupEntity));
  EXPECT_FALSE(document.registry().valid(rectEntity));
}

TEST(SVGElementLifetimeTests, ReattachedElementIsNotCollectedWhenHandleIsReleased) {
  SVGDocument document;
  SVGSVGElement root = document.svgElement();
  std::optional<SVGRectElement> rect;
  rect.emplace(SVGRectElement::Create(document));
  const Entity rectEntity = rect->entityHandle().entity();
  root.appendChild(*rect);
  root.removeChild(*rect);

  root.appendChild(*rect);
  rect.reset();

  EXPECT_TRUE(document.registry().valid(rectEntity));
}

TEST(SVGElementLifetimeTests, DetachedElementDoesNotResolveByReferenceWhileHeld) {
  SVGDocument document;
  SVGSVGElement root = document.svgElement();
  SVGRectElement rect = SVGRectElement::Create(document);
  rect.setId("target");
  root.appendChild(rect);

  EXPECT_TRUE(Reference("#target").resolve(document.registry()).has_value());
  EXPECT_TRUE(Reference("external.svg#target").resolveFragment(document.registry()).has_value());

  root.removeChild(rect);

  EXPECT_FALSE(Reference("#target").resolve(document.registry()).has_value());
  EXPECT_FALSE(Reference("external.svg#target").resolveFragment(document.registry()).has_value());

  root.appendChild(rect);

  EXPECT_TRUE(Reference("#target").resolve(document.registry()).has_value());
  EXPECT_TRUE(Reference("external.svg#target").resolveFragment(document.registry()).has_value());
}

TEST(SVGElementLifetimeTests, DetachedDuplicateIdDoesNotMaskAttachedReference) {
  SVGDocument document;
  SVGSVGElement root = document.svgElement();
  SVGRectElement detached = SVGRectElement::Create(document);
  SVGRectElement attached = SVGRectElement::Create(document);
  detached.setId("target");
  attached.setId("target");
  root.appendChild(detached);
  root.appendChild(attached);

  root.removeChild(detached);

  const std::optional<ResolvedReference> resolved =
      Reference("#target").resolve(document.registry());
  ASSERT_TRUE(resolved.has_value());
  EXPECT_EQ(resolved->handle.entity(), attached.entityHandle().entity());
}

TEST(SVGElementLifetimeTests, XmlNodeRemoveUsesLifetimeAwareSvgMutation) {
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto maybeDocument = parser::SVGParser::ParseSVG(
      R"(<svg xmlns="http://www.w3.org/2000/svg"><rect id="target"/></svg>)", warnings);
  ASSERT_FALSE(maybeDocument.hasError());

  SVGDocument document = std::move(maybeDocument.result());
  std::optional<SVGElement> retained = document.querySelector("#target");
  ASSERT_TRUE(retained.has_value());
  const Entity rectEntity = retained->entityHandle().entity();
  std::optional<xml::XMLNode> retainedNode = xml::XMLNode::TryCast(retained->entityHandle());
  ASSERT_TRUE(retainedNode.has_value());

  retainedNode->remove();

  EXPECT_FALSE(LifetimeOf(*retained).isAttached());
  EXPECT_FALSE(document.querySelector("#target").has_value());
  EXPECT_TRUE(document.registry().valid(rectEntity));

  retained.reset();

  EXPECT_FALSE(document.registry().valid(rectEntity));
}

TEST(SVGElementLifetimeTests, XmlNodeAppendChildReattachesDetachedSvgSubtree) {
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto maybeDocument = parser::SVGParser::ParseSVG(
      R"(<svg xmlns="http://www.w3.org/2000/svg"><g id="parent"><rect id="target"/></g></svg>)",
      warnings);
  ASSERT_FALSE(maybeDocument.hasError());

  SVGDocument document = std::move(maybeDocument.result());
  std::optional<SVGElement> parent = document.querySelector("#parent");
  std::optional<SVGElement> retained = document.querySelector("#target");
  ASSERT_TRUE(parent.has_value());
  ASSERT_TRUE(retained.has_value());
  std::optional<xml::XMLNode> parentNode = xml::XMLNode::TryCast(parent->entityHandle());
  std::optional<xml::XMLNode> retainedNode = xml::XMLNode::TryCast(retained->entityHandle());
  ASSERT_TRUE(parentNode.has_value());
  ASSERT_TRUE(retainedNode.has_value());

  retained->remove();
  ASSERT_FALSE(LifetimeOf(*retained).isAttached());

  parentNode->appendChild(*retainedNode);

  EXPECT_TRUE(LifetimeOf(*retained).isAttached());
  EXPECT_TRUE(document.querySelector("#target").has_value());
}

}  // namespace
}  // namespace donner::svg
