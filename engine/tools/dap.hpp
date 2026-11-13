#pragma once

// The transport of the Debug Adapter Protocol (docs/SPEC.md section 18): messages are JSON texts,
// each after a header "Content-Length: <bytes>\r\n\r\n". The JSON itself is read and written by
// debugger.cpp, so this header has no JSON library.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace vnmaki {

/// `json` with the DAP header in front of it.
[[nodiscard]] std::string frame_dap_message(std::string_view json);

/// Cuts a byte stream into messages. Feed it whatever arrives, in any pieces.
class DapFramer {
 public:
  void feed(std::string_view bytes);
  /// The body of the next complete message, or nothing. A header block with no usable
  /// Content-Length is dropped.
  [[nodiscard]] std::optional<std::string> next();

 private:
  std::string buffer_;
};

/// Where the debugger reads requests and writes responses and events. Neither call blocks.
class DapChannel {
 public:
  enum class Status : std::uint8_t {
    kNothing,  ///< No whole message has arrived yet.
    kMessage,  ///< `body` has the next message.
    kClosed,   ///< The input ended (EOF) and no message is left.
  };

  DapChannel() = default;
  DapChannel(const DapChannel&) = delete;
  DapChannel& operator=(const DapChannel&) = delete;
  DapChannel(DapChannel&&) = delete;
  DapChannel& operator=(DapChannel&&) = delete;
  virtual ~DapChannel() = default;

  [[nodiscard]] virtual Status receive(std::string& body) = 0;
  /// Sends one message; `json` is the body, without the header.
  virtual void send(std::string_view json) = 0;
};

}  // namespace vnmaki
