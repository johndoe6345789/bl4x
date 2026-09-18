#pragma once
// A baked mesh in the exact form the SDL3CPlusPlus bl4 package uploads,
// so loading is a read rather than a parse: assimp chewing through 22 GB
// of OBJ text was what made streaming the full map slow.
//
// Layout (little endian, the only target):
//   char[4] "BL4M", uint32 version(1), uint32 submesh_count
//   float[6] bounds (min xyz, max xyz) over every submesh
//   per submesh:
//     uint32 vertex_count, index_count, texture_path_length
//     char[texture_path_length] texture path, relative to the mesh file
//     float[10 * vertex_count] x,y,z, u,v, lm_u,lm_v, nx,ny,nz
//     uint32[index_count]
#include <string>
#include <vector>

namespace bl4 {

struct MeshData;

// texture_files is parallel to mesh.material_paths ("" = no map), the
// same list write_obj's .mtl gets.
void write_binary_mesh(const MeshData& mesh, const std::string& path,
                       const std::vector<std::string>& texture_files);

// Converts one already-baked OBJ (+ its .mtl) into the binary form,
// for a bake made before this format existed. Returns false if the OBJ
// can't be read.
bool convert_obj_to_binary(const std::string& obj_path, const std::string& out_path);

}  // namespace bl4
