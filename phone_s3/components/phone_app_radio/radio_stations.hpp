#pragma once

#include <string>
#include <vector>

namespace radio {

struct Station {
    std::string name;
    std::string url;
};

enum class ListResult {
    OK,
    NO_CARD,        // the microSD card is not mounted (absent, or lent to a PC as a USB drive)
    NO_FILE,        // no /sdcard/appdata/radio/stations.json
    BAD_FILE,       // too large, not JSON, or no "stations" array
    EMPTY,          // valid JSON without any usable station
};

/**
 * Read the station list from /sdcard/appdata/radio/stations.json: {"stations": [{"name": ..., "url": ...}, ...]}
 * or a bare array. Entries without a name or with a URL that does not start with http are skipped;
 * up to 100 stations, file up to 64 KB. There is no built-in list: the file is the list.
 */
ListResult load_stations(std::vector<Station> &out);

} // namespace radio
