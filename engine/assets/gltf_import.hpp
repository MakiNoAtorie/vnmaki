#pragma once

// The glTF importer (docs/SPEC.md sections 9 and 19) only checks the file: it leaves the file as it
// is. It uses cgltf (cgltf_impl.cpp has the implementation of the library).

#include <string>
#include <string_view>
#include <vector>

#include "engine/vfs/vfs.hpp"

namespace vnmaki {

/// The problems of the glTF 2.0 file (`.gltf` or `.glb`) at `path`, one message for each, in the
/// form "<path>: <problem>". It is empty when the file is valid. The problems:
///   - the file cannot be read or is not glTF 2.0 (cgltf's parser);
///   - an external buffer or image (`uri` that is not a `data:` URI) that is not in the Vfs. The
///     uri is relative to the folder of the file;
///   - what `cgltf_validate` finds (a bad index, a range outside of a buffer, ...), and a buffer
///     that is shorter than the file says.
/// The last check needs the buffers, so it only runs when none is missing.
[[nodiscard]] std::vector<std::string> validate_gltf(const Vfs& vfs, std::string_view path);

}  // namespace vnmaki
