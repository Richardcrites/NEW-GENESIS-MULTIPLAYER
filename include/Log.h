#pragma once

// Release-hardening log gate.
//
// DEV build (no PULSAR_HARDENED, i.e. RelWithDebInfo): logs exactly as before via REX,
// so the crash-dump workflow (NAS logs, dump_stack.py) is unchanged.
//
// HARDENED release build (PULSAR_HARDENED defined only in the Release config): INFO/WARN
// are compiled OUT entirely, so the literal strings, which name functions/RVAs/behaviour
// and are the #1 reversing aid, never reach the shipped binary.
//
// Iron rule: only the hardened Release config defines PULSAR_HARDENED. NEVER ship the dev
// build. Use PLOG_INFO/PLOG_WARN at call sites instead of REX::INFO/REX::WARN.

#ifdef PULSAR_HARDENED
#  define PLOG_INFO(...) ((void)0)
#  define PLOG_WARN(...) ((void)0)
#else
#  define PLOG_INFO(...) REX::INFO(__VA_ARGS__)
#  define PLOG_WARN(...) REX::WARN(__VA_ARGS__)
#endif

// PLOG_DIAG: a CURATED, sanitized high-level channel that STAYS in the hardened release
// so testers can send a useful log. Use it ONLY for plain-language, user-level events
// (connection up/down, host/join result, key errors, crashguard fired) with NO RVAs,
// function names, or offsets. These literals SHIP in the binary, so never put internal
// addresses or reverse-engineering detail in a PLOG_DIAG message.
#define PLOG_DIAG(...) REX::INFO(__VA_ARGS__)
