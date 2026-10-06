/*
 * wasm2c runtime configuration for com.recomp.gcn, included first by the vendored
 * wasm-rt.h so every build (editor addon, console builds, standalone runner) compiles the
 * generated code the same way: no mmap / guard pages, no bounds checks of its own
 * (gcnw_mem_ops.h masks addresses instead), no signal handlers.
 */
#ifndef GCNW_CONFIG_H
#define GCNW_CONFIG_H

#ifndef WASM_RT_USE_MMAP
#define WASM_RT_USE_MMAP 0
#endif
#ifndef WASM_RT_MEMCHECK_BOUNDS_CHECK
#define WASM_RT_MEMCHECK_BOUNDS_CHECK 1
#endif
#ifndef WASM_RT_NONCONFORMING_UNCHECKED_STACK_EXHAUSTION
#define WASM_RT_NONCONFORMING_UNCHECKED_STACK_EXHAUSTION 1
#endif
#ifndef WASM_RT_SKIP_SIGNAL_RECOVERY
#define WASM_RT_SKIP_SIGNAL_RECOVERY 1
#endif

#endif /* GCNW_CONFIG_H */
