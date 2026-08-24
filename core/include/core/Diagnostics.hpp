#pragma once

#include <vector>

namespace goblins {

// Числа, зашитые в законы мира: они одинаковы для любого мира и потому не
// вынесены ни в конфигурацию, ни в файл сохранения, а живут константами
// рядом с местом использования (см. комментарий в TerrainParams.hpp). Пока
// система настраивается, их всё равно нужно видеть на экране — иначе
// объяснить поведение симуляции можно только чтением исходников, а
// сопоставить его с картинкой нельзя вовсе.
//
// Отсюда — только ЧТЕНИЕ: константы остаются constexpr там, где лежат, и
// каждый файл лишь перечисляет свои в одной функции, ниже по тексту от
// самих значений. Это осознанно дешевле, чем превращать шесть десятков
// констант в поля рантайм-структуры и тащить их через World, сеть и файл
// сохранения: разделение "настраиваемый параметр против константы закона"
// от такого протаскивания как раз и размылось бы.
struct ConstantInfo {
    // Откуда константа — имя стадии/системы, а не файла: клиент
    // группирует по нему строки в оверлее.
    const char* group;
    // Имя ровно как в исходнике (kWaterSlopeBoost и т.п.) — чтобы
    // увиденное на экране число можно было найти поиском по коду.
    const char* name;
    float value;
};

// Каждая из этих функций определена в том же .cpp, где лежат перечисляемые
// ею константы (они в анонимном namespace и снаружи не видны).
void appendTerrainConstants(std::vector<ConstantInfo>& out);
void appendHydrologyConstants(std::vector<ConstantInfo>& out);
void appendPlantSystemConstants(std::vector<ConstantInfo>& out);
void appendPlantGeneticsConstants(std::vector<ConstantInfo>& out);
void appendGrassSeedingConstants(std::vector<ConstantInfo>& out);
void appendTreeSeedingConstants(std::vector<ConstantInfo>& out);
void appendAnimalSystemConstants(std::vector<ConstantInfo>& out);
void appendAnimalGeneticsConstants(std::vector<ConstantInfo>& out);
void appendAnimalSeedingConstants(std::vector<ConstantInfo>& out);
void appendGoblinSystemConstants(std::vector<ConstantInfo>& out);
void appendGoblinGeneticsConstants(std::vector<ConstantInfo>& out);
void appendGoblinSeedingConstants(std::vector<ConstantInfo>& out);

// Все константы ядра разом, в порядке стадий игрового цикла
// (06_GameLoop.md, п.3): генерация террейна, затем гидрология, затем
// растения, затем животные. Строится заново на каждый вызов — вызывается
// он раз на world_init, а не в цикле.
std::vector<ConstantInfo> coreConstants();

} // namespace goblins
