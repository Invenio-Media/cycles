/* SPDX-FileCopyrightText: 2026 Invenio Media
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/node_registry.h"

#include "graph/node_enum.h"
#include "graph/node_type.h"
#include "util/transform.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

HDCYCLES_NAMESPACE_OPEN_SCOPE

CCL_NAMESPACE_USING_DIRECTIVE

namespace {

std::string Quote(const std::string &text)
{
  std::string out = "\"";
  for (const char c : text) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char escaped[8];
          snprintf(escaped, sizeof(escaped), "\\u%04x", c);
          out += escaped;
        }
        else {
          out += c;
        }
    }
  }
  return out + "\"";
}

/* JSON has no infinity or NaN: such a default is null, as defaults without a plain value are. */
std::string Number(const double value)
{
  if (!std::isfinite(value)) {
    return "null";
  }
  char text[32];
  snprintf(text, sizeof(text), "%.9g", value);
  return text;
}

/* The default as JSON, or null for sockets without a plain value (closures, nodes, arrays,
 * transforms). */
std::string DefaultValue(const SocketType &socket)
{
  const void *value = socket.default_value;
  if (!value) {
    return "null";
  }
  switch (socket.type) {
    case SocketType::BOOLEAN:
      return *static_cast<const bool *>(value) ? "true" : "false";
    case SocketType::INT:
      return std::to_string(*static_cast<const int *>(value));
    case SocketType::UINT:
      return std::to_string(*static_cast<const uint *>(value));
    case SocketType::FLOAT:
      return Number(*static_cast<const float *>(value));
    case SocketType::COLOR:
    case SocketType::VECTOR:
    case SocketType::POINT:
    case SocketType::NORMAL: {
      const float3 v = *static_cast<const float3 *>(value);
      return "[" + Number(v.x) + ", " + Number(v.y) + ", " + Number(v.z) + "]";
    }
    case SocketType::POINT2: {
      const float2 v = *static_cast<const float2 *>(value);
      return "[" + Number(v.x) + ", " + Number(v.y) + "]";
    }
    case SocketType::STRING:
      return Quote(static_cast<const ustring *>(value)->string());
    case SocketType::ENUM: {
      const int index = *static_cast<const int *>(value);
      return socket.enum_values && socket.enum_values->exists(index) ?
                 Quote((*socket.enum_values)[index].string()) :
                 "null";
    }
    default:
      return "null";
  }
}

/* Enum names in value order, so a menu lists them as Cycles numbers them. */
std::string EnumNames(const NodeEnum &values)
{
  std::vector<std::pair<int, std::string>> items;
  for (const auto &item : values) {
    items.emplace_back(item.second, item.first.string());
  }
  std::sort(items.begin(), items.end());
  std::string out = "[";
  for (size_t i = 0; i < items.size(); ++i) {
    out += (i ? ", " : "") + Quote(items[i].second);
  }
  return out + "]";
}

}  // namespace

std::string HdCyclesShaderNodesJson()
{
  std::vector<ustring> names = NodeType::type_names();
  std::sort(names.begin(), names.end(), [](const ustring &a, const ustring &b) {
    return a.string() < b.string();
  });

  std::string json = "{\n";
  bool firstNode = true;
  for (const ustring &name : names) {
    const NodeType *type = NodeType::find(name);
    if (!type || type->type != NodeType::SHADER) {
      continue;
    }
    json += std::string(firstNode ? "" : ",\n") + "  " + Quote(name.string()) + ": {\n";
    firstNode = false;

    json += "    \"inputs\": [";
    bool first = true;
    for (const SocketType &socket : type->inputs) {
      json += std::string(first ? "\n" : ",\n") +
              "      {\"name\": " + Quote(socket.name.string()) +
              ", \"ui_name\": " + Quote(socket.ui_name.string()) +
              ", \"type\": " + Quote(SocketType::type_name(socket.type).string()) +
              ", \"linkable\": " + ((socket.flags & SocketType::LINKABLE) ? "true" : "false") +
              ", \"internal\": " + ((socket.flags & SocketType::INTERNAL) ? "true" : "false") +
              ", \"default_link\": " +
              ((socket.flags & SocketType::DEFAULT_LINK_MASK) ? "true" : "false") +
              ", \"default\": " + DefaultValue(socket);
      if (socket.enum_values) {
        json += ", \"enum\": " + EnumNames(*socket.enum_values);
      }
      json += "}";
      first = false;
    }
    json += first ? "],\n" : "\n    ],\n";

    json += "    \"outputs\": [";
    first = true;
    for (const SocketType &socket : type->outputs) {
      json += std::string(first ? "\n" : ",\n") +
              "      {\"name\": " + Quote(socket.name.string()) +
              ", \"ui_name\": " + Quote(socket.ui_name.string()) +
              ", \"type\": " + Quote(SocketType::type_name(socket.type).string()) + "}";
      first = false;
    }
    json += first ? "]\n  }" : "\n    ]\n  }";
  }
  return json + "\n}\n";
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
