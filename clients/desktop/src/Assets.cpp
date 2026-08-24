#include "Assets.hpp"

#include <fstream>
#include <iostream>
#include <map>
#include <sstream>

#include "platform/ExecutablePath.hpp"

namespace Assets {

namespace {

// Расширение файла рисунка. Своё, а не .txt: по нему видно, что файл не
// заметка рядом с ресурсами, а сам ресурс.
constexpr const char* kSpriteExtension = ".spr";

// Разбор файла рисунка. Формат:
//
//   # строки, начинающиеся с решётки, и пустые — пропускаются
//   size <ширина> <высота>
//   frame <имя>
//   <высота строк ровно по ширине знаков>
//   frame <имя>
//   ...
//
// Всякая беда — отсутствие размера, кадр не той высоты, строка не той
// длины — это испорченный ресурс, и читать его дальше нельзя: половина
// рисунка хуже, чем никакого, потому что выглядит как рисунок. Поэтому
// разбор возвращает пустой лист и говорит, где именно споткнулся.
bool parse(const std::filesystem::path& path, std::istream& input, SpriteSheet& out, std::string& error) {
    std::string line;
    int lineNumber = 0;
    std::vector<std::string> current;
    std::string currentName;

    const auto finishFrame = [&]() {
        if (currentName.empty()) {
            return true;
        }
        if (static_cast<int>(current.size()) != out.height) {
            error = "frame '" + currentName + "' has " + std::to_string(current.size()) + " rows, expected " +
                    std::to_string(out.height);
            return false;
        }
        out.names.push_back(currentName);
        out.frames.push_back(current);
        current.clear();
        currentName.clear();
        return true;
    };

    while (std::getline(input, line)) {
        ++lineNumber;
        // Файл мог быть записан в Windows-переводах строк, а прочитан на
        // другой системе: возврат каретки — не знак рисунка.
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty() || line[0] == '#') {
            continue;
        }

        if (line.rfind("size ", 0) == 0) {
            std::istringstream parts(line.substr(5));
            if (!(parts >> out.width >> out.height) || out.width <= 0 || out.height <= 0) {
                error = "line " + std::to_string(lineNumber) + ": bad size";
                return false;
            }
            continue;
        }

        if (line.rfind("frame ", 0) == 0) {
            if (out.width == 0) {
                error = "line " + std::to_string(lineNumber) + ": frame before size";
                return false;
            }
            if (!finishFrame()) {
                return false;
            }
            currentName = line.substr(6);
            continue;
        }

        if (currentName.empty()) {
            error = "line " + std::to_string(lineNumber) + ": picture outside of any frame";
            return false;
        }
        if (static_cast<int>(line.size()) != out.width) {
            error = "line " + std::to_string(lineNumber) + ": row is " + std::to_string(line.size()) +
                    " characters, expected " + std::to_string(out.width);
            return false;
        }
        current.push_back(line);
    }

    if (!finishFrame()) {
        return false;
    }
    if (out.frames.empty()) {
        error = "no frames in '" + path.filename().string() + "'";
        return false;
    }
    return true;
}

SpriteSheet load(const std::string& name) {
    const auto path = directory() / "sprites" / (name + kSpriteExtension);
    SpriteSheet sheet;

    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "Assets: no sprite file '" << path.string() << "'.\n";
        return {};
    }

    std::string error;
    if (!parse(path, file, sheet, error)) {
        std::cerr << "Assets: '" << path.string() << "' is broken (" << error << ").\n";
        return {};
    }
    return sheet;
}

} // namespace

int SpriteSheet::frameIndex(std::string_view name) const {
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (names[i] == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::filesystem::path directory() {
    return goblins::getExecutableDirectory() / "assets";
}

const SpriteSheet& sprites(const std::string& name) {
    // Кэш на всё время работы: рисунок читается с диска один раз. Пустой
    // лист неудачи кладётся сюда же — иначе клиент искал бы пропавший файл
    // каждый кадр и каждый же кадр писал бы об этом в консоль.
    static std::map<std::string, SpriteSheet> cache;
    const auto found = cache.find(name);
    if (found != cache.end()) {
        return found->second;
    }
    return cache.emplace(name, load(name)).first->second;
}

} // namespace Assets
