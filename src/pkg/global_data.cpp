#include "pkg/global_data.hpp"

#include "core/reader.hpp"
#include "io/iostore.hpp"

namespace bl4 {

GlobalData::GlobalData(IoStore& global_container) {
    ChunkId names_id = ChunkId::make(0, 0, ChunkType::ScriptObjects);
    int64_t idx = global_container.find(names_id);
    if (idx < 0) throw std::runtime_error("global.utoc has no ScriptObjects chunk");
    auto data = global_container.read(static_cast<uint32_t>(idx));
    Reader r(data);
    names_ = load_name_batch(r);

    int32_t count = r.read<int32_t>();
    objects_.reserve(static_cast<size_t>(count) * 2);
    for (int32_t i = 0; i < count; ++i) {
        ScriptObjectEntry e;
        e.object_name = read_mapped_name(r);
        e.global_index.raw = r.read<uint64_t>();
        e.outer_index.raw = r.read<uint64_t>();
        e.cdo_class_index.raw = r.read<uint64_t>();
        objects_[e.global_index.raw] = e;
    }
}

const ScriptObjectEntry* GlobalData::find(ObjIndex idx) const {
    auto it = objects_.find(idx.raw);
    return it == objects_.end() ? nullptr : &it->second;
}

std::string GlobalData::full_path(ObjIndex idx) const {
    std::string path;
    int guard = 0;
    while (!idx.is_null() && guard++ < 64) {
        const ScriptObjectEntry* e = find(idx);
        if (!e) break;
        std::string seg = resolve(e->object_name);
        path = path.empty() ? seg : seg + "." + path;
        idx = e->outer_index;
    }
    return path;
}

}  // namespace bl4
