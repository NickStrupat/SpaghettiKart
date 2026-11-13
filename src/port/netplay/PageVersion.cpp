#include "PageVersion.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace PageVersion {

// The page's side is window.skPageVersion, in src/port/web/shell.html. The game's main loop runs on the page's main
// thread, which is where window is.

void Check() {
#ifdef __EMSCRIPTEN__
    EM_ASM({
        if (window.skPageVersion) {
            window.skPageVersion.check();
        }
    });
#endif
}

State GetState() {
#ifdef __EMSCRIPTEN__
    // In the order of State. (A list with commas would split the macro's argument.)
    const int state = EM_ASM_INT({
        const states = "unknown downloading waiting newest".split(" ");
        return window.skPageVersion ? Math.max(0, states.indexOf(window.skPageVersion.state)) : 0;
    });
    return static_cast<State>(state);
#else
    return State::Unknown;
#endif
}

} // namespace PageVersion
