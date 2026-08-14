#pragma once

namespace goblins {

// Все параметры генерации террейна — явно здесь, ни одного зашитого
// числа внутри TerrainGenerator.cpp. core не знает о JSON/конфигурации
// (07_TechStack.md, п.6: core не зависит от server) — это plain-структура
// с разумными значениями по умолчанию; server сам решает, каким её
// заполнить (из своей конфигурации, см. shared/config/Config.hpp
// ::TerrainConfig и маппинг в server/main.cpp).
struct TerrainParams {
    // Частоты фрактального шума (fBm) для каждого слоя. Меньше — крупнее
    // пятна/формы рельефа, больше — мельче и чаще.
    float heightNoiseFrequency = 0.02f;
    float rockNoiseFrequency = 0.05f;
    float compactionNoiseFrequency = 0.04f;
    float moistureNoiseFrequency = 0.03f;
    float mineralsNoiseFrequency = 0.06f;

    // Параметры фрактального шума, общие для всех слоёв.
    int noiseOctaves = 4;
    float noiseLacunarity = 2.0f;
    float noiseGain = 0.5f;

    // Насколько каменистость/утрамбованность поднимают высоту рельефа —
    // вода тем самым физически огибает такие участки, без ручных
    // исключений "здесь реки быть не может".
    float rockHeightBump = 0.35f;
    float compactionHeightBump = 0.25f;

    // Река: явное русло (не эмерджентное накопление стока) — сколько
    // рек, базовая ширина в тайлах (гуляет по руслу за счёт шума),
    // извилистость (0 — прямая линия, 1 — максимальный меандр; у быстрых
    // рек фактически подавляется — см. TerrainGenerator.cpp), базовая
    // глубина воды (тоже с шумом по длине) и верхняя граница скорости
    // течения — у каждой реки своя случайная скорость до этого предела,
    // у пруда скорость всегда 0 (см. WaterComponent).
    int riverCount = 3;
    float riverWidth = 3.0f;
    float riverSinuosity = 0.5f;
    float riverDepth = 0.9f;
    float riverMaxFlowSpeed = 1.0f;

    // Минимальный уклон дна русла: на сколько дно ГАРАНТИРОВАННО
    // понижается на каждый тайл пути от истока к устью. Без него русло —
    // просто траншея постоянной глубины, вырезанная в шумном рельефе:
    // она наследует все его бугры и ямы, вода из истока стекает в
    // ближайший локальный минимум внутри траншеи и стоит там, потому что
    // "вниз по руслу" физически не существует. С ним дно монотонно
    // убывает — река действительно течёт (см. TerrainGenerator.cpp).
    // Там, где рельеф и так падает быстрее, берётся рельеф: это нижняя
    // граница уклона, а не жёсткий профиль.
    float riverBedSlope = 0.01f;

    // Пруды (Priority-Flood): минимальная глубина впадины, чтобы
    // считаться прудом, и допустимый размер связной области в тайлах.
    // maxPondSize = 0 — без верхнего ограничения. pondDepth — средняя
    // глубина воды пруда в тех же единицах, что и riverDepth (глубина
    // тайла, не множитель): у каждого пруда своя форма впадины
    // (Priority-Flood), поэтому глубина внутри пруда не одна и та же
    // всюду, а масштабируется формой впадины так, чтобы СРЕДНЯЯ по пруду
    // глубина равнялась ровно pondDepth — как у реки depthMul даёт шум
    // вдоль русла вокруг среднего riverDepth.
    float minPondDepth = 0.01f;
    int minPondSize = 1;
    int maxPondSize = 0;
    float pondDepth = 0.9f;

    // Влажность: во сколько тайлов спадает примерно вдвое влияние воды
    // на влажность, насколько вода её поднимает и насколько каменистость
    // снижает (камень хуже держит влагу).
    float moistureFalloff = 8.0f;
    float waterMoistureBoost = 0.7f;
    float rockMoistureReduction = 0.3f;

    // Минералы (SoilComponent.minerals, целое число): на обычной почве —
    // шум (mineralsNoiseFrequency выше), в среднем по карте дающий
    // mineralsAverage; на клетках реки — не шум, а всегда одно и то же
    // фиксированное значение riverMinerals (см. TerrainGenerator.cpp).
    // Дальнейшее движение минералов между тайлами по правилу песочной
    // кучи — HydrologySystem, не генерация.
    float mineralsAverage = 10.0f;
    int riverMinerals = 10;

    // Порог влажности для правила переноса минералов (см.
    // WorldPropertiesComponent) — свойство мира: генерация выбирает его
    // один раз и записывает на World Entity, дальше HydrologySystem
    // только читает, никогда не меняет (06_GameLoop.md, п.1a).
    float mineralMoistureThreshold = 0.5f;

    // Источники воды (WaterSourceComponent, см. TerrainGenerator.cpp):
    // помимо истока каждой сгенерированной реки (получает источник
    // автоматически, по одному на реку), это ещё waterSourceCount
    // "родников" в случайных точках карты. waterEvaporationRate и
    // waterSourceStrength — тоже свойства мира (как mineralMoistureThreshold
    // выше): выбираются здесь, при генерации, и HydrologySystem их
    // только читает. waterSourceStrength — абсолютный приток (глубина за
    // тик), не множитель waterEvaporationRate: не должен зависеть от
    // того, насколько маленькое испарение настроено.
    int waterSourceCount = 3;
    float waterEvaporationRate = 0.00004f;
    float waterSourceStrength = 0.05f;

    // Скорость растекания воды на ровном месте и добавка за уклон (см.
    // WorldPropertiesComponent) — тоже свойства мира: выбираются здесь,
    // при генерации, HydrologySystem только читает.
    float waterFlowRate = 0.3f;
    float waterSlopeBoost = 5.0f;

    // Эрозия: скорость вымывания породы потоком и потолок выемки
    // относительно соседа (см. WorldPropertiesComponent) — тоже свойства
    // мира: выбираются здесь, при генерации, HydrologySystem только
    // читает.
    float soilErosionRate = 0.05f;
    float maxErosionDepth = 0.5f;
    float erosionSpreadRate = 0.05f;

    // Доля глубины, стекающая за край карты за тик у тайлов на самой
    // границе Области (см. WorldPropertiesComponent) — тоже свойство
    // мира: выбирается здесь, при генерации, HydrologySystem только
    // читает.
    float edgeDrainRate = 0.01f;
};

} // namespace goblins
