#include "kimera_dsg/dynamic_scene_graph.h"
#include "kimera_dsg/edge_attributes.h"

#include <glog/logging.h>
#include <pcl/conversions.h>

#include <list>

namespace kimera {

using Node = SceneGraphNode;
using Edge = SceneGraphEdge;
using NodeRef = DynamicSceneGraph::NodeRef;
using DynamicNodeRef = DynamicSceneGraph::DynamicNodeRef;
using EdgeRef = DynamicSceneGraph::EdgeRef;

DynamicSceneGraph::DynamicSceneGraph(LayerId mesh_layer_id)
    : DynamicSceneGraph(getDefaultLayerIds(), mesh_layer_id) {}

DynamicSceneGraph::DynamicSceneGraph(const LayerIds& layer_ids, LayerId mesh_layer_id)
    : mesh_layer_id(mesh_layer_id), layer_ids(layer_ids), next_mesh_edge_idx_(0) {
  if (layer_ids.empty()) {
    throw std::domain_error("scene graph cannot be initialized without layers");
  }

  if (std::find(layer_ids.begin(), layer_ids.end(), mesh_layer_id) != layer_ids.end()) {
    throw std::domain_error("mesh layer id must be unique");
  }

  clear();
}

void DynamicSceneGraph::clear() {
  layers_.clear();
  dynamic_layers_.clear();

  node_lookup_.clear();

  interlayer_edges_.reset();
  dynamic_interlayer_edges_.reset();

  mesh_vertices_.reset();
  mesh_faces_.reset();

  clearMeshEdges();

  for (const auto& id : layer_ids) {
    layers_[id] = std::make_unique<SceneGraphLayer>(id);
  }
}

// TODO(nathan) consider refactoring to use operator[]
bool DynamicSceneGraph::createDynamicLayer(LayerId layer, LayerPrefix layer_prefix) {
  if (hasLayer(layer, layer_prefix)) {
    return false;
  }

  if (!dynamic_layers_.count(layer)) {
    dynamic_layers_[layer] = DynamicLayers();
  }

  dynamic_layers_[layer].emplace(
      layer_prefix, std::make_unique<DynamicSceneGraphLayer>(layer, layer_prefix));
  return true;
}

bool DynamicSceneGraph::emplaceNode(LayerId layer_id,
                                    NodeId node_id,
                                    NodeAttributes::Ptr&& attrs) {
  if (!layers_.count(layer_id)) {
    LOG(WARNING) << "Invalid layer: " << layer_id;
    return false;
  }

  if (node_lookup_.count(node_id)) {
    return false;
  }

  const bool successful = layers_[layer_id]->emplaceNode(node_id, std::move(attrs));
  if (successful) {
    node_lookup_[node_id] = layer_id;
  }

  return successful;
}

bool DynamicSceneGraph::emplaceNode(LayerId layer,
                                    LayerPrefix prefix,
                                    std::chrono::nanoseconds time,
                                    NodeAttributes::Ptr&& attrs,
                                    bool add_edge) {
  bool has_layer = false;
  NodeSymbol new_node_id = prefix.makeId(0);
  if (hasLayer(layer, prefix)) {
    has_layer = true;
    new_node_id = prefix.makeId(dynamic_layers_[layer][prefix]->next_node_);
  }

  if (hasNode(new_node_id)) {
    LOG(ERROR) << "scene graph contains node " << new_node_id.getLabel()
               << ". fix conflicting prefix: " << prefix.str();
    return false;
  }

  if (!has_layer) {
    createDynamicLayer(layer, prefix);
  }

  if (!dynamic_layers_[layer][prefix]->emplaceNode(time, std::move(attrs), add_edge)) {
    return false;
  }

  node_lookup_[new_node_id] = {layer, prefix};
  return true;
}

bool DynamicSceneGraph::insertNode(Node::Ptr&& node) {
  if (!node) {
    return false;
  }

  if (node_lookup_.count(node->id)) {
    return false;
  }

  // we grab these here to avoid problems with move
  const LayerId node_layer = node->layer;
  const NodeId node_id = node->id;

  if (!hasLayer(node_layer)) {
    return false;
  }

  const bool successful = layers_[node_layer]->insertNode(std::move(node));
  if (successful) {
    node_lookup_[node_id] = node_layer;
  }

  return successful;
}

BaseLayer& DynamicSceneGraph::layerFromKey(const LayerKey& key) {
  const auto& layer = static_cast<const DynamicSceneGraph*>(this)->layerFromKey(key);
  return const_cast<BaseLayer&>(layer);
}

const BaseLayer& DynamicSceneGraph::layerFromKey(const LayerKey& key) const {
  if (key.dynamic) {
    return *dynamic_layers_.at(key.layer).at(key.prefix);
  } else {
    return *layers_.at(key.layer);
  }
}

bool DynamicSceneGraph::insertEdge(NodeId source,
                                   NodeId target,
                                   EdgeAttributes::Ptr&& edge_info) {
  LayerKey source_key, target_key;
  if (hasEdge(source, target, &source_key, &target_key)) {
    return false;
  }

  if (!source_key || !target_key) {
    return false;
  }

  auto attrs = (edge_info == nullptr) ? std::make_unique<EdgeAttributes>()
                                      : std::move(edge_info);

  if (source_key == target_key) {
    return layerFromKey(source_key).insertEdge(source, target, std::move(attrs));
  }

  if (!addAncestry(source, target, source_key, target_key)) {
    return false;
  }

  if (source_key.dynamic || target_key.dynamic) {
    dynamic_interlayer_edges_.insert(source, target, std::move(attrs));
  } else {
    interlayer_edges_.insert(source, target, std::move(attrs));
  }

  return true;
}

bool DynamicSceneGraph::insertMeshEdge(NodeId source,
                                       size_t mesh_vertex,
                                       bool allow_invalid_mesh) {
  if (!hasNode(source)) {
    return false;
  }

  if (!allow_invalid_mesh) {
    if (!mesh_vertices_ || mesh_vertex >= mesh_vertices_->size()) {
      return false;
    }
  }

  if (hasMeshEdge(source, mesh_vertex)) {
    return false;
  }

  mesh_edges_.emplace(std::piecewise_construct,
                      std::forward_as_tuple(next_mesh_edge_idx_),
                      std::forward_as_tuple(source, mesh_vertex));
  mesh_edges_node_lookup_[source][mesh_vertex] = next_mesh_edge_idx_;
  mesh_edges_vertex_lookup_[mesh_vertex][source] = next_mesh_edge_idx_;
  next_mesh_edge_idx_++;
  return true;
}

bool DynamicSceneGraph::hasLayer(LayerId layer_id) const {
  if (layer_id != mesh_layer_id) {
    return layers_.count(layer_id) != 0;
  }

  return hasMesh();
}

bool DynamicSceneGraph::hasLayer(LayerId layer, LayerPrefix layer_prefix) const {
  if (!dynamic_layers_.count(layer)) {
    return 0;
  }

  return dynamic_layers_.at(layer).count(layer_prefix) != 0;
}

bool DynamicSceneGraph::hasNode(NodeId node_id) const {
  return node_lookup_.count(node_id) != 0;
}

bool DynamicSceneGraph::hasMesh() const {
  return mesh_vertices_ != nullptr && mesh_faces_ != nullptr;
}

const SceneGraphLayer& DynamicSceneGraph::getLayer(LayerId layer) const {
  if (!hasLayer(layer)) {
    std::stringstream ss;
    ss << "missing layer " << layer;
    throw std::out_of_range(ss.str());
  }

  return *layers_.at(layer);
}

const DynamicSceneGraphLayer& DynamicSceneGraph::getLayer(LayerId layer,
                                                          LayerPrefix prefix) const {
  if (!hasLayer(layer, prefix)) {
    std::stringstream ss;
    ss << "missing dynamic layer " << layer << "(" << prefix.str() << ")";
    throw std::out_of_range(ss.str());
  }

  return *dynamic_layers_.at(layer).at(prefix);
}

std::optional<NodeRef> DynamicSceneGraph::getNode(NodeId node_id) const {
  auto iter = node_lookup_.find(node_id);
  if (iter == node_lookup_.end()) {
    return std::nullopt;
  }

  return std::cref(*getNodePtr(node_id, iter->second));
}

std::optional<LayerKey> DynamicSceneGraph::getLayerForNode(NodeId node_id) const {
  auto iter = node_lookup_.find(node_id);
  if (iter == node_lookup_.end()) {
    return std::nullopt;
  }

  return iter->second;
}

std::optional<DynamicNodeRef> DynamicSceneGraph::getDynamicNode(NodeId node_id) const {
  auto iter = node_lookup_.find(node_id);
  if (iter == node_lookup_.end()) {
    return std::nullopt;
  }

  const auto& info = iter->second;
  if (!info.dynamic) {
    return std::nullopt;
  }

  return dynamic_layers_.at(info.layer).at(info.prefix)->getNode(node_id);
}

std::optional<EdgeRef> DynamicSceneGraph::getEdge(NodeId source, NodeId target) const {
  if (!hasEdge(source, target)) {
    return std::nullopt;
  }

  // defer to layers if it is a intralayer edge
  const auto& source_key = node_lookup_.at(source);
  const auto& target_key = node_lookup_.at(target);
  if (source_key == target_key) {
    return layerFromKey(source_key).getEdge(source, target);
  }

  if (source_key.dynamic || target_key.dynamic) {
    return std::cref(dynamic_interlayer_edges_.get(source, target));
  } else {
    return std::cref(interlayer_edges_.get(source, target));
  }
}

bool DynamicSceneGraph::removeNode(NodeId node_id) {
  if (!hasNode(node_id)) {
    return false;
  }

  const auto info = node_lookup_.at(node_id);

  if (mesh_edges_node_lookup_.count(node_id)) {
    std::list<size_t> mesh_edge_targets_to_remove;
    for (const auto& vertex_edge_pair : mesh_edges_node_lookup_.at(node_id)) {
      mesh_edge_targets_to_remove.push_back(vertex_edge_pair.first);
    }

    for (const auto& vertex : mesh_edge_targets_to_remove) {
      removeMeshEdge(node_id, vertex);
    }
  }

  auto node = getNodePtr(node_id, info);
  if (node->hasParent()) {
    removeInterlayerEdge(node_id, node->parent_);
  }

  std::set<NodeId> targets_to_erase = node->children_;
  for (const auto& target : targets_to_erase) {
    removeInterlayerEdge(node_id, target);
  }

  layerFromKey(info).removeNode(node_id);
  node_lookup_.erase(node_id);
  return true;
}

bool DynamicSceneGraph::hasEdge(NodeId source,
                                NodeId target,
                                LayerKey* source_key,
                                LayerKey* target_key) const {
  auto source_iter = node_lookup_.find(source);
  if (source_iter == node_lookup_.end()) {
    return false;
  }

  auto target_iter = node_lookup_.find(target);
  if (target_iter == node_lookup_.end()) {
    return false;
  }

  if (source_key != nullptr) {
    *source_key = source_iter->second;
  }

  if (target_key != nullptr) {
    *target_key = target_iter->second;
  }

  if (source_iter->second == target_iter->second) {
    return layerFromKey(source_iter->second).hasEdge(source, target);
  }

  if (source_iter->second.dynamic || target_iter->second.dynamic) {
    return dynamic_interlayer_edges_.contains(source, target);
  } else {
    return interlayer_edges_.contains(source, target);
  }
}

bool DynamicSceneGraph::removeEdge(NodeId source, NodeId target) {
  LayerKey source_key, target_key;
  if (!hasEdge(source, target, &source_key, &target_key)) {
    return false;
  }

  if (!source_key || !target_key) {
    return false;
  }

  if (source_key == target_key) {
    return layerFromKey(source_key).removeEdge(source, target);
  }

  removeInterlayerEdge(source, target, source_key, target_key);
  return true;
}

bool DynamicSceneGraph::removeMeshEdge(NodeId source, size_t mesh_vertex) {
  if (!hasMeshEdge(source, mesh_vertex)) {
    return false;
  }

  mesh_edges_.erase(mesh_edges_node_lookup_.at(source).at(mesh_vertex));

  mesh_edges_node_lookup_.at(source).erase(mesh_vertex);
  if (mesh_edges_node_lookup_.at(source).empty()) {
    mesh_edges_node_lookup_.erase(source);
  }

  mesh_edges_vertex_lookup_.at(mesh_vertex).erase(source);
  if (mesh_edges_vertex_lookup_.at(mesh_vertex).empty()) {
    mesh_edges_vertex_lookup_.erase(mesh_vertex);
  }

  next_mesh_edge_idx_++;
  return true;
}

bool DynamicSceneGraph::isDynamic(NodeId source) const {
  auto iter = node_lookup_.find(source);
  if (iter == node_lookup_.end()) {
    return false;
  }

  return iter->second.dynamic;
}

size_t DynamicSceneGraph::numDynamicLayersOfType(LayerId layer) const {
  if (!dynamic_layers_.count(layer)) {
    return 0;
  }

  return dynamic_layers_.at(layer).size();
}

size_t DynamicSceneGraph::numDynamicLayers() const {
  size_t num_layers = 0;
  for (const auto& id_layer_group : dynamic_layers_) {
    num_layers += id_layer_group.second.size();
  }
  return num_layers;
}

void DynamicSceneGraph::clearMeshEdges() {
  mesh_edges_.clear();
  mesh_edges_node_lookup_.clear();
  mesh_edges_vertex_lookup_.clear();
}

void DynamicSceneGraph::setMeshDirectly(const pcl::PolygonMesh& mesh) {
  mesh_vertices_.reset(new MeshVertices());
  pcl::fromPCLPointCloud2(mesh.cloud, *mesh_vertices_);

  mesh_faces_.reset(new MeshFaces(mesh.polygons.begin(), mesh.polygons.end()));
}

void DynamicSceneGraph::setMesh(const MeshVertices::Ptr& vertices,
                                const std::shared_ptr<MeshFaces>& faces,
                                bool invalidate_all_edges) {
  if (!vertices) {
    VLOG(1) << "received empty mesh. resetting all mesh edges";
    mesh_vertices_.reset();
    mesh_faces_.reset();
    clearMeshEdges();
    return;
  }

  mesh_faces_ = faces;
  mesh_vertices_ = vertices;

  if (invalidate_all_edges) {
    clearMeshEdges();
    return;
  }

  size_t max_vertex = mesh_vertices_->size();
  std::list<MeshEdge> invalid_edges;
  for (const auto& vertex_map_pair : mesh_edges_vertex_lookup_) {
    const size_t vertex_id = vertex_map_pair.first;
    if (vertex_id < max_vertex) {
      continue;
    }

    for (const auto& node_edge_pair : mesh_edges_vertex_lookup_.at(vertex_id)) {
      invalid_edges.push_back(mesh_edges_.at(node_edge_pair.second));
    }
  }

  for (const auto edge : invalid_edges) {
    removeMeshEdge(edge.source_node, edge.mesh_vertex);
  }
}

bool DynamicSceneGraph::hasMeshEdge(NodeId source, size_t mesh_vertex) const {
  if (!mesh_edges_node_lookup_.count(source)) {
    return false;
  }

  if (!mesh_edges_node_lookup_.at(source).count(mesh_vertex)) {
    return false;
  }

  return true;
}

bool DynamicSceneGraph::mergeNodes(NodeId node_from, NodeId node_to) {
  if (!hasNode(node_from) || !hasNode(node_to)) {
    return false;
  }

  if (node_from == node_to) {
    return false;
  }

  const auto info = node_lookup_.at(node_from);

  if (info != node_lookup_.at(node_to)) {
    return false;  // Cannot merge nodes of different layers
  }

  Node* node = layers_[info.layer]->nodes_.at(node_from).get();

  // Remove parent
  if (node->hasParent()) {
    rewireInterlayerEdge(node_from, node->parent_, node_to, node->parent_);
  }

  // Reconnect children
  std::set<NodeId> targets_to_rewire = node->children_;
  for (const auto& target : targets_to_rewire) {
    rewireInterlayerEdge(node_from, target, node_to, target);
  }

  // TODO(nathan) dynamic merge
  layers_[info.layer]->mergeNodes(node_from, node_to);
  node_lookup_.erase(node_from);
  return true;
}

size_t DynamicSceneGraph::numLayers() const {
  const size_t static_size = layers_.size() + 1;  // for the mesh

  size_t unique_dynamic_layers = 0;
  for (const auto& id_layer_group_pair : dynamic_layers_) {
    if (!layers_.count(id_layer_group_pair.first) &&
        id_layer_group_pair.first != mesh_layer_id) {
      unique_dynamic_layers++;
    }
  }

  return static_size + unique_dynamic_layers;
}

size_t DynamicSceneGraph::numNodes() const {
  size_t total_nodes = 0u;
  for (const auto& id_layer_pair : layers_) {
    total_nodes += id_layer_pair.second->numNodes();
  }

  return total_nodes + numDynamicNodes() +
         ((mesh_vertices_ == nullptr) ? 0 : mesh_vertices_->size());
}

size_t DynamicSceneGraph::numDynamicNodes() const {
  size_t total_nodes = 0u;
  for (const auto& layer_group : dynamic_layers_) {
    for (const auto& prefix_layer_pair : layer_group.second) {
      total_nodes += prefix_layer_pair.second->numNodes();
    }
  }

  return total_nodes;
}

size_t DynamicSceneGraph::numEdges() const {
  size_t total_edges = interlayer_edges_.size();
  for (const auto& id_layer_pair : layers_) {
    total_edges += id_layer_pair.second->numEdges();
  }

  for (const auto& id_group_pair : dynamic_layers_) {
    for (const auto& prefix_layer_pair : id_group_pair.second) {
      total_edges += prefix_layer_pair.second->numEdges();
    }
  }

  return total_edges + mesh_edges_.size() + dynamic_interlayer_edges_.size();
}

bool DynamicSceneGraph::updateFromLayer(SceneGraphLayer& other_layer,
                                        std::unique_ptr<Edges>&& edges) {
  // TODO(nathan) consider condensing with mergeGraph
  if (!layers_.count(other_layer.id)) {
    LOG(ERROR) << "Scene graph does not have layer: " << other_layer.id;
    return false;
  }

  auto& internal_layer = *layers_.at(other_layer.id);
  for (auto& id_node_pair : other_layer.nodes_) {
    if (internal_layer.hasNode(id_node_pair.first)) {
      // just copy the attributes (prior edge information should be preserved)
      internal_layer.nodes_[id_node_pair.first]->attributes_ =
          std::move(id_node_pair.second->attributes_);
    } else {
      // we need to let the scene graph know about new nodes
      node_lookup_[id_node_pair.first] = internal_layer.id;
      internal_layer.nodes_[id_node_pair.first] = std::move(id_node_pair.second);
      internal_layer.nodes_status_[id_node_pair.first] = NodeStatus::NEW;
    }
  }

  // we just invalidated all the nodes in the other layer, so we better reset everything
  other_layer.reset();

  if (!edges) {
    return true;
  }

  for (auto& id_edge_pair : *edges) {
    auto& edge = id_edge_pair.second;
    if (internal_layer.hasEdge(edge.source, edge.target)) {
      internal_layer.edges_.edges.at(id_edge_pair.first).info = std::move(edge.info);
      continue;
    }

    internal_layer.insertEdge(edge.source, edge.target, std::move(edge.info));
  }

  // we just invalidated all the info for the new edges, so reset the edges
  edges.reset();
  return true;
}

bool DynamicSceneGraph::mergeGraph(const DynamicSceneGraph& other,
                                   bool allow_invalid_mesh,
                                   bool clear_mesh_edges,
                                   std::map<LayerId, bool>* update_map,
                                   bool update_dynamic) {
  for (const auto& id_layers : other.dynamicLayers()) {
    const LayerId layer = id_layers.first;

    for (const auto& prefix_layer : id_layers.second) {
      const auto prefix = prefix_layer.first;
      if (!hasLayer(layer, prefix)) {
        createDynamicLayer(layer, prefix);
      }

      dynamic_layers_[layer][prefix]->mergeLayer(
          *prefix_layer.second, &node_lookup_, update_dynamic);
    }
  }

  std::vector<NodeId> removed_nodes;
  for (const auto& id_layer : other.layers()) {
    const LayerId layer = id_layer.first;
    if (!hasLayer(layer)) {
      continue;
    }

    const bool update =
        (update_map && update_map->count(layer)) ? update_map->at(layer) : true;
    layers_[layer]->mergeLayer(*id_layer.second, &node_lookup_, update);
    id_layer.second->getRemovedNodes(removed_nodes, false);
  }

  for (const auto& removed_id : removed_nodes) {
    removeNode(removed_id);
  }

  for (const auto& id_edge_pair : other.interlayer_edges()) {
    const auto& edge = id_edge_pair.second;
    insertEdge(edge.source, edge.target, edge.info->clone());
  }

  for (const auto& id_edge_pair : other.dynamic_interlayer_edges()) {
    const auto& edge = id_edge_pair.second;
    insertEdge(edge.source, edge.target, edge.info->clone());
  }

  if (clear_mesh_edges) {
    clearMeshEdges();
  }

  for (const auto& id_mesh_edge : other.mesh_edges_) {
    insertMeshEdge(id_mesh_edge.second.source_node,
                   id_mesh_edge.second.mesh_vertex,
                   allow_invalid_mesh);
  }

  // TODO(Yun) check the other mesh info (faces, vertices etc. )
  return true;
}

std::vector<NodeId> DynamicSceneGraph::getRemovedNodes(bool clear_removed) {
  std::vector<NodeId> to_return;
  visitLayers([&](LayerKey, BaseLayer* layer) {
    layer->getRemovedNodes(to_return, clear_removed);
  });
  return to_return;
}

std::vector<NodeId> DynamicSceneGraph::getNewNodes(bool clear_new) {
  std::vector<NodeId> to_return;
  visitLayers(
      [&](LayerKey, BaseLayer* layer) { layer->getNewNodes(to_return, clear_new); });
  return to_return;
}

std::vector<EdgeKey> DynamicSceneGraph::getRemovedEdges(bool clear_removed) {
  std::vector<EdgeKey> to_return;
  visitLayers([&](LayerKey, BaseLayer* layer) {
    layer->getRemovedEdges(to_return, clear_removed);
  });

  interlayer_edges_.getRemoved(to_return, clear_removed);
  dynamic_interlayer_edges_.getRemoved(to_return, clear_removed);
  return to_return;
}

std::vector<EdgeKey> DynamicSceneGraph::getNewEdges(bool clear_new) {
  std::vector<EdgeKey> to_return;
  visitLayers(
      [&](LayerKey, BaseLayer* layer) { layer->getNewEdges(to_return, clear_new); });

  interlayer_edges_.getNew(to_return, clear_new);
  dynamic_interlayer_edges_.getNew(to_return, clear_new);
  return to_return;
}

std::optional<Eigen::Vector3d> DynamicSceneGraph::getMeshPosition(size_t idx) const {
  if (!mesh_vertices_) {
    return std::nullopt;
  }

  if (idx >= mesh_vertices_->size()) {
    return std::nullopt;
  }

  const pcl::PointXYZRGBA& point = mesh_vertices_->at(idx);
  Eigen::Vector3d pos(point.x, point.y, point.z);
  return pos;
}

std::vector<size_t> DynamicSceneGraph::getMeshConnectionIndices(NodeId node) const {
  std::vector<size_t> to_return;
  if (!mesh_edges_node_lookup_.count(node)) {
    return to_return;
  }

  for (const auto& id_edge_pair : mesh_edges_node_lookup_.at(node)) {
    to_return.push_back(id_edge_pair.first);
  }

  return to_return;
}

Eigen::Vector3d DynamicSceneGraph::getPosition(NodeId node) const {
  auto iter = node_lookup_.find(node);
  if (iter == node_lookup_.end()) {
    throw std::out_of_range("node " + NodeSymbol(node).getLabel() +
                            " is not in the graph");
  }

  auto info = iter->second;
  if (info.dynamic) {
    return dynamic_layers_.at(info.layer).at(info.prefix)->getPosition(node);
  }

  return layers_.at(info.layer)->getPosition(node);
}

void DynamicSceneGraph::invalidateMeshVertex(size_t index) {
  if (!mesh_edges_vertex_lookup_.count(index)) {
    return;
  }

  std::list<NodeId> nodes;
  for (const auto& node_edge_pair : mesh_edges_vertex_lookup_[index]) {
    nodes.push_back(node_edge_pair.first);
  }

  for (const auto& node : nodes) {
    removeMeshEdge(node, index);
  }
}

void DynamicSceneGraph::visitLayers(const LayerVisitor& cb) {
  for (auto& id_layer_pair : layers_) {
    cb(id_layer_pair.first, id_layer_pair.second.get());
  }

  for (auto& id_group_pair : dynamic_layers_) {
    for (auto& prefix_layer_pair : id_group_pair.second) {
      cb(LayerKey(id_group_pair.first, prefix_layer_pair.first),
         prefix_layer_pair.second.get());
    }
  }
}

SceneGraphNode* DynamicSceneGraph::getNodePtr(NodeId node, const LayerKey& info) const {
  if (info.dynamic) {
    const auto idx = NodeSymbol(node).categoryId();
    return dynamic_layers_.at(info.layer).at(info.prefix)->nodes_.at(idx).get();
  } else {
    return layers_.at(info.layer)->nodes_.at(node).get();
  }
}

bool DynamicSceneGraph::addAncestry(NodeId source,
                                    NodeId target,
                                    const LayerKey& source_key,
                                    const LayerKey& target_key) {
  SceneGraphNode* source_node = getNodePtr(source, source_key);
  SceneGraphNode* target_node = getNodePtr(target, target_key);
  if (source_key.isParent(target_key)) {
    if (target_node->hasParent()) {
      return false;
    }
    source_node->children_.insert(target);
    target_node->setParent(source);
  } else if (target_key.isParent(source_key)) {
    if (source_node->hasParent()) {
      return false;
    }
    target_node->children_.insert(source);
    source_node->setParent(target);
  } else {
    source_node->siblings_.insert(target);
    target_node->siblings_.insert(source);
  }

  return true;
}

void DynamicSceneGraph::removeAncestry(NodeId source,
                                       NodeId target,
                                       const LayerKey& source_key,
                                       const LayerKey& target_key) {
  SceneGraphNode* source_node = getNodePtr(source, source_key);
  SceneGraphNode* target_node = getNodePtr(target, target_key);

  if (source_key.isParent(target_key)) {
    source_node->children_.erase(target);
    target_node->clearParent();
  } else if (target_key.isParent(source_key)) {
    target_node->children_.erase(source);
    source_node->clearParent();
  } else {
    source_node->siblings_.erase(target);
    target_node->siblings_.erase(source);
  }
}

void DynamicSceneGraph::removeInterlayerEdge(NodeId source,
                                             NodeId target,
                                             const LayerKey& source_key,
                                             const LayerKey& target_key) {
  removeAncestry(source, target, source_key, target_key);
  if (source_key.dynamic || target_key.dynamic) {
    dynamic_interlayer_edges_.remove(source, target);
  } else {
    interlayer_edges_.remove(source, target);
  }
}

void DynamicSceneGraph::rewireInterlayerEdge(NodeId source,
                                             NodeId target,
                                             NodeId new_source,
                                             NodeId new_target) {
  if (source == new_source && target == new_target) {
    return;
  }

  const auto& source_key = node_lookup_.at(source);
  const auto& target_key = node_lookup_.at(target);

  LayerKey new_source_key, new_target_key;
  if (hasEdge(new_source, new_target, &new_source_key, &new_target_key)) {
    removeInterlayerEdge(source, target, source_key, target_key);
    return;
  }

  removeAncestry(source, target, source_key, target_key);
  addAncestry(new_source, new_target, new_source_key, new_target_key);

  // TODO(nathan) edges can technically jump from dynamic to static, so problems with
  // index not being available in other container
  EdgeAttributes::Ptr attrs;
  if (source_key.dynamic || target_key.dynamic) {
    attrs = dynamic_interlayer_edges_.get(source, target).info->clone();
    dynamic_interlayer_edges_.remove(source, target);
  } else {
    attrs = interlayer_edges_.get(source, target).info->clone();
    interlayer_edges_.remove(source, target);
  }

  if (new_source_key.dynamic || target_key.dynamic) {
    dynamic_interlayer_edges_.insert(new_source, new_target, std::move(attrs));
  } else {
    interlayer_edges_.insert(new_source, new_target, std::move(attrs));
  }
}

pcl::PolygonMesh DynamicSceneGraph::getMesh() const {
  pcl::PolygonMesh mesh;
  pcl::toPCLPointCloud2(*mesh_vertices_, mesh.cloud);
  mesh.polygons = *mesh_faces_;
  return mesh;
};

DynamicSceneGraph::LayerIds getDefaultLayerIds() {
  return {KimeraDsgLayers::OBJECTS,
          KimeraDsgLayers::PLACES,
          KimeraDsgLayers::ROOMS,
          KimeraDsgLayers::BUILDINGS};
}

}  // namespace kimera
