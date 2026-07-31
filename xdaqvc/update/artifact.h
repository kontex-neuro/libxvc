#pragma once

#include <filesystem>
#include <string>

#include "types.h"

namespace xvc::update
{

// Streams the file in chunks; safe for the ~46 MB release tarballs (never loads it whole).
// Returns the digest as lowercase hex.
//
// ADR 0001: implemented against platform crypto -- BCrypt (CNG) on Windows, CommonCrypto on
// macOS -- so that libxvc ships no bundled crypto library and OS vendors own CVE patching.
// The two code paths are the reason the known-answer tests in test_update_sha256.cc are
// mandatory rather than optional; they are the only thing checking that the paths agree.
Result<std::string> calculate_sha256(const std::filesystem::path &path);

// Checks size first, then hash, and reports them as DISTINCT error codes.
//
// The distinction is load-bearing (ADR 0005): a size mismatch is almost always a truncated
// transfer and retrying it is correct, whereas a hash mismatch on a correctly-sized file
// indicates corruption or tampering. Callers must never retry HashMismatch -- the old
// implementation retried both identically three times, which wasted time and treated a
// security signal as noise.
Result<void> verify_file(const std::filesystem::path &path, const Artifact &artifact);

}  // namespace xvc::update
