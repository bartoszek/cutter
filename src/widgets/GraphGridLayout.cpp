#include "GraphGridLayout.h"

#include <unordered_set>
#include <unordered_map>
#include <queue>
#include <stack>
#include <cassert>

#include "common/BinaryTrees.h"


/** @class GraphGridLayout

Basic familiarity with graph algorithms is recommended.

# Terms used:
- **Vertex**, **node**, **block** - read description of graph for definition. Within this text vertex and node are
used interchangeably with block due to code being written for visualizing basic block control flow graph.
- **edge** - read description of graph for definition for precise definition.
- **DAG** - directed acyclic graph, graph using directed edges which doesn't have cycles. DAG may contain loops if
following them would require going in both directions of edges. Example 1->2 1->3 3->2 is a DAG, 2->1 1->3 3->2
isn't a DAG.
- **DFS** - depth first search, a graph traversal algorithm
- **toposort** - topological sorting, process of ordering a DAG vertices that all edges go from vertices earlier in the
toposort order to vertices later in toposort order. There are multiple algorithms for implementing toposort operation.
Single DAG can have multiple valid topological orderings, a toposort algorithm can be designed to prioritize a specific
one from all valid toposort orders. Example: for graph 1->4, 2->1, 2->3, 3->4 valid topological orders are [2,1,3,4] and
[2,3,1,4].

# High level structure of the algorithm
1. select subset of edges that form a DAG (remove cycles)
2. toposort the DAG
3. choose a subset of edges that form a tree and assign layers
4. assign node positions within grid using tree structure, child subtrees are placed side by side with parent on top
5. perform edge routing
6. calculate column and row pixel positions based on node sizes and amount edges between the rows


Contrary to many other layered graph drawing algorithm this implementation doesn't perform node reordering to minimize
edge crossing. This simplifies implementation, and preserves original control flow structure for conditional jumps (
true jump on one side, false jump on other). Due to most of control flow being result of structured programming
constructs like if/then/else and loops, resulting layout is usually readable without node reordering within layers.


# Description of grid.
To simplify the layout algorithm initial steps assume that all nodes have the same size and edges are zero width.
After placing the nodes and routing the edges it is known which nodes are in in which row and column, how
many edges are between each pair of rows. Using this information positions are converted from the grid cells
to pixel coordinates. Routing 0 width edges between rows can also be interpreted as every second row and column being
reserved for edges. The row numbers in code are using first interpretation. To allow better centering of nodes one
above other each node is 2 columns wide and 1 row high.

\image html graph_grid.svg

# 1-2 Cycle removal and toposort

Cycle removal and toposort are done at the same time during single DFS traversal. In case entrypoint is part of a loop
DFS started from entrypoint. This ensures that entrypoint is at the top of resulting layout if possible. Resulting
toposort order is used in many of the following layout steps that require calculating some property of a vertex based
on child property or the other way around. Using toposort order such operations can be implemented iteration through
array in either forward or reverse direction. To prevent running out of stack memory when processing large graphs
DFS is implemented non-recursively.

# Row assignment

Rows are assigned in toposort order from top to bottom, with nodes row being max(predecessor.row)+1. This ensures
that loop edges are only ones going from deeper levels to previous layers.

To further simply node placement a subset of edges is selected which forms a tree. This turns DAG drawing problem
into a tree drawing problem. For each node in level n following nodes which have level exactly n+1 are greedily
assigned as child nodes in tree. If a node already has parent assigned then corresponding edge is not part of tree.

# Node position assignment

Since the graph has been reduced to a tree, node placement is more or less putting subtrees side by side with
parent on top. There is some room for interpretation what exactly side by side means and where exactly on top is.
Drawing the graph either too dense or too big may make it less readable so there are configuration options which allow
choosing these things resulting in more or less dense layout.

Once the subtrees are placed side by side. Parent node can be placed either in the middle of horizontal bounds or
in the middle of direct children. First option results in narrower layout and more vertical columns. Second option
results in nodes being more spread out which may help seeing where each edge goes.

In more compact mode two subtrees are placed side by side taking into account their shape. In wider mode
bounding box of shorter subtree is used instead of exact shape. This gives slightly sparse layout without it being too
wide.

\image html graph_parent_placement.svg

# Edge routing
Edge routing can be split into: main column selection, rough routing, segment offset calculation.

Transition from source to target row is done using single vertical segment. This is called main column.

Rough routing creates the path of edge using up to 5 segments using grid coordinates.
Due to nodes being placed in a grid. Horizontal segments of edges can't intersect with any nodes.
The path for edges is chosen so that it consists of at most 5 segments, typically resulting in sideways U shape or
square Z shape.
- short vertical segment from node to horizontal line
- move to empty column
- vertical segment between starting row and end row, an empty column can always be found, in the worst case there are empty columns at the sides of drawing
- horizontal segment to target node column
- short vertical segment connecting to target node

There are 3 special cases:
- source and target nodes are in the same column with no nodes between - single vertical segment
- column bellow stating node is empty - segments 1-3 are merged
- column above target node is empty - segments 3-5 are merged
Vertical segment intersection with nodes is prevented using a 2d array marking which vertical segments are blocked and
naively iterating through all rows between start and end at the desired column.

After rough routing segment offsets are calculated relative to their corresponding edge column. This ensures that
two segments don't overlap. Segment offsets within each column are assigned greedily with some heuristics for
assignment order to reduce amount of edge crossings and result in more visually pleasing output for a typical CFG
graph.
Each segment gets assigned an offset that is maximum of previously assigned offsets overlapping with current
segment + segment spacing.
Assignment order is chosen based on:
* direction of previous and last segment - helps reducing crossings and place the segments between nodes
* segment length - reduces crossing when segment endpoints have the same structure as valid parentheses expression
* edge length - establishes some kind of order when single node is connected to many edges, typically a block
  with switch statement or block after switch statement.

*/


GraphGridLayout::GraphGridLayout(GraphGridLayout::LayoutType layoutType)
    : GraphLayout({})
    , layoutType(layoutType)
{
    switch (layoutType) {
    case LayoutType::Narrow:
        tightSubtreePlacement = true;
        parentBetweenDirectChild = false;
        break;
    case LayoutType::Medium:
        tightSubtreePlacement = false;
        parentBetweenDirectChild = false;
        break;
    case LayoutType::Wide:
        tightSubtreePlacement = false;
        parentBetweenDirectChild = true;
        break;
    }
}

std::vector<ut64> GraphGridLayout::topoSort(LayoutState &state, ut64 entry)
{
    auto &blocks = *state.blocks;

    // Run DFS to:
    // * select backwards/loop edges
    // * perform toposort
    std::vector<ut64> blockOrder;
    enum class State : uint8_t {
        NotVisited = 0,
        InStack,
        Visited
    };
    std::unordered_map<ut64, State> visited;
    visited.reserve(state.blocks->size());
    std::stack<std::pair<ut64, size_t>> stack;
    auto dfsFragment = [&visited, &blocks, &state, &stack, &blockOrder](ut64 first) {
        visited[first] = State::InStack;
        stack.push({first, 0});
        while (!stack.empty()) {
            auto v = stack.top().first;
            auto edge_index = stack.top().second;
            const auto &block = blocks[v];
            if (edge_index < block.edges.size()) {
                ++stack.top().second;
                auto target = block.edges[edge_index].target;
                auto &targetState = visited[target];
                if (targetState == State::NotVisited) {
                    targetState = State::InStack;
                    stack.push({target, 0});
                    state.grid_blocks[v].dag_edge.push_back(target);
                } else if (targetState == State::Visited) {
                    state.grid_blocks[v].dag_edge.push_back(target);
                } // else {  targetState == 1 in stack, loop edge }
            } else {
                stack.pop();
                visited[v] = State::Visited;
                blockOrder.push_back(v);
            }
        }
    };

    // Start with entry so that if start of function block is part of loop it
    // is still kept at top unless it's impossible to do while maintaining
    // topological order.
    dfsFragment(entry);
    for (auto &blockIt : blocks) {
        if (visited[blockIt.first] == State::NotVisited) {
            dfsFragment(blockIt.first);
        }
    }

    return blockOrder;
}

void GraphGridLayout::assignRows(GraphGridLayout::LayoutState &state, const std::vector<unsigned long long> &blockOrder)
{
    for (auto it = blockOrder.rbegin(), end = blockOrder.rend(); it != end; it++) {
        auto &block = state.grid_blocks[*it];
        int nextLevel = block.row + 1;
        for (auto target : block.dag_edge) {
            auto &targetBlock = state.grid_blocks[target];
            targetBlock.row = std::max(targetBlock.row, nextLevel);
        }
    }
}

void GraphGridLayout::selectTree(GraphGridLayout::LayoutState &state)
{
    for (auto &blockIt : state.grid_blocks) {
        auto &block = blockIt.second;
        for (auto targetId : block.dag_edge) {
            auto &targetBlock = state.grid_blocks[targetId];
            if (!targetBlock.has_parent && targetBlock.row == block.row + 1) {
                block.tree_edge.push_back(targetId);
                targetBlock.has_parent = true;
            }
        }
    }
}

void GraphGridLayout::CalculateLayout(std::unordered_map<ut64, GraphBlock> &blocks, ut64 entry,
                                      int &width, int &height) const
{
    LayoutState layoutState;
    layoutState.blocks = &blocks;

    for (auto &it : blocks) {
        GridBlock block;
        block.id = it.first;
        layoutState.grid_blocks[it.first] = block;
    }

    auto blockOrder = topoSort(layoutState, entry);
    computeAllBlockPlacement(blockOrder, layoutState);

    for (auto &blockIt : blocks) {
        layoutState.edge[blockIt.first].resize(blockIt.second.edges.size());
        for (size_t i = 0; i < blockIt.second.edges.size(); i++) {
            layoutState.edge[blockIt.first][i].dest = blockIt.second.edges[i].target;
        }
    }
    for (const auto &edgeList : layoutState.edge) {
        auto &startBlock = layoutState.grid_blocks[edgeList.first];
        startBlock.outputCount++;
        for (auto &edge : edgeList.second) {
            auto &targetBlock = layoutState.grid_blocks[edge.dest];
            targetBlock.inputCount++;
        }
    }

    layoutState.columns = 1;
    layoutState.rows = 1;
    for (auto &node : layoutState.grid_blocks) {
        // count is at least index + 1
        layoutState.rows = std::max(layoutState.rows, size_t(node.second.row) + 1);
        // block is 2 column wide
        layoutState.columns = std::max(layoutState.columns, size_t(node.second.col) + 2);
    }

    layoutState.rowHeight.assign(layoutState.rows, 0);
    layoutState.columnWidth.assign(layoutState.columns, 0);
    for (auto &node : layoutState.grid_blocks) {
        const auto &inputBlock = blocks[node.first];
        layoutState.rowHeight[node.second.row] = std::max(inputBlock.height,
                                                          layoutState.rowHeight[node.second.row]);
        layoutState.columnWidth[node.second.col] = std::max(inputBlock.width / 2,
                                                            layoutState.columnWidth[node.second.col]);
        layoutState.columnWidth[node.second.col + 1] = std::max(inputBlock.width / 2,
                                                                layoutState.columnWidth[node.second.col + 1]);
    }

    routeEdges(layoutState);

    convertToPixelCoordinates(layoutState, width, height);
}

void GraphGridLayout::findMergePoints(GraphGridLayout::LayoutState &state) const
{
    for (auto &blockIt : state.grid_blocks) {
        auto &block = blockIt.second;
        GridBlock *mergeBlock = nullptr;
        int grandChildCount = 0;
        for (auto edge : block.tree_edge) {
            auto &targetBlock = state.grid_blocks[edge];
            if (targetBlock.tree_edge.size()) {
                mergeBlock = &state.grid_blocks[targetBlock.tree_edge[0]];
            }
            grandChildCount += targetBlock.tree_edge.size();
        }
        if (!mergeBlock || grandChildCount != 1) {
            continue;
        }
        int blocksGoingToMerge = 0;
        int blockWithTreeEdge = 0;
        for (auto edge : block.tree_edge) {
            auto &targetBlock = state.grid_blocks[edge];
            bool goesToMerge = false;
            for (auto secondEdgeTarget : targetBlock.dag_edge) {
                if (secondEdgeTarget == mergeBlock->id) {
                    goesToMerge = true;
                    break;
                }
            }
            if (goesToMerge) {
                if (targetBlock.tree_edge.size() == 1) {
                    blockWithTreeEdge = blocksGoingToMerge;
                }
                blocksGoingToMerge++;
            } else {
                break;
            }
        }
        if (blocksGoingToMerge) {
            state.grid_blocks[block.tree_edge[blockWithTreeEdge]].col = blockWithTreeEdge * 2 -
                                                                        (blocksGoingToMerge - 1);
        }
    }
}

void GraphGridLayout::computeAllBlockPlacement(const std::vector<ut64> &blockOrder,
                                               LayoutState &layoutState) const
{
    assignRows(layoutState, blockOrder);
    selectTree(layoutState);
    findMergePoints(layoutState);


    // Shapes of subtrees are maintained using linked lists. Each value within list is column relative to previous row.
    // This allows moving things around by changing only first value in list.
    LinkedListPool<int> sides(blockOrder.size() * 2); // *2 = two sides for each node

    // Process nodes in the order from bottom to top. Ensures that all subtrees are processed before parent node.
    for (auto blockId : blockOrder) {
        auto &block = layoutState.grid_blocks[blockId];
        if (block.tree_edge.size() == 0) {
            block.row_count = 1;
            block.col = 0;
            block.lastRowRight = 2;
            block.lastRowLeft = 0;
            block.leftPosition = 0;
            block.rightPosition = 2;

            block.leftSideShape = sides.makeList(0);
            block.rightSideShape = sides.makeList(2);
        } else {
            auto &firstChild = layoutState.grid_blocks[block.tree_edge[0]];
            auto leftSide = firstChild.leftSideShape; // left side of block children subtrees processed so far
            auto rightSide = firstChild.rightSideShape;
            block.row_count = firstChild.row_count;
            block.lastRowRight = firstChild.lastRowRight;
            block.lastRowLeft = firstChild.lastRowLeft;
            block.leftPosition = firstChild.leftPosition;
            block.rightPosition = firstChild.rightPosition;
            // Place children subtrees side by side
            for (size_t i = 1; i < block.tree_edge.size(); i++) {
                auto &child = layoutState.grid_blocks[block.tree_edge[i]];
                int minPos = INT_MIN;
                int leftPos = 0;
                int rightPos = 0;
                auto leftIt = sides.head(rightSide);
                auto rightIt = sides.head(child.leftSideShape);
                int maxLeftWidth = 0;
                int minRightPos = child.col;

                while (leftIt && rightIt) { // process part of subtrees that touch when put side by side
                    leftPos += *leftIt;
                    rightPos += *rightIt;
                    minPos = std::max(minPos, leftPos - rightPos);
                    maxLeftWidth = std::max(maxLeftWidth, leftPos);
                    minRightPos = std::min(minRightPos, rightPos);
                    ++leftIt;
                    ++rightIt;
                }
                int rightTreeOffset = 0;
                if (tightSubtreePlacement) {
                    rightTreeOffset = minPos; // mode a) place subtrees as close as possible
                } else {
                    // mode b) use bounding box for shortest subtree and full shape of other side
                    if (leftIt) {
                        rightTreeOffset = maxLeftWidth - child.leftPosition;
                    } else {
                        rightTreeOffset = block.rightPosition - minRightPos;
                    }

                }
                // Calculate the new shape after putting the two subtrees side by side
                child.col += rightTreeOffset;
                if (leftIt) {
                    *leftIt -= (rightTreeOffset + child.lastRowRight - leftPos);
                    rightSide = sides.append(child.rightSideShape, sides.splitTail(rightSide, leftIt));
                } else if (rightIt) {
                    *rightIt += (rightPos + rightTreeOffset - block.lastRowLeft);
                    leftSide = sides.append(leftSide, sides.splitTail(child.leftSideShape, rightIt));

                    rightSide = child.rightSideShape;
                    block.lastRowRight = child.lastRowRight + rightTreeOffset;
                    block.lastRowLeft = child.lastRowLeft + rightTreeOffset;
                } else {
                    rightSide = child.rightSideShape;
                }
                *sides.head(rightSide) += rightTreeOffset;
                block.row_count = std::max(block.row_count, child.row_count);
                block.leftPosition = std::min(block.leftPosition, child.leftPosition + rightTreeOffset);
                block.rightPosition = std::max(block.rightPosition, rightTreeOffset + child.rightPosition);
            }

            int col = 0;
            // Calculate parent position
            if (parentBetweenDirectChild) {
                // mode a) keep one child to the left, other to the right
                for (auto target : block.tree_edge) {
                    col += layoutState.grid_blocks[target].col;
                }
                col /= block.tree_edge.size();
            } else {
                // mode b) somewhere between left most direct child and right most, preferably in the middle of
                // horizontal dimensions. Results layout looks more like single vertical line.
                col = (block.rightPosition + block.leftPosition) / 2 - 1;
                col = std::max(col, layoutState.grid_blocks[block.tree_edge.front()].col - 1);
                col = std::min(col, layoutState.grid_blocks[block.tree_edge.back()].col + 1);
            }
            block.col += col; // += instead of = to keep offset calculated in previous steps
            block.row_count += 1;
            block.leftPosition = std::min(block.leftPosition, block.col);
            block.rightPosition = std::max(block.rightPosition, block.col + 2);

            *sides.head(leftSide) -= block.col;
            block.leftSideShape = sides.append(sides.makeList(block.col), leftSide);

            *sides.head(rightSide) -= block.col + 2;
            block.rightSideShape = sides.append(sides.makeList(block.col + 2), rightSide);

            // Keep children positions relative to parent so that moving parent moves whole subtree
            for (auto target : block.tree_edge) {
                auto &targetBlock = layoutState.grid_blocks[target];
                targetBlock.col -= block.col;
            }
        }
    }

    // Calculate root positions. Typical function should have one root node that matches with entrypoint.
    // There can be more of them in case of switch statement analysis failure, unreahable basic blocks or
    // using the algorithm for non control flow graphs.
    int nextEmptyColumn = 0;
    for (auto &blockIt : layoutState.grid_blocks) {
        auto &block = blockIt.second;
        if (block.row == 0) { // place all the roots first
            auto offset = -block.leftPosition;
            block.col += nextEmptyColumn + offset;
            nextEmptyColumn = block.rightPosition + offset;
        }
    }
    // Visit all nodes top to bottom, converting relative positions to absolute.
    for (auto it = blockOrder.rbegin(), end = blockOrder.rend(); it != end; it++) {
        auto &block = layoutState.grid_blocks[*it];
        assert(block.col >= 0);
        for (auto childId : block.tree_edge) {
            auto &childBlock = layoutState.grid_blocks[childId];
            childBlock.col += block.col;
        }
    }
}

void GraphGridLayout::routeEdges(GraphGridLayout::LayoutState &state) const
{
    calculateEdgeMainColumn(state);
    roughRouting(state);
    elaborateEdgePlacement(state);
}

void GraphGridLayout::calculateEdgeMainColumn(GraphGridLayout::LayoutState &state) const
{
    // Find an empty column as close as possible to start or end block's column.
    // Use sweep line approach processing events sorted by row top to bottom. Use an appropriate tree structure
    // to contain blocks above sweep line and query for nearest column which isn't blocked by a block.

    struct Event {
        size_t blockId;
        size_t edgeId;
        int row;
        enum Type {
            Edge = 0,
            Block = 1
        } type;
    };
    // create events
    std::vector<Event> events;
    events.reserve(state.grid_blocks.size() * 2);
    for (const auto &it : state.grid_blocks) {
        events.push_back({it.first, 0, it.second.row, Event::Block});
        const auto &inputBlock = (*state.blocks)[it.first];
        int startRow = it.second.row + 1;

        auto gridEdges = state.edge[it.first];
        gridEdges.resize(inputBlock.edges.size());
        for (size_t i = 0; i < inputBlock.edges.size(); i++) {
            auto targetId = inputBlock.edges[i].target;
            gridEdges[i].dest = targetId;
            const auto &targetGridBlock = state.grid_blocks[targetId];
            int endRow = targetGridBlock.row;
            events.push_back({it.first, i, std::max(startRow, endRow), Event::Edge});
        }
    }
    std::sort(events.begin(), events.end(), [](const Event &a, const Event &b) {
        if (a.row != b.row) {
            return a.row < b.row;
        }
        return static_cast<int>(a.type) < static_cast<int>(b.type);
    });

    // process events and choose main column for each edge
    PointSetMinTree blockedColumns(state.columns + 1, -1);
    for (const auto &event : events) {
        if (event.type == Event::Block) {
            auto block = state.grid_blocks[event.blockId];
            blockedColumns.set(block.col + 1, event.row);
        } else {
            auto block = state.grid_blocks[event.blockId];
            int column = block.col + 1;
            auto &edge = state.edge[event.blockId][event.edgeId];
            const auto &targetBlock = state.grid_blocks[edge.dest];
            auto topRow = std::min(block.row + 1, targetBlock.row);
            auto targetColumn = targetBlock.col + 1;

            // Prefer using the same column as starting node, it allows reducing amount of segments.
            if (blockedColumns.valueAtPoint(column) < topRow) {
                edge.mainColumn = column;
            } else if (blockedColumns.valueAtPoint(targetColumn) < topRow) { // next try target block column
                edge.mainColumn = targetColumn;
            } else {
                auto nearestLeft = blockedColumns.rightMostLessThan(column, topRow);
                auto nearestRight = blockedColumns.leftMostLessThan(column, topRow);
                // There should always be empty column at the sides of drawing
                assert(nearestLeft != -1 && nearestRight != -1);

                // Choose closest column. Take into account distance to source and target block columns.
                auto distanceLeft = column - nearestLeft + abs(targetColumn - nearestLeft);
                auto distanceRight = nearestRight - column + abs(targetColumn - nearestRight);

                // For upward edges try to make a loop instead of 8 shape,
                // it is slightly longer but produces less crossing.
                if (targetBlock.row < block.row) {
                    if (targetColumn < column && blockedColumns.valueAtPoint(column + 1) < topRow &&
                            column - targetColumn <= distanceLeft + 2) {
                        edge.mainColumn = column + 1;
                        continue;
                    } else if (targetColumn > column && blockedColumns.valueAtPoint(column - 1) < topRow &&
                               targetColumn - column <= distanceRight + 2) {
                        edge.mainColumn = column - 1;
                        continue;
                    }
                }

                if (distanceLeft != distanceRight) {
                    edge.mainColumn = distanceLeft < distanceRight ? nearestLeft : nearestRight;
                } else {
                    // In case of tie choose based on edge index. Should result in true branches being mostly on one
                    // side, false branches on other side.
                    edge.mainColumn = event.edgeId < state.edge[event.blockId].size() / 2 ? nearestLeft : nearestRight;
                }
            }
        }
    }
}

void GraphGridLayout::roughRouting(GraphGridLayout::LayoutState &state) const
{
    auto getSpacingOverride = [this](int blockWidth, int edgeCount) {
        int maxSpacing = blockWidth / edgeCount;
        if (maxSpacing < layoutConfig.edgeHorizontalSpacing) {
            return std::max(maxSpacing, 1);
        }
        return 0;
    };

    for (auto &blockIt : state.grid_blocks) {
        auto &blockEdges = state.edge[blockIt.first];
        for (size_t i = 0; i < blockEdges.size(); i++) {
            auto &edge = blockEdges[i];
            const auto &start = blockIt.second;
            const auto &target = state.grid_blocks[edge.dest];

            edge.addPoint(start.row + 1, start.col + 1);
            if (edge.mainColumn != start.col + 1) {
                edge.addPoint(start.row + 1, start.col + 1, edge.mainColumn < start.col + 1 ? -1 : 1);
                edge.addPoint(start.row + 1, edge.mainColumn, target.row <= start.row ? -2 : 0);
            }
            int mainColumnKind = 0;
            if (edge.mainColumn < start.col + 1 && edge.mainColumn < target.col + 1) {
                mainColumnKind = +2;
            } else if (edge.mainColumn > start.col + 1 && edge.mainColumn > target.col + 1) {
                mainColumnKind = -2;
            } else if (edge.mainColumn == start.col + 1 && edge.mainColumn != target.col + 1) {
                mainColumnKind = edge.mainColumn < target.col + 1 ? 1 : -1;
            } else if (edge.mainColumn == target.col + 1 && edge.mainColumn != start.col + 1) {
                mainColumnKind = edge.mainColumn < start.col + 1 ? 1 : -1;
            }
            edge.addPoint(target.row, edge.mainColumn, mainColumnKind);
            if (target.col + 1 != edge.mainColumn) {
                edge.addPoint(target.row, target.col + 1, target.row <= start.row ? 2 : 0);
                edge.addPoint(target.row, target.col + 1, target.col + 1 < edge.mainColumn ? 1 : -1);
            }

            // reduce edge spacing when there is large amount of edges connected to single block
            auto startSpacingOverride = getSpacingOverride((*state.blocks)[start.id].width, start.outputCount);
            auto targetSpacingOverride = getSpacingOverride((*state.blocks)[target.id].width,
                                                            target.inputCount);
            edge.points.front().spacingOverride = startSpacingOverride;
            edge.points.back().spacingOverride = targetSpacingOverride;


            int length = 0;
            for (size_t i = 1; i < edge.points.size(); i++) {
                length += abs(edge.points[i].row - edge.points[i - 1].row) +
                          abs(edge.points[i].col - edge.points[i - 1].col);
            }
            edge.secondaryPriority = 2 * length + (target.row >= start.row ? 1 : 0);
        }
    }
}

namespace {
/**
 * @brief Single segment of an edge. An edge can be drawn using multiple horizontal and vertical segments.
 * x y meaning matches vertical segments. For horizontal segments axis are swapped.
 */
struct EdgeSegment {
    int y0;
    int y1;
    int x;
    int edgeIndex;
    int secondaryPriority;
    int16_t kind;
    int16_t spacingOverride; //< segment spacing override, 0 if default spacing should be used
};
struct NodeSide {
    int x;
    int y0;
    int y1;
    int size; //< block size in the x axis direction
};
}

/**
 * @brief Calculate segment offsets relative to their column
 *
 * Argument naming uses terms for vertical segments, but the function can be used for horizontal segments as well.
 *
 * @param segments Segments that need to be processed.
 * @param edgeOffsets Output argument for returning segment offsets relative to their columns.
 * @param edgeColumnWidth InOut argument describing how much column with edges take. Initial value used as minimal
 * value. May be increased to depending on amount of segments in each column and how tightly they are packed.
 * @param nodeRightSide Right side of nodes. Used to reduce space reserved for edges by placing them between nodes.
 * @param nodeLeftSide Same as right side.
 * @param columnWidth
 * @param H All the segmement and node coordinates y0 and y1 are expected to be in range [0;H)
 * @param segmentSpacing The expected spacing between two segments in the same column. Actual spacing may be smaller
 * for nodes with many edges.
 */
void calculateSegmentOffsets(
    std::vector<EdgeSegment> &segments,
    std::vector<int> &edgeOffsets,
    std::vector<int> &edgeColumnWidth,
    std::vector<NodeSide> &nodeRightSide,
    std::vector<NodeSide> &nodeLeftSide,
    const std::vector<int> &columnWidth,
    size_t H,
    int segmentSpacing)
{
    for (auto &segment : segments) {
        if (segment.y0 > segment.y1) {
            std::swap(segment.y0, segment.y1);
        }
    }

    std::sort(segments.begin(), segments.end(), [](const EdgeSegment & a, const EdgeSegment & b) {
        if (a.x != b.x) return a.x < b.x;
        if (a.kind != b.kind) return a.kind < b.kind;
        auto aSize = a.y1 - a.y0;
        auto bSize = b.y1 - b.y0;
        if (aSize != bSize) {
            if (a.kind != 1) {
                return aSize < bSize;
            } else {
                return aSize > bSize;
            }
        }
        if (a.kind != 1) {
            return a.secondaryPriority < b.secondaryPriority;
        } else {
            return a.secondaryPriority > b.secondaryPriority;
        }
        return false;
    });

    auto compareNode = [](const NodeSide & a, const NodeSide & b) {
        return a.x < b.x;
    };
    sort(nodeRightSide.begin(), nodeRightSide.end(), compareNode);
    sort(nodeLeftSide.begin(), nodeLeftSide.end(), compareNode);

    RangeAssignMaxTree maxSegment(H, INT_MIN);
    auto nextSegmentIt = segments.begin();
    auto rightSideIt = nodeRightSide.begin();
    auto leftSideIt = nodeLeftSide.begin();
    while (nextSegmentIt != segments.end()) {
        int x = nextSegmentIt->x;

        int leftColumWidth = 0;
        if (x > 0) {
            leftColumWidth = columnWidth[x - 1];
        }
        maxSegment.setRange(0, H, -leftColumWidth);
        while (rightSideIt != nodeRightSide.end() && rightSideIt->x + 1 < x) {
            rightSideIt++;
        }
        while (rightSideIt != nodeRightSide.end() && rightSideIt->x + 1 == x) {
            maxSegment.setRange(rightSideIt->y0, rightSideIt->y1 + 1, rightSideIt->size - leftColumWidth);
            rightSideIt++;
        }

        while (nextSegmentIt != segments.end() && nextSegmentIt->x == x && nextSegmentIt->kind <= 1) {
            int y = maxSegment.rangeMaximum(nextSegmentIt->y0, nextSegmentIt->y1 + 1);
            if (nextSegmentIt->kind != -2) {
                y = std::max(y, 0);
            }
            y += nextSegmentIt->spacingOverride ? nextSegmentIt->spacingOverride : segmentSpacing;
            maxSegment.setRange(nextSegmentIt->y0, nextSegmentIt->y1 + 1, y);
            edgeOffsets[nextSegmentIt->edgeIndex] = y;
            nextSegmentIt++;
        }

        auto firstRightSideSegment = nextSegmentIt;
        auto middleWidth = std::max(maxSegment.rangeMaximum(0, H), 0);

        int rightColumnWidth = 0;
        if (x < static_cast<int>(columnWidth.size())) {
            rightColumnWidth = columnWidth[x];
        }

        maxSegment.setRange(0, H, -rightColumnWidth);
        while (leftSideIt != nodeLeftSide.end() && leftSideIt->x < x) {
            leftSideIt++;
        }
        while (leftSideIt != nodeLeftSide.end() && leftSideIt->x == x) {
            maxSegment.setRange(leftSideIt->y0, leftSideIt->y1 + 1, leftSideIt->size - rightColumnWidth);
            leftSideIt++;
        }
        while (nextSegmentIt != segments.end() && nextSegmentIt->x == x) {
            int y = maxSegment.rangeMaximum(nextSegmentIt->y0, nextSegmentIt->y1 + 1);
            y += nextSegmentIt->spacingOverride ? nextSegmentIt->spacingOverride : segmentSpacing;
            maxSegment.setRange(nextSegmentIt->y0, nextSegmentIt->y1 + 1, y);
            edgeOffsets[nextSegmentIt->edgeIndex] = y;
            nextSegmentIt++;
        }
        auto rightSideMiddle = std::max(maxSegment.rangeMaximum(0, H), 0);
        rightSideMiddle = std::max(rightSideMiddle, edgeColumnWidth[x] - middleWidth - segmentSpacing);
        for (auto it = firstRightSideSegment; it != nextSegmentIt; ++it) {
            edgeOffsets[it->edgeIndex] = middleWidth + (rightSideMiddle - edgeOffsets[it->edgeIndex]) +
                                         segmentSpacing;
        }
        edgeColumnWidth[x] = middleWidth + segmentSpacing + rightSideMiddle;
    }
}


/**
 * @brief Center the segments to the middle of edge columns when possible.
 * @param segmentOffsets offsets relative to the left side edge column.
 * @param edgeColumnWidth widths of edge columns
 * @param segments either all horizontal or all vertical edge segments
 * @param minSpacing spacing between segments
 */
static void centerEdges(
    std::vector<int> &segmentOffsets,
    const std::vector<int> &edgeColumnWidth,
    const std::vector<EdgeSegment> &segments,
    int minSpacing)
{
    /* Split segments in each edge column into non intersecting chunks. Center each chunk separately.
     *
     * Process segment endpoints sorted by x and y. Maintain count of currently started segments. When number of
     * active segments reaches 0 there is empty space between chunks.
     */
    struct Event {
        int x;
        int y;
        int index;
        bool start;
    };
    std::vector<Event> events;
    events.reserve(segments.size() * 2);
    for (const auto &segment : segments) {
        auto offset = segmentOffsets[segment.edgeIndex];
        // Exclude segments which are outside edge column and between the blocks. It's hard to ensure that moving
        // them doesn't cause overlap with blocks.
        if (offset >= 0 && offset <= edgeColumnWidth[segment.x]) {
            events.push_back({segment.x, segment.y0, segment.edgeIndex, true});
            events.push_back({segment.x, segment.y1, segment.edgeIndex, false});
        }
    }
    std::sort(events.begin(), events.end(), [](const Event & a, const Event & b) {
        if (a.x != b.x) return a.x < b.x;
        if (a.y != b.y) return a.y < b.y;
        // Process segment start events before end to ensure that activeSegmentCount doesn't go negative and only
        // reaches 0 at the end of chunk.
        return int(a.start) > int(b.start);
    });

    auto it = events.begin();
    while (it != events.end()) {
        auto chunkStart = it++;
        int activeSegmentCount = 1;
        int chunkWidth = 0;
        while (activeSegmentCount > 0) {
            activeSegmentCount += it->start ? 1 : -1;
            chunkWidth = std::max(chunkWidth, segmentOffsets[it->index]);
            it++;
        }
        // leftMost segment position includes padding on the left side so add it on the right side as well
        chunkWidth += minSpacing;

        int spacing = (std::max(edgeColumnWidth[chunkStart->x], minSpacing) - chunkWidth) / 2;
        for (auto segment = chunkStart; segment != it; segment++) {
            if (segment->start) {
                segmentOffsets[segment->index] += spacing;
            }
        }
    }
}

/**
 * @brief Convert segment coordinates from arbitary range to continuous range starting at 0.
 * @param segments
 * @param leftSides
 * @param rightSides
 * @return Size of compressed coordinate range.
 */
static int compressCoordinates(std::vector<EdgeSegment> &segments,
                               std::vector<NodeSide> &leftSides,
                               std::vector<NodeSide> &rightSides)
{
    std::vector<int> positions;
    positions.reserve((segments.size() + leftSides.size()) * 2);
    for (const auto &segment : segments) {
        positions.push_back(segment.y0);
        positions.push_back(segment.y1);
    }
    for (const auto &segment : leftSides) {
        positions.push_back(segment.y0);
        positions.push_back(segment.y1);
    }
    // y0 and y1 in rightSides should match leftSides

    std::sort(positions.begin(), positions.end());
    auto lastUnique = std::unique(positions.begin(), positions.end());
    positions.erase(lastUnique, positions.end());

    auto positionToIndex = [&] (int position) {
        size_t index = std::lower_bound(positions.begin(), positions.end(), position) - positions.begin();
        assert(index < positions.size());
        return index;
    };
    for (auto &segment : segments) {
        segment.y0 = positionToIndex(segment.y0);
        segment.y1 = positionToIndex(segment.y1);
    }
    assert(leftSides.size() == rightSides.size());
    for (size_t i = 0; i < leftSides.size(); i++) {
        leftSides[i].y0 = rightSides[i].y0 = positionToIndex(leftSides[i].y0);
        leftSides[i].y1 = rightSides[i].y1 = positionToIndex(leftSides[i].y1);
    }
    return positions.size();
}

void GraphGridLayout::elaborateEdgePlacement(GraphGridLayout::LayoutState &state) const
{
    int edgeIndex = 0;

    auto segmentFromPoint =
    [&edgeIndex](const Point & point, const GridEdge & edge, int y0, int y1, int x) {
        EdgeSegment segment;
        segment.y0 = y0;
        segment.y1 = y1;
        segment.x = x;
        segment.edgeIndex = edgeIndex++;
        segment.kind = point.kind;
        segment.spacingOverride = point.spacingOverride;
        segment.secondaryPriority = edge.secondaryPriority;
        return segment;
    };


    std::vector<EdgeSegment> segments;
    std::vector<NodeSide> rightSides;
    std::vector<NodeSide> leftSides;
    std::vector<int> edgeOffsets;

    // Vertical segments
    for (auto &edgeListIt : state.edge) {
        for (const auto &edge : edgeListIt.second) {
            for (size_t j = 1; j < edge.points.size(); j += 2) {
                segments.push_back(segmentFromPoint(edge.points[j], edge,
                    edge.points[j-1].row * 2, // edges in even rows
                    edge.points[j].row * 2,
                    edge.points[j].col));
            }
        }
    }
    for (auto &blockIt : state.grid_blocks) {
        auto &node = blockIt.second;
        auto width = (*state.blocks)[blockIt.first].width;
        auto leftWidth = width / 2;
        // not the same as leftWidth, you would think that one pixel offset isn't visible, but it is
        auto rightWidth = width - leftWidth;
        int row = node.row * 2 + 1; // blocks in odd rows
        leftSides.push_back({node.col, row, row, leftWidth});
        rightSides.push_back({node.col + 1, row, row, rightWidth});
    }
    state.edgeColumnWidth.assign(state.columns + 1, layoutConfig.blockHorizontalSpacing);
    state.edgeColumnWidth[0] = state.edgeColumnWidth.back() = layoutConfig.edgeHorizontalSpacing;
    edgeOffsets.resize(edgeIndex);
    calculateSegmentOffsets(segments, edgeOffsets, state.edgeColumnWidth, rightSides, leftSides,
                            state.columnWidth, 2 * state.rows + 1, layoutConfig.edgeHorizontalSpacing);
    centerEdges(edgeOffsets, state.edgeColumnWidth, segments, layoutConfig.blockHorizontalSpacing);
    edgeIndex = 0;

    auto copySegmentsToEdges = [&](bool col) {
        int edgeIndex = 0;
        for (auto &edgeListIt : state.edge) {
            for (auto &edge : edgeListIt.second) {
                for (size_t j = col ? 1 : 2; j < edge.points.size(); j += 2) {
                    edge.points[j].offset = edgeOffsets[edgeIndex++];
                }
            }
        }
    };
    auto oldColumnWidths = state.columnWidth;
    adjustColumnWidths(state);
    for (auto &segment : segments) {
        auto &offset = edgeOffsets[segment.edgeIndex];
        if (segment.kind == -2) {
            offset -= (state.edgeColumnWidth[segment.x - 1] / 2 +  state.columnWidth[segment.x - 1]) -
                      oldColumnWidths[segment.x - 1];
        } else if (segment.kind == 2) {
            offset += (state.edgeColumnWidth[segment.x + 1] / 2 +  state.columnWidth[segment.x]) -
                      oldColumnWidths[segment.x];
        }
    }
    calculateColumnOffsets(state.columnWidth, state.edgeColumnWidth,
                           state.columnOffset, state.edgeColumnOffset);
    copySegmentsToEdges(true);


    // Horizontal segments
    // Use exact x coordinates obtained from vertical segment placment.
    segments.clear();
    leftSides.clear();
    rightSides.clear();

    edgeIndex = 0;
    for (auto &edgeListIt : state.edge) {
        for (const auto &edge : edgeListIt.second) {
            for (size_t j = 2; j < edge.points.size(); j += 2) {
                int y0 = state.edgeColumnOffset[edge.points[j - 1].col] + edge.points[j - 1].offset;
                int y1 = state.edgeColumnOffset[edge.points[j + 1].col] + edge.points[j + 1].offset;
                segments.push_back(segmentFromPoint(edge.points[j], edge, y0, y1, edge.points[j].row));
            }
        }
    }
    edgeOffsets.resize(edgeIndex);
    for (auto &blockIt : state.grid_blocks) {
        auto &node = blockIt.second;
        auto blockWidth = (*state.blocks)[node.id].width;
        int leftSide = state.edgeColumnOffset[node.col + 1] + state.edgeColumnWidth[node.col + 1] / 2 -
                       blockWidth / 2;
        int rightSide = leftSide + blockWidth;

        int h = (*state.blocks)[blockIt.first].height;
        int freeSpace = state.rowHeight[node.row] - h;
        int topProfile = state.rowHeight[node.row];
        int bottomProfile = h;
        if (verticalBlockAlignmentMiddle) {
            topProfile -= freeSpace / 2;
            bottomProfile += freeSpace / 2;
        }
        leftSides.push_back({node.row, leftSide, rightSide, topProfile});
        rightSides.push_back({node.row, leftSide, rightSide, bottomProfile});
    }
    state.edgeRowHeight.assign(state.rows + 1, layoutConfig.blockVerticalSpacing);
    state.edgeRowHeight[0] = state.edgeRowHeight.back() = layoutConfig.edgeVerticalSpacing;
    edgeOffsets.resize(edgeIndex);
    auto compressedCoordinates = compressCoordinates(segments, leftSides, rightSides);
    calculateSegmentOffsets(segments, edgeOffsets, state.edgeRowHeight, rightSides, leftSides,
                            state.rowHeight, compressedCoordinates, layoutConfig.edgeVerticalSpacing);
    copySegmentsToEdges(false);
}

void GraphGridLayout::adjustColumnWidths(GraphGridLayout::LayoutState &state) const
{
    state.rowHeight.assign(state.rows, 0);
    state.columnWidth.assign(state.columns, 0);
    for (auto &node : state.grid_blocks) {
        const auto &inputBlock = (*state.blocks)[node.first];
        state.rowHeight[node.second.row] = std::max(inputBlock.height, state.rowHeight[node.second.row]);
        int edgeWidth = state.edgeColumnWidth[node.second.col + 1];
        int columnWidth = (inputBlock.width - edgeWidth) / 2;
        state.columnWidth[node.second.col] = std::max(columnWidth, state.columnWidth[node.second.col]);
        state.columnWidth[node.second.col + 1] = std::max(columnWidth,
                                                          state.columnWidth[node.second.col + 1]);
    }
}

int GraphGridLayout::calculateColumnOffsets(const std::vector<int> &columnWidth,
                                            std::vector<int> &edgeColumnWidth, std::vector<int> &columnOffset,
                                            std::vector<int> &edgeColumnOffset)
{
    assert(edgeColumnWidth.size() == columnWidth.size() + 1);
    int position = 0;
    edgeColumnOffset.resize(edgeColumnWidth.size());
    columnOffset.resize(columnWidth.size());
    for (size_t i = 0; i < columnWidth.size(); i++) {
        edgeColumnOffset[i] = position;
        position += edgeColumnWidth[i];
        columnOffset[i] = position;
        position += columnWidth[i];
    }
    edgeColumnOffset.back() = position;
    position += edgeColumnWidth.back();
    return position;
}

void GraphGridLayout::convertToPixelCoordinates(
    GraphGridLayout::LayoutState &state,
    int &width,
    int &height) const
{
    // calculate row and column offsets
    width = calculateColumnOffsets(state.columnWidth, state.edgeColumnWidth,
                                   state.columnOffset, state.edgeColumnOffset);
    height = calculateColumnOffsets(state.rowHeight, state.edgeRowHeight,
                                    state.rowOffset, state.edgeRowOffset);

    // block pixel positions
    for (auto &block : (*state.blocks)) {
        const auto &gridBlock = state.grid_blocks[block.first];

        block.second.x = state.edgeColumnOffset[gridBlock.col + 1] +
                         state.edgeColumnWidth[gridBlock.col + 1] / 2 - block.second.width / 2;
        block.second.y = state.rowOffset[gridBlock.row];
        if (verticalBlockAlignmentMiddle) {
            block.second.y += (state.rowHeight[gridBlock.row] - block.second.height) / 2;
        }
    }
    // edge pixel positions
    for (auto &it : (*state.blocks)) {
        auto &block = it.second;
        for (size_t i = 0; i < block.edges.size(); i++) {
            auto &resultEdge = block.edges[i];
            const auto &target = (*state.blocks)[resultEdge.target];
            resultEdge.polyline.clear();
            resultEdge.polyline.push_back(QPointF(0, block.y + block.height));

            const auto &edge = state.edge[it.first][i];
            for (size_t j = 1; j < edge.points.size(); j++) {
                if (j & 1) { // vertical segment
                    int column = edge.points[j].col;
                    int x = state.edgeColumnOffset[column] + edge.points[j].offset;
                    resultEdge.polyline.back().setX(x);
                    resultEdge.polyline.push_back(QPointF(x, 0));
                } else { // horizontal segment
                    int row = edge.points[j].row;
                    int y = state.edgeRowOffset[row] + edge.points[j].offset;
                    resultEdge.polyline.back().setY(y);
                    resultEdge.polyline.push_back(QPointF(0, y));
                }
            }
            resultEdge.polyline.back().setY(target.y);
        }
    }
}

