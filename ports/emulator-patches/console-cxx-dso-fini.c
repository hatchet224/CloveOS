/* SPDX-License-Identifier: MIT
 * AArch64 Linux shared-library finalizer for the current Zig 0.13 toolchain.
 * Its shared CRT supplies __dso_handle and __cxa_atexit registration, but omits
 * the GCC crtbeginS __cxa_finalize hook. Without this hook, dlclose unmaps C++
 * destructor callbacks that libc later calls at process exit.
 * __cxa_finalize is idempotent, so this is also safe with a complete GNU CRT.
 */
#if defined(__linux__) && defined(__aarch64__)
extern void *__dso_handle;
extern void __cxa_finalize(void *handle);
__attribute__((destructor)) static void console_finalize_cpp_dso(void)
{
    __cxa_finalize(&__dso_handle);
}
#endif
