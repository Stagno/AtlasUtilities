//===--------------------------------------------------------------------------------*- C++ -*-===//
//                          _
//                         | |
//                       __| | __ ___      ___ ___
//                      / _` |/ _` \ \ /\ / / '_  |
//                     | (_| | (_| |\ V  V /| | | |
//                      \__,_|\__,_| \_/\_/ |_| |_| - Compiler Toolchain
//
//
//  This file is distributed under the MIT License (MIT).
//  See LICENSE.txt for details.
//
//===------------------------------------------------------------------------------------------===//

#include <assert.h>
#include <iostream>
#include <optional>
#include <string>

#include "atlas/grid/Grid.h"
#include "atlas/grid/StructuredGrid.h"
#include "atlas/grid/detail/grid/Structured.h"
#include "atlas/meshgenerator.h"
#include <atlas/library/Library.h>
#include <atlas/mesh/HybridElements.h>
#include <atlas/mesh/Mesh.h>
#include <atlas/mesh/Nodes.h>
#include <atlas/mesh/actions/BuildEdges.h>
#include <atlas/output/Gmsh.h>
#include <atlas/util/CoordinateEnums.h>

#include "AtlasExtractSubmesh.h"
#include "AtlasToNetcdf.h"

namespace {
bool TriangleInBB(const atlas::Mesh& mesh, int cellIdx, std::tuple<double, double> bblo,
                  std::tuple<double, double> bbhi) {
  const atlas::mesh::HybridElements::Connectivity& cellToNode = mesh.cells().node_connectivity();
  int node0 = cellToNode(cellIdx, 0);
  int node1 = cellToNode(cellIdx, 1);
  int node2 = cellToNode(cellIdx, 2);

  auto xy = atlas::array::make_view<double, 2>(mesh.nodes().xy());
  auto getXY = [xy](int nodeIdx) -> std::tuple<double, double> {
    return {xy(nodeIdx, atlas::LON), xy(nodeIdx, atlas::LAT)};
  };

  auto [x0, y0] = getXY(node0);
  auto [x1, y1] = getXY(node1);
  auto [x2, y2] = getXY(node2);

  auto inBB = [&](double x, double y) {
    return x > std::get<0>(bblo) && y > std::get<1>(bblo) && x < std::get<0>(bbhi) &&
           y < std::get<1>(bbhi);
  };

  return inBB(x0, y0) || inBB(x1, y1) || inBB(x2, y2);
}
} // namespace

atlas::Mesh AtlasMeshRect(int ny) {
  atlas::Grid grid;
  int nx = 3 * ny;

  // Create grid

  // this is adapted from
  // https://github.com/ecmwf/atlas/blob/a0017406f7ae54d306c9585113201af18d86fa40/src/tests/grid/test_grids.cc#L352
  //
  //    here, the grid is simple right triangles with strict up/down orientation. a transform will
  //    be applied later using the AtlasToCartesian wrapper to make the tris equilateral
  {
    using XSpace = atlas::StructuredGrid::XSpace;
    using YSpace = atlas::StructuredGrid::YSpace;

    // grid = atlas::StructuredGrid{XSpace{xspace}, YSpace{yspace}};
    auto x = atlas::grid::LinearSpacing(0, nx, nx, false);
    auto y = atlas::grid::LinearSpacing(0, ny, ny, false);
    grid = atlas::StructuredGrid{x, y};
  }

  auto meshgen = atlas::StructuredMeshGenerator{atlas::util::Config("angle", -1.)};
  auto mesh = meshgen.generate(grid);

  auto xy = atlas::array::make_view<double, 2>(mesh.nodes().xy());
  for(int nodeIdx = 0; nodeIdx < mesh.nodes().size(); nodeIdx++) {
    double x = xy(nodeIdx, atlas::LON);
    double y = xy(nodeIdx, atlas::LAT);
    x = x - 0.5 * y;
    y = y * sqrt(3) / 2.;
    xy(nodeIdx, atlas::LON) = x;
    xy(nodeIdx, atlas::LAT) = y;
  }

  double newHeight = (ny - 1) * sqrt(3) / 2.;
  double length = newHeight * 2;

  std::vector<int> keep;
  std::tuple<double, double> lo{0., -std::numeric_limits<double>::max()};
  std::tuple<double, double> hi{length + length / (nx)*0.1, std::numeric_limits<double>::max()};
  for(int cellIdx = 0; cellIdx < mesh.cells().size(); cellIdx++) {
    if(TriangleInBB(mesh, cellIdx, lo, hi)) {
      keep.push_back(cellIdx);
    }
  }

  auto rectMesh = AtlasExtractSubMeshMinimal(mesh, keep);
  auto xyRect = atlas::array::make_view<double, 2>(rectMesh.nodes().xy());
  double xMin = std::numeric_limits<double>::max();
  double yMin = std::numeric_limits<double>::max();
  double xMax = -std::numeric_limits<double>::max();
  double yMax = -std::numeric_limits<double>::max();
  for(int nodeIdx = 0; nodeIdx < rectMesh.nodes().size(); nodeIdx++) {
    double x = xyRect(nodeIdx, atlas::LON);
    double y = xyRect(nodeIdx, atlas::LAT);
    xMin = fmin(x, xMin);
    yMin = fmin(y, yMin);
    xMax = fmax(x, xMax);
    yMax = fmax(y, yMax);
  }
  double lX = xMax - xMin;
  double lY = yMax - yMin;
  // re-center
  for(int nodeIdx = 0; nodeIdx < rectMesh.nodes().size(); nodeIdx++) {
    double x = xyRect(nodeIdx, atlas::LON);
    double y = xyRect(nodeIdx, atlas::LAT);
    xyRect(nodeIdx, atlas::LON) = x - xMin - lX / 2;
    xyRect(nodeIdx, atlas::LAT) = y - yMin - lY / 2;
  }

  // scale (single scale factor to exactly preserve equilateral edge lengths)
  double scale = 180 / lY;
  for(int nodeIdx = 0; nodeIdx < rectMesh.nodes().size(); nodeIdx++) {
    double x = xyRect(nodeIdx, atlas::LON);
    double y = xyRect(nodeIdx, atlas::LAT);
    xyRect(nodeIdx, atlas::LON) = x * scale;
    xyRect(nodeIdx, atlas::LAT) = y * scale;
  }

  return rectMesh;
}