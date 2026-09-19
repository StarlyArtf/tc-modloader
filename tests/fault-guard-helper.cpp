/* Helper DLL for tests/fault-guard-probe.cpp: a fault raised in a separately
   loaded module, which is the shape of a plugin callback. */
extern "C" __declspec(dllexport) void tc_fault_guard_boom() {
    volatile int* nowhere = reinterpret_cast<volatile int*>(8);
    *nowhere = 1;
}
