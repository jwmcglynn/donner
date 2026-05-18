#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include "nlohmann/json.hpp"
#include "tools/mcp-servers/editor-control/EditorControlSession.h"

namespace donner::editor::mcp {
namespace {

using nlohmann::json;

std::string LowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

bool ParseContentLength(std::string_view value, size_t* out) {
  size_t result = 0;
  bool sawDigit = false;
  for (const char c : value) {
    if (std::isspace(static_cast<unsigned char>(c))) {
      continue;
    }
    if (!std::isdigit(static_cast<unsigned char>(c))) {
      return false;
    }
    sawDigit = true;
    const size_t digit = static_cast<size_t>(c - '0');
    if (result > (std::numeric_limits<size_t>::max() - digit) / 10) {
      return false;
    }
    result = result * 10 + digit;
  }
  if (!sawDigit) {
    return false;
  }
  *out = result;
  return true;
}

bool ReadStringMember(const json& object, std::string_view key, std::string* out) {
  if (!object.is_object()) {
    return false;
  }
  const auto it = object.find(std::string(key));
  if (it == object.end() || !it->is_string()) {
    return false;
  }
  *out = it->get<std::string>();
  return true;
}

const json* FindObjectMember(const json& object, std::string_view key) {
  if (!object.is_object()) {
    return nullptr;
  }
  const auto it = object.find(std::string(key));
  if (it == object.end() || !it->is_object()) {
    return nullptr;
  }
  return &*it;
}

const json* FindMember(const json& object, std::string_view key) {
  if (!object.is_object()) {
    return nullptr;
  }
  const auto it = object.find(std::string(key));
  return it == object.end() ? nullptr : &*it;
}

std::optional<json> ReadMessage(std::istream& in) {
  std::string line;
  size_t contentLength = 0;

  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      break;
    }

    const size_t colon = line.find(':');
    if (colon == std::string::npos) {
      continue;
    }
    const std::string key = LowerAscii(line.substr(0, colon));
    std::string value = line.substr(colon + 1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
      value.erase(value.begin());
    }
    if (key == "content-length") {
      if (!ParseContentLength(value, &contentLength)) {
        return std::nullopt;
      }
    }
  }

  if (!in.good() && contentLength == 0) {
    return std::nullopt;
  }
  if (contentLength == 0) {
    return json::parse("{}", nullptr, false);
  }

  std::string body(contentLength, '\0');
  in.read(body.data(), static_cast<std::streamsize>(contentLength));
  if (in.gcount() != static_cast<std::streamsize>(contentLength)) {
    return std::nullopt;
  }

  json parsed = json::parse(body, nullptr, false);
  if (parsed.is_discarded()) {
    return std::nullopt;
  }
  return parsed;
}

void WriteMessage(std::ostream& out, const json& message) {
  const std::string body = message.dump();
  out << "Content-Length: " << body.size() << "\r\n\r\n" << body;
  out.flush();
}

json ErrorResponse(const json& id, int code, const std::string& message) {
  return json{
      {"jsonrpc", "2.0"},
      {"id", id},
      {"error", {{"code", code}, {"message", message}}},
  };
}

json ToolResultToMcp(const ToolCallResult& result) {
  json content = json::array();
  content.push_back(json{
      {"type", "text"},
      {"text", result.body.dump(2)},
  });

  for (const EncodedImage& image : result.images) {
    content.push_back(json{
        {"type", "image"},
        {"data", image.dataBase64},
        {"mimeType", image.mimeType},
    });
  }

  json response{{"content", std::move(content)}};
  if (result.isError) {
    response["isError"] = true;
  }
  return response;
}

json HandleRequest(EditorControlSession* session, const json& request) {
  json id = nullptr;
  if (const json* requestId = FindMember(request, "id")) {
    id = *requestId;
  }

  std::string method;
  if (!ReadStringMember(request, "method", &method)) {
    return ErrorResponse(id, -32600, "request must contain a string method");
  }

  if (method == "initialize") {
    std::string protocolVersion = "2024-11-05";
    if (const json* params = FindObjectMember(request, "params")) {
      std::string requestedProtocolVersion;
      if (ReadStringMember(*params, "protocolVersion", &requestedProtocolVersion)) {
        protocolVersion = requestedProtocolVersion;
      }
    }
    return json{
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result",
         {
             {"protocolVersion", protocolVersion},
             {"capabilities", {{"tools", json::object()}}},
             {"serverInfo", {{"name", "donner-editor-control"}, {"version", "0.1.0"}}},
             {"instructions",
              "Headless Donner editor control: load SVGs, select by CSS selector, synthesize "
              "drag frames, and inspect compositor layers without OS permissions."},
         }},
    };
  }

  if (method == "ping") {
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", json::object()}};
  }

  if (method == "tools/list") {
    return json{
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result", {{"tools", EditorControlSession::toolList()}}},
    };
  }

  if (method == "tools/call") {
    const json* params = FindObjectMember(request, "params");
    if (params == nullptr) {
      return ErrorResponse(id, -32602, "tools/call params must be an object");
    }

    std::string name;
    if (!ReadStringMember(*params, "name", &name)) {
      return ErrorResponse(id, -32602, "tools/call params.name must be a string");
    }

    json emptyArguments = json::object();
    const json* arguments = &emptyArguments;
    if (const json* argumentsMember = FindMember(*params, "arguments")) {
      if (!argumentsMember->is_object()) {
        return ErrorResponse(id, -32602, "tools/call params.arguments must be an object");
      }
      arguments = argumentsMember;
    }

    return json{
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result", ToolResultToMcp(session->handleToolCall(name, *arguments))},
    };
  }

  return ErrorResponse(id, -32601, "method not found: " + method);
}

}  // namespace
}  // namespace donner::editor::mcp

int main() {
  if (const char* bwd = std::getenv("BUILD_WORKING_DIRECTORY")) {
    std::filesystem::current_path(bwd);
  }

  donner::editor::mcp::EditorControlSession session;

  while (true) {
    std::optional<nlohmann::json> request = donner::editor::mcp::ReadMessage(std::cin);
    if (!request.has_value()) {
      break;
    }

    const bool hasId = request->is_object() && request->contains("id");
    std::string method;
    (void)donner::editor::mcp::ReadStringMember(*request, "method", &method);
    if (method.rfind("notifications/", 0) == 0 || !hasId) {
      continue;
    }

    donner::editor::mcp::WriteMessage(std::cout,
                                      donner::editor::mcp::HandleRequest(&session, *request));
  }

  return 0;
}
