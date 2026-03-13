#include "donner/svg/components/SpatialGrid.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/EcsRegistry.h"

using testing::IsEmpty;
using testing::SizeIs;
using testing::UnorderedElementsAre;

namespace donner::svg::components {
namespace {

class SpatialGridTest : public ::testing::Test {
protected:
  Registry registry;

  Entity createEntity() { return registry.create(); }
};

TEST_F(SpatialGridTest, EmptyGrid) {
  SpatialGrid grid;

  EXPECT_FALSE(grid.isBuilt());
  EXPECT_EQ(grid.size(), 0u);
  EXPECT_THAT(grid.query(Vector2d(0.0, 0.0)), IsEmpty());
  EXPECT_THAT(grid.queryRect(Boxd(Vector2d(-10.0, -10.0), Vector2d(10.0, 10.0))), IsEmpty());
}

TEST_F(SpatialGridTest, SingleElement) {
  SpatialGrid grid(Boxd(Vector2d(0.0, 0.0), Vector2d(100.0, 100.0)), 10.0);
  Entity e = createEntity();

  grid.insert(e, Boxd(Vector2d(20.0, 20.0), Vector2d(30.0, 30.0)), /*drawOrder=*/0);

  EXPECT_TRUE(grid.isBuilt());
  EXPECT_EQ(grid.size(), 1u);

  // Query inside the entity's bounds.
  auto inside = grid.query(Vector2d(25.0, 25.0));
  ASSERT_THAT(inside, SizeIs(1));
  EXPECT_EQ(inside[0], e);

  // Query outside the entity's bounds but inside the grid.
  EXPECT_THAT(grid.query(Vector2d(50.0, 50.0)), IsEmpty());

  // Query at the entity's exact corner (should be included since bounds are inclusive).
  auto corner = grid.query(Vector2d(20.0, 20.0));
  ASSERT_THAT(corner, SizeIs(1));
  EXPECT_EQ(corner[0], e);
}

TEST_F(SpatialGridTest, MultipleElements) {
  SpatialGrid grid(Boxd(Vector2d(0.0, 0.0), Vector2d(100.0, 100.0)), 10.0);
  Entity e1 = createEntity();
  Entity e2 = createEntity();
  Entity e3 = createEntity();

  // Three non-overlapping entities in different regions.
  grid.insert(e1, Boxd(Vector2d(5.0, 5.0), Vector2d(15.0, 15.0)), /*drawOrder=*/0);
  grid.insert(e2, Boxd(Vector2d(50.0, 50.0), Vector2d(60.0, 60.0)), /*drawOrder=*/1);
  grid.insert(e3, Boxd(Vector2d(80.0, 80.0), Vector2d(90.0, 90.0)), /*drawOrder=*/2);

  EXPECT_EQ(grid.size(), 3u);

  // Each query should return only the entity at that location.
  auto r1 = grid.query(Vector2d(10.0, 10.0));
  ASSERT_THAT(r1, SizeIs(1));
  EXPECT_EQ(r1[0], e1);

  auto r2 = grid.query(Vector2d(55.0, 55.0));
  ASSERT_THAT(r2, SizeIs(1));
  EXPECT_EQ(r2[0], e2);

  auto r3 = grid.query(Vector2d(85.0, 85.0));
  ASSERT_THAT(r3, SizeIs(1));
  EXPECT_EQ(r3[0], e3);

  // Query in empty space returns nothing.
  EXPECT_THAT(grid.query(Vector2d(35.0, 35.0)), IsEmpty());
}

TEST_F(SpatialGridTest, OverlappingElements) {
  SpatialGrid grid(Boxd(Vector2d(0.0, 0.0), Vector2d(100.0, 100.0)), 10.0);
  Entity e1 = createEntity();
  Entity e2 = createEntity();
  Entity e3 = createEntity();

  // All three entities overlap in the region around (25, 25).
  grid.insert(e1, Boxd(Vector2d(10.0, 10.0), Vector2d(30.0, 30.0)), /*drawOrder=*/0);
  grid.insert(e2, Boxd(Vector2d(20.0, 20.0), Vector2d(40.0, 40.0)), /*drawOrder=*/1);
  grid.insert(e3, Boxd(Vector2d(15.0, 15.0), Vector2d(35.0, 35.0)), /*drawOrder=*/2);

  // Query the overlap region: all three should be returned.
  auto results = grid.query(Vector2d(25.0, 25.0));
  EXPECT_THAT(results, SizeIs(3));

  // Verify all entities are present (order is tested in DrawOrderSorting).
  std::vector<Entity> resultVec(results.begin(), results.end());
  EXPECT_THAT(resultVec, UnorderedElementsAre(e1, e2, e3));

  // Query a point where only e2 overlaps. With cellSize=10, cells are [0,10),[10,20),...
  // Point (45,45) is in cell (4,4). e2 [20,40] reaches cell 4 (colForX(40)=4), but
  // e1 [10,30] only reaches cell 3 and e3 [15,35] only reaches cell 3.
  auto partial = grid.query(Vector2d(45.0, 45.0));
  std::vector<Entity> partialVec(partial.begin(), partial.end());
  EXPECT_THAT(partialVec, UnorderedElementsAre(e2));
}

TEST_F(SpatialGridTest, DrawOrderSorting) {
  SpatialGrid grid(Boxd(Vector2d(0.0, 0.0), Vector2d(100.0, 100.0)), 10.0);
  Entity e1 = createEntity();
  Entity e2 = createEntity();
  Entity e3 = createEntity();

  // Insert with varying draw orders (not in sorted order).
  grid.insert(e1, Boxd(Vector2d(10.0, 10.0), Vector2d(30.0, 30.0)), /*drawOrder=*/5);
  grid.insert(e2, Boxd(Vector2d(10.0, 10.0), Vector2d(30.0, 30.0)), /*drawOrder=*/10);
  grid.insert(e3, Boxd(Vector2d(10.0, 10.0), Vector2d(30.0, 30.0)), /*drawOrder=*/1);

  auto results = grid.query(Vector2d(20.0, 20.0));
  ASSERT_THAT(results, SizeIs(3));

  // Expect front-to-back order: highest drawOrder first.
  EXPECT_EQ(results[0], e2);  // drawOrder 10
  EXPECT_EQ(results[1], e1);  // drawOrder 5
  EXPECT_EQ(results[2], e3);  // drawOrder 1
}

TEST_F(SpatialGridTest, Remove) {
  SpatialGrid grid(Boxd(Vector2d(0.0, 0.0), Vector2d(100.0, 100.0)), 10.0);
  Entity e1 = createEntity();
  Entity e2 = createEntity();

  grid.insert(e1, Boxd(Vector2d(10.0, 10.0), Vector2d(30.0, 30.0)), /*drawOrder=*/0);
  grid.insert(e2, Boxd(Vector2d(10.0, 10.0), Vector2d(30.0, 30.0)), /*drawOrder=*/1);

  EXPECT_EQ(grid.size(), 2u);

  // Verify e1 is present.
  auto before = grid.query(Vector2d(20.0, 20.0));
  std::vector<Entity> beforeVec(before.begin(), before.end());
  EXPECT_THAT(beforeVec, UnorderedElementsAre(e1, e2));

  // Remove e1.
  grid.remove(e1);
  EXPECT_EQ(grid.size(), 1u);

  // Verify e1 is gone but e2 remains.
  auto after = grid.query(Vector2d(20.0, 20.0));
  ASSERT_THAT(after, SizeIs(1));
  EXPECT_EQ(after[0], e2);
}

TEST_F(SpatialGridTest, QueryOutOfBounds) {
  SpatialGrid grid(Boxd(Vector2d(0.0, 0.0), Vector2d(100.0, 100.0)), 10.0);
  Entity e = createEntity();

  grid.insert(e, Boxd(Vector2d(40.0, 40.0), Vector2d(60.0, 60.0)), /*drawOrder=*/0);

  // Query points outside the grid world bounds.
  EXPECT_THAT(grid.query(Vector2d(-10.0, -10.0)), IsEmpty());
  EXPECT_THAT(grid.query(Vector2d(200.0, 200.0)), IsEmpty());
  EXPECT_THAT(grid.query(Vector2d(50.0, -5.0)), IsEmpty());
  EXPECT_THAT(grid.query(Vector2d(-5.0, 50.0)), IsEmpty());
}

TEST_F(SpatialGridTest, QueryRect) {
  SpatialGrid grid(Boxd(Vector2d(0.0, 0.0), Vector2d(100.0, 100.0)), 10.0);
  Entity e1 = createEntity();
  Entity e2 = createEntity();
  Entity e3 = createEntity();

  grid.insert(e1, Boxd(Vector2d(5.0, 5.0), Vector2d(15.0, 15.0)), /*drawOrder=*/0);
  grid.insert(e2, Boxd(Vector2d(50.0, 50.0), Vector2d(60.0, 60.0)), /*drawOrder=*/1);
  grid.insert(e3, Boxd(Vector2d(80.0, 80.0), Vector2d(90.0, 90.0)), /*drawOrder=*/2);

  // Query rect that encompasses only e1.
  auto r1 = grid.queryRect(Boxd(Vector2d(0.0, 0.0), Vector2d(20.0, 20.0)));
  std::vector<Entity> r1Vec(r1.begin(), r1.end());
  EXPECT_THAT(r1Vec, UnorderedElementsAre(e1));

  // Query rect that encompasses e2 and e3.
  auto r23 = grid.queryRect(Boxd(Vector2d(45.0, 45.0), Vector2d(95.0, 95.0)));
  std::vector<Entity> r23Vec(r23.begin(), r23.end());
  EXPECT_THAT(r23Vec, UnorderedElementsAre(e2, e3));

  // Query rect that encompasses all three.
  auto all = grid.queryRect(Boxd(Vector2d(0.0, 0.0), Vector2d(100.0, 100.0)));
  std::vector<Entity> allVec(all.begin(), all.end());
  EXPECT_THAT(allVec, UnorderedElementsAre(e1, e2, e3));

  // Query rect that hits nothing.
  auto none = grid.queryRect(Boxd(Vector2d(30.0, 30.0), Vector2d(40.0, 40.0)));
  EXPECT_THAT(none, IsEmpty());
}

TEST_F(SpatialGridTest, LargeElementSpanningManyCells) {
  SpatialGrid grid(Boxd(Vector2d(0.0, 0.0), Vector2d(100.0, 100.0)), 10.0);
  Entity e = createEntity();

  // Entity covers the entire grid.
  grid.insert(e, Boxd(Vector2d(0.0, 0.0), Vector2d(100.0, 100.0)), /*drawOrder=*/0);

  EXPECT_EQ(grid.size(), 1u);

  // Should be found at any point within the grid.
  auto center = grid.query(Vector2d(50.0, 50.0));
  ASSERT_THAT(center, SizeIs(1));
  EXPECT_EQ(center[0], e);

  auto topLeft = grid.query(Vector2d(1.0, 1.0));
  ASSERT_THAT(topLeft, SizeIs(1));
  EXPECT_EQ(topLeft[0], e);

  auto bottomRight = grid.query(Vector2d(99.0, 99.0));
  ASSERT_THAT(bottomRight, SizeIs(1));
  EXPECT_EQ(bottomRight[0], e);
}

TEST_F(SpatialGridTest, PointOnCellBoundary) {
  // With cellSize=10, the grid has cells [0,10), [10,20), [20,30), etc.
  SpatialGrid grid(Boxd(Vector2d(0.0, 0.0), Vector2d(100.0, 100.0)), 10.0);
  Entity e = createEntity();

  // Entity spans [15, 15] to [25, 25], crossing the cell boundary at x=20, y=20.
  // This entity occupies cells (1,1), (1,2), (2,1), (2,2).
  grid.insert(e, Boxd(Vector2d(15.0, 15.0), Vector2d(25.0, 25.0)), /*drawOrder=*/0);

  // Query exactly on the cell boundary (x=20.0, y=20.0) -> cell (2,2), entity is present.
  auto onBoundary = grid.query(Vector2d(20.0, 20.0));
  ASSERT_THAT(onBoundary, SizeIs(1));
  EXPECT_EQ(onBoundary[0], e);

  // Query on the cell boundary at the entity edge -> cell (1,1), entity is present.
  auto atEdge = grid.query(Vector2d(15.0, 15.0));
  ASSERT_THAT(atEdge, SizeIs(1));
  EXPECT_EQ(atEdge[0], e);

  // Query in a cell that the entity does NOT occupy.
  // Point (5,5) is in cell (0,0), entity starts at col 1.
  EXPECT_THAT(grid.query(Vector2d(5.0, 5.0)), IsEmpty());
  // Point (35,35) is in cell (3,3), entity ends at col 2.
  EXPECT_THAT(grid.query(Vector2d(35.0, 35.0)), IsEmpty());
}

TEST_F(SpatialGridTest, UpdateDirtyEntitiesMovesEntity) {
  SpatialGrid grid(Boxd(Vector2d(0.0, 0.0), Vector2d(100.0, 100.0)), 10.0);
  Entity e1 = createEntity();
  Entity e2 = createEntity();

  // Insert two entities.
  grid.insert(e1, Boxd(Vector2d(10.0, 10.0), Vector2d(20.0, 20.0)), /*drawOrder=*/0);
  grid.insert(e2, Boxd(Vector2d(50.0, 50.0), Vector2d(60.0, 60.0)), /*drawOrder=*/1);

  // e1 is at (15,15), e2 is at (55,55).
  EXPECT_THAT(grid.query(Vector2d(15.0, 15.0)), SizeIs(1));
  EXPECT_THAT(grid.query(Vector2d(55.0, 55.0)), SizeIs(1));
  EXPECT_THAT(grid.query(Vector2d(85.0, 85.0)), IsEmpty());

  // Simulate moving e1 to (80,80)-(90,90): remove old, insert new.
  grid.remove(e1);
  grid.insert(e1, Boxd(Vector2d(80.0, 80.0), Vector2d(90.0, 90.0)), /*drawOrder=*/0);

  // e1 should now be at (85,85), not at (15,15).
  EXPECT_THAT(grid.query(Vector2d(15.0, 15.0)), IsEmpty());
  EXPECT_THAT(grid.query(Vector2d(85.0, 85.0)), SizeIs(1));

  // e2 should remain unchanged.
  auto r = grid.query(Vector2d(55.0, 55.0));
  ASSERT_THAT(r, SizeIs(1));
  EXPECT_EQ(r[0], e2);
}

TEST_F(SpatialGridTest, UpdateDirtyEntitiesRemovesMissingEntity) {
  SpatialGrid grid(Boxd(Vector2d(0.0, 0.0), Vector2d(100.0, 100.0)), 10.0);
  Entity e1 = createEntity();

  grid.insert(e1, Boxd(Vector2d(10.0, 10.0), Vector2d(20.0, 20.0)), /*drawOrder=*/0);
  EXPECT_EQ(grid.size(), 1u);

  // Removing an entity that doesn't exist in the grid is a no-op.
  Entity e2 = createEntity();
  grid.remove(e2);
  EXPECT_EQ(grid.size(), 1u);

  // Removing e1 works.
  grid.remove(e1);
  EXPECT_EQ(grid.size(), 0u);
  EXPECT_THAT(grid.query(Vector2d(15.0, 15.0)), IsEmpty());
}

}  // namespace
}  // namespace donner::svg::components
