#pragma once
// Geometry UE builds at runtime that the bake has to build itself:
//
// - Spline meshes: a short mesh bent along a cubic Hermite spline, as
//   USplineMeshComponent::CalcSliceTransform does -- each vertex's
//   position along the forward axis picks a point on the spline, and its
//   cross-section is placed in the spline's frame there (offset, roll and
//   scale interpolated between the ends). Roads, pipes and rivers.
// - Lakes: WaterBodyLakeComponent has no mesh in cooked data; its
//   surface is the polygon of its spline outline, triangulated here.
//
// Both work in UE space (centimetres, Z up) on the component's local
// coordinates, before the bake's to_engine_space conversion.
#include <vector>

#include "pkg/mesh.hpp"

namespace bl4 {

// params: the walker's kSplineParamCount floats (StartPos, StartTangent,
// EndPos, EndTangent, StartScale.xy, EndScale.xy, StartOffset.xy,
// EndOffset.xy, StartRoll, EndRoll, ForwardAxis, SplineUpDir,
// SplineBoundaryMin, SplineBoundaryMax, bSmoothInterpRollScale).
void deform_spline_mesh(MeshData& mesh, const std::vector<float>& params);

// outline: xyz per point, a closed loop in the lake's own space. Returns
// an empty mesh if the outline cannot be triangulated.
MeshData triangulate_lake(const std::vector<float>& outline);

}  // namespace bl4
