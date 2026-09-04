// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// build_identity.h — WHICH BUILD IS ACTUALLY EXECUTING.
//
// ⛔ THE DEFECT THIS EXISTS FOR, IN PLAIN WORDS.  Replacing the plug-in file does not change what
//    REAPER is already running, and until this file nothing in the product could tell you which
//    one it had.  Doc 267 installed a new dylib, did not relaunch, and ran an arm against the OLD
//    code while every available check agreed everything was fine:
//      * `shasum` on the installed dylib reads THE FILE ON DISK, not the code mapped into the
//        process — they are the same object only until someone overwrites the file;
//      * `pgrep -x REAPER` reports that A REAPER is running, never that a NEW one is;
//      * the discovery file's `version` read 1.17.0 for BOTH builds, because a version string only
//        moves when a release moves it.
//    The only quantity that separated them was a payload field that beat happened to add for an
//    unrelated reason.  ⇒ A claim about "the shipped build" must be read from the RUNNING
//    PROCESS's OWN ANSWER to a question only that build can answer.
//
// ⭐ WHAT IS REPORTED, AND WHY IT IS THE RIGHT QUANTITY.  The Mach-O `LC_UUID` load command is
//    written by the linker from the content of the image it links: different code gives a
//    different UUID, identical code gives the same one.  It is therefore a CONTENT identity, not a
//    timestamp and not a version.  This reader takes it from the mach_header ALREADY MAPPED INTO
//    THE PROCESS (found through `dladdr` on a function defined in this image), so what it returns
//    is a property of the executing code and CANNOT be changed by writing to the file on disk.
//    Compare it with `dwarfdump --uuid <dylib>`: agreement means the running code is that file;
//    disagreement means the file has been replaced since the process loaded it, which is exactly
//    the gap this file exists for, and it is not visible any other way.
//
// ⚠️ A UNIVERSAL BINARY HAS ONE LC_UUID PER SLICE.  This dylib ships x86_64 + arm64, so
//    `dwarfdump --uuid` prints TWO and the process has loaded exactly ONE of them.  The honest
//    comparison is therefore "the live UUID is among the file's UUIDs", not "equals the file's
//    UUID", and any reader that asserts the stronger form is wrong on a fat binary.
//
// ⛔ NON-MACH-O HOSTS GET NULL, NOT A SUBSTITUTE.  ELF build-ids and PE debug GUIDs are different
//    mechanisms; inventing a fallback that looks like a UUID would report a number that does not
//    mean what the field says it means: an absence is stated, never floored.
//
// ⚠️ WHAT IT DOES NOT SAY: nothing about whether that build is CORRECT, whether it matches a tag,
//    or whether the source it came from is committed.  It answers one question — is the code
//    running the code you think you installed — and that question had no answer before.

#pragma once

#include <string>

#if defined(__APPLE__)
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>

#include <cstdint>
#include <cstdio>
#endif

namespace reaper_mcp {

// The address of this function is inside THIS image; dladdr turns it into that image's base.
// It must not be inlined away, so it is deliberately non-trivial to elide.
inline const void* buildIdentityAnchor() { static const char kAnchor = 'i'; return &kAnchor; }

// Lowercase 32-hex-digit LC_UUID of the LOADED image, or "" where the mechanism does not exist.
inline std::string loadedImageUuid() {
#if defined(__APPLE__)
    Dl_info info;
    if (dladdr(buildIdentityAnchor(), &info) == 0 || info.dli_fbase == nullptr) return std::string();
    const auto* hdr = reinterpret_cast<const mach_header_64*>(info.dli_fbase);
    if (hdr->magic != MH_MAGIC_64) return std::string();
    const auto* cmd = reinterpret_cast<const load_command*>(
        reinterpret_cast<const std::uint8_t*>(hdr) + sizeof(mach_header_64));
    for (std::uint32_t i = 0; i < hdr->ncmds; ++i) {
        if (cmd->cmdsize == 0) break;  // a malformed table must not spin forever
        if (cmd->cmd == LC_UUID) {
            const auto* uc = reinterpret_cast<const uuid_command*>(cmd);
            char buf[33];
            for (int b = 0; b < 16; ++b) std::snprintf(buf + b * 2, 3, "%02x", uc->uuid[b]);
            return std::string(buf, 32);
        }
        cmd = reinterpret_cast<const load_command*>(
            reinterpret_cast<const std::uint8_t*>(cmd) + cmd->cmdsize);
    }
    return std::string();
#else
    return std::string();
#endif
}

// How the value above was obtained — reported beside it so a reader never has to guess whether an
// empty string means "unavailable here" or "the reader failed".
inline const char* loadedImageUuidMethod() {
#if defined(__APPLE__)
    return "LC_UUID of the Mach-O image mapped into this process, via dladdr on a symbol in it - "
           "a property of the EXECUTING code, not of any file on disk. A universal binary carries "
           "one per slice, so compare with `dwarfdump --uuid` as MEMBERSHIP, not equality.";
#else
    return "no Mach-O LC_UUID on this platform; null is reported rather than a substitute that "
           "would not mean what this field means";
#endif
}

// The translation unit's compile stamp. ⚠️ WEAKER THAN THE UUID AND REPORTED AS SUCH: it moves
// when THIS header's including TU is recompiled, which a partial rebuild may not do, and two
// different builds compiled in the same minute share it. It is context for a human, never an
// identity to compare.
inline const char* buildCompiledAt() { return __DATE__ " " __TIME__; }

}  // namespace reaper_mcp
