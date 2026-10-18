// Turns "the process ended" into a structured cause the AI can act on:
// OOM_KILLED, TIMEOUT, MODULE_NOT_FOUND (with the module), SYNTAX_ERROR (file
// and line), ILLEGAL_INSTRUCTION (old CPU), DISK_FULL, ... Self-repair loops
// work far better from these than from raw tracebacks.
#pragma once

#include "platform.hpp"

namespace maas {

struct FailureInput {
  std::string task_kind;    // script, install
  std::string interpreter;  // python, node, ...
  ExitInfo exit;
  std::string kill_reason;  // set when MAAS itself stopped the job
  std::string log_tail;     // last part of the job's output
  uint64_t peak_kb = 0;
  int64_t memory_mb = 0;
  int timeout_sec = 0;
  std::string stored_name;  // script file name, to map stored paths back
};

// Returns {"failure_kind": "...", "summary": "...", "hint": "...", ...}.
// failure_kind is "OK" when the job succeeded.
json classify_failure(const FailureInput& input);

// Common Python import names whose pip package is called something else.
std::string pip_name_for_module(const std::string& module);

}  // namespace maas
