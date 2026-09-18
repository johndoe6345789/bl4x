#pragma once
// Walkable-surface coverage for a bake: which ground cells have an
// upward-facing surface over them (terrain, or any other mesh). Fed a
// flat instance list by D:\BL4Export\coverage_prep.py; the hole finding
// and the picture happen back in Python (coverage_post.py). Here only
// because rasterizing a few hundred million triangles in Python took
// hours.
#include <string>
#include <vector>

namespace bl4 {

// args: instances.bin models.txt bake_dir cell x0 z0 width height out.raw
// out.raw: width*height bytes, bit 0 = terrain, bit 1 = other surface.
int run_coverage_raster(const std::vector<std::string>& args);

}  // namespace bl4
