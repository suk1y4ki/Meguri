#pragma once

#include <string>

namespace meguri::io {

enum class ComfyMetadataKind {
    None,
    Workflow,
    Prompt,
};

struct ComfyMetadata {
    ComfyMetadataKind kind = ComfyMetadataKind::None;
    std::string json;
};

// Extracts ComfyUI metadata embedded in media files.
// For PNG, ComfyUI stores "workflow" and "prompt" text chunks. The UI workflow
// is preferred because ComfyUI itself prioritizes it when loading generated PNGs.
ComfyMetadata extract_comfy_metadata(const std::wstring& path);

}  // namespace meguri::io
