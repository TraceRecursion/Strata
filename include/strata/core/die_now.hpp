// include/strata/core/die_now.hpp - ending the engine from a fatal path.
#pragma once

#include <cstdio>
#include <cstdlib>

namespace strata::core {

// A fatal error has been reported already: end the process NOW.  std::exit runs the static destructors and the
// atexit handlers, and a worker thread that reaches it while another thread still holds one of the engine's locks
// makes that cleanup wait for good - the process then lives on as one spinning thread, the control pipe stays
// open, and the server waits for engine_silence_s plus the STOP allowance (minutes) before it kills it.  _Exit
// skips all of it: the pipe closes at once, the server sees the engine stop and starts it again in a minute or two.
[[noreturn]] inline void die_now(int code = 1) {
    std::fflush(nullptr);
    std::_Exit(code);
}

}  // namespace strata::core