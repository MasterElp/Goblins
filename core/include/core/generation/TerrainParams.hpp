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
    // рек, их ширина в тайлах, извилистость (0 — прямая линия, 1 —
    // максимальный меандр), глубина воды и скорость течения (у пруда
    // скорость всегда 0 — см. WaterComponent).
    int riverCount = 3;
    float riverWidth = 3.0f;
    float riverSinuosity = 0.5f;
    float riverDepth = 1.5f;
    float riverFlowSpeed = 1.0f;

    // Пруды (Priority-Flood): минимальная глубина впадины, чтобы
    // считаться прудом, и допустимый размер связной области в тайлах.
    // maxPondSize = 0 — без верхнего ограничения.
    float minPondDepth = 0.01f;
    int minPondSize = 1;
    int maxPondSize = 0;
    float pondDepthScale = 4.0f;

    // Влажность: во сколько тайлов спадает примерно вдвое влияние воды
    // на влажность, насколько вода её поднимает и насколько каменистость
    // снижает (камень хуже держит влагу).
    float moistureFalloff = 8.0f;
    float waterMoistureBoost = 0.7f;
    float rockMoistureReduction = 0.3f;
};

} // namespace goblins
