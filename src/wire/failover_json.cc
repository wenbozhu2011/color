#include "failover_json.h"

#include <cctype>
#include <cstdlib>

namespace color {
namespace {

// ---- serialization helpers ----

void append_escaped(std::string& out, const std::string& s) {
  out += '"';
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          static const char* hex = "0123456789abcdef";
          out += "\\u00";
          out += hex[(c >> 4) & 0xf];
          out += hex[c & 0xf];
        } else {
          out += c;
        }
    }
  }
  out += '"';
}

// ---- a minimal recursive-descent parser for the shapes we emit ----

struct Parser {
  const char* p;
  const char* end;
  bool ok = true;

  void ws() {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
  }
  bool eat(char c) {
    ws();
    if (p < end && *p == c) {
      ++p;
      return true;
    }
    ok = false;
    return false;
  }
  bool peek(char c) {
    ws();
    return p < end && *p == c;
  }

  std::string parse_string() {
    std::string s;
    if (!eat('"')) return s;
    while (p < end && *p != '"') {
      char c = *p++;
      if (c == '\\' && p < end) {
        char e = *p++;
        switch (e) {
          case '"': s += '"'; break;
          case '\\': s += '\\'; break;
          case '/': s += '/'; break;
          case 'n': s += '\n'; break;
          case 'r': s += '\r'; break;
          case 't': s += '\t'; break;
          case 'b': s += '\b'; break;
          case 'f': s += '\f'; break;
          case 'u': {
            if (end - p >= 4) {
              int v = 0;
              for (int i = 0; i < 4; ++i) {
                char h = *p++;
                v <<= 4;
                if (h >= '0' && h <= '9') v |= h - '0';
                else if (h >= 'a' && h <= 'f') v |= h - 'a' + 10;
                else if (h >= 'A' && h <= 'F') v |= h - 'A' + 10;
              }
              if (v < 0x80) s += static_cast<char>(v);  // ASCII is enough here
            }
            break;
          }
          default: s += e;
        }
      } else {
        s += c;
      }
    }
    if (p < end && *p == '"') ++p; else ok = false;
    return s;
  }

  std::uint64_t parse_u64() {
    ws();
    const char* start = p;
    while (p < end && (std::isdigit((unsigned char)*p) || *p == '-')) ++p;
    return std::strtoull(std::string(start, p).c_str(), nullptr, 10);
  }

  bool parse_bool() {
    ws();
    if (end - p >= 4 && std::string(p, p + 4) == "true") { p += 4; return true; }
    if (end - p >= 5 && std::string(p, p + 5) == "false") { p += 5; return false; }
    ok = false;
    return false;
  }

  // Skip an arbitrary JSON value (for keys we don't recognise).
  void skip_value() {
    ws();
    if (p >= end) { ok = false; return; }
    if (*p == '"') { parse_string(); return; }
    if (*p == '{' || *p == '[') {
      char open = *p, close = (open == '{') ? '}' : ']';
      ++p;
      int depth = 1;
      while (p < end && depth > 0) {
        if (*p == '"') { parse_string(); continue; }
        if (*p == open) ++depth;
        else if (*p == close) --depth;
        ++p;
      }
      return;
    }
    while (p < end && *p != ',' && *p != '}' && *p != ']') ++p;
  }
};

}  // namespace

std::string to_json(const Checkpoint& cp) {
  std::string s = "{\"version\":1";
  s += ",\"committed_upto\":" + std::to_string(cp.committed_upto);
  s += ",\"event_count\":" + std::to_string(cp.event_count);
  s += ",\"history_hash\":" + std::to_string(cp.history_hash);
  s += ",\"buffer\":[";
  for (std::size_t i = 0; i < cp.buffer.size(); ++i) {
    const auto& b = cp.buffer[i];
    if (i) s += ',';
    s += "{\"seq\":" + std::to_string(b.seq) + ",\"payload\":";
    append_escaped(s, b.payload);
    s += ",\"hash\":" + std::to_string(b.hash) + "}";
  }
  s += "]}";
  return s;
}

bool from_json_checkpoint(const std::string& in, Checkpoint& out) {
  Parser p{in.data(), in.data() + in.size()};
  out = Checkpoint{};
  if (!p.eat('{')) return false;
  while (!p.peek('}') && p.ok) {
    std::string key = p.parse_string();
    if (!p.eat(':')) return false;
    if (key == "committed_upto") out.committed_upto = p.parse_u64();
    else if (key == "event_count") out.event_count = p.parse_u64();
    else if (key == "history_hash") out.history_hash = p.parse_u64();
    else if (key == "buffer") {
      if (!p.eat('[')) return false;
      while (!p.peek(']') && p.ok) {
        BufferedResponse b{};
        if (!p.eat('{')) return false;
        while (!p.peek('}') && p.ok) {
          std::string k = p.parse_string();
          if (!p.eat(':')) return false;
          if (k == "seq") b.seq = p.parse_u64();
          else if (k == "payload") b.payload = p.parse_string();
          else if (k == "hash") b.hash = p.parse_u64();
          else p.skip_value();
          if (!p.peek('}')) p.eat(',');
        }
        p.eat('}');
        out.buffer.push_back(std::move(b));
        if (!p.peek(']')) p.eat(',');
      }
      p.eat(']');
    } else {
      p.skip_value();
    }
    if (!p.peek('}')) p.eat(',');
  }
  p.eat('}');
  return p.ok;
}

std::string to_json(const Replay& rp) {
  std::string s = "{\"from\":" + std::to_string(rp.from) + ",\"events\":[";
  for (std::size_t i = 0; i < rp.events.size(); ++i) {
    const auto& e = rp.events[i];
    if (i) s += ',';
    s += std::string("{\"t\":\"") + (e.is_response ? "r" : "R") + "\"";
    s += ",\"seq\":" + std::to_string(e.seq);
    if (e.is_response) {
      s += ",\"payload\":";
      append_escaped(s, e.payload);
      s += ",\"hash\":" + std::to_string(e.hash);
    }
    s += "}";
  }
  s += "]}";
  return s;
}

bool from_json_replay(const std::string& in, Replay& out) {
  Parser p{in.data(), in.data() + in.size()};
  out = Replay{};
  if (!p.eat('{')) return false;
  while (!p.peek('}') && p.ok) {
    std::string key = p.parse_string();
    if (!p.eat(':')) return false;
    if (key == "from") out.from = p.parse_u64();
    else if (key == "events") {
      if (!p.eat('[')) return false;
      while (!p.peek(']') && p.ok) {
        ReplayEvent e{};
        if (!p.eat('{')) return false;
        while (!p.peek('}') && p.ok) {
          std::string k = p.parse_string();
          if (!p.eat(':')) return false;
          if (k == "t") e.is_response = (p.parse_string() == "r");
          else if (k == "seq") e.seq = p.parse_u64();
          else if (k == "payload") e.payload = p.parse_string();
          else if (k == "hash") e.hash = p.parse_u64();
          else p.skip_value();
          if (!p.peek('}')) p.eat(',');
        }
        p.eat('}');
        out.events.push_back(std::move(e));
        if (!p.peek(']')) p.eat(',');
      }
      p.eat(']');
    } else {
      p.skip_value();
    }
    if (!p.peek('}')) p.eat(',');
  }
  p.eat('}');
  return p.ok;
}

}  // namespace color
