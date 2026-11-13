#include "engine/tools/dap.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace vnmaki {

namespace {

constexpr std::string_view kHeaderEnd = "\r\n\r\n";
constexpr std::string_view kContentLength = "content-length:";

std::string lower(std::string_view text) {
  std::string out{text};
  std::ranges::transform(out, out.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

// The Content-Length of a header block, if it has a usable one.
std::optional<std::size_t> content_length(std::string_view header) {
  const std::string text = lower(header);
  const std::size_t at = text.find(kContentLength);
  if (at == std::string::npos) {
    return std::nullopt;
  }
  std::size_t begin = at + kContentLength.size();
  while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t')) {
    ++begin;
  }
  std::size_t length = 0;
  const char* const first = text.data() + begin;
  const char* const last = text.data() + text.size();
  const auto [end, error] = std::from_chars(first, last, length);
  if (error != std::errc{} || end == first) {
    return std::nullopt;
  }
  return length;
}

}  // namespace

std::string frame_dap_message(std::string_view json) {
  std::string out = "Content-Length: " + std::to_string(json.size()) + "\r\n\r\n";
  out += json;
  return out;
}

void DapFramer::feed(std::string_view bytes) { buffer_ += bytes; }

std::optional<std::string> DapFramer::next() {
  while (true) {
    const std::size_t end = buffer_.find(kHeaderEnd);
    if (end == std::string::npos) {
      return std::nullopt;
    }
    const std::size_t body_start = end + kHeaderEnd.size();
    const std::optional<std::size_t> length =
        content_length(std::string_view{buffer_}.substr(0, end));
    if (!length) {
      buffer_.erase(0, body_start);  // not a message header: skip it
      continue;
    }
    if (buffer_.size() - body_start < *length) {
      return std::nullopt;
    }
    std::string body = buffer_.substr(body_start, *length);
    buffer_.erase(0, body_start + *length);
    return body;
  }
}

}  // namespace vnmaki
