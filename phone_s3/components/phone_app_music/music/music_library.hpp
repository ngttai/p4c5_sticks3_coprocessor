#pragma once
// Adapted from the music module of esp_chatbot_v1 (same author) for the phone example.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "id3_parser.hpp"

namespace music {

enum class MetaState : uint8_t {
    Pending,
    Ready,
    Failed,
};

struct Track {
    std::string path;
    std::string display_name;
    MetaState state = MetaState::Pending;
    TrackMeta meta = {};
};

struct ScanHooks {
    // Called periodically while scanning; returning false aborts the scan.
    bool (*checkpoint)(void *ctx) = nullptr;
    void *ctx = nullptr;
};

class Library {
public:
    static constexpr size_t kMaxTracks = 300;
    // Folder levels below the root that are scanned.
    static constexpr int kScanDepth = 3;

    explicit Library(std::string root): root_(std::move(root)) {}

    // Lists audio files in the root and kScanDepth folder levels below, sorted by path.
    size_t scan();

    // Builds a track list without touching the stored one. Returns false when aborted.
    bool scan_into(std::vector<Track> &out, const ScanHooks &hooks = {}) const;
    void adopt(std::vector<Track> &&tracks) { tracks_ = std::move(tracks); }

    size_t count() const { return tracks_.size(); }
    const Track *track(size_t index) const { return index < tracks_.size() ? &tracks_[index] : nullptr; }

    void apply_meta(size_t index, const TrackMeta &meta);
    void mark_failed(size_t index);

    // Reads tag and duration for one track.
    ParseResult load_meta(size_t index, const ParseOptions &options = {});

    // Tag title when available, otherwise the name derived from the path.
    static const char *title_of(const Track &track);

private:
    std::string root_;
    std::vector<Track> tracks_;
};

} // namespace music
