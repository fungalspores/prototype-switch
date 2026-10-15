# [PROTOTYPE] и [PROTOTYPE 2] для Nintendo Switch

[English](README.md)

Нативные порты **Xbox 360**-версий Prototype (2009) и Prototype 2 (2012) на Nintendo Switch, сделанные
**статической рекомпиляцией** ([ReXGlue](https://github.com/rexglue/rexglue-sdk)): код игр переведён из PowerPC
в C++ и собран под ARM64 для Horizon OS, а их команды Direct3D рисуются через Vulkan (NVK/Mesa).
Без эмулятора и без эмуляции видеочипа Xbox 360.

**Бесплатно. Не для продажи.** Новости: [t.me/fspores](https://t.me/fspores)

В репозитории **нет файлов игр** — нужна своя копия игр для Xbox 360.

## Скачать

Готовые сборки (NRO, настройки, NSP-форвардеры для HOME) — в разделе **Releases**.

## Что получается

| | Prototype | Prototype 2 |
|---|---|---|
| Порт | этот проект (fungalspores) | [penettrator](https://github.com/penettrator/prototype2-nx), собран из его исходников с нашими исправлениями |
| Картинка | родные 1280x720 | 960x544 + FSR 1 (в оригинальном релизе 640x352) |
| FPS* | меню и ролики 30–60, в игре 11–25 | город 25–30 |
| Звук | есть | есть |
| Сохранения | работают (исправлены) | работают |

\* Switch OLED (Mariko), sys-clk / Horizon OC: CPU 1785 / GPU 768–1152 / RAM 2333 МГц. На штатных частотах ниже.

## Установка

Нужны Atmosphère 1.8.0+ с sigpatches, DBI (или другой установщик NSP), на SD ~7 ГБ (Prototype) и ~8 ГБ
(Prototype 2). FAT32 подходит.

1. Скопируйте папку `switch/` из релиза в корень SD-карты:
   ```
   sdmc:/switch/prototype-nx/prototype.nro, prototype.toml
   sdmc:/switch/prototype2-nx/prototype2.nro, prototype2.toml
   ```
2. Положите файлы своей игры:
   - Prototype → `sdmc:/switch/prototype-nx/game/` (внутри `default.xex`)
   - Prototype 2 → `sdmc:/switch/prototype2-nx/game_root/` (внутри `default.xex`)

   Из ISO: `extract-xiso -x "Prototype.iso" -d game`. Папка `$SystemUpdate` не нужна.
3. Установите через DBI оба NSP из папки `nsp/` и запускайте игры с HOME.
   Без NSP: удерживайте **R** при запуске любой установленной игры и выберите порт в Homebrew-меню.
   **Не через Альбом** — в режиме апплета не хватает памяти.

### Проверенные издания

- Prototype: Xbox 360, title ID 4156084E (русское издание).
- Prototype 2: `default.xex` SHA-256 `f6caf82c433b0bb20fe701912388b1e7ee8d6b529d8fc275af960ac2e9ddcab8`.

Код рекомпилирован под конкретный `default.xex`, другие издания могут не запуститься.

## Настройки

`prototype.toml` / `prototype2.toml` рядом с NRO. После изменения перезапустите игру.

Графика Prototype 2 (наша / в оригинальном релизе):

| Настройка | Наша | Оригинал | |
|---|---|---|---|
| `p2_render_width` x `p2_render_height` | 960x544 | 640x352 | внутреннее разрешение, растягивается FSR |
| `p2_native_texture_lod_bias` | 0.5 | 1.0 | меньше = резче текстуры |
| `p2_native_max_anisotropy` | 2 | 1 | |
| `p2_geo_lod_bias_scale` | 2.0 | 4.0 | меньше = детальнее модели |
| `p2_shadow_range_scale` | 0.5 | 0.3 | |
| `p2_ped_max_scale`, `p2_vehicle_max_scale` | 0.3, 0.15 | 0.3, 0.15 | толпа сильнее всего грузит процессор |

`user_language`: 12 — русский, 1 — английский (зависит от вашей копии).

Prototype 2 использует все четыре ядра и греет консоль сильнее первой части. С Horizon OC / sys-clk задайте
ей (`0150524F54320000`) отдельный профиль частот. На старой ревизии (Erista) высокий разгон не используйте.

## Что исправлено

**Prototype** (порт с нуля: ReXGlue + SDK masseffect-nx со слоем Horizon + нативный рендер из Prototype 2 NX):
- Зависания в роликах, переходах и после паузы: главный поток держал критические секции сцены и ждал,
  пока поток отрисовки разберёт переполненный буфер команд, а тому были нужны эти секции. Теперь на время
  ожидания главный поток отпускает свои секции (`src/proto_deadlock_fix.cpp`); если буфер переполняется
  во время разбора, поток отрисовки разбирает его сам на месте.
- «Повреждённые» сохранения: игра пишет в первое слово сейва размер файла и сверяет его при загрузке, а SDK
  отдавал размер, запомненный при открытии (0). Исправлено в `XFile::Write` / `SetLength`.
- Заглушки подписи консоли (`XeKeysConsolePrivateKeySign` / `...SignatureVerification`) отвечали «неверно».
- Замки по адресу вместо одного мьютекса на весь процесс для атомарных участков игры; обращения к памяти
  гостя без `volatile` (из Prototype 2 NX); звук и 30–60 FPS в меню с нативным рендером.

**Prototype 2** (порт penettrator, собран из его исходников под GPL-3.0):
- Те же исправления SDK (сохранения, подписи, критические секции).
- Картинка лучше (см. «Настройки»).
- Без опроса датчиков температуры: открытая на всю игру сессия `ts` совпадала с падением модуля Horizon OC
  `hoc:clk` под нагрузкой (и следом `pcv`/`ptm`/`Bus`).

Перед каждой игрой на 3 секунды показывается заставка с благодарностями (пропуск — A/B/+).

## Сборка

Всё собиралось **на Android-телефоне** (Alpine Linux в PRoot, aarch64) без Docker; на обычном Linux так же.
Кратко — в [README.md](README.md#building), раздел Building.

## Благодарности

- **penettrator** — порт Prototype 2 ([prototype2-nx](https://github.com/penettrator/prototype2-nx)). Спасибо!
- **NebadasSwifty** — [masseffect-nx](https://github.com/nebadasSwifty/masseffect-nx), SDK со слоем Horizon.
- **StevensND** — слой Horizon для ReXGlue и [nfsmw-nx](https://github.com/StevensND/nfsmw-nx).
- **R-drg** — [project8-sw](https://github.com/R-drg/project8-sw), основа нативного рендера.
- **Tom Clay** и [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk); проект [Xenia](https://xenia.jp).
- **danfromtico** — [mesa-switch](https://github.com/danfromtico/mesa-switch); проект [Mesa](https://mesa3d.org).
- **Skywalker25** — [Forwarder-Mod](https://github.com/Skywalker25/Forwarder-Mod); switchbrew (libnx); devkitPro.
- AMD FidelityFX Super Resolution 1.

## Лицензия

GPL-3.0 (`LICENSE`); сторонние компоненты — под своими лицензиями (`prototype2/THIRD_PARTY_NOTICES.md`).
Prototype — торговая марка её владельцев. Проект не связан с Radical Entertainment, Activision, Microsoft или
Nintendo и не содержит их файлов.
