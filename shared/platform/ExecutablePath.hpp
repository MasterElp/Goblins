#pragma once

#include <filesystem>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <vector>
#else
#include <unistd.h>
#endif

namespace goblins {

// Папка, где лежит текущий исполняемый файл. Нужна, чтобы конфигурация
// могла жить "рядом с бинарником" независимо от того, откуда программу
// запустили (рабочая директория — не то же самое, что расположение exe).
inline std::filesystem::path getExecutableDirectory() {
#if defined(_WIN32)
    char buffer[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
    if (length == 0 || length == MAX_PATH) {
        return std::filesystem::current_path();
    }
    return std::filesystem::path(buffer).parent_path();
#elif defined(__APPLE__)
    std::vector<char> buffer(1024);
    uint32_t size = static_cast<uint32_t>(buffer.size());
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        buffer.resize(size);
        if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
            return std::filesystem::current_path();
        }
    }
    return std::filesystem::canonical(buffer.data()).parent_path();
#else
    std::error_code ec;
    auto path = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) {
        return std::filesystem::current_path();
    }
    return path.parent_path();
#endif
}

} // namespace goblins
