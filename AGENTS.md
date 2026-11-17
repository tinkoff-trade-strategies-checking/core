# core

C++ ядро симуляции рынка проекта «Т-Инвестиции: тренажёр и лаборатория стратегий». Здесь проходит рыночный цикл: время, индикаторы, исполнение DSL, заявки, риск-проверки, портфель.

Общие знания — в репозитории `documentation` (в рабочем пространстве рядом: `../documentation/`, на GitHub: https://github.com/tinkoff-trade-strategies-checking/documentation). Карта документов — `README.md` там; читай только то, что нужно для задачи. Процесс работы и указатели на остальные репозитории — в репозитории `home` (`../home/`), откуда запускаются агенты.

Если документация репозитория противоречит существующему коду, следуй соглашениям, принятым в самом коде, а в финальном ответе укажи каждое найденное несоответствие между документацией и кодом и предложи исправления. Исключение — инварианты из `documentation/invariants.md`: их нарушение в коде считается багом, его не повторяют, а сообщают о нём.

## Стек и связи
- C++20, CMake, vcpkg.
- **Simulation Core** — независимая библиотека с CLI для replay и тестов.
- **Simulation Worker** — gRPC-сервер (Protocol Buffers) и адаптер чтения ClickHouse. Задание от Java: версия данных, период, параметры; ответ: события и результат.
- Читает только ClickHouse (диапазоны крупными блоками, read-only). К MOEX и PostgreSQL не обращается.

## Как работать над задачей
Полностью — `home/workflow.md`. Коротко:
1. Работа идёт по issue с доски https://github.com/orgs/tinkoff-trade-strategies-checking/projects/2. Перед стартом прочитай issue вместе с комментариями: там ход работы и отвергнутые варианты.
2. Прочитай ФТ, INV и ADR из задачи. Найди похожий код и переиспользуй его. Отход от ADR — только через новый ADR.
3. Ветка `<номер-issue>-кратко`, изменения вместе с тестами, PR с `Closes #N`.
4. Не успел закончить — оставь в issue комментарий: что сделано, что осталось, что пробовали и почему отказались.
5. Изменение сделало документы неверными — обнови их в том же PR (этот файл, `status.md`, `decisions.md`, ADR).

## Критично
Ядро отвечает за большую часть инвариантов (`documentation/invariants.md`):
- **INV-1.** Стратегия и пользователь получают только данные, доступные на текущий момент; чтение будущего обнаруживается до запуска и во время исполнения (OK / предупреждение / ошибка).
- **INV-2, INV-3.** Заявка исполняется не раньше open следующего бара, одной моделью для человека и алгоритма: комиссия, биржевой сбор, спред, проскальзывание, лимит ликвидности. Стоп и тейк в одной свече — сначала стоп, сделка помечается.
- **INV-4.** Детерминизм: никаких системных часов, случайности без seed, порядка обхода unordered-контейнеров или многопоточных гонок в расчёте.
- **INV-6.** Деньги — десятичный тип с фиксированной точностью, не `double`; результат должен сходиться с Java до копейки.
- **INV-9.** Алгоритмы не получают новостей.

## Когда обновлять документы
Общая таблица «что изменилось → что обновить» — `home/workflow.md`, раздел «Когда обновлять документы». Обновляй в том же PR, что и код. Для этого репозитория:
- новая команда CMake, тестов, запуска воркера, CLI replay → «Команды» ниже;
- изменение модели исполнения (порядок, издержки, ликвидность) → `decisions.md`, при необходимости `invariants.md`;
- изменение gRPC-контракта (`.proto`) → `architecture.md` и уведомить `backend`;
- выбор десятичного типа для денег, библиотеки, формата чтения ClickHouse → ADR;
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
  - `money/` — `Decimal` (9 знаков, `__int128`, ADR-0008), `RoundingMode`, перевод в proto (`proto_conv.hpp`).
  - `market/` — `Bar`, `Security`, `BarSource`, `InMemoryBarSource`; `Timeframe`, `Instrument`, `StepBar`, `Timeline` — общая шкала времени сессии (шаги, торговые дни, `afterHalt`, `slice`; прогрев — `loadWithWarmup`/`withWarmup`, бары истории до шага 0 с шагами `-W…-1` в `warmupBars()`, срез берёт W шагов перед собой); `CalendarSource`/`InMemoryCalendarSource`.
  - `portfolio/` — `Portfolio`: деньги, позиции, средняя цена, пары «покупка → продажа» по FIFO (`RoundTrip`), `EquityPoint`.
  - `exec/` — модель исполнения (INV-2, INV-3): `CostConfig`, `fillPrice`, `amounts`, `ExecutionEngine` (`validate`, `processStep`, `processNaive` — ФТ-24 при `naive = true`), `LiquidityBudget`; `conditional_orders.cpp` — лимит, стоп, стоп-лимит, тейк-профит, трейлинг, OCO, срок «день», `cancelOrder`; `exec_common.hpp` — внутренний заголовок (выбор количества, запись сделки).
  - `engine/` — `SessionEngine` (проигрывание журнала по шагам, кривая стоимости, `positionValues`, остановка по концу периода и банкротству; копия — снимок состояния) и `replaySession`; типы журнала — `journal.hpp` (`PlaceCmd`, `CancelCmd`, `ReplaceCmd`, `JournalEntry`, `CommandResult`).
  - `dsl/` — язык стратегий (`dsl.md`): `parser` (лексер, парсер, AST, E-SYN), `checker` (E-TYPE, E-BEC, E-LA01/02, W-LA01, параметры), `linter` (L001 с исправлениями), `indicators`, `interpreter` (`StrategyInstance`: правила на баре → `Intent`; переопределения параметров перепроверяются `check()`, INV-1; `toDecimalPrice`).
  - `run/` — `runBacktest` (`backtest.cpp`: книга заявок правил, снятие продаж при нулевой позиции; `detail::warmUp` — прогрев стратегии на барах истории без торговли, его же зовёт разбор расхождений), `runtime_checks.cpp` (E-RT01, W-RT01, W-RT02), `digest.cpp` (`digestOf`, FNV-1a 64).
  - `report/` — `computeReport` (метрики ФТ-17, издержки по видам, бенчмарк, `stopTakeSameBar`), `benchmark.hpp` (`BenchmarkSeries`, `benchmarkRange`, `benchmarkEquity` — кривая «капитал в индексе» по полностью пройденным дням), типы `Finding`, `CostBreakdown`, `CostFlag`.
  - `analysis/` — `applyStress` (ФТ-23, копия `Timeline` со стрессом), `costBreakdown`/`costFlags` (ФТ-24: Naive → Delay → Fees → Spread → Slippage → Liquidity), `walkForward` (ФТ-22: `paramGrid`, окна по торговым дням, выбор по net-доходности, `chainedReturn`), `analyzeDivergences` (ФТ-18…20: алгоритм на месте человека, цена ошибки копией `SessionEngine`, консенсус, пропущенные сделки).
- `src/worker/` — `simulation_worker`: gRPC-сервер; сервис (`worker_service.*`) вынесен в `simulation_worker_lib` для тестов. `Ping` отвечает сам, остальные RPC вызывают обработчики из `handlers.hpp` (`Context{bars, calendar}`), каждый в своём `handlers/<имя>.cpp`. Готовы все RPC контракта: `CheckStrategy`, `ReplaySession`, `GetBars`, `RunBacktest` (стресс, поток прогресса, отчёт, цепочка издержек; бенчмарк в прогоне со стрессом не преобразуется), `RunWalkForward`, `AnalyzeDivergences`. Ошибки ядра переводятся в gRPC-статус одной функцией `statusFromCurrentException` (`convert/market.*`): нет данных — `FAILED_PRECONDITION`, прочие ошибки данных — `NOT_FOUND`, ошибка DSL или параметров — `INVALID_ARGUMENT`, ClickHouse недоступен — `UNAVAILABLE`, испорченные данные — `INTERNAL`. Общие конвертеры — `convert/{market,state,report,run,diagnostics}.*` (`toSessionSpec`, `loadTimeline`, `metricsInput`, `toProto(RunOutput…)`; `loadTimeline` читает и прогрев `MarketSpec.warmup_bars`, `appendBars` отдаёт бары с прогревом; `loadBenchmark` — дневной ряд индекса для доходности и кривой `Report.benchmark_equity`; `RunResult.journal` — журнал стратегии через `toProto(JournalEntry)`, `ReplaySession` по нему повторяет прогон). Адрес — `--listen`, иначе `$WORKER_ADDRESS`, иначе `0.0.0.0:50051`; бары и календарь — ClickHouse по `$CLICKHOUSE_URL` (не задан — предупреждение в stderr и `127.0.0.1:8123`). В библиотеку автоматически входят `handlers/*.cpp` и `convert/*.cpp`.
- `src/worker/clickhouse/` — своя библиотека `simulation_clickhouse` (ADR-0013): `ClickHouseBarSource` читает свечи по HTTP-интерфейсу ClickHouse (libcurl, `readonly=1`, только `snapshots_ready`, свечи из сделок — через `candles_from_trades`), `ClickHouseCalendarSource` — торговый календарь (`trading_calendar`; календарь строит импортёр по акциям, не по индексу IMOEX).
- `src/cli/` — `sim_cli`: `version`; `replay` пока не реализован (прогоны идут через воркер).
- `tests/` — GoogleTest: `core_tests` (области, `tests/build/flags_test.cpp` — проверка `-ffp-contract=off`) и `worker_tests` (`tests/worker/*_test.cpp` — RPC через сервер в процессе, `worker_test_support.hpp`; `tests/clickhouse/` — тесты `ClickHouseBarSource`, интеграционные пропускаются без `CLICKHOUSE_URL`; `tests/money/*_generated_test.cpp` — в `worker_tests`, потому что ему нужен protobuf). Тест детерминизма (INV-4) — `tests/run/determinism_test.cpp` (`Determinism.SameDigestTwice`). `tests/support/*.cpp` — общие фикстуры, входят в оба набора: `fixture1()` — снапшот `fixture-1` с бумагами `FIXA` и `FIXB` (у `FIXB` нет бара на шаге 3).
- `testdata/` — входные файлы тестов, путь — макрос `CORE_TESTDATA_DIR`: `dsl/reference.lab` (эталонная стратегия из `dsl.md`), `reference-portfolio.md` — эталонная таблица 19 сделок (INV-6), её читает `tests/exec/reference_portfolio_test.cpp`.
- `Dockerfile.dev` — образ для сборки, тестов и проверки стиля (зависимости из apt, `clang-format-18`, `clang-tidy-18`); `Dockerfile` — runtime-образ воркера (multi-stage).
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

Интеграционный тест `ClickHouseBarSource` (`ClickHouseIntegration.*`) идёт на настоящем ClickHouse с фикстурой-снапшотом и пропускается без `CLICKHOUSE_URL` (формат `http://user:password@host:8123/база`). Запуск (из корня рабочего пространства; пароли любые):

```bash
export POSTGRES_PASSWORD=x CLICKHOUSE_PASSWORD=secret
docker compose -p chit -f backend/deploy/docker-compose.yml -f backend/tools/marketdata-import/compose.test.yml up -d clickhouse
CLICKHOUSE_URL=http://ttsc:secret@127.0.0.1:8123/ttsc marketdata-import fixture   # backend/tools/marketdata-import/README.md
# тест внутри Docker-образа, в сети compose (после `scripts/dev.sh build`):
docker run --rm --network chit_default -e CLICKHOUSE_URL=http://ttsc:secret@clickhouse:8123/ttsc   -v "$PWD/core:/src" -v pac-core-build:/src/build -w /src pac-core-dev   build/debug/tests/worker_tests --gtest_filter='ClickHouseIntegration*'
docker compose -p chit -f backend/deploy/docker-compose.yml -f backend/tools/marketdata-import/compose.test.yml down -v
```

Без gRPC (только ядро, CLI и тесты ядра): `cmake --preset debug -DCORE_BUILD_WORKER=OFF`.

Запуск (из каталога сборки `build/<пресет>/`; в Docker — через `scripts/dev.sh shell`):

```bash
src/worker/simulation_worker --listen 0.0.0.0:50051   # воркер (или WORKER_ADDRESS=...), останавливается по SIGTERM/SIGINT
src/cli/sim_cli replay                                 # пока не реализован (прогоны идут через воркер)
src/cli/sim_cli version
```

Runtime-образ воркера: `docker build -t simulation-worker .` (порт 50051).

CI (`.github/workflows/ci.yml`): джоба `style` (`format-check` и `tidy` в образе `Dockerfile.dev`); сборка с `-Wall -Wextra -Wpedantic -Wshadow -Werror -ffp-contract=off` (INV-4; `-ffast-math` запрещён, CMake завершается ошибкой), `ctest`, сборка runtime-образа.
