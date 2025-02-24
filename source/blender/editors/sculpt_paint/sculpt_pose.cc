/* SPDX-FileCopyrightText: 2020 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "MEM_guardedalloc.h"

#include "BLI_enumerable_thread_specific.hh"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"

#include "DNA_brush_types.h"
#include "DNA_object_types.h"

#include "BKE_brush.hh"
#include "BKE_ccg.hh"
#include "BKE_colortools.hh"
#include "BKE_mesh.hh"
#include "BKE_paint.hh"
#include "BKE_pbvh_api.hh"

#include "mesh_brush_common.hh"
#include "paint_intern.hh"
#include "sculpt_cloth.hh"
#include "sculpt_face_set.hh"
#include "sculpt_flood_fill.hh"
#include "sculpt_hide.hh"
#include "sculpt_intern.hh"
#include "sculpt_pose.hh"
#include "sculpt_smooth.hh"

#include "bmesh.hh"

#include <cmath>
#include <cstdlib>

namespace blender::ed::sculpt_paint::pose {

static void solve_ik_chain(IKChain &ik_chain, const float3 &initial_target, const bool use_anchor)
{
  MutableSpan<IKChainSegment> segments = ik_chain.segments;

  /* Set the initial target. */
  float3 target = initial_target;

  /* Solve the positions and rotations of all segments in the chain. */
  for (const int i : segments.index_range()) {
    /* Calculate the rotation to orientate the segment to the target from its initial state. */
    float3 current_orientation = math::normalize(target - segments[i].orig);
    float3 initial_orientation = math::normalize(segments[i].initial_head -
                                                 segments[i].initial_orig);
    rotation_between_vecs_to_quat(segments[i].rot, initial_orientation, current_orientation);

    /* Rotate the segment by calculating a new head position. */
    float3 current_head_position = segments[i].orig + current_orientation * segments[i].len;

    /* Move the origin of the segment towards the target. */
    float3 current_origin_position = target - current_head_position;

    /* Store the new head and origin positions to the segment. */
    segments[i].head = current_head_position;
    segments[i].orig += current_origin_position;

    /* Use the origin of this segment as target for the next segment in the chain. */
    target = segments[i].orig;
  }

  /* Move back the whole chain to preserve the anchor point. */
  if (use_anchor) {
    float3 anchor_diff = segments.last().initial_orig - segments.last().orig;
    for (const int i : segments.index_range()) {
      segments[i].orig += anchor_diff;
      segments[i].head += anchor_diff;
    }
  }
}

static void solve_roll_chain(IKChain &ik_chain, const Brush &brush, const float roll)
{
  MutableSpan<IKChainSegment> segments = ik_chain.segments;

  for (const int i : segments.index_range()) {
    float3 initial_orientation = math::normalize(segments[i].initial_head -
                                                 segments[i].initial_orig);
    float initial_rotation[4];
    float current_rotation[4];

    /* Calculate the current roll angle using the brush curve. */
    float current_roll = roll * BKE_brush_curve_strength(&brush, i, segments.size());

    axis_angle_normalized_to_quat(initial_rotation, initial_orientation, 0.0f);
    axis_angle_normalized_to_quat(current_rotation, initial_orientation, current_roll);

    /* Store the difference of the rotations in the segment rotation. */
    rotation_between_quats_to_quat(segments[i].rot, current_rotation, initial_rotation);
  }
}

static void solve_translate_chain(IKChain &ik_chain, const float delta[3])
{
  for (IKChainSegment &segment : ik_chain.segments) {
    /* Move the origin and head of each segment by delta. */
    add_v3_v3v3(segment.head, segment.initial_head, delta);
    add_v3_v3v3(segment.orig, segment.initial_orig, delta);

    /* Reset the segment rotation. */
    unit_qt(segment.rot);
  }
}

static void solve_scale_chain(IKChain &ik_chain, const float scale[3])
{
  for (IKChainSegment &segment : ik_chain.segments) {
    /* Assign the scale to each segment. */
    copy_v3_v3(segment.scale, scale);
  }
}

struct BrushLocalData {
  Vector<float3> positions;
  Vector<float> factors;
  Vector<float> segment_weights;
  Vector<float3> segment_translations;
  Vector<float3> translations;
};

BLI_NOINLINE static void calc_segment_translations(const Span<float3> positions,
                                                   const IKChainSegment &segment,
                                                   const MutableSpan<float3> translations)
{
  BLI_assert(positions.size() == translations.size());
  for (const int i : positions.index_range()) {
    float3 position = positions[i];
    const ePaintSymmetryAreas symm_area = SCULPT_get_vertex_symm_area(position);
    position = math::transform_point(segment.pivot_mat_inv[int(symm_area)], position);
    position = math::transform_point(segment.trans_mat[int(symm_area)], position);
    position = math::transform_point(segment.pivot_mat[int(symm_area)], position);
    translations[i] = position - positions[i];
  }
}

BLI_NOINLINE static void add_arrays(const MutableSpan<float3> a, const Span<float3> b)
{
  BLI_assert(a.size() == b.size());
  for (const int i : a.index_range()) {
    a[i] += b[i];
  }
}

static void calc_mesh(const Depsgraph &depsgraph,
                      const Sculpt &sd,
                      const Brush &brush,
                      const Span<float3> positions_eval,
                      const bke::pbvh::MeshNode &node,
                      Object &object,
                      BrushLocalData &tls,
                      const MutableSpan<float3> positions_orig)
{
  SculptSession &ss = *object.sculpt;
  const StrokeCache &cache = *ss.cache;
  const Mesh &mesh = *static_cast<Mesh *>(object.data);

  const Span<int> verts = node.verts();
  const Span<float3> positions = gather_data_mesh(positions_eval, verts, tls.positions);
  const OrigPositionData orig_data = orig_position_data_get_mesh(object, node);

  tls.factors.resize(verts.size());
  const MutableSpan<float> factors = tls.factors;
  fill_factor_from_hide_and_mask(mesh, verts, factors);
  auto_mask::calc_vert_factors(depsgraph, object, cache.automasking.get(), node, verts, factors);

  tls.translations.resize(verts.size());
  const MutableSpan<float3> translations = tls.translations;
  translations.fill(float3(0));

  tls.segment_weights.resize(verts.size());
  tls.segment_translations.resize(verts.size());
  const MutableSpan<float> segment_weights = tls.segment_weights;
  const MutableSpan<float3> segment_translations = tls.segment_translations;

  for (const IKChainSegment &segment : cache.pose_ik_chain->segments) {
    calc_segment_translations(orig_data.positions, segment, segment_translations);
    gather_data_mesh(segment.weights.as_span(), verts, segment_weights);
    scale_translations(segment_translations, segment_weights);
    add_arrays(translations, segment_translations);
  }
  scale_translations(translations, factors);

  switch (eBrushDeformTarget(brush.deform_target)) {
    case BRUSH_DEFORM_TARGET_GEOMETRY:
      reset_translations_to_original(translations, positions, orig_data.positions);
      write_translations(
          depsgraph, sd, object, positions_eval, verts, translations, positions_orig);
      break;
    case BRUSH_DEFORM_TARGET_CLOTH_SIM:
      add_arrays(translations, orig_data.positions);
      scatter_data_mesh(
          translations.as_span(), verts, cache.cloth_sim->deformation_pos.as_mutable_span());
      break;
  }
}

static void calc_grids(const Depsgraph &depsgraph,
                       const Sculpt &sd,
                       const Brush &brush,
                       const bke::pbvh::GridsNode &node,
                       Object &object,
                       BrushLocalData &tls)
{
  SculptSession &ss = *object.sculpt;
  const StrokeCache &cache = *ss.cache;
  SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;

  const Span<int> grids = node.grids();
  const Span<float3> positions = gather_grids_positions(subdiv_ccg, grids, tls.positions);
  const OrigPositionData orig_data = orig_position_data_get_grids(object, node);

  tls.factors.resize(positions.size());
  const MutableSpan<float> factors = tls.factors;
  fill_factor_from_hide_and_mask(subdiv_ccg, grids, factors);
  auto_mask::calc_grids_factors(depsgraph, object, cache.automasking.get(), node, grids, factors);

  tls.translations.resize(positions.size());
  const MutableSpan<float3> translations = tls.translations;
  translations.fill(float3(0));

  tls.segment_weights.resize(positions.size());
  tls.segment_translations.resize(positions.size());
  const MutableSpan<float> segment_weights = tls.segment_weights;
  const MutableSpan<float3> segment_translations = tls.segment_translations;

  for (const IKChainSegment &segment : cache.pose_ik_chain->segments) {
    calc_segment_translations(orig_data.positions, segment, segment_translations);
    gather_data_grids(subdiv_ccg, segment.weights.as_span(), grids, segment_weights);
    scale_translations(segment_translations, segment_weights);
    add_arrays(translations, segment_translations);
  }
  scale_translations(translations, factors);

  switch (eBrushDeformTarget(brush.deform_target)) {
    case BRUSH_DEFORM_TARGET_GEOMETRY:
      reset_translations_to_original(translations, positions, orig_data.positions);
      clip_and_lock_translations(sd, ss, orig_data.positions, translations);
      apply_translations(translations, grids, subdiv_ccg);
      break;
    case BRUSH_DEFORM_TARGET_CLOTH_SIM:
      add_arrays(translations, orig_data.positions);
      scatter_data_grids(subdiv_ccg,
                         translations.as_span(),
                         grids,
                         cache.cloth_sim->deformation_pos.as_mutable_span());
      break;
  }
}

static void calc_bmesh(const Depsgraph &depsgraph,
                       const Sculpt &sd,
                       const Brush &brush,
                       bke::pbvh::BMeshNode &node,
                       Object &object,
                       BrushLocalData &tls)
{
  SculptSession &ss = *object.sculpt;
  const StrokeCache &cache = *ss.cache;

  const Set<BMVert *, 0> &verts = BKE_pbvh_bmesh_node_unique_verts(&node);
  const Span<float3> positions = gather_bmesh_positions(verts, tls.positions);
  Array<float3> orig_positions(verts.size());
  Array<float3> orig_normals(verts.size());
  orig_position_data_gather_bmesh(*ss.bm_log, verts, orig_positions, orig_normals);

  tls.factors.resize(verts.size());
  const MutableSpan<float> factors = tls.factors;
  fill_factor_from_hide_and_mask(*ss.bm, verts, factors);
  auto_mask::calc_vert_factors(depsgraph, object, cache.automasking.get(), node, verts, factors);

  tls.translations.resize(verts.size());
  const MutableSpan<float3> translations = tls.translations;
  translations.fill(float3(0));

  tls.segment_weights.resize(verts.size());
  tls.segment_translations.resize(verts.size());
  const MutableSpan<float> segment_weights = tls.segment_weights;
  const MutableSpan<float3> segment_translations = tls.segment_translations;

  for (const IKChainSegment &segment : cache.pose_ik_chain->segments) {
    calc_segment_translations(orig_positions, segment, segment_translations);
    gather_data_bmesh(segment.weights.as_span(), verts, segment_weights);
    scale_translations(segment_translations, segment_weights);
    add_arrays(translations, segment_translations);
  }
  scale_translations(translations, factors);

  switch (eBrushDeformTarget(brush.deform_target)) {
    case BRUSH_DEFORM_TARGET_GEOMETRY:
      reset_translations_to_original(translations, positions, orig_positions);
      clip_and_lock_translations(sd, ss, orig_positions, translations);
      apply_translations(translations, verts);
      break;
    case BRUSH_DEFORM_TARGET_CLOTH_SIM:
      add_arrays(translations, orig_positions);
      scatter_data_bmesh(
          translations.as_span(), verts, cache.cloth_sim->deformation_pos.as_mutable_span());
      break;
  }
}

struct PoseGrowFactorData {
  float3 pos_avg;
  int pos_count;
  static PoseGrowFactorData join(const PoseGrowFactorData &a, const PoseGrowFactorData &b)
  {
    PoseGrowFactorData joined;
    joined.pos_avg = a.pos_avg + b.pos_avg;
    joined.pos_count = a.pos_count + b.pos_count;
    return joined;
  }
};

struct GrowFactorLocalData {
  Vector<int> vert_indices;
  Vector<Vector<int>> vert_neighbors;
};

BLI_NOINLINE static void add_fake_neighbors(const Span<int> fake_neighbors,
                                            const Span<int> verts,
                                            const MutableSpan<Vector<int>> neighbors)
{
  for (const int i : verts.index_range()) {
    if (fake_neighbors[verts[i]] != FAKE_NEIGHBOR_NONE) {
      neighbors[i].append(fake_neighbors[verts[i]]);
    }
  }
}

static void grow_factors_mesh(const ePaintSymmetryFlags symm,
                              const float3 &pose_initial_position,
                              const Span<float3> vert_positions,
                              const OffsetIndices<int> faces,
                              const Span<int> corner_verts,
                              const GroupedSpan<int> vert_to_face_map,
                              const Span<bool> hide_vert,
                              const Span<bool> hide_poly,
                              const Span<int> fake_neighbors,
                              const Span<float> prev_mask,
                              const bke::pbvh::MeshNode &node,
                              GrowFactorLocalData &tls,
                              const MutableSpan<float> pose_factor,
                              PoseGrowFactorData &gftd)
{
  const Span<int> verts = hide::node_visible_verts(node, hide_vert, tls.vert_indices);

  tls.vert_neighbors.resize(verts.size());
  const MutableSpan<Vector<int>> neighbors = tls.vert_neighbors;
  calc_vert_neighbors(faces, corner_verts, vert_to_face_map, hide_poly, verts, neighbors);

  if (!fake_neighbors.is_empty()) {
    add_fake_neighbors(fake_neighbors, verts, neighbors);
  }

  for (const int i : verts.index_range()) {
    const int vert = verts[i];

    float max = 0.0f;
    for (const int neighbor : neighbors[i]) {
      max = std::max(max, prev_mask[neighbor]);
    }

    if (max > prev_mask[vert]) {
      const float3 &position = vert_positions[verts[i]];
      pose_factor[vert] = max;
      if (SCULPT_check_vertex_pivot_symmetry(position, pose_initial_position, symm)) {
        gftd.pos_avg += position;
        gftd.pos_count++;
      }
    }
  }
}

static void grow_factors_grids(const ePaintSymmetryFlags symm,
                               const float3 &pose_initial_position,
                               const SubdivCCG &subdiv_ccg,
                               const Span<int> fake_neighbors,
                               const Span<float> prev_mask,
                               const bke::pbvh::GridsNode &node,
                               const MutableSpan<float> pose_factor,
                               PoseGrowFactorData &gftd)
{
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
  const Span<CCGElem *> elems = subdiv_ccg.grids;
  const BitGroupVector<> &grid_hidden = subdiv_ccg.grid_hidden;
  const Span<int> grids = node.grids();

  for (const int i : grids.index_range()) {
    const int grid = grids[i];
    CCGElem *elem = elems[grid];
    const int start = key.grid_area * grid;
    for (const short y : IndexRange(key.grid_size)) {
      for (const short x : IndexRange(key.grid_size)) {
        const int offset = CCG_grid_xy_to_index(key.grid_size, x, y);
        if (!grid_hidden.is_empty() && grid_hidden[grid][offset]) {
          continue;
        }
        const int vert = start + offset;

        SubdivCCGNeighbors neighbors;
        BKE_subdiv_ccg_neighbor_coords_get(
            subdiv_ccg, SubdivCCGCoord{grids[i], x, y}, false, neighbors);

        float max = 0.0f;
        for (const SubdivCCGCoord neighbor : neighbors.coords) {
          max = std::max(max, prev_mask[neighbor.to_index(key)]);
        }
        if (!fake_neighbors.is_empty()) {
          if (fake_neighbors[vert] != FAKE_NEIGHBOR_NONE) {
            max = std::max(max, prev_mask[fake_neighbors[vert]]);
          }
        }

        if (max > prev_mask[vert]) {
          const float3 &position = CCG_elem_offset_co(key, elem, offset);
          pose_factor[vert] = max;
          if (SCULPT_check_vertex_pivot_symmetry(position, pose_initial_position, symm)) {
            gftd.pos_avg += position;
            gftd.pos_count++;
          }
        }
      }
    }
  }
}

static void grow_factors_bmesh(const ePaintSymmetryFlags symm,
                               const float3 &pose_initial_position,
                               const Span<int> fake_neighbors,
                               const Span<float> prev_mask,
                               bke::pbvh::BMeshNode &node,
                               const MutableSpan<float> pose_factor,
                               PoseGrowFactorData &gftd)
{
  const Set<BMVert *, 0> &verts = BKE_pbvh_bmesh_node_unique_verts(&node);

  Vector<BMVert *, 64> neighbors;

  for (BMVert *bm_vert : verts) {
    const int vert = BM_elem_index_get(bm_vert);

    float max = 0.0f;
    for (const BMVert *neighbor : vert_neighbors_get_bmesh(*bm_vert, neighbors)) {
      max = std::max(max, prev_mask[BM_elem_index_get(neighbor)]);
    }
    if (!fake_neighbors.is_empty()) {
      if (fake_neighbors[vert] != FAKE_NEIGHBOR_NONE) {
        max = std::max(max, prev_mask[fake_neighbors[vert]]);
      }
    }

    if (max > prev_mask[vert]) {
      const float3 &position = bm_vert->co;
      pose_factor[vert] = max;
      if (SCULPT_check_vertex_pivot_symmetry(position, pose_initial_position, symm)) {
        gftd.pos_avg += position;
        gftd.pos_count++;
      }
    }
  }
}

/* Grow the factor until its boundary is near to the offset pose origin or outside the target
 * distance. */
static void grow_pose_factor(const Depsgraph &depsgraph,
                             Object &ob,
                             SculptSession &ss,
                             float pose_origin[3],
                             float pose_target[3],
                             float max_len,
                             float *r_pose_origin,
                             MutableSpan<float> pose_factor)
{
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);
  const ePaintSymmetryFlags symm = SCULPT_mesh_symmetry_xyz_get(ob);

  IndexMaskMemory memory;
  const IndexMask node_mask = bke::pbvh::all_leaf_nodes(pbvh, memory);
  const Span<int> fake_neighbors = ss.fake_neighbors.fake_neighbor_index;

  bool grow_next_iteration = true;
  float prev_len = FLT_MAX;
  Array<float> prev_mask(SCULPT_vertex_count_get(ob));
  while (grow_next_iteration) {
    prev_mask.as_mutable_span().copy_from(pose_factor);

    PoseGrowFactorData gftd;
    threading::EnumerableThreadSpecific<GrowFactorLocalData> all_tls;
    switch (pbvh.type()) {
      case bke::pbvh::Type::Mesh: {
        MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
        const Mesh &mesh = *static_cast<const Mesh *>(ob.data);
        const Span<float3> vert_positions = bke::pbvh::vert_positions_eval(depsgraph, ob);
        const OffsetIndices faces = mesh.faces();
        const Span<int> corner_verts = mesh.corner_verts();
        const GroupedSpan<int> vert_to_face_map = mesh.vert_to_face_map();
        const bke::AttributeAccessor attributes = mesh.attributes();
        const VArraySpan<bool> hide_vert = *attributes.lookup<bool>(".hide_vert",
                                                                    bke::AttrDomain::Point);
        const VArraySpan<bool> hide_poly = *attributes.lookup<bool>(".hide_poly",
                                                                    bke::AttrDomain::Face);
        gftd = threading::parallel_reduce(
            node_mask.index_range(),
            1,
            PoseGrowFactorData{},
            [&](const IndexRange range, PoseGrowFactorData gftd) {
              GrowFactorLocalData &tls = all_tls.local();
              node_mask.slice(range).foreach_index([&](const int i) {
                grow_factors_mesh(symm,
                                  pose_target,
                                  vert_positions,
                                  faces,
                                  corner_verts,
                                  vert_to_face_map,
                                  hide_vert,
                                  hide_poly,
                                  fake_neighbors,
                                  prev_mask,
                                  nodes[i],
                                  tls,
                                  pose_factor,
                                  gftd);
              });
              return gftd;
            },
            PoseGrowFactorData::join);
        break;
      }
      case bke::pbvh::Type::Grids: {
        MutableSpan<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
        const SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
        gftd = threading::parallel_reduce(
            node_mask.index_range(),
            1,
            PoseGrowFactorData{},
            [&](const IndexRange range, PoseGrowFactorData gftd) {
              node_mask.slice(range).foreach_index([&](const int i) {
                grow_factors_grids(symm,
                                   pose_target,
                                   subdiv_ccg,
                                   fake_neighbors,
                                   prev_mask,
                                   nodes[i],
                                   pose_factor,
                                   gftd);
              });
              return gftd;
            },
            PoseGrowFactorData::join);
        break;
      }
      case bke::pbvh::Type::BMesh: {
        MutableSpan<bke::pbvh::BMeshNode> nodes = pbvh.nodes<bke::pbvh::BMeshNode>();
        gftd = threading::parallel_reduce(
            node_mask.index_range(),
            1,
            PoseGrowFactorData{},
            [&](const IndexRange range, PoseGrowFactorData gftd) {
              node_mask.slice(range).foreach_index([&](const int i) {
                grow_factors_bmesh(
                    symm, pose_target, fake_neighbors, prev_mask, nodes[i], pose_factor, gftd);
              });
              return gftd;
            },
            PoseGrowFactorData::join);
        break;
      }
    }

    if (gftd.pos_count != 0) {
      gftd.pos_avg /= float(gftd.pos_count);
      if (pose_origin) {
        /* Test with pose origin. Used when growing the factors to compensate the Origin Offset. */
        /* Stop when the factor's avg_pos starts moving away from the origin instead of getting
         * closer to it. */
        float len = math::distance(gftd.pos_avg, float3(pose_origin));
        if (len < prev_len) {
          prev_len = len;
          grow_next_iteration = true;
        }
        else {
          grow_next_iteration = false;
          pose_factor.copy_from(prev_mask);
        }
      }
      else {
        /* Test with length. Used to calculate the origin positions of the IK chain. */
        /* Stops when the factors have grown enough to generate a new segment origin. */
        float len = math::distance(gftd.pos_avg, float3(pose_target));
        if (len < max_len) {
          prev_len = len;
          grow_next_iteration = true;
        }
        else {
          grow_next_iteration = false;
          if (r_pose_origin) {
            copy_v3_v3(r_pose_origin, gftd.pos_avg);
          }
          pose_factor.copy_from(prev_mask);
        }
      }
    }
    else {
      if (r_pose_origin) {
        copy_v3_v3(r_pose_origin, pose_target);
      }
      grow_next_iteration = false;
    }
  }
}

static bool vert_inside_brush_radius(const float3 &vertex,
                                     const float3 &br_co,
                                     float radius,
                                     char symm)
{
  for (char i = 0; i <= symm; ++i) {
    if (SCULPT_is_symmetry_iteration_valid(i, symm)) {
      const float3 location = symmetry_flip(br_co, ePaintSymmetryFlags(i));
      if (math::distance(location, vertex) < radius) {
        return true;
      }
    }
  }
  return false;
}

/**
 * \param fallback_floodfill_origin: In topology mode this stores the furthest point from the
 * stroke origin for cases when a pose origin based on the brush radius can't be set.
 */
static bool topology_floodfill(const Depsgraph &depsgraph,
                               const Object &object,
                               const float3 &pose_initial_co,
                               const float radius,
                               const int symm,
                               const PBVHVertRef to_v,
                               const bool is_duplicate,
                               MutableSpan<float> pose_factor,
                               float3 &fallback_floodfill_origin,
                               float3 &pose_origin,
                               int &tot_co)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  int to_v_i = BKE_pbvh_vertex_to_index(pbvh, to_v);

  const float *co = SCULPT_vertex_co_get(depsgraph, object, to_v);

  if (!pose_factor.is_empty()) {
    pose_factor[to_v_i] = 1.0f;
  }

  if (len_squared_v3v3(pose_initial_co, fallback_floodfill_origin) <
      len_squared_v3v3(pose_initial_co, co))
  {
    copy_v3_v3(fallback_floodfill_origin, co);
  }

  if (vert_inside_brush_radius(co, pose_initial_co, radius, symm)) {
    return true;
  }
  if (SCULPT_check_vertex_pivot_symmetry(co, pose_initial_co, symm)) {
    if (!is_duplicate) {
      add_v3_v3(pose_origin, co);
      tot_co++;
    }
  }

  return false;
}

/**
 * \param fallback_origin: If we can't find any face set to continue, use the position of all
 * vertices that have the current face set.
 */
static bool face_sets_floodfill(const Depsgraph &depsgraph,
                                const Object &object,
                                const float3 &pose_initial_co,
                                const float radius,
                                const int symm,
                                const bool is_first_iteration,
                                const PBVHVertRef to_v,
                                const bool is_duplicate,
                                MutableSpan<float> pose_factor,
                                Set<int> &visited_face_sets,
                                MutableBoundedBitSpan is_weighted,
                                float3 &fallback_origin,
                                int &fallback_count,
                                int &current_face_set,
                                bool &next_face_set_found,
                                int &next_face_set,
                                PBVHVertRef &next_vertex,
                                float3 &pose_origin,
                                int &tot_co)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const int index = BKE_pbvh_vertex_to_index(pbvh, to_v);
  const PBVHVertRef vertex = to_v;
  bool visit_next = false;

  const float *co = SCULPT_vertex_co_get(depsgraph, object, vertex);
  const bool symmetry_check = SCULPT_check_vertex_pivot_symmetry(co, pose_initial_co, symm) &&
                              !is_duplicate;

  /* First iteration. Continue expanding using topology until a vertex is outside the brush radius
   * to determine the first face set. */
  if (current_face_set == SCULPT_FACE_SET_NONE) {

    pose_factor[index] = 1.0f;
    is_weighted[index].set();

    if (vert_inside_brush_radius(co, pose_initial_co, radius, symm)) {
      const int visited_face_set = face_set::vert_face_set_get(object, vertex);
      visited_face_sets.add(visited_face_set);
    }
    else if (symmetry_check) {
      current_face_set = face_set::vert_face_set_get(object, vertex);
      visited_face_sets.add(current_face_set);
    }
    return true;
  }

  /* We already have a current face set, so we can start checking the face sets of the vertices. */
  /* In the first iteration we need to check all face sets we already visited as the flood fill may
   * still not be finished in some of them. */
  bool is_vertex_valid = false;
  if (is_first_iteration) {
    for (const int visited_face_set : visited_face_sets) {
      is_vertex_valid |= face_set::vert_has_face_set(object, vertex, visited_face_set);
    }
  }
  else {
    is_vertex_valid = face_set::vert_has_face_set(object, vertex, current_face_set);
  }

  if (!is_vertex_valid) {
    return visit_next;
  }

  if (!is_weighted[index]) {
    pose_factor[index] = 1.0f;
    is_weighted[index].set();
    visit_next = true;
  }

  /* Fallback origin accumulation. */
  if (symmetry_check) {
    add_v3_v3(fallback_origin, SCULPT_vertex_co_get(depsgraph, object, vertex));
    fallback_count++;
  }

  if (!symmetry_check || face_set::vert_has_unique_face_set(object, vertex)) {
    return visit_next;
  }

  /* We only add coordinates for calculating the origin when it is possible to go from this
   * vertex to another vertex in a valid face set for the next iteration. */
  bool count_as_boundary = false;

  SculptVertexNeighborIter ni;
  SCULPT_VERTEX_NEIGHBORS_ITER_BEGIN (object, vertex, ni) {
    int next_face_set_candidate = face_set::vert_face_set_get(object, ni.vertex);

    /* Check if we can get a valid face set for the next iteration from this neighbor. */
    if (face_set::vert_has_unique_face_set(object, ni.vertex) &&
        !visited_face_sets.contains(next_face_set_candidate))
    {
      if (!next_face_set_found) {
        next_face_set = next_face_set_candidate;
        next_vertex = ni.vertex;
        next_face_set_found = true;
      }
      count_as_boundary = true;
    }
  }
  SCULPT_VERTEX_NEIGHBORS_ITER_END(ni);

  /* Origin accumulation. */
  if (count_as_boundary) {
    add_v3_v3(pose_origin, SCULPT_vertex_co_get(depsgraph, object, vertex));
    tot_co++;
  }
  return visit_next;
}

/* Public functions. */

void calc_pose_data(const Depsgraph &depsgraph,
                    Object &ob,
                    SculptSession &ss,
                    const float3 &initial_location,
                    float radius,
                    float pose_offset,
                    float3 &r_pose_origin,
                    MutableSpan<float> r_pose_factor)
{
  SCULPT_vertex_random_access_ensure(ob);

  /* Calculate the pose rotation point based on the boundaries of the brush factor. */
  flood_fill::FillData flood = flood_fill::init_fill(ob);
  flood_fill::add_initial_with_symmetry(
      depsgraph, ob, flood, ss.active_vert_ref(), !r_pose_factor.is_empty() ? radius : 0.0f);

  const int symm = SCULPT_mesh_symmetry_xyz_get(ob);

  int tot_co = 0;
  float3 pose_origin(0);
  float3 fallback_floodfill_origin = initial_location;
  flood_fill::execute(ob, flood, [&](PBVHVertRef /*from_v*/, PBVHVertRef to_v, bool is_duplicate) {
    return topology_floodfill(depsgraph,
                              ob,
                              initial_location,
                              radius,
                              symm,
                              to_v,
                              is_duplicate,
                              r_pose_factor,
                              fallback_floodfill_origin,
                              pose_origin,
                              tot_co);
  });

  if (tot_co > 0) {
    pose_origin /= float(tot_co);
  }
  else {
    pose_origin = fallback_floodfill_origin;
  }

  /* Offset the pose origin. */
  float3 pose_d = math::normalize(pose_origin - initial_location);
  pose_origin += pose_d * radius * pose_offset;
  r_pose_origin = pose_origin;

  /* Do the initial grow of the factors to get the first segment of the chain with Origin Offset.
   */
  if (pose_offset != 0.0f && !r_pose_factor.is_empty()) {
    grow_pose_factor(depsgraph, ob, ss, pose_origin, pose_origin, 0, nullptr, r_pose_factor);
  }
}

/* Init the IK chain with empty weights. */
static std::unique_ptr<IKChain> ik_chain_new(const int totsegments, const int totverts)
{
  std::unique_ptr<IKChain> ik_chain = std::make_unique<IKChain>();
  ik_chain->segments.reinitialize(totsegments);
  for (IKChainSegment &segment : ik_chain->segments) {
    segment.weights = Array<float>(totverts, 0.0f);
  }
  return ik_chain;
}

/* Init the origin/head pairs of all the segments from the calculated origins. */
static void ik_chain_origin_heads_init(IKChain &ik_chain, const float3 &initial_location)
{
  float3 origin;
  float3 head;
  for (const int i : ik_chain.segments.index_range()) {
    if (i == 0) {
      head = initial_location;
      origin = ik_chain.segments[i].orig;
    }
    else {
      head = ik_chain.segments[i - 1].orig;
      origin = ik_chain.segments[i].orig;
    }
    ik_chain.segments[i].orig = origin;
    ik_chain.segments[i].initial_orig = origin;
    ik_chain.segments[i].head = head;
    ik_chain.segments[i].initial_head = head;
    ik_chain.segments[i].len = math::distance(head, origin);
    ik_chain.segments[i].scale = float3(1.0f);
  }
}

static int brush_num_effective_segments(const Brush &brush)
{
  /* Scaling multiple segments at the same time is not supported as the IK solver can't handle
   * changes in the segment's length. It will also required a better weight distribution to avoid
   * artifacts in the areas affected by multiple segments. */
  if (ELEM(brush.pose_deform_type,
           BRUSH_POSE_DEFORM_SCALE_TRASLATE,
           BRUSH_POSE_DEFORM_SQUASH_STRETCH))
  {
    return 1;
  }
  return brush.pose_ik_segments;
}

static std::unique_ptr<IKChain> pose_ik_chain_init_topology(const Depsgraph &depsgraph,
                                                            Object &ob,
                                                            SculptSession &ss,
                                                            const Brush &brush,
                                                            const float3 &initial_location,
                                                            const float radius)
{

  const float chain_segment_len = radius * (1.0f + brush.pose_offset);
  float3 next_chain_segment_target;

  int totvert = SCULPT_vertex_count_get(ob);
  PBVHVertRef nearest_vertex = nearest_vert_calc(depsgraph, ob, initial_location, FLT_MAX, true);
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);
  int nearest_vertex_index = BKE_pbvh_vertex_to_index(pbvh, nearest_vertex);

  /* Init the buffers used to keep track of the changes in the pose factors as more segments are
   * added to the IK chain. */

  /* This stores the whole pose factors values as they grow through the mesh. */
  Array<float> pose_factor_grow(totvert, 0.0f);

  /* This stores the previous status of the factors when growing a new iteration. */
  Array<float> pose_factor_grow_prev(totvert, 0.0f);

  pose_factor_grow[nearest_vertex_index] = 1.0f;

  const int tot_segments = brush_num_effective_segments(brush);
  std::unique_ptr<IKChain> ik_chain = ik_chain_new(tot_segments, totvert);

  /* Calculate the first segment in the chain using the brush radius and the pose origin offset. */
  next_chain_segment_target = initial_location;
  calc_pose_data(depsgraph,
                 ob,
                 ss,
                 next_chain_segment_target,
                 radius,
                 brush.pose_offset,
                 ik_chain->segments[0].orig,
                 pose_factor_grow);

  next_chain_segment_target = ik_chain->segments[0].orig;

  /* Init the weights of this segment and store the status of the pose factors to start calculating
   * new segment origins. */
  for (int j = 0; j < totvert; j++) {
    ik_chain->segments[0].weights[j] = pose_factor_grow[j];
    pose_factor_grow_prev[j] = pose_factor_grow[j];
  }

  /* Calculate the next segments in the chain growing the pose factors. */
  for (const int i : ik_chain->segments.index_range().drop_front(1)) {

    /* Grow the factors to get the new segment origin. */
    grow_pose_factor(depsgraph,
                     ob,
                     ss,
                     nullptr,
                     next_chain_segment_target,
                     chain_segment_len,
                     ik_chain->segments[i].orig,
                     pose_factor_grow);
    next_chain_segment_target = ik_chain->segments[i].orig;

    /* Create the weights for this segment from the difference between the previous grow factor
     * iteration an the current iteration. */
    for (int j = 0; j < totvert; j++) {
      ik_chain->segments[i].weights[j] = pose_factor_grow[j] - pose_factor_grow_prev[j];
      /* Store the current grow factor status for the next iteration. */
      pose_factor_grow_prev[j] = pose_factor_grow[j];
    }
  }

  ik_chain_origin_heads_init(*ik_chain, initial_location);

  return ik_chain;
}

static std::unique_ptr<IKChain> ik_chain_init_face_sets(const Depsgraph &depsgraph,
                                                        Object &ob,
                                                        SculptSession &ss,
                                                        const Brush &brush,
                                                        const float radius)
{

  int totvert = SCULPT_vertex_count_get(ob);

  const int tot_segments = brush_num_effective_segments(brush);
  const int symm = SCULPT_mesh_symmetry_xyz_get(ob);

  std::unique_ptr<IKChain> ik_chain = ik_chain_new(tot_segments, totvert);

  Set<int> visited_face_sets;

  /* Each vertex can only be assigned to one face set. */
  BitVector<> is_weighted(totvert);

  int current_face_set = SCULPT_FACE_SET_NONE;

  PBVHVertRef current_vertex = ss.active_vert_ref();

  for (const int i : ik_chain->segments.index_range()) {
    const bool is_first_iteration = i == 0;

    flood_fill::FillData flood = flood_fill::init_fill(ob);
    flood_fill::add_initial_with_symmetry(depsgraph, ob, flood, current_vertex, FLT_MAX);

    visited_face_sets.add(current_face_set);

    MutableSpan<float> pose_factor = ik_chain->segments[i].weights;
    int tot_co = 0;
    bool next_face_set_found = false;
    int next_face_set = SCULPT_FACE_SET_NONE;
    PBVHVertRef next_vertex{};
    float3 pose_origin(0);
    float3 fallback_origin(0);
    int fallback_count = 0;

    const float3 pose_initial_co = SCULPT_vertex_co_get(depsgraph, ob, current_vertex);
    flood_fill::execute(
        ob, flood, [&](PBVHVertRef /*from_v*/, PBVHVertRef to_v, bool is_duplicate) {
          return face_sets_floodfill(depsgraph,
                                     ob,
                                     pose_initial_co,
                                     radius,
                                     symm,
                                     is_first_iteration,
                                     to_v,
                                     is_duplicate,
                                     pose_factor,
                                     visited_face_sets,
                                     is_weighted,
                                     fallback_origin,
                                     fallback_count,
                                     current_face_set,
                                     next_face_set_found,
                                     next_face_set,
                                     next_vertex,
                                     pose_origin,
                                     tot_co);
        });

    if (tot_co > 0) {
      ik_chain->segments[i].orig = pose_origin / float(tot_co);
    }
    else if (fallback_count > 0) {
      ik_chain->segments[i].orig = fallback_origin / float(fallback_count);
    }
    else {
      ik_chain->segments[i].orig = float3(0);
    }

    current_face_set = next_face_set;
    current_vertex = next_vertex;
  }

  ik_chain_origin_heads_init(*ik_chain, SCULPT_vertex_co_get(depsgraph, ob, ss.active_vert_ref()));

  return ik_chain;
}

static bool face_sets_fk_find_masked_floodfill(const Object &object,
                                               const int initial_face_set,
                                               const PBVHVertRef from_v,
                                               const PBVHVertRef to_v,
                                               const bool is_duplicate,
                                               Set<int> &visited_face_sets,
                                               MutableSpan<int> floodfill_it,
                                               int &masked_face_set_it,
                                               int &masked_face_set,
                                               int &target_face_set)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  int from_v_i = BKE_pbvh_vertex_to_index(pbvh, from_v);
  int to_v_i = BKE_pbvh_vertex_to_index(pbvh, to_v);

  if (!is_duplicate) {
    floodfill_it[to_v_i] = floodfill_it[from_v_i] + 1;
  }
  else {
    floodfill_it[to_v_i] = floodfill_it[from_v_i];
  }

  const int to_face_set = face_set::vert_face_set_get(object, to_v);
  if (!visited_face_sets.contains(to_face_set)) {
    if (face_set::vert_has_unique_face_set(object, to_v) &&
        !face_set::vert_has_unique_face_set(object, from_v) &&
        face_set::vert_has_face_set(object, from_v, to_face_set))
    {

      visited_face_sets.add(to_face_set);

      if (floodfill_it[to_v_i] >= masked_face_set_it) {
        masked_face_set = to_face_set;
        masked_face_set_it = floodfill_it[to_v_i];
      }

      if (target_face_set == SCULPT_FACE_SET_NONE) {
        target_face_set = to_face_set;
      }
    }
  }

  return face_set::vert_has_face_set(object, to_v, initial_face_set);
}

static bool pose_face_sets_fk_set_weights_floodfill(const Object &object,
                                                    const PBVHVertRef to_v,
                                                    const int masked_face_set,
                                                    MutableSpan<float> fk_weights)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  int to_v_i = BKE_pbvh_vertex_to_index(pbvh, to_v);

  fk_weights[to_v_i] = 1.0f;
  return !face_set::vert_has_face_set(object, to_v, masked_face_set);
}

static std::unique_ptr<IKChain> ik_chain_init_face_sets_fk(const Depsgraph &depsgraph,
                                                           Object &ob,
                                                           SculptSession &ss,
                                                           const float radius,
                                                           const float3 &initial_location)
{
  const int totvert = SCULPT_vertex_count_get(ob);

  std::unique_ptr<IKChain> ik_chain = ik_chain_new(1, totvert);

  const PBVHVertRef active_vertex = ss.active_vert_ref();
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);
  int active_vertex_index = BKE_pbvh_vertex_to_index(pbvh, active_vertex);

  const int active_face_set = face_set::active_face_set_get(ob);

  Set<int> visited_face_sets;
  Array<int> floodfill_it(totvert);
  floodfill_it[active_vertex_index] = 1;

  int masked_face_set = SCULPT_FACE_SET_NONE;
  int target_face_set = SCULPT_FACE_SET_NONE;
  {
    int masked_face_set_it = 0;
    flood_fill::FillData flood = flood_fill::init_fill(ob);
    flood_fill::add_initial(flood, active_vertex);
    flood_fill::execute(ob, flood, [&](PBVHVertRef from_v, PBVHVertRef to_v, bool is_duplicate) {
      return face_sets_fk_find_masked_floodfill(ob,
                                                active_face_set,
                                                from_v,
                                                to_v,
                                                is_duplicate,
                                                visited_face_sets,
                                                floodfill_it,
                                                masked_face_set_it,
                                                masked_face_set,
                                                target_face_set);
    });
  }

  int origin_count = 0;
  float3 origin_acc(0.0f);
  for (int i = 0; i < totvert; i++) {
    PBVHVertRef vertex = BKE_pbvh_index_to_vertex(ob, i);

    if (floodfill_it[i] != 0 && face_set::vert_has_face_set(ob, vertex, active_face_set) &&
        face_set::vert_has_face_set(ob, vertex, masked_face_set))
    {
      origin_acc += SCULPT_vertex_co_get(depsgraph, ob, vertex);
      origin_count++;
    }
  }

  int target_count = 0;
  float3 target_acc(0.0f);
  if (target_face_set != masked_face_set) {
    for (int i = 0; i < totvert; i++) {
      PBVHVertRef vertex = BKE_pbvh_index_to_vertex(ob, i);

      if (floodfill_it[i] != 0 && face_set::vert_has_face_set(ob, vertex, active_face_set) &&
          face_set::vert_has_face_set(ob, vertex, target_face_set))
      {
        target_acc += SCULPT_vertex_co_get(depsgraph, ob, vertex);
        target_count++;
      }
    }
  }

  if (origin_count > 0) {
    ik_chain->segments[0].orig = origin_acc / float(origin_count);
  }
  else {
    ik_chain->segments[0].orig = float3(0);
  }

  if (target_count > 0) {
    ik_chain->segments[0].head = target_acc / target_count;
    ik_chain->grab_delta_offset = ik_chain->segments[0].head - initial_location;
  }
  else {
    ik_chain->segments[0].head = initial_location;
  }

  {
    flood_fill::FillData flood = flood_fill::init_fill(ob);
    flood_fill::add_initial_with_symmetry(depsgraph, ob, flood, ss.active_vert_ref(), radius);
    MutableSpan<float> fk_weights = ik_chain->segments[0].weights;
    flood_fill::execute(
        ob, flood, [&](PBVHVertRef /*from_v*/, PBVHVertRef to_v, bool /*is_duplicate*/) {
          return pose_face_sets_fk_set_weights_floodfill(ob, to_v, masked_face_set, fk_weights);
        });
  }

  ik_chain_origin_heads_init(*ik_chain, ik_chain->segments[0].head);
  return ik_chain;
}

static std::unique_ptr<IKChain> ik_chain_init(const Depsgraph &depsgraph,
                                              Object &ob,
                                              SculptSession &ss,
                                              const Brush &brush,
                                              const float3 &initial_location,
                                              const float radius)
{
  std::unique_ptr<IKChain> ik_chain;

  const bool use_fake_neighbors = !(brush.flag2 & BRUSH_USE_CONNECTED_ONLY);

  if (use_fake_neighbors) {
    SCULPT_fake_neighbors_ensure(depsgraph, ob, brush.disconnected_distance_max);
    SCULPT_fake_neighbors_enable(ob);
  }

  switch (brush.pose_origin_type) {
    case BRUSH_POSE_ORIGIN_TOPOLOGY:
      ik_chain = pose_ik_chain_init_topology(depsgraph, ob, ss, brush, initial_location, radius);
      break;
    case BRUSH_POSE_ORIGIN_FACE_SETS:
      ik_chain = ik_chain_init_face_sets(depsgraph, ob, ss, brush, radius);
      break;
    case BRUSH_POSE_ORIGIN_FACE_SETS_FK:
      ik_chain = ik_chain_init_face_sets_fk(depsgraph, ob, ss, radius, initial_location);
      break;
  }

  if (use_fake_neighbors) {
    SCULPT_fake_neighbors_disable(ob);
  }

  return ik_chain;
}

void pose_brush_init(const Depsgraph &depsgraph, Object &ob, SculptSession &ss, const Brush &brush)
{
  /* Init the IK chain that is going to be used to deform the vertices. */
  ss.cache->pose_ik_chain = ik_chain_init(
      depsgraph, ob, ss, brush, ss.cache->location, ss.cache->radius);

  /* Smooth the weights of each segment for cleaner deformation. */
  for (IKChainSegment &segment : ss.cache->pose_ik_chain->segments) {
    smooth::blur_geometry_data_array(ob, brush.pose_smooth_iterations, segment.weights);
  }
}

std::unique_ptr<SculptPoseIKChainPreview> preview_ik_chain_init(const Depsgraph &depsgraph,
                                                                Object &ob,
                                                                SculptSession &ss,
                                                                const Brush &brush,
                                                                const float3 &initial_location,
                                                                const float radius)
{
  const IKChain chain = *ik_chain_init(depsgraph, ob, ss, brush, initial_location, radius);
  std::unique_ptr<SculptPoseIKChainPreview> preview = std::make_unique<SculptPoseIKChainPreview>();

  preview->initial_head_coords.reinitialize(chain.segments.size());
  preview->initial_orig_coords.reinitialize(chain.segments.size());
  for (const int i : chain.segments.index_range()) {
    preview->initial_head_coords[i] = chain.segments[i].initial_head;
    preview->initial_orig_coords[i] = chain.segments[i].initial_orig;
  }

  return preview;
}

static void sculpt_pose_do_translate_deform(SculptSession &ss, const Brush &brush)
{
  IKChain &ik_chain = *ss.cache->pose_ik_chain;
  BKE_curvemapping_init(brush.curve);
  solve_translate_chain(ik_chain, ss.cache->grab_delta);
}

/* Calculate a scale factor based on the grab delta. */
static float calc_scale_from_grab_delta(SculptSession &ss, const float3 &ik_target)
{
  IKChain &ik_chain = *ss.cache->pose_ik_chain;
  const float3 segment_dir = math::normalize(ik_chain.segments[0].initial_head -
                                             ik_chain.segments[0].initial_orig);
  float4 plane;
  plane_from_point_normal_v3(plane, ik_chain.segments[0].initial_head, segment_dir);
  const float segment_len = ik_chain.segments[0].len;
  return segment_len / (segment_len - dist_signed_to_plane_v3(ik_target, plane));
}

static void calc_scale_deform(SculptSession &ss, const Brush &brush)
{
  IKChain &ik_chain = *ss.cache->pose_ik_chain;

  float3 ik_target = ss.cache->location + ss.cache->grab_delta;

  /* Solve the IK for the first segment to include rotation as part of scale if enabled. */
  if (!(brush.flag2 & BRUSH_POSE_USE_LOCK_ROTATION)) {
    solve_ik_chain(ik_chain, ik_target, brush.flag2 & BRUSH_POSE_IK_ANCHORED);
  }

  float3 scale(calc_scale_from_grab_delta(ss, ik_target));

  /* Write the scale into the segments. */
  solve_scale_chain(ik_chain, scale);
}

static void calc_twist_deform(SculptSession &ss, const Brush &brush)
{
  IKChain &ik_chain = *ss.cache->pose_ik_chain;

  /* Calculate the maximum roll. 0.02 radians per pixel works fine. */
  float roll = (ss.cache->initial_mouse[0] - ss.cache->mouse[0]) * ss.cache->bstrength * 0.02f;
  BKE_curvemapping_init(brush.curve);
  solve_roll_chain(ik_chain, brush, roll);
}

static void calc_rotate_deform(SculptSession &ss, const Brush &brush)
{
  IKChain &ik_chain = *ss.cache->pose_ik_chain;

  /* Calculate the IK target. */
  float3 ik_target = ss.cache->location + ss.cache->grab_delta + ik_chain.grab_delta_offset;

  /* Solve the IK positions. */
  solve_ik_chain(ik_chain, ik_target, brush.flag2 & BRUSH_POSE_IK_ANCHORED);
}

static void calc_rotate_twist_deform(SculptSession &ss, const Brush &brush)
{
  if (ss.cache->invert) {
    calc_twist_deform(ss, brush);
  }
  else {
    calc_rotate_deform(ss, brush);
  }
}

static void calc_scale_translate_deform(SculptSession &ss, const Brush &brush)
{
  if (ss.cache->invert) {
    sculpt_pose_do_translate_deform(ss, brush);
  }
  else {
    calc_scale_deform(ss, brush);
  }
}

static void calc_squash_stretch_deform(SculptSession &ss, const Brush & /*brush*/)
{
  IKChain &ik_chain = *ss.cache->pose_ik_chain;

  float3 ik_target = ss.cache->location + ss.cache->grab_delta;

  float3 scale;
  scale[2] = calc_scale_from_grab_delta(ss, ik_target);
  scale[0] = scale[1] = sqrtf(1.0f / scale[2]);

  /* Write the scale into the segments. */
  solve_scale_chain(ik_chain, scale);
}

static void align_pivot_local_space(float r_mat[4][4],
                                    ePaintSymmetryFlags symm,
                                    ePaintSymmetryAreas symm_area,
                                    IKChainSegment *segment,
                                    const float3 &grab_location)
{
  const float3 symm_head = SCULPT_flip_v3_by_symm_area(
      segment->head, symm, symm_area, grab_location);
  const float3 symm_orig = SCULPT_flip_v3_by_symm_area(
      segment->orig, symm, symm_area, grab_location);

  float3 segment_origin_head = math::normalize(symm_head - symm_orig);

  copy_v3_v3(r_mat[2], segment_origin_head);
  ortho_basis_v3v3_v3(r_mat[0], r_mat[1], r_mat[2]);
}

void do_pose_brush(const Depsgraph &depsgraph,
                   const Sculpt &sd,
                   Object &ob,
                   const IndexMask &node_mask)
{
  SculptSession &ss = *ob.sculpt;
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);
  const Brush &brush = *BKE_paint_brush_for_read(&sd.paint);
  const ePaintSymmetryFlags symm = SCULPT_mesh_symmetry_xyz_get(ob);

  /* The pose brush applies all enabled symmetry axis in a single iteration, so the rest can be
   * ignored. */
  if (ss.cache->mirror_symmetry_pass != 0) {
    return;
  }

  IKChain &ik_chain = *ss.cache->pose_ik_chain;

  switch (brush.pose_deform_type) {
    case BRUSH_POSE_DEFORM_ROTATE_TWIST:
      calc_rotate_twist_deform(ss, brush);
      break;
    case BRUSH_POSE_DEFORM_SCALE_TRASLATE:
      calc_scale_translate_deform(ss, brush);
      break;
    case BRUSH_POSE_DEFORM_SQUASH_STRETCH:
      calc_squash_stretch_deform(ss, brush);
      break;
  }

  /* Flip the segment chain in all symmetry axis and calculate the transform matrices for each
   * possible combination. */
  /* This can be optimized by skipping the calculation of matrices where the symmetry is not
   * enabled. */
  for (int symm_it = 0; symm_it < PAINT_SYMM_AREAS; symm_it++) {
    for (const int i : ik_chain.segments.index_range()) {
      ePaintSymmetryAreas symm_area = ePaintSymmetryAreas(symm_it);

      float symm_rot[4];
      copy_qt_qt(symm_rot, ik_chain.segments[i].rot);

      /* Flip the origins and rotation quats of each segment. */
      SCULPT_flip_quat_by_symm_area(symm_rot, symm, symm_area, ss.cache->orig_grab_location);
      float3 symm_orig = SCULPT_flip_v3_by_symm_area(
          ik_chain.segments[i].orig, symm, symm_area, ss.cache->orig_grab_location);
      float3 symm_initial_orig = SCULPT_flip_v3_by_symm_area(
          ik_chain.segments[i].initial_orig, symm, symm_area, ss.cache->orig_grab_location);

      float pivot_local_space[4][4];
      unit_m4(pivot_local_space);

      /* Align the segment pivot local space to the Z axis. */
      if (brush.pose_deform_type == BRUSH_POSE_DEFORM_SQUASH_STRETCH) {
        align_pivot_local_space(pivot_local_space,
                                symm,
                                symm_area,
                                &ik_chain.segments[i],
                                ss.cache->orig_grab_location);
        unit_m4(ik_chain.segments[i].trans_mat[symm_it].ptr());
      }
      else {
        quat_to_mat4(ik_chain.segments[i].trans_mat[symm_it].ptr(), symm_rot);
      }

      /* Apply segment scale to the transform. */
      for (int scale_i = 0; scale_i < 3; scale_i++) {
        mul_v3_fl(ik_chain.segments[i].trans_mat[symm_it][scale_i],
                  ik_chain.segments[i].scale[scale_i]);
      }

      translate_m4(ik_chain.segments[i].trans_mat[symm_it].ptr(),
                   symm_orig[0] - symm_initial_orig[0],
                   symm_orig[1] - symm_initial_orig[1],
                   symm_orig[2] - symm_initial_orig[2]);

      unit_m4(ik_chain.segments[i].pivot_mat[symm_it].ptr());
      translate_m4(
          ik_chain.segments[i].pivot_mat[symm_it].ptr(), symm_orig[0], symm_orig[1], symm_orig[2]);
      mul_m4_m4_post(ik_chain.segments[i].pivot_mat[symm_it].ptr(), pivot_local_space);

      invert_m4_m4(ik_chain.segments[i].pivot_mat_inv[symm_it].ptr(),
                   ik_chain.segments[i].pivot_mat[symm_it].ptr());
    }
  }

  threading::EnumerableThreadSpecific<BrushLocalData> all_tls;
  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh: {
      MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
      Mesh &mesh = *static_cast<Mesh *>(ob.data);
      const Span<float3> positions_eval = bke::pbvh::vert_positions_eval(depsgraph, ob);
      MutableSpan<float3> positions_orig = mesh.vert_positions_for_write();
      threading::parallel_for(node_mask.index_range(), 1, [&](const IndexRange range) {
        BrushLocalData &tls = all_tls.local();
        node_mask.slice(range).foreach_index([&](const int i) {
          calc_mesh(depsgraph, sd, brush, positions_eval, nodes[i], ob, tls, positions_orig);
          BKE_pbvh_node_mark_positions_update(nodes[i]);
        });
      });
      break;
    }
    case bke::pbvh::Type::Grids: {
      MutableSpan<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
      threading::parallel_for(node_mask.index_range(), 1, [&](const IndexRange range) {
        BrushLocalData &tls = all_tls.local();
        node_mask.slice(range).foreach_index(
            [&](const int i) { calc_grids(depsgraph, sd, brush, nodes[i], ob, tls); });
      });
      break;
    }
    case bke::pbvh::Type::BMesh: {
      MutableSpan<bke::pbvh::BMeshNode> nodes = pbvh.nodes<bke::pbvh::BMeshNode>();
      threading::parallel_for(node_mask.index_range(), 1, [&](const IndexRange range) {
        BrushLocalData &tls = all_tls.local();
        node_mask.slice(range).foreach_index(
            [&](const int i) { calc_bmesh(depsgraph, sd, brush, nodes[i], ob, tls); });
      });
      break;
    }
  }
}

}  // namespace blender::ed::sculpt_paint::pose
