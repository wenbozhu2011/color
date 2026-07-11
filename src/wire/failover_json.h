// JSON serialization for the failover wire/file formats.
//
// The Color core structs are transport-agnostic; this converts the two that
// cross a boundary — the persisted Checkpoint (a JSON file) and the Replay (a
// JSON request body) — to and from JSON. Shared by the libcurl client and the
// net_http server so both agree on the encoding.
#ifndef COLOR_FAILOVER_JSON_H
#define COLOR_FAILOVER_JSON_H

#include <string>

#include "color_checkpoint.h"

namespace color {

std::string to_json(const Checkpoint& cp);
bool from_json_checkpoint(const std::string& s, Checkpoint& out);

std::string to_json(const Replay& rp);
bool from_json_replay(const std::string& s, Replay& out);

}  // namespace color

#endif  // COLOR_FAILOVER_JSON_H
