#include "isolated-cursor.hpp"
#define SetCursorPos tc_test_cursor::set
#define TC_CLOCK_DRIVER 1
#define tc_mod_load clock_base_load
#include "../examples/clock/plugin.cpp"
#undef tc_mod_load
#undef SetCursorPos

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* plugin) {
    const int result = clock_base_load(h, plugin);
    return result ? result : (tc_test_cursor::install() ? 0 : 20);
}
