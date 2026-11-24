#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace radio {

enum class Status {
    STOPPED,
    CONNECTING,
    PLAYING,
    RETRYING,       // the stream dropped or could not open: trying again
    FAILED,         // gave up after 3 attempts without sound, or the speaker is busy
};

/**
 * HLS internet radio through the GMF simple player (port of the radio of MetalioClaw4): playlist and
 * segments over HTTP(S) with the certificate bundle, AAC/MP3/TS segments, played at the stream rate
 * on the speaker (mixed to mono); the sound feeds the shared spectrum (phone_spectrum). All calls return at once; none
 * takes the LVGL lock.
 */
void play(const std::string &url);     // start, or switch to another station
void stop();
void shutdown();                       // give the speaker back, stop and end the tasks (returns within 0.2 s)
Status status();
bool busy_speaker();                   // the last FAILED was a busy speaker (another app plays)
void format(char *buf, size_t len);    // "AAC · 48 kHz stereo", empty before the first sound
void title(char *buf, size_t len);     // song of an Icecast stream (ICY StreamTitle), empty if none

} // namespace radio
