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
| Порт | этот проект (fungalspores) | [penettrator](https://github.com/penettrator/prototype2-nx) 0.2.0, собран из его исходников с нашими исправлениями |
| Картинка | 720x400 + FSR 1, настраивается (от 640 до 1280) | 800x448 + FSR 1, настраивается (от 640 до 1200) |
| FPS* | меню и ролики 30–60, в игре 22–30 (минимум 17 вместо 9 с версии 1.4.0) | город 25–30 |
| Звук | есть | есть |
| Сохранения | работают (исправлены) | работают |

\* Switch Lite (Mariko), Horizon OC: CPU 1963 / GPU 1152 / RAM 2333 МГц. На штатных частотах ниже.

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
3. Установите через DBI оба NSP из папки `nsp/` и запускайте игры с HOME. Они запускаются через
   `rex-start.nro`: 5 секунд показывается название игры, **+** открывает её настройки (внутреннее разрешение),
   они пишутся прямо в TOML игры.
   Без NSP: удерживайте **R** при запуске любой установленной игры и выберите порт в Homebrew-меню.
   **Не через Альбом** — в режиме апплета не хватает памяти.

### Проверенные издания

- Prototype: Xbox 360, title ID 4156084E (русское издание).
- Prototype 2: `default.xex` SHA-256 `f6caf82c433b0bb20fe701912388b1e7ee8d6b529d8fc275af960ac2e9ddcab8`.

Код рекомпилирован под конкретный `default.xex`, другие издания могут не запуститься.

## Настройки

`prototype.toml` / `prototype2.toml` рядом с NRO. После изменения перезапустите игру.

| Настройка | Prototype | Prototype 2 | |
|---|---|---|---|
| внутреннее разрешение | `proto_render_width` x `_height` 720x400 | `p2_render_width` x `_height` 800x448 | игра сама рисует в меньший кадр (сцена, глубина, эффекты), FSR растягивает его на экран. Высота округляется вниз до кратной 16 |
| `p2_native_max_anisotropy` | 1 | 2 | |
| `p2_native_shadow_max_cascades` | 2 | 4 | каскады теней (4 в самой игре) |
| `p2_ped_max_scale`, `p2_vehicle_max_scale` | - | 0.15, 0.15 | пешеходы и машины (1.0 как в игре); толпа сильнее всего грузит процессор |

`user_language`: 12 — русский, 1 — английский (зависит от вашей копии).

Prototype 2 использует все четыре ядра и греет консоль сильнее первой части. С Horizon OC / sys-clk задайте
ей (`0150524F54320000`) отдельный профиль частот. На старой ревизии (Erista) высокий разгон не используйте.

## Экран настроек (`rex-start/`)

Маленькая homebrew-программа между иконкой HOME и портом, по образцу
[winenx-start](https://github.com/fungalspores/winenx-start): фреймбуфер libnx и системный шрифт через FreeType,
без SDL. Предлагает только внутреннее разрешение (Prototype от 640 до 1280, Prototype 2 от 640x352 до 1200x672, а ещё
Silent Hill: Downpour), остальные строки копирует как есть (с
кавычками и переносами); высота подставляется под выбранную ширину. Ничего не нажали — игра просто запускается.

## Что исправлено

**Prototype** (порт с нуля: ReXGlue + SDK masseffect-nx со слоем Horizon + нативный рендер из Prototype 2 NX):
- Зависания в роликах, переходах и после паузы: главный поток держал критические секции сцены и ждал,
  пока поток отрисовки разберёт переполненный буфер команд, а тому были нужны эти секции. Теперь на время
  ожидания главный поток отпускает свои секции (`src/proto_deadlock_fix.cpp`); если буфер переполняется
  во время разбора, поток отрисовки разбирает его сам на месте.
- «Повреждённые» сохранения: игра пишет в первое слово сейва размер файла и сверяет его при загрузке, а SDK
  отдавал размер, запомненный при открытии (0). Исправлено в `XFile::Write` / `SetLength`.
- Заглушки подписи консоли (`XeKeysConsolePrivateKeySign` / `...SignatureVerification`) отвечали «неверно».
- Оптимизация по профилю (PGO) со своим профилем: сборка с `-DPROTO_PGO=GEN` пишет `/pgo-proto` на карту
  (`src/proto_pgo.cpp`), после игры профиль забирается с консоли, релиз собирается с
  `-DPROTO_PGO=USE -DPROTO_PGO_DATA=<папка>`.
- Настраиваемое внутреннее разрешение: игра жёстко создаёт буфер 1280x720 в `sub_82A0E118`;
  `tools/prepare_proto_codegen.py` переводит его на `proto_render_width` / `proto_render_height`.
- Рендер обновлён до версии из Prototype 2 0.2.0 (кэш текстур, пакетные запросы видимости, исправлено
  исчезновение материалов после долгого открытия карты).
- Замки по адресу вместо одного мьютекса на весь процесс для атомарных участков игры; обращения к памяти
  гостя без `volatile` (из Prototype 2 NX); звук и 30–60 FPS в меню с нативным рендером.

**Prototype 2** (порт penettrator, собран из его исходников под GPL-3.0):
- Те же исправления SDK (сохранения, подписи, критические секции).
- Картинка лучше (см. «Настройки»).
- Без опроса датчиков температуры: открытая на всю игру сессия `ts` совпадала с падением модуля Horizon OC
  `hoc:clk` под нагрузкой (и следом `pcv`/`ptm`/`Bus`).

**Prototype, 1.4.0** (Prototype 2 в релизе 1.4.0 тот же, что в 1.3.0: у него с теми же правками картинка стала
мыльнее, а FPS не вырос, поэтому для него их пока не выпускаем):
- Вернулся PGO. Релизы 1.2.0 и 1.3.0 по ошибке собраны без профиля (выпуск шёл из папки сборки, где он был
  выключен); `tools/make_release.sh` теперь не соберёт раздачу из сборки без профиля.
- Ускоряющие правки сгенерированного кода из [mgr-nx-deko](https://github.com/tuancapoo/mgr-nx-deko)
  (`tools/mgr/`): гостевой `stwcx.` теперь `__atomic_compare_exchange_n` вместо `__sync_bool_compare_and_swap`,
  после которого GCC на A57 ставит `dmb ish` (в Prototype было 611 таких барьеров, осталось 24); разворот байтов
  каждой векторной загрузки и записи делается через REV64 + EXT, которые GCC сворачивает в один TBL с константой,
  вместо загрузки маски, AND и TBL (векторных AND было 9687, стало 1770); перестановки слов с константой стали
  одной перестановкой вместо цепочки вставок. Нижний FPS в бою поднялся примерно с 9 до 17.
- Кеш конвейеров и список прогрева сбрасываются на карту (fsync) перед переименованием: выход через HOME терял
  собранные за сессию конвейеры, и при следующем запуске они собирались заново.
Перед каждой игрой на 3 секунды показывается заставка с благодарностями (пропуск — A/B/+).

## Сборка

Всё собиралось **на Android-телефоне** (Alpine Linux в PRoot, aarch64) без Docker; на обычном Linux так же.
Кратко — в [README.md](README.md#building), раздел Building.

## Благодарности

- **penettrator** — порт Prototype 2 ([prototype2-nx](https://github.com/penettrator/prototype2-nx)). Спасибо!
- **NebadasSwifty** — [masseffect-nx](https://github.com/nebadasSwifty/masseffect-nx), SDK со слоем Horizon.
- **StevensND** — слой Horizon для ReXGlue и [nfsmw-nx](https://github.com/StevensND/nfsmw-nx).
- **R-drg** — [project8-sw](https://github.com/R-drg/project8-sw), основа нативного рендера.
- **tuancapoo**: [mgr-nx-deko](https://github.com/tuancapoo/mgr-nx-deko), ускоряющие проходы по сгенерированному коду в `tools/mgr/`.
- **Tom Clay** и [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk); проект [Xenia](https://xenia.jp).
- **danfromtico** — [mesa-switch](https://github.com/danfromtico/mesa-switch); проект [Mesa](https://mesa3d.org).
- **Skywalker25** — [Forwarder-Mod](https://github.com/Skywalker25/Forwarder-Mod); switchbrew (libnx); devkitPro.
- AMD FidelityFX Super Resolution 1.

## Лицензия

GPL-3.0 (`LICENSE`); сторонние компоненты — под своими лицензиями (`prototype2/THIRD_PARTY_NOTICES.md`).
Prototype — торговая марка её владельцев. Проект не связан с Radical Entertainment, Activision, Microsoft или
Nintendo и не содержит их файлов.
