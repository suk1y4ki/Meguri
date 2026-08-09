#include "comfy_metadata.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <map>
#include <vector>

#include "nlohmann/json.hpp"

namespace meguri::io {

namespace {

bool read_exact(std::ifstream& file, char* out, std::streamsize size) {
    file.read(out, size);
    return file.gcount() == size;
}

bool read_u32_be(std::ifstream& file, uint32_t* out) {
    unsigned char bytes[4]{};
    if (!read_exact(file, reinterpret_cast<char*>(bytes), 4)) return false;
    *out = (static_cast<uint32_t>(bytes[0]) << 24) |
           (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) |
           static_cast<uint32_t>(bytes[3]);
    return true;
}

uint16_t read_u16_mem(const std::vector<char>& data, size_t offset, bool little) {
    if (offset + 2 > data.size()) return 0;
    const auto b0 = static_cast<unsigned char>(data[offset]);
    const auto b1 = static_cast<unsigned char>(data[offset + 1]);
    return little ? static_cast<uint16_t>(b0 | (b1 << 8))
                  : static_cast<uint16_t>((b0 << 8) | b1);
}

uint32_t read_u32_mem(const std::vector<char>& data, size_t offset, bool little) {
    if (offset + 4 > data.size()) return 0;
    const auto b0 = static_cast<unsigned char>(data[offset]);
    const auto b1 = static_cast<unsigned char>(data[offset + 1]);
    const auto b2 = static_cast<unsigned char>(data[offset + 2]);
    const auto b3 = static_cast<unsigned char>(data[offset + 3]);
    return little ? (static_cast<uint32_t>(b0) | (static_cast<uint32_t>(b1) << 8) |
                     (static_cast<uint32_t>(b2) << 16) | (static_cast<uint32_t>(b3) << 24))
                  : ((static_cast<uint32_t>(b0) << 24) | (static_cast<uint32_t>(b1) << 16) |
                     (static_cast<uint32_t>(b2) << 8) | static_cast<uint32_t>(b3));
}

bool read_u64_be(std::ifstream& file, uint64_t* out) {
    unsigned char bytes[8]{};
    if (!read_exact(file, reinterpret_cast<char*>(bytes), 8)) return false;
    uint64_t value = 0;
    for (unsigned char byte : bytes) {
        value = (value << 8) | byte;
    }
    *out = value;
    return true;
}

std::string json_value_to_text(const nlohmann::json& value) {
    if (value.is_string()) return value.get<std::string>();
    return value.dump();
}

ComfyMetadata metadata_from_combined_json(const std::string& text) {
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return {};
    if (j.contains("workflow")) {
        return {ComfyMetadataKind::Workflow, json_value_to_text(j["workflow"])};
    }
    if (j.contains("Workflow")) {
        return {ComfyMetadataKind::Workflow, json_value_to_text(j["Workflow"])};
    }
    if (j.contains("prompt")) {
        return {ComfyMetadataKind::Prompt, json_value_to_text(j["prompt"])};
    }
    if (j.contains("Prompt")) {
        return {ComfyMetadataKind::Prompt, json_value_to_text(j["Prompt"])};
    }
    return {};
}

void capture_text_chunk(const std::vector<char>& data, std::string* workflow,
                        std::string* prompt) {
    auto nul = std::find(data.begin(), data.end(), '\0');
    if (nul == data.end()) return;
    const std::string key(data.begin(), nul);
    const std::string text(nul + 1, data.end());
    if (key == "workflow") {
        *workflow = text;
    } else if (key == "prompt") {
        *prompt = text;
    }
}

void capture_itxt_chunk(const std::vector<char>& data, std::string* workflow,
                        std::string* prompt) {
    size_t pos = 0;
    auto next_nul = [&](size_t from) -> size_t {
        while (from < data.size() && data[from] != '\0') ++from;
        return from;
    };

    const size_t key_end = next_nul(pos);
    if (key_end >= data.size()) return;
    const std::string key(data.begin(), data.begin() + key_end);
    pos = key_end + 1;
    if (pos + 2 > data.size()) return;

    const unsigned char compression_flag = static_cast<unsigned char>(data[pos++]);
    ++pos;  // compression method
    if (compression_flag != 0) return;

    const size_t language_end = next_nul(pos);
    if (language_end >= data.size()) return;
    pos = language_end + 1;
    const size_t translated_end = next_nul(pos);
    if (translated_end >= data.size()) return;
    pos = translated_end + 1;

    const std::string text(data.begin() + static_cast<std::ptrdiff_t>(pos), data.end());
    if (key == "workflow") {
        *workflow = text;
    } else if (key == "prompt") {
        *prompt = text;
    }
}

ComfyMetadata extract_png_metadata(const std::wstring& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};

    constexpr unsigned char kPngSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    unsigned char signature[8]{};
    if (!read_exact(file, reinterpret_cast<char*>(signature), 8)) return {};
    for (int i = 0; i < 8; ++i) {
        if (signature[i] != kPngSignature[i]) return {};
    }

    std::string workflow;
    std::string prompt;
    constexpr uint32_t kMaxTextChunk = 64u * 1024u * 1024u;

    while (file) {
        uint32_t length = 0;
        if (!read_u32_be(file, &length)) break;

        char type_chars[4]{};
        if (!read_exact(file, type_chars, 4)) break;
        const std::string type(type_chars, type_chars + 4);

        if (type == "tEXt") {
            if (length > kMaxTextChunk) return {};
            std::vector<char> data(length);
            if (length > 0 && !read_exact(file, data.data(), length)) break;
            capture_text_chunk(data, &workflow, &prompt);
        } else if (type == "iTXt") {
            if (length > kMaxTextChunk) return {};
            std::vector<char> data(length);
            if (length > 0 && !read_exact(file, data.data(), length)) break;
            capture_itxt_chunk(data, &workflow, &prompt);
        } else if (type == "IEND") {
            break;
        } else {
            file.seekg(length, std::ios::cur);
            if (!file) break;
        }

        char crc[4]{};
        if (!read_exact(file, crc, 4)) break;
    }

    if (!workflow.empty()) return {ComfyMetadataKind::Workflow, workflow};
    if (!prompt.empty()) return {ComfyMetadataKind::Prompt, prompt};
    return {};
}

void capture_colon_metadata(const std::string& text, std::string* workflow,
                            std::string* prompt) {
    const size_t colon = text.find(':');
    if (colon == std::string::npos) return;
    const std::string key = text.substr(0, colon);
    const std::string value = text.substr(colon + 1);
    if (key == "workflow" || key == "Workflow") {
        *workflow = value;
    } else if (key == "prompt" || key == "Prompt") {
        *prompt = value;
    }
}

void capture_webp_exif(const std::vector<char>& exif, std::string* workflow,
                       std::string* prompt) {
    if (exif.size() < 8) return;
    const bool little = exif[0] == 'I' && exif[1] == 'I';
    const bool big = exif[0] == 'M' && exif[1] == 'M';
    if (!little && !big) return;
    if (read_u16_mem(exif, 2, little) != 42) return;

    const uint32_t ifd_offset = read_u32_mem(exif, 4, little);
    if (ifd_offset + 2 > exif.size()) return;
    const uint16_t count = read_u16_mem(exif, ifd_offset, little);

    for (uint16_t i = 0; i < count; ++i) {
        const size_t entry = static_cast<size_t>(ifd_offset) + 2 + static_cast<size_t>(i) * 12;
        if (entry + 12 > exif.size()) break;
        const uint16_t type = read_u16_mem(exif, entry + 2, little);
        const uint32_t values = read_u32_mem(exif, entry + 4, little);
        const uint32_t value_offset = read_u32_mem(exif, entry + 8, little);
        if (type != 2 || values == 0) continue;

        const size_t start = values <= 4 ? entry + 8 : static_cast<size_t>(value_offset);
        if (start >= exif.size()) continue;
        const size_t available = exif.size() - start;
        const size_t length = std::min<size_t>(values, available);
        std::string text(exif.begin() + static_cast<std::ptrdiff_t>(start),
                         exif.begin() + static_cast<std::ptrdiff_t>(start + length));
        while (!text.empty() && text.back() == '\0') text.pop_back();
        capture_colon_metadata(text, workflow, prompt);
    }
}

ComfyMetadata extract_webp_metadata(const std::wstring& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};

    char header[12]{};
    if (!read_exact(file, header, 12)) return {};
    if (std::string(header, header + 4) != "RIFF" || std::string(header + 8, header + 12) != "WEBP") {
        return {};
    }

    std::string workflow;
    std::string prompt;
    constexpr uint32_t kMaxExifChunk = 64u * 1024u * 1024u;
    while (file) {
        char type[4]{};
        if (!read_exact(file, type, 4)) break;
        uint32_t length = 0;
        unsigned char len_bytes[4]{};
        if (!read_exact(file, reinterpret_cast<char*>(len_bytes), 4)) break;
        length = static_cast<uint32_t>(len_bytes[0]) | (static_cast<uint32_t>(len_bytes[1]) << 8) |
                 (static_cast<uint32_t>(len_bytes[2]) << 16) |
                 (static_cast<uint32_t>(len_bytes[3]) << 24);

        if (std::string(type, type + 4) == "EXIF") {
            if (length > kMaxExifChunk) return {};
            std::vector<char> data(length);
            if (length > 0 && !read_exact(file, data.data(), length)) break;
            if (data.size() >= 6 &&
                std::string(data.begin(), data.begin() + 6) == std::string("Exif\0\0", 6)) {
                data.erase(data.begin(), data.begin() + 6);
            }
            capture_webp_exif(data, &workflow, &prompt);
        } else {
            file.seekg(length, std::ios::cur);
            if (!file) break;
        }
        if (length % 2 == 1) file.seekg(1, std::ios::cur);
        if (!workflow.empty()) break;
    }

    if (!workflow.empty()) return {ComfyMetadataKind::Workflow, workflow};
    if (!prompt.empty()) return {ComfyMetadataKind::Prompt, prompt};
    return {};
}

bool type_is(const std::array<char, 4>& type, const char* value) {
    return type[0] == value[0] && type[1] == value[1] && type[2] == value[2] &&
           type[3] == value[3];
}

bool type_is_comment(const std::array<char, 4>& type) {
    return static_cast<unsigned char>(type[0]) == 0xa9 && type[1] == 'c' && type[2] == 'm' &&
           type[3] == 't';
}

uint32_t type_as_u32(const std::array<char, 4>& type) {
    return (static_cast<uint32_t>(static_cast<unsigned char>(type[0])) << 24) |
           (static_cast<uint32_t>(static_cast<unsigned char>(type[1])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(type[2])) << 8) |
           static_cast<uint32_t>(static_cast<unsigned char>(type[3]));
}

std::string ascii_lower(std::string text) {
    for (char& ch : text) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return text;
}

std::string normalize_json_text(std::string text) {
    while (!text.empty() && text.back() == '\0') text.pop_back();

    // Some older video nodes stored the JSON as a JSON string. Unwrap it before
    // handing the workflow text to the clipboard.
    for (int pass = 0; pass < 2; ++pass) {
        const nlohmann::json parsed = nlohmann::json::parse(text, nullptr, false);
        if (parsed.is_discarded()) break;
        if (parsed.is_string()) {
            text = parsed.get<std::string>();
            continue;
        }
        if (parsed.is_object() || parsed.is_array()) return parsed.dump();
        return {};
    }

    const size_t json_start = text.find_first_of("{[");
    if (json_start == std::string::npos) return {};
    text.erase(0, json_start);
    const nlohmann::json parsed = nlohmann::json::parse(text, nullptr, false);
    if (parsed.is_discarded() || (!parsed.is_object() && !parsed.is_array())) return {};
    return parsed.dump();
}

void capture_keyed_isobmff_text(const std::string& key, const std::string& text,
                                std::string* workflow, std::string* prompt) {
    const std::string normalized = normalize_json_text(text);
    if (normalized.empty()) return;

    const std::string lower_key = ascii_lower(key);
    if (lower_key == "workflow") {
        *workflow = normalized;
    } else if (lower_key == "prompt") {
        *prompt = normalized;
    }
}

struct IsoBox {
    uint64_t payload_start = 0;
    uint64_t end = 0;
    std::array<char, 4> type{};
};

bool read_isobmff_box(std::ifstream& file, uint64_t parent_end, IsoBox* box) {
    const std::streampos position = file.tellg();
    if (position < 0) return false;
    const uint64_t box_start = static_cast<uint64_t>(position);
    if (box_start + 8 > parent_end) return false;

    uint32_t size32 = 0;
    if (!read_u32_be(file, &size32) || !read_exact(file, box->type.data(), 4)) return false;

    uint64_t header_size = 8;
    uint64_t box_size = size32;
    if (size32 == 1) {
        if (!read_u64_be(file, &box_size)) return false;
        header_size = 16;
    } else if (size32 == 0) {
        box_size = parent_end - box_start;
    }

    if (box_size < header_size || box_size > parent_end - box_start) return false;
    box->payload_start = box_start + header_size;
    box->end = box_start + box_size;
    return true;
}

using MetadataKeys = std::map<uint32_t, std::string>;

MetadataKeys parse_isobmff_keys(std::ifstream& file, uint64_t start, uint64_t end) {
    MetadataKeys keys;
    if (start + 8 > end) return keys;
    file.seekg(static_cast<std::streamoff>(start + 4), std::ios::beg);  // version/flags

    uint32_t entry_count = 0;
    if (!read_u32_be(file, &entry_count) || entry_count > 65536) return {};
    for (uint32_t index = 1; index <= entry_count; ++index) {
        const std::streampos position = file.tellg();
        if (position < 0 || static_cast<uint64_t>(position) + 8 > end) break;

        uint32_t entry_size = 0;
        std::array<char, 4> name_space{};
        if (!read_u32_be(file, &entry_size) || !read_exact(file, name_space.data(), 4)) break;
        const uint64_t entry_start = static_cast<uint64_t>(position);
        if (entry_size < 8 || entry_size > end - entry_start) break;

        const size_t name_size = static_cast<size_t>(entry_size - 8);
        std::string name(name_size, '\0');
        if (name_size > 0 &&
            !read_exact(file, name.data(), static_cast<std::streamsize>(name_size))) {
            break;
        }
        const std::string lower_name = ascii_lower(name);
        if (lower_name == "workflow" || lower_name == "prompt") keys[index] = lower_name;
        file.seekg(static_cast<std::streamoff>(entry_start + entry_size), std::ios::beg);
    }
    return keys;
}

void parse_isobmff_ilst(std::ifstream& file, uint64_t start, uint64_t end,
                        const MetadataKeys& keys, std::string* workflow,
                        std::string* prompt) {
    constexpr uint64_t kMaxMetadataPayload = 64ull * 1024ull * 1024ull;
    file.seekg(static_cast<std::streamoff>(start), std::ios::beg);

    while (file) {
        IsoBox item;
        if (!read_isobmff_box(file, end, &item)) break;
        const auto key = keys.find(type_as_u32(item.type));
        if (key != keys.end()) {
            file.seekg(static_cast<std::streamoff>(item.payload_start), std::ios::beg);
            while (file) {
                IsoBox child;
                if (!read_isobmff_box(file, item.end, &child)) break;
                if (type_is(child.type, "data") && child.payload_start + 8 <= child.end) {
                    const uint64_t text_start = child.payload_start + 8;  // type and locale
                    const uint64_t text_size = child.end - text_start;
                    if (text_size <= kMaxMetadataPayload) {
                        file.seekg(static_cast<std::streamoff>(text_start), std::ios::beg);
                        std::string text(static_cast<size_t>(text_size), '\0');
                        if (text.empty() ||
                            read_exact(file, text.data(), static_cast<std::streamsize>(text.size()))) {
                            capture_keyed_isobmff_text(key->second, text, workflow, prompt);
                        }
                    }
                }
                file.seekg(static_cast<std::streamoff>(child.end), std::ios::beg);
            }
        }
        file.seekg(static_cast<std::streamoff>(item.end), std::ios::beg);
        if (!workflow->empty()) return;
    }
}

void parse_isobmff_meta(std::ifstream& file, uint64_t start, uint64_t end,
                        std::string* workflow, std::string* prompt) {
    MetadataKeys keys;
    std::vector<std::pair<uint64_t, uint64_t>> item_lists;
    file.seekg(static_cast<std::streamoff>(start), std::ios::beg);

    while (file) {
        IsoBox child;
        if (!read_isobmff_box(file, end, &child)) break;
        if (type_is(child.type, "keys")) {
            keys = parse_isobmff_keys(file, child.payload_start, child.end);
        } else if (type_is(child.type, "ilst")) {
            item_lists.emplace_back(child.payload_start, child.end);
        }
        file.seekg(static_cast<std::streamoff>(child.end), std::ios::beg);
    }

    if (keys.empty()) return;
    for (const auto& item_list : item_lists) {
        parse_isobmff_ilst(file, item_list.first, item_list.second, keys, workflow, prompt);
        if (!workflow->empty()) return;
    }
}

bool is_isobmff_container(const std::array<char, 4>& type) {
    return type_is(type, "moov") || type_is(type, "udta") || type_is(type, "meta") ||
           type_is(type, "ilst") || type_is(type, "----") || type_is_comment(type);
}

void capture_isobmff_text(const std::string& text, std::string* workflow,
                          std::string* prompt) {
    const ComfyMetadata combined = metadata_from_combined_json(text);
    if (combined.kind == ComfyMetadataKind::Workflow) {
        *workflow = combined.json;
    } else if (combined.kind == ComfyMetadataKind::Prompt) {
        *prompt = combined.json;
    }
}

void scan_isobmff_boxes(std::ifstream& file, uint64_t end, int depth, std::string* workflow,
                        std::string* prompt) {
    if (depth > 8) return;
    constexpr uint64_t kMaxMetadataPayload = 64ull * 1024ull * 1024ull;

    while (file && static_cast<uint64_t>(file.tellg()) + 8 <= end) {
        IsoBox box;
        if (!read_isobmff_box(file, end, &box)) return;
        uint64_t payload_start = box.payload_start;
        const uint64_t box_end = box.end;

        if (type_is(box.type, "meta")) {
            payload_start += 4;  // FullBox version/flags.
            if (payload_start <= box_end) {
                parse_isobmff_meta(file, payload_start, box_end, workflow, prompt);
                if (!workflow->empty()) return;
            }
        }

        if (type_is(box.type, "data")) {
            const uint64_t payload_size = box_end - payload_start;
            if (payload_size > 8 && payload_size <= kMaxMetadataPayload) {
                file.seekg(static_cast<std::streamoff>(payload_start + 8), std::ios::beg);
                std::vector<char> bytes(static_cast<size_t>(payload_size - 8));
                if (!bytes.empty() && read_exact(file, bytes.data(),
                                                 static_cast<std::streamsize>(bytes.size()))) {
                    while (!bytes.empty() && bytes.back() == '\0') bytes.pop_back();
                    capture_isobmff_text(std::string(bytes.begin(), bytes.end()), workflow,
                                         prompt);
                }
            }
        } else if (payload_start < box_end && is_isobmff_container(box.type)) {
            file.seekg(static_cast<std::streamoff>(payload_start), std::ios::beg);
            scan_isobmff_boxes(file, box_end, depth + 1, workflow, prompt);
        }

        file.seekg(static_cast<std::streamoff>(box_end), std::ios::beg);
        if (!workflow->empty()) return;
    }
}

ComfyMetadata extract_isobmff_metadata(const std::wstring& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    file.seekg(0, std::ios::end);
    const uint64_t size = static_cast<uint64_t>(file.tellg());
    file.seekg(0, std::ios::beg);

    std::string workflow;
    std::string prompt;
    scan_isobmff_boxes(file, size, 0, &workflow, &prompt);
    if (!workflow.empty()) return {ComfyMetadataKind::Workflow, workflow};
    if (!prompt.empty()) return {ComfyMetadataKind::Prompt, prompt};
    return {};
}

bool has_extension(const std::wstring& path, const wchar_t* ext) {
    if (!ext) return false;
    const std::wstring suffix(ext);
    if (path.size() < suffix.size()) return false;
    const size_t offset = path.size() - suffix.size();
    for (size_t i = 0; i < suffix.size(); ++i) {
        wchar_t a = path[offset + i];
        wchar_t b = suffix[i];
        if (a >= L'A' && a <= L'Z') a = static_cast<wchar_t>(a - L'A' + L'a');
        if (b >= L'A' && b <= L'Z') b = static_cast<wchar_t>(b - L'A' + L'a');
        if (a != b) return false;
    }
    return true;
}

}  // namespace

ComfyMetadata extract_comfy_metadata(const std::wstring& path) {
    if (has_extension(path, L".png")) return extract_png_metadata(path);
    if (has_extension(path, L".webp")) return extract_webp_metadata(path);
    if (has_extension(path, L".mp4") || has_extension(path, L".m4v") ||
        has_extension(path, L".mov")) {
        return extract_isobmff_metadata(path);
    }
    return {};
}

}  // namespace meguri::io
