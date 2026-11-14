#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace vnmaki {

/// Turns a virtual path into its canonical form, or returns std::nullopt if
/// the path is not allowed.
///
/// A virtual path names a file relative to the VFS root, never a host path.
/// The canonical form is UTF-8, uses '/' between segments, and has no
/// leading or trailing '/', no empty segment and no "." or "..". The root
/// itself is the empty string.
///
/// Normalization:
/// - '\' is accepted as a separator and becomes '/'.
/// - Empty segments ("a//b") and "." are dropped.
/// - ".." removes the previous segment.
///
/// Rejected, so a path can never leave the mount root:
/// - ".." that would climb above the root ("../x", "a/../../x").
/// - Absolute paths: a leading '/' or '\' (POSIX root, UNC share) and any
///   ':' (drive letter, NTFS stream).
///
/// Rejected, so a path that works on one desktop OS works on all of them:
/// - Invalid UTF-8, control characters (including NUL) and <>:"|?*.
/// - A segment ending in '.' or ' ' (Windows strips these, aliasing files).
/// - Windows device names, with or without extension: CON, PRN, AUX, NUL,
///   CONIN$, CONOUT$, COM1-COM9, LPT1-LPT9 (in any letter case).
///
/// Case policy: virtual paths are case-sensitive on every platform.
/// Normalization never changes letter case, and mounts match each segment
/// exactly, even on a case-insensitive host file system. "BG.png" and
/// "bg.png" are different paths, so a game that runs from loose files on
/// Windows or macOS behaves the same on Linux and from a pack.
[[nodiscard]] std::optional<std::string> normalize_virtual_path(std::string_view path);

}  // namespace vnmaki
