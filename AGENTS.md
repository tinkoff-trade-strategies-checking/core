# core

C++ ядро симуляции рынка проекта «Т-Инвестиции: тренажёр и лаборатория стратегий». Здесь проходит рыночный цикл: время, заявки, исполнение, портфель, метрики.

Общие знания — в репозитории `documentation` (в рабочем пространстве рядом: `../documentation/`, на GitHub: https://github.com/tinkoff-trade-strategies-checking/documentation). Карта документов — `README.md` там; читай только то, что нужно для задачи. Процесс работы и указатели на остальные репозитории — в репозитории `home` (`../home/`), откуда запускаются агенты.

Если документация репозитория противоречит существующему коду, следуй соглашениям, принятым в самом коде, а в финальном ответе укажи каждое найденное несоответствие между документацией и кодом и предложи исправления. Исключение — инварианты из `documentation/invariants.md`: их нарушение в коде считается багом, его не повторяют, а сообщают о нём.

## Стек и связи
- C++20, CMake, vcpkg; язык стратегий `dsl` подключается в CMake через `FetchContent` (`tinkoff-trade-strategies-checking/dsl` по зафиксированному SHA или локальный путь `-DDSL_SOURCE_DIR=<путь>` для офлайн-сборки) и собирается с `-ffp-contract=off` (`tests/build/flags_test.cpp`).
- **Simulation Core** — независимая библиотека с CLI для replay и тестов.
- **Simulation Worker** — gRPC-сервер (Protocol Buffers) и адаптер чтения ClickHouse. Задание от Java: версия данных, период, параметры; ответ: события и результат.
- Читает только ClickHouse (диапазоны крупными блоками, read-only). К MOEX и PostgreSQL не обращается.

## Как работать над задачей
Полностью — `home/workflow.md`. Коротко:
1. Работа идёт по issue с доски https://github.com/orgs/tinkoff-trade-strategies-checking/projects/2. Перед стартом прочитай issue вместе с комментариями: там ход работы и отвергнутые варианты.
2. Прочитай ФТ, INV и решения раздела «Архитектура» в `decisions.md` из задачи. Найди похожий код и переиспользуй его. Отход от принятого решения — только явно, PR в `decisions.md` с причиной.
3. Ветка `<номер-issue>-кратко`, изменения вместе с тестами, PR с `Closes #N`.
4. Не успел закончить — оставь в issue комментарий: что сделано, что осталось, что пробовали и почему отказались.
5. Изменение сделало документы неверными — обнови их в том же PR (этот файл, `status.md`, `decisions.md`).

## Критично
Ядро отвечает за большую часть инвариантов (`documentation/invariants.md`):
- **INV-1.** Пользователь получает только данные, доступные на текущий момент (бары позже курсора наружу не уходят).
- **INV-2, INV-3.** Заявка исполняется не раньше open следующего бара одной моделью: комиссия, биржевой сбор, спред, проскальзывание, лимит ликвидности. Стоп и тейк в одной свече — сначала стоп, сделка помечается.
- **INV-4.** Детерминизм: никаких системных часов, случайности без seed, порядка обхода unordered-контейнеров или многопоточных гонок в расчёте.
- **INV-6.** Деньги — десятичный тип с фиксированной точностью, не `double`; результат должен сходиться с Java до копейки.

## Когда обновлять документы
Общая таблица «что изменилось → что обновить» — `home/workflow.md`, раздел «Когда обновлять документы». Обновляй в том же PR, что и код. Для этого репозитория:
- новая команда CMake, тестов, запуска воркера, CLI replay → «Команды» ниже;
- изменение модели исполнения (порядок, издержки, ликвидность) → `decisions.md`, при необходимости `invariants.md`;
- изменение gRPC-контракта (`.proto`) → `architecture.md` и уведомить `backend`;
- выбор десятичного типа для денег, библиотеки, формата чтения ClickHouse → `decisions.md`, «Архитектура»;
- тест детерминизма или чтения будущего добавлен в CI → колонка «Проверка» в `invariants.md`.

## Стандарты
Стиль C++ — LLVM, решение владельца продукта: `home/standards.md`, раздел «Стиль C++». Конфиги — `.clang-format` (`BasedOnStyle: LLVM`) и `.clang-tidy` (`readability-identifier-naming`); проверяются в CI, падение — ошибка.
- Имена: типы, enum и их значения — `PascalCase`; переменные, параметры и поля (без суффикса `_`) — `PascalCase`; функции и методы — `camelCase`; константы — `k` + `PascalCase`: все `constexpr` (в том числе локальные) и `const` глобальные и статические; пространства имён — `lower_case`; макросы — `UPPER_CASE`; файлы — `snake_case`.
- Поле не может называться как его тип (`Level Level`): `-Wchanges-meaning`. Давай полю смысловое имя (`Severity`, `TimelineRef`, `ReasonCode`).
- `-Wshadow` обязателен: при именах в PascalCase параметр с именем поля молча меняет смысл. Параметру конструктора давай другое имя по смыслу (`Period` для поля `N`), локальной — не называй как глобальный тип.
- Не трогай сгенерированный код protobuf/gRPC и внешние API (макросы GoogleTest, методы protobuf-сообщений вроде `set_units`); имя, обязанное совпасть с внешним API, помечай `// NOLINT(readability-identifier-naming)` с пояснением.
- Structured bindings (`auto [a, b] = ...`) clang-tidy 18 не проверяет: называй их `PascalCase` сам.
- Формат и имена: перед коммитом `scripts/dev.sh format`, а имена приводит `scripts/dev.sh tidy-fix` (после него снова `format`). Форматируй только clang-format 18 через `scripts/dev.sh`, не format-on-save IDE другой версии: расхождение версий ломает `format-check`. Коммит «только формат» — в `.git-blame-ignore-revs`.

## Структура
- `proto/` — источник правды для gRPC-контрактов (`simulation.proto`); меняя его, обнови `architecture.md` и уведоми `backend`.
- `src/core/` — библиотека `simulation_core`: чистый C++20, без gRPC и protobuf; публичные заголовки в `src/core/include/core/<область>/`. Область — каталог `src/core/<область>/` с исходниками и `tests/<область>/` с `*_test.cpp`, у обоих свой `CMakeLists.txt` с `file(GLOB CONFIGURE_DEPENDS)`: новый файл подхватывается сам, новая область — каталогом и именем в списке `foreach` в `src/core/CMakeLists.txt` и `tests/CMakeLists.txt`. Области:
  - `money/` — `Decimal` (9 знаков, `__int128`, `decisions.md`, «Деньги в C++»), `RoundingMode`, перевод в proto (`proto_conv.hpp`).
  - `market/` — `Bar`, `Security`, `BarSource`, `InMemoryBarSource`, ошибки (`MarketDataError`, `SnapshotNotFound`, `SecurityNotFound`, `SecurityParamsMismatch`); `date_utils.hpp` (`daysFromCivil`, `civilFromDays`, `moscowOffsetMs`, `moscowYmd`, `moscowDayStartMs`, `parseIsoDate`, `formatIsoDate` — фиксированная таблица переходов МСК без системной базы tz: UTC+3 с 2014-10-26, UTC+4 в 2011-03-27…2014-10-26, сезонный перевод РФ ранее); `Timeframe`, `Instrument`, `StepBar`, `Timeline` — общая шкала времени сессии (шаги, торговые дни, `afterHalt`, `slice`, `instrumentKey`, `totalBars`; прогрев — `loadWithWarmup`/`withWarmup`, `Timeline::kDefaultWarmupBars` = 200, бары истории до шага 0 с шагами `-W…-1` в `warmupBars()`, срез берёт W шагов перед собой); `CalendarSource`/`InMemoryCalendarSource`.
  - `portfolio/` — `Portfolio`: деньги, позиции, средняя цена, пары «покупка → продажа» по FIFO (`RoundTrip`), `EquityPoint`.
  - `exec/` — модель исполнения (INV-2, INV-3): `CostConfig`, `fillPrice`, `amounts`, `ExecutionEngine` (`validate`, `processStep`), `LiquidityBudget`; `conditional_orders.cpp` — лимит, стоп, стоп-лимит, тейк-профит, трейлинг, OCO, срок «день», `cancelOrder`; `exec_common.hpp` — внутренний заголовок (выбор количества, запись сделки, `markChanged`, `cancelSellOrdersOnPositionClosed`).
  - `engine/` — `SessionEngine` (проигрывание журнала по шагам, кривая стоимости, `positionValues`, остановка по концу периода и банкротству; копия — снимок состояния), `replaySession` и `estimateOrder` (оценка заявки по close последнего бара до курсора без изменения журнала, INV-1, INV-8); типы журнала — `journal.hpp` (`PlaceCmd`, `CancelCmd`, `ReplaceCmd`, `JournalEntry`, `CommandResult`).
  - `digest.cpp` в `engine/` — `digestOf(SessionEngine)`: FNV-1a 64 по сделкам, кривой стоимости и статусу (INV-4).
  - `report/` — `computeReport` (метрики ФТ-17, издержки по видам и `Costs.Total`, кривая просадки `Underwater` — % от пика, 2 знака HALF_UP, бенчмарк, `stopTakeSameBar`); `Fill::cost()` и `Fill::Seq` (номер сделки в журнале) — в `exec/`, `cost`/`cost_pct`/`amount_pct`, `seq` у сделок и пар FIFO, `current_day` ответа `ReplaySession` — core#54; `report/derived.hpp` — проценты портфеля и позиций от начального капитала (`portfolioPcts`, `positionValuePct`, `positionPnlPct`, `CostLedger` — накопленные издержки по шагам), `instrumentChange` (изменение цены за шаг и период по барам ≤ курсора), `Portfolio::costBasis` (себестоимость открытых лотов), `Metrics.MaxDrawdownDays` (число дней серии просадки), `metrics_input.hpp` (`buildMetricsInput`, `lastCompletedDayYmd` — единая сборка `MetricsInput` для воркера и `sim_cli`), `benchmark.hpp` (`BenchmarkSeries`, `benchmarkRange`, `benchmarkEquity` — кривая «капитал в индексе» по полностью пройденным дням), `ruonia.hpp` (`DailyRate`, `RuoniaSeries`, `MatchedRuonia`, `matchRuoniaRates` — ставки RUONIA_{d-1} по календарным дням Actual/365 с forward fill; `findPreviousTradingDay`, `previousTradingDay` — поиск предыдущего торгового дня по календарю с фолбэком на прогрев).
  - `blind/` — `blind.hpp`: расчёты слепого режима (`blindP0`, `resolveBlindOrderPrices`, `computeBlindScales`, `scaleBlindPrice`, `scaleBlindVolume`, `toPercentOfCapital`, `toBlindBar`, `toBlindPortfolio`, `toBlindOrder`, `toBlindFill`, `toBlindStepEvents`, `toBlindReport`; цены `×100/P0`, `V_rel`, суммы в % начального капитала, `lotValueAtStart`, 2 знака HALF_UP, без будущего INV-1, без реальных меток времени и тикеров INV-7 — в proto только шаг и индекс бумаги `0..N-1`; до появления P0 по первому бару ≤ курсора бумага отсутствует в `history`/`events`/`orders` слепого вида целиком; при `OrderSpec.IsBlindPrice = true` `resolveBlindOrderPrices` в `estimateOrder` (до валидации) переводит цены в рубли `P_blind·P0/100`, округляя к `PriceStep` не в пользу трейдера — `CEILING` для покупки, `FLOOR` для продажи, сбрасывая `IsBlindPrice` и возвращая `OrderEstimate::ResolvedOrder` / `EstimateOrderResponse.resolved_order` для записи в журнал; `ReplaySession` / `ExecutionEngine::validate` отклоняет команды журнала с `IsBlindPrice = true` как `Rejected` с `Reason::InvalidParams` и полем `"isBlindPrice"`).
  - `analysis/` — `period_classifier.hpp`: классификатор категории периода (`classifyPeriods`, `classifyPeriodAt`: σ по `ln(C_d/C_{d-1})·√252`, MDD по дневным close IMOEX, пороги P33/P80 линейной интерполяцией Type 7 `q·(N−1)` по всему снапшоту, при совпадении условий Calm и Stress — например, P33 = P80 — побеждает Stress, порядок по дате INV-4).
  - `strategy/` — интеграция `dsl` и проверки ФТ-14: `price_bridge.hpp` (`toDslBar`, `rawPriceToDecimal` — перевод `double` → `Decimal` через `std::to_chars` и округление `HALF_UP` к `PriceStep`, INV-6), `runtime_detectors.hpp` (`combineCheckLevel`, `staticLookaheadLevel`, `detectGuessedGaps` `W-RT01`, `detectTooSmoothResult` `W-RT02`, `applyRuntimeChecks`), `strategy_runner.hpp` (`runStrategy`, `buildStrategyReport` — прогон стратегии поверх `SessionEngine` с прогревом и полями `PortfolioSnapshot::{BarsInPosition,PositionReopenedOnBar}`, кооперативной отменой и бюджетом времени НФТ-7, `E-RT01`).
- `src/worker/` — `simulation_worker`: gRPC-сервер; сервис (`worker_service.*`) вынесен в `simulation_worker_lib` для тестов. `Ping` отвечает сам, остальные RPC вызывают обработчики из `handlers.hpp` (`Context{bars, calendar, ruonia, cache}`), каждый в своём `handlers/<имя>.cpp`. Потокобезопасный LRU-кэш шкал времени `TimelineCache` (`timeline_cache.*`) кэширует `core::Timeline` в памяти процесса по ключу `(snapshot_id, бумаги, timeframe, from, to, warmup)` с двойным лимитом — по числу записей (`kDefaultCapacity = 64`) и суммарному числу баров включая прогрев (`kDefaultMaxBars = 2'000'000`, шкалы крупнее лимита не кэшируются) — и ответы календаря `previousTradingDay` по ключу `(snapshot_id, FirstYmd)` для выполнения НФТ-7 (шаг плеера p95 ≤ 300 мс; повторный вызов не обращается к ClickHouse). Готовы все RPC контракта: `Ping`, `ReplaySession`, `GetBars`, `EstimateOrder`, `ClassifyPeriods`, `CheckStrategy`. Ошибки ядра переводятся в gRPC-статус одной функцией `statusFromCurrentException` (`convert/market.*`): нет данных или расхождение `lot_size`/`price_step` с `securities` (`SnapshotNotFound`, `SecurityNotFound`, `SecurityParamsMismatch`) — `FAILED_PRECONDITION`, интервал недоступен (`IntervalUnavailable`) — `NOT_FOUND`, ошибка параметров (`std::invalid_argument`) — `INVALID_ARGUMENT`, нарушение целостности движка (`IntegrityViolation`) — `INTERNAL` с явным префиксом `E-RT01: ...`, ClickHouse недоступен (`ClickHouseError`) — `UNAVAILABLE`, прочие ошибки данных (`MarketDataError`) и остальные исключения — `INTERNAL`. Общие конвертеры — `convert/{market,state,report,blind}.*` (`toSessionSpec`, `loadTimeline`, `metricsInput` -> `core::buildMetricsInput`; `loadTimeline` читает и прогрев `MarketSpec.warmup_bars`, `appendBars` отдаёт бары с прогревом; `loadBenchmark` — дневной ряд индекса для доходности и кривой `Report.benchmark_equity` (шкала индекса кэшируется по полному периоду сессии, `UpToYmd` обрезает ряд после чтения); `Report.round_trips` — пары FIFO из `Portfolio::roundTrips()` (ключ бумаги, шаги, лоты, цены, P&L до и после издержек); `loadRuonia` — дневной ряд ставок RUONIA; `previousTradingDay` — кэширующая обёртка над `core::findPreviousTradingDay`/`core::previousTradingDay`, торговый день перед первым днём шкалы по календарю снапшота, запасной путь — последний день прогрева; от него Δ и ставка первого дня отчёта. Если раньше в календаре торговых дней нет, Δ первого дня = 1, ставка — последняя ≤ d−1, ставка дня d не берётся никогда, INV-1; `state.*` — перенос `rule_name` и `because` в `JournalEntry` и `Order`; `report.*` — маппинг `CheckLevel` ↔ `pb::Level`, `Finding` и `Report.{level,findings}`; `fillBlindView` — заполняет `ReplaySessionResponse.blind_view` при `ReplaySessionRequest.blind = true`, оставляя рублёвые поля неизменными). Адрес — `--listen`, иначе `$WORKER_ADDRESS`, иначе `0.0.0.0:50051`; бары и календарь — ClickHouse по `$CLICKHOUSE_URL` (не задан — предупреждение в stderr и `127.0.0.1:8123`). В библиотеку автоматически входят `handlers/*.cpp`, `convert/*.cpp` и `timeline_cache.cpp`.
- `src/worker/clickhouse/` — своя библиотека `simulation_clickhouse`: `ClickHouseBarSource` читает бары по HTTP-интерфейсу ClickHouse (libcurl, `readonly=1`; интервалы 1 мин…1 д читаются напрямую, если предрассчитаны, либо агрегируются из минутных свечей `candles` с `interval_sec = 60`), `ClickHouseCalendarSource` — торговый календарь, `ClickHouseRuoniaSource` — дневной ряд RUONIA. Контракт схемы ClickHouse для импортёра:
  - `candles(snapshot_id, secid, interval_sec, ts_open, available_at, open, high, low, close, volume, value)`
  - `securities(snapshot_id, secid, sector, lot_size, price_step)`
  - `trading_calendar(snapshot_id, date, is_trading)`
  - `ruonia(snapshot_id, date, rate)`
  - `snapshots_ready(snapshot_id)`
- `src/cli/` — `sim_cli`: `version`, `replay` (`--fixture` для встроенной фикстуры `fixture-1` или `<файл.json>` для JSON-сценария, вывод отчёта и метрик в JSON; собирает `MetricsInput` через `core::buildMetricsInput` и `core::previousTradingDay` идентично воркеру; поддерживает необязательный `market.benchmark_ticker` — если не задан или отсутствует в снапшоте, включая `--fixture`, `benchmark_return` равен `null`).
- `tests/` — GoogleTest и RapidCheck: `core_tests` (области, `tests/build/flags_test.cpp` — проверка `-ffp-contract=off`) и `worker_tests` (`tests/worker/*_test.cpp` — RPC через сервер в процессе, `worker_test_support.hpp`, сверка `sim_cli` и воркера `cli_worker_parity_test.cpp`; `tests/clickhouse/` — тесты `ClickHouseBarSource`, интеграционные пропускаются без `CLICKHOUSE_URL`, а при `CLICKHOUSE_REQUIRED=1` падают; `tests/money/*_generated_test.cpp` — в `worker_tests`, потому что ему нужен protobuf). Тесты на свойства инвариантов (RapidCheck): INV-6 (`tests/money/decimal_property_test.cpp`), INV-1, INV-2, INV-4, INV-8 (`tests/engine/invariants_property_test.cpp`). Тест детерминизма (INV-4) — `tests/engine/determinism_test.cpp` (`Determinism.SameDigestTwice`: два проигрывания журнала — один `digestOf`). `src/fixtures/support/fixture1.*` (`core_fixtures`) — общие фикстуры, входят в `sim_cli` и оба набора тестов: `fixture1()` — снапшот `fixture-1` с бумагами `FIXA` и `FIXB` (у `FIXB` нет бара на шаге 3).
- `testdata/` — входные файлы тестов, путь — макрос `CORE_TESTDATA_DIR`: `reference-portfolio.md` — эталонная таблица 19 сделок (INV-6), её читает `tests/exec/reference_portfolio_test.cpp`.
- `Dockerfile.dev` — образ для сборки, тестов и проверки стиля (зависимости из apt, `clang-format-18`, `clang-tidy-18`); `Dockerfile` — runtime-образ воркера (multi-stage, с `git` в `build`-слое для `FetchContent` зависимости `dsl`).
- CMake использует только `find_package`, поэтому зависимости можно брать из apt или из vcpkg (`vcpkg.json`).

## Команды
На хосте может не быть компилятора с C++20 — основной путь это Docker. Сборка всегда с `-j2` (мало RAM).

```bash
scripts/dev.sh image   # собрать образ pac-core-dev (один раз)
scripts/dev.sh build   # конфигурация и сборка (пресет debug)
scripts/dev.sh test    # сборка и ctest
scripts/dev.sh shell   # оболочка в контейнере
scripts/dev.sh format        # clang-format по src/ и tests/ (стиль LLVM)
scripts/dev.sh format-check  # проверка формата без изменений, ошибка при нарушениях
scripts/dev.sh tidy          # clang-tidy: именование; ошибка при предупреждениях
scripts/dev.sh tidy-fix      # clang-tidy --fix: автоисправление имён (потом format)
PRESET=release scripts/dev.sh test   # другой пресет; JOBS=4 — другое число потоков
```

Без Docker (Linux с g++ ≥ 11, cmake ≥ 3.25, ninja и `libgrpc++-dev protobuf-compiler-grpc libprotobuf-dev libgtest-dev`):

```bash
cmake --preset debug && cmake --build --preset debug -j2 && ctest --preset debug
```

Через vcpkg (нужен `VCPKG_ROOT`): те же команды с пресетами `vcpkg-debug` / `vcpkg-release`.

Интеграционный тест `ClickHouseBarSource` (`ClickHouseIntegration.*`) идёт на настоящем ClickHouse с фикстурой-снапшотом и пропускается без `CLICKHOUSE_URL` (формат `http://user:password@host:8123/база`); при `CLICKHOUSE_REQUIRED=1` отсутствие `CLICKHOUSE_URL` приводит к падению теста. В CI запускается в джобе `build-test` (сервис `clickhouse`, `CLICKHOUSE_REQUIRED=1`) с SQL-фикстурой `testdata/clickhouse-fixture.sql`. Локальный запуск (Docker):

```bash
docker run -d --rm --name clickhouse-test -p 8123:8123 \
  -e CLICKHOUSE_USER=ttsc -e CLICKHOUSE_PASSWORD=secret -e CLICKHOUSE_DB=ttsc \
  -e CLICKHOUSE_DEFAULT_ACCESS_MANAGEMENT=1 clickhouse/clickhouse-server:24.8
# Подождать старта сервера, затем загрузить фикстуру:
docker exec -i clickhouse-test clickhouse-client --user ttsc --password secret -n < testdata/clickhouse-fixture.sql
# Запуск интеграционного теста:
CLICKHOUSE_URL=http://ttsc:secret@localhost:8123/ttsc build/debug/tests/worker_tests --gtest_filter='ClickHouseIntegration.*'
docker stop clickhouse-test
```

Без gRPC (только ядро, CLI и тесты ядра): `cmake --preset debug -DCORE_BUILD_WORKER=OFF`.

Запуск (из каталога сборки `build/<пресет>/`; в Docker — через `scripts/dev.sh shell`):

```bash
src/worker/simulation_worker --listen 0.0.0.0:50051   # воркер (или WORKER_ADDRESS=...), останавливается по SIGTERM/SIGINT
src/cli/sim_cli replay --fixture                         # прогон встроенной фикстуры fixture-1 (или <файл.json>)
src/cli/sim_cli version
```

Runtime-образ воркера: `docker build -t simulation-worker .` (порт 50051).

CI (`.github/workflows/ci.yml`): джоба `style` (`format-check` и `tidy` в образе `Dockerfile.dev`); джоба `build-test` (сборка с `-Wall -Wextra -Wpedantic -Wshadow -Werror -ffp-contract=off`, INV-4, `-ffast-math` запрещён; `ctest` с сервисом ClickHouse и `CLICKHOUSE_REQUIRED=1`); джоба `docker-image` (сборка runtime-образа и `sim_cli version`); джоба `invariants` (`documentation/scripts/check-invariants.py --core core --frontend frontend` — проверка ссылок `invariants.md`, обязательная проверка ruleset `main`).
