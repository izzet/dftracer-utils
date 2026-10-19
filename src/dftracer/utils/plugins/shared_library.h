#ifndef DFTRACER_UTILS_PLUGINS_SHARED_LIBRARY_H
#define DFTRACER_UTILS_PLUGINS_SHARED_LIBRARY_H

#include <dftracer/utils/core/common/filesystem.h>

#include <string>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace dftracer::utils::plugins::detail {

inline void close_library(void* handle) {
#ifdef _WIN32
    FreeLibrary(static_cast<HMODULE>(handle));
#else
    dlclose(handle);
#endif
}

inline void* open_library(const std::string& path) {
#ifdef _WIN32
    std::error_code ec;
    auto absolute = fs::absolute(fs::u8path(path), ec);
    if (ec) {
        SetLastError(ERROR_PATH_NOT_FOUND);
        return nullptr;
    }
    absolute.make_preferred();
    return LoadLibraryExW(
        absolute.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
#else
    return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

inline void* library_symbol(void* handle, const char* name) {
#ifdef _WIN32
    return reinterpret_cast<void*>(
        GetProcAddress(static_cast<HMODULE>(handle), name));
#else
    return dlsym(handle, name);
#endif
}

inline std::string library_error() {
#ifdef _WIN32
    return std::system_category().message(static_cast<int>(GetLastError()));
#else
    const char* error = dlerror();
    return error ? error : "unknown dlopen error";
#endif
}

}  // namespace dftracer::utils::plugins::detail

#endif  // DFTRACER_UTILS_PLUGINS_SHARED_LIBRARY_H
