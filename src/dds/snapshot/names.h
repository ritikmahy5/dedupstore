#pragma once

#include <string_view>

namespace dds {

// A valid directory entry name (ON_DISK_FORMAT.md section 6; G10): 1 to 255 bytes (POSIX
// NAME_MAX), no '/', no NUL, and not "." or "..". Any other bytes are allowed: names are byte
// strings, not text. Decoding a tree rejects every other name as corruption, so a restore can never
// be steered outside its destination by a name.
bool IsValidName(std::string_view name);

// A valid symlink target: 1 to 4095 bytes (PATH_MAX - 1), no NUL.
bool IsValidSymlinkTarget(std::string_view target);

}  // namespace dds
