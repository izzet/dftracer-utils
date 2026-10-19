#ifndef DFTRACER_UTILS_CORE_COMMON_EXPORT_H
#define DFTRACER_UTILS_CORE_COMMON_EXPORT_H

/*
 * Symbol visibility for the dftracer-utils stable C ABIs. Freestanding - pure
 * preprocessor, no includes - so even a self-contained plugin ABI can use it.
 *
 *   DFTU_EXPORT         mark a stable-ABI symbol default-visible, so it stays
 *                      exported even when the library hides symbols by default.
 *   DFTU_LOCAL          force a symbol hidden (never exported).
 *   DFTU_PLUGIN_EXPORT  a plugin's exported entry point.
 *
 * Windows libraries export their definitions through the build system.
 * Declarations stay unadorned so including another component's ABI does
 * not re-export its functions. Plugin entry points are explicitly exported.
 */
#if defined(_WIN32) || defined(__CYGWIN__)
#define DFTU_EXPORT
#define DFTU_LOCAL
#define DFTU_PLUGIN_EXPORT __declspec(dllexport)

// Inline code that reads shared data needs an explicit import on Windows.
#if defined(DFTU_CORE_SHARED)
#if defined(DFTU_BUILDING_CORE)
#define DFTU_CORE_EXPORT __declspec(dllexport)
#else
#define DFTU_CORE_EXPORT __declspec(dllimport)
#endif
#else
#define DFTU_CORE_EXPORT
#endif
#if defined(DFTU_UTILITIES_SHARED)
#if defined(DFTU_BUILDING_UTILITIES)
#define DFTU_UTILITIES_EXPORT __declspec(dllexport)
#else
#define DFTU_UTILITIES_EXPORT __declspec(dllimport)
#endif
#else
#define DFTU_UTILITIES_EXPORT
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define DFTU_EXPORT __attribute__((visibility("default")))
#define DFTU_LOCAL __attribute__((visibility("hidden")))
#define DFTU_PLUGIN_EXPORT DFTU_EXPORT
#define DFTU_CORE_EXPORT DFTU_EXPORT
#define DFTU_UTILITIES_EXPORT DFTU_EXPORT
#else
#define DFTU_EXPORT
#define DFTU_LOCAL
#define DFTU_PLUGIN_EXPORT DFTU_EXPORT
#define DFTU_CORE_EXPORT DFTU_EXPORT
#define DFTU_UTILITIES_EXPORT DFTU_EXPORT
#endif

#endif  // DFTRACER_UTILS_CORE_COMMON_EXPORT_H
