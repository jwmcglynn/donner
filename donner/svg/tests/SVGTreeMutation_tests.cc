#include <gtest/gtest.h>

#include <cstdint>

#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGGElement.h"
#include "donner/svg/SVGRectElement.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/components/NodeLifetimeComponent.h"

namespace donner::svg {
namespace {

using components::DirtyFlagsComponent;
using components::NodeLifetimeComponent;

const NodeLifetimeComponent& LifetimeOf(const SVGElement& element) {
  return element.entityHandle().get<NodeLifetimeComponent>();
}

bool HasDirtyFlags(const SVGElement& element, uint16_t flags) {
  const auto* dirty = element.entityHandle().try_get<DirtyFlagsComponent>();
  return dirty != nullptr && dirty->test(flags);
}

TEST(SVGTreeMutationTests, RootStartsAttachedAndNewElementsStartDetached) {
  SVGDocument document;

  const SVGSVGElement root = document.svgElement();
  EXPECT_TRUE(LifetimeOf(root).isAttached());
  EXPECT_TRUE(LifetimeOf(root).detachedRoot == entt::null);

  const SVGRectElement rect = SVGRectElement::Create(document);
  EXPECT_FALSE(LifetimeOf(rect).isAttached());
  EXPECT_EQ(LifetimeOf(rect).detachedRoot, rect.unsafeEntityHandle().entity());
}

TEST(SVGTreeMutationTests, AppendToDetachedParentKeepsInsertedSubtreeDetached) {
  SVGDocument document;
  SVGGElement group = SVGGElement::Create(document);
  SVGRectElement rect = SVGRectElement::Create(document);

  group.appendChild(rect);

  EXPECT_FALSE(LifetimeOf(group).isAttached());
  EXPECT_EQ(LifetimeOf(group).detachedRoot, group.unsafeEntityHandle().entity());
  EXPECT_FALSE(LifetimeOf(rect).isAttached());
  EXPECT_EQ(LifetimeOf(rect).detachedRoot, group.unsafeEntityHandle().entity());
}

TEST(SVGTreeMutationTests, AppendDetachedSubtreeToRootMarksWholeSubtreeAttached) {
  SVGDocument document;
  SVGSVGElement root = document.svgElement();
  SVGGElement group = SVGGElement::Create(document);
  SVGRectElement rect = SVGRectElement::Create(document);
  group.appendChild(rect);

  root.appendChild(group);

  EXPECT_TRUE(LifetimeOf(group).isAttached());
  EXPECT_TRUE(LifetimeOf(group).detachedRoot == entt::null);
  EXPECT_TRUE(LifetimeOf(rect).isAttached());
  EXPECT_TRUE(LifetimeOf(rect).detachedRoot == entt::null);
}

TEST(SVGTreeMutationTests, StructuralMutationsBumpDocumentRevision) {
  SVGDocument document;
  SVGSVGElement root = document.svgElement();
  SVGRectElement rect = SVGRectElement::Create(document);
  const std::uint64_t afterCreateRevision = document.handle()->revision();

  root.appendChild(rect);

  const std::uint64_t afterAppendRevision = document.handle()->revision();
  EXPECT_GT(afterAppendRevision, afterCreateRevision);

  root.removeChild(rect);

  EXPECT_GT(document.handle()->revision(), afterAppendRevision);
}

TEST(SVGTreeMutationTests, AttributeMutationsBumpDocumentRevision) {
  SVGDocument document;
  SVGRectElement rect = SVGRectElement::Create(document);
  const std::uint64_t beforeAttributeRevision = document.handle()->revision();

  rect.setAttribute("data-state", "dirty");

  EXPECT_GT(document.handle()->revision(), beforeAttributeRevision);
}

TEST(SVGTreeMutationTests, RemoveChildMarksWholeSubtreeDetached) {
  SVGDocument document;
  SVGSVGElement root = document.svgElement();
  SVGGElement group = SVGGElement::Create(document);
  SVGRectElement rect = SVGRectElement::Create(document);
  root.appendChild(group);
  group.appendChild(rect);

  root.removeChild(group);

  EXPECT_FALSE(group.parentElement().has_value());
  EXPECT_EQ(rect.parentElement(), group);
  EXPECT_FALSE(LifetimeOf(group).isAttached());
  EXPECT_EQ(LifetimeOf(group).detachedRoot, group.unsafeEntityHandle().entity());
  EXPECT_FALSE(LifetimeOf(rect).isAttached());
  EXPECT_EQ(LifetimeOf(rect).detachedRoot, group.unsafeEntityHandle().entity());
}

TEST(SVGTreeMutationTests, ReappendDetachedSubtreeMarksWholeSubtreeAttachedAgain) {
  SVGDocument document;
  SVGSVGElement root = document.svgElement();
  SVGGElement group = SVGGElement::Create(document);
  SVGRectElement rect = SVGRectElement::Create(document);
  root.appendChild(group);
  group.appendChild(rect);
  root.removeChild(group);

  root.appendChild(group);

  EXPECT_EQ(group.parentElement(), root);
  EXPECT_TRUE(LifetimeOf(group).isAttached());
  EXPECT_TRUE(LifetimeOf(rect).isAttached());
}

TEST(SVGTreeMutationTests, QuerySelectorStopsSeeingDetachedSubtreeAndSeesReattach) {
  SVGDocument document;
  SVGSVGElement root = document.svgElement();
  SVGRectElement rect = SVGRectElement::Create(document);
  rect.setId("target");
  root.appendChild(rect);
  ASSERT_TRUE(document.querySelector("#target").has_value());

  root.removeChild(rect);

  EXPECT_FALSE(document.querySelector("#target").has_value());

  root.appendChild(rect);

  EXPECT_TRUE(document.querySelector("#target").has_value());
}

TEST(SVGTreeMutationTests, MovingChildMarksOldAndNewParentsDirty) {
  SVGDocument document;
  SVGSVGElement root = document.svgElement();
  SVGGElement oldParent = SVGGElement::Create(document);
  SVGGElement newParent = SVGGElement::Create(document);
  SVGRectElement child = SVGRectElement::Create(document);
  root.appendChild(oldParent);
  root.appendChild(newParent);
  oldParent.appendChild(child);

  document.registry().clear<DirtyFlagsComponent>();

  newParent.appendChild(child);

  EXPECT_TRUE(HasDirtyFlags(oldParent, DirtyFlagsComponent::All));
  EXPECT_TRUE(HasDirtyFlags(newParent, DirtyFlagsComponent::All));
  EXPECT_TRUE(HasDirtyFlags(child, DirtyFlagsComponent::All));
  EXPECT_TRUE(LifetimeOf(child).isAttached());
}

TEST(SVGTreeMutationTests, ReplacingChildWithItselfKeepsNodeAttached) {
  SVGDocument document;
  SVGSVGElement root = document.svgElement();
  SVGRectElement rect = SVGRectElement::Create(document);
  root.appendChild(rect);

  root.replaceChild(rect, rect);

  EXPECT_EQ(rect.parentElement(), root);
  EXPECT_TRUE(LifetimeOf(rect).isAttached());
}

TEST(SVGTreeMutationTests, RejectsCycleBeforeMutatingTree) {
  SVGDocument document;
  SVGSVGElement root = document.svgElement();
  SVGGElement group = SVGGElement::Create(document);
  SVGRectElement rect = SVGRectElement::Create(document);
  root.appendChild(group);
  group.appendChild(rect);

  EXPECT_DEATH(
      { rect.appendChild(group); }, "Cannot insert an ancestor as a child of its descendant");
  EXPECT_EQ(group.parentElement(), root);
  EXPECT_EQ(rect.parentElement(), group);
}

TEST(SVGTreeMutationTests, RejectsReferenceNodeFromAnotherParentBeforeMutatingTree) {
  SVGDocument document;
  SVGSVGElement root = document.svgElement();
  SVGGElement parent = SVGGElement::Create(document);
  SVGGElement otherParent = SVGGElement::Create(document);
  SVGRectElement reference = SVGRectElement::Create(document);
  SVGRectElement inserted = SVGRectElement::Create(document);
  root.appendChild(parent);
  root.appendChild(otherParent);
  otherParent.appendChild(reference);

  EXPECT_DEATH(
      { parent.insertBefore(inserted, reference); }, "referenceNode must be a child of parent");
  EXPECT_FALSE(inserted.parentElement().has_value());
  EXPECT_EQ(reference.parentElement(), otherParent);
}

}  // namespace
}  // namespace donner::svg
