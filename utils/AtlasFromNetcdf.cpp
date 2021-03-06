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

#include "AtlasFromNetcdf.h"

#include <assert.h>
#include <iostream>
#include <math.h>

#include <netcdf>

#include <atlas/array/Array.h>
#include <atlas/array/ArrayView.h>
#include <atlas/array/IndexView.h>
#include <atlas/library/Library.h>
#include <atlas/library/config.h>
#include <atlas/mesh/ElementType.h>
#include <atlas/mesh/Elements.h>
#include <atlas/mesh/HybridElements.h>
#include <atlas/mesh/Nodes.h>
#include <atlas/mesh/actions/BuildCellCentres.h>
#include <atlas/mesh/actions/BuildEdges.h>
#include <atlas/mesh/actions/BuildPeriodicBoundaries.h>
#include <atlas/meshgenerator/detail/MeshGeneratorImpl.h>
#include <atlas/output/Gmsh.h>
#include <atlas/util/CoordinateEnums.h>

namespace {
// dummy partition identifier. always zero throughout since this reader does not support
// partitioning (yet)
const int defaultPartition = 0;

template <typename loadT>
std::vector<loadT> LoadField(const netCDF::NcFile& dataFile, const std::string& name) {
  netCDF::NcVar data = dataFile.getVar(name.c_str());
  if(data.isNull()) {
    return {};
  }
  assert(data.getDimCount() == 1);
  size_t size = data.getDim(0).getSize();
  std::vector<loadT> ret(size);
  data.getVar(ret.data());
  return ret;
}

template <typename loadT>
std::tuple<std::vector<loadT>, size_t, size_t> Load2DField(const netCDF::NcFile& dataFile,
                                                           const std::string& name) {
  netCDF::NcVar data = dataFile.getVar(name.c_str());
  if(data.isNull()) {
    return {};
  }
  assert(data.getDimCount() == 2);
  size_t stride = data.getDim(0).getSize();
  size_t elPerStride = data.getDim(1).getSize();
  size_t size = stride * elPerStride;
  std::vector<loadT> ret(size);
  data.getVar(ret.data());
  return {ret, stride, elPerStride};
}

bool NodesFromNetCDF(const netCDF::NcFile& dataFile, atlas::Mesh& mesh) {
  auto lon = LoadField<double>(dataFile, "vlon");
  auto lat = LoadField<double>(dataFile, "vlat");
  if(lon.size() == 0 || lat.size() == 0) {
    std::cout << "lat / long variable not found\n";
    return false;
  }
  if(lon.size() != lat.size()) {
    std::cout << "lat / long not of consistent sizes!\n";
    return false;
  }

  int numNodes = lat.size();

  // define nodes and associated properties for Atlas meshs
  mesh.nodes().resize(numNodes);
  atlas::mesh::Nodes& nodes = mesh.nodes();
  auto lonlat = atlas::array::make_view<double, 2>(nodes.lonlat());

  // we currently don't care about parts, so myPart is always 0 and remotde_idx == glb_idx
  auto glb_idx_node = atlas::array::make_view<atlas::gidx_t, 1>(nodes.global_index());
  auto remote_idx = atlas::array::make_indexview<atlas::idx_t, 1>(nodes.remote_index());
  auto part = atlas::array::make_view<int, 1>(nodes.partition());

  // no ghosts currently (ghost = false always) and no flags are set
  auto ghost = atlas::array::make_view<int, 1>(nodes.ghost());
  auto flags = atlas::array::make_view<int, 1>(nodes.flags());

  auto radToLat = [](double rad) { return rad / (0.5 * M_PI) * 90; };
  auto radToLon = [](double rad) { return rad / (M_PI)*180; };

  for(int nodeIdx = 0; nodeIdx < numNodes; nodeIdx++) {
    // following the same pattern here as in
    lonlat(nodeIdx, atlas::LON) = radToLon(lon[nodeIdx]);
    lonlat(nodeIdx, atlas::LAT) = radToLat(lat[nodeIdx]);

    glb_idx_node(nodeIdx) = nodeIdx;
    remote_idx(nodeIdx) = nodeIdx;

    part(nodeIdx) = defaultPartition;
    ghost(nodeIdx) = false;
    atlas::mesh::Nodes::Topology::reset(flags(nodeIdx));
  }

  return true;
}

bool CellsFromNetCDF(const netCDF::NcFile& dataFile, atlas::Mesh& mesh) {
  auto [cellToVertex, vertexPerCell, ncells] = Load2DField<int>(dataFile, "vertex_of_cell");
  if(vertexPerCell != 3) {
    std::cout << "not a triangle mesh\n";
    return false;
  }

  // define cells and associated properties
  mesh.cells().add(new atlas::mesh::temporary::Triangle(), ncells);
  auto cells_part = atlas::array::make_view<int, 1>(mesh.cells().partition());
  atlas::mesh::HybridElements::Connectivity& node_connectivity = mesh.cells().node_connectivity();
  atlas::array::ArrayView<atlas::gidx_t, 1> glb_idx_cell =
      atlas::array::make_view<atlas::gidx_t, 1>(mesh.cells().global_index());

  for(size_t cellIdx = 0; cellIdx < ncells; cellIdx++) {
    // indices in netcdf are 1 based, data is column major
    atlas::idx_t tri_nodes[3] = {cellToVertex[0 * ncells + cellIdx] - 1,
                                 cellToVertex[1 * ncells + cellIdx] - 1,
                                 cellToVertex[2 * ncells + cellIdx] - 1};
    node_connectivity.set(cellIdx, tri_nodes);
    glb_idx_cell[cellIdx] = cellIdx;
    cells_part(cellIdx) = defaultPartition;
  }

  return true;
}

template <typename ConnectivityT>
bool AddNeighborList(const netCDF::NcFile& dataFile, atlas::Mesh& mesh,
                     const std::string nbhListName, size_t yPerXExpected,
                     ConnectivityT& connectivity) {
  auto [xToY, yPerX, numY] = Load2DField<int>(dataFile, nbhListName);
  if(yPerX != yPerXExpected) {
    std::cout << "number of neighbors per element not as expected!\n";
    return false;
  }
  for(size_t elemIdx = 0; elemIdx < numY; elemIdx++) {
    atlas::idx_t yOfX[yPerX];
    for(size_t innerIdx = 0; innerIdx < yPerX; innerIdx++) {
      // indices in netcdf are 1 based, data is column major
      yOfX[innerIdx] = xToY[innerIdx * numY + elemIdx] - 1;
    }
    connectivity.set(elemIdx, yOfX);
  }
  return true;
}

// Pre-allocate neighborhood table, snippet taken from
// https://github.com/ecmwf/atlas/blob/98ff1f20dc883ba2dfd7658196622b9bc6d6fceb/src/atlas/mesh/actions/BuildEdges.cc#L73
template <typename ConnectivityT>
void AllocNbhTable(ConnectivityT& connectivity, int numElements, int nbhPerElem) {
  std::vector<int> init(numElements * nbhPerElem, connectivity.missing_value());
  connectivity.add(numElements, nbhPerElem, init.data());
}
} // namespace

std::optional<atlas::Mesh> AtlasMeshFromNetCDFMinimal(const std::string& filename) {
  try {
    atlas::Mesh mesh;
    netCDF::NcFile dataFile(filename.c_str(), netCDF::NcFile::read);

    if(!NodesFromNetCDF(dataFile, mesh)) {
      return {};
    }

    if(!CellsFromNetCDF(dataFile, mesh)) {
      return {};
    }

    return mesh;
  } catch(netCDF::exceptions::NcException& e) {
    std::cout << e.what() << "\n";
    return std::nullopt;
  }
}

std::optional<atlas::Mesh> AtlasMeshFromNetCDFComplete(const std::string& filename) {
  auto maybeMesh = AtlasMeshFromNetCDFMinimal(filename);
  if(!maybeMesh.has_value()) {
    return {};
  }

  try {
    atlas::Mesh mesh = maybeMesh.value();
    netCDF::NcFile dataFile(filename.c_str(), netCDF::NcFile::read);

    int numEdgesA = LoadField<int>(dataFile, "edge_index").size();
    int numEdgesB = LoadField<int>(dataFile, "elat").size();
    // Explanation: base grids obtained from DWD feature only the edge_index field, while the files
    // generated by using the web interface feature only the elat value.

    if(numEdgesA == 0 && numEdgesB == 0) {
      std::cout << "no edges found in netcdf file!\n";
      return {};
    }

    int numEdges = std::max(numEdgesA, numEdgesB);
    assert(numEdges > 0);

    // had no edges so far, add them
    mesh.edges().add(new atlas::mesh::temporary::Line(), numEdges);

    const int verticesPerEdge = 2;
    const int cellsPerEdge = 2;
    const int cellsPerNode = 6; // maximum is 6, some with 5 exist
    const int edgesPerNode = 6; // maximum is 6, some with 5 exist
    const int edgesPerCell = 3;

    // Allocate & fill neighbor tables from file
    // ------------------------------

    // Edges
    AllocNbhTable(mesh.edges().cell_connectivity(), mesh.edges().size(), cellsPerEdge);
    if(!AddNeighborList(dataFile, mesh, "adjacent_cell_of_edge", cellsPerEdge,
                        mesh.edges().cell_connectivity())) {
      return {};
    }
    AllocNbhTable(mesh.edges().node_connectivity(), mesh.edges().size(), verticesPerEdge);
    if(!AddNeighborList(dataFile, mesh, "edge_vertices", verticesPerEdge,
                        mesh.edges().node_connectivity())) {
      return {};
    }
    // edge to edge connectivity not supported so far
    // AllocNbhTable(mesh.edges().edge_connectivity(), ??, ??);

    // Nodes
    AllocNbhTable(mesh.nodes().cell_connectivity(), mesh.nodes().size(), cellsPerNode);
    if(!AddNeighborList(dataFile, mesh, "cells_of_vertex", cellsPerNode,
                        mesh.nodes().cell_connectivity())) {
      return {};
    }
    AllocNbhTable(mesh.nodes().edge_connectivity(), mesh.nodes().size(), edgesPerNode);
    if(!AddNeighborList(dataFile, mesh, "edges_of_vertex", edgesPerNode,
                        mesh.nodes().edge_connectivity())) {
      return {};
    }
    // ATLAS has no conn. tables for node to node
    // AllocNbhTable(mesh.nodes().node_connectivity(), mesh.nodes().size(), nodesPerNode);

    // Cells
    // cell to node was already computed in AtlasMeshFromNetCDFMinimal
    AllocNbhTable(mesh.cells().edge_connectivity(), mesh.cells().size(), edgesPerCell);
    if(!AddNeighborList(dataFile, mesh, "edge_of_cell", edgesPerCell,
                        mesh.cells().edge_connectivity())) {
      return {};
    }
    // cell to cell supported by atlas but not present in ICON netcdf
    // AllocNbhTable(mesh.cells().cell_connectivity(), mesh.cells().size(), cellsPerEdge);

    return mesh;
  } catch(netCDF::exceptions::NcException& e) {
    std::cout << e.what() << "\n";
    return std::nullopt;
  }
}