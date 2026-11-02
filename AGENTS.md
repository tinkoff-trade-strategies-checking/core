# core

C++ ядро симуляции рынка проекта «Т-Инвестиции: тренажёр и лаборатория стратегий». Здесь проходит рыночный цикл: время, индикаторы, исполнение DSL, заявки, риск-проверки, портфель.

Общие знания — в репозитории `documentation` (в рабочем пространстве рядом: `../documentation/`, на GitHub: https://github.com/tinkoff-trade-strategies-checking/documentation). Карта документов — `AGENTS.md` там; читай только то, что нужно для задачи.

Если документация репозитория противоречит существующему коду, следуй соглашениям, принятым в самом коде, а в финальном ответе укажи каждое найденное несоответствие между документацией и кодом и предложи исправления. Исключение — инварианты из `documentation/invariants.md`: их нарушение в коде считается багом, его не повторяют, а сообщают о нём.

## Стек и связи
- C++20, CMake, vcpkg.
- **Simulation Core** — независимая библиотека с CLI для replay и тестов.
- **Simulation Worker** — gRPC-сервер (Protocol Buffers) и адаптер чтения ClickHouse. Задание от Java: версия данных, период, параметры; ответ: события и результат.
- Читает только ClickHouse (диапазоны крупными блоками, read-only). К MOEX и PostgreSQL не обращается.

## Как работать над задачей
Полностью — `documentation/workflow.md`. Коротко:
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
Общая таблица «что изменилось → что обновить» — `documentation/workflow.md`, раздел «Когда обновлять документы». Обновляй в том же PR, что и код. Для этого репозитория:
- новая команда CMake, тестов, запуска воркера, CLI replay → «Команды» ниже;
- изменение модели исполнения (порядок, издержки, ликвидность) → `decisions.md`, при необходимости `invariants.md`;
- изменение gRPC-контракта (`.proto`) → `architecture.md` и уведомить `backend`;
- выбор десятичного типа для денег, библиотеки, формата чтения ClickHouse → ADR;
- тест детерминизма или чтения будущего добавлен в CI → колонка «Проверка» в `invariants.md`.

## Структура
- `proto/` — источник правды для gRPC-контрактов (`simulation.proto`); меняя его, обнови `architecture.md` и уведоми `backend`.
- `src/core/` — библиотека `simulation_core`: чистый C++20, без gRPC и protobuf; публичные заголовки в `src/core/include/core/<область>/`. Области: `money market portfolio exec engine dsl run report analysis`. Область — каталог `src/core/<область>/` с исходниками, `tests/<область>/` с `*_test.cpp` и своими `CMakeLists.txt` (`file(GLOB CONFIGURE_DEPENDS)`): новый файл подхватывается сам, новая область — одним каталогом и именем в списке `foreach` в `src/core/CMakeLists.txt` и `tests/CMakeLists.txt`. `money/` — `Decimal` и правила округления; `market/` — рыночные данные: `Bar`, `BarSource`, `InMemoryBarSource`, `BarCursor` (INV-1: наружу только бары с `index <= current`); `Timeline` — общая шкала времени сессии (шаги, торговые дни, `afterHalt`), `CalendarSource`/`InMemoryCalendarSource` — торговый календарь. `analysis/` — `applyStress` (стресс-сценарий ФТ-23: гэп, волатильность, объём; копия `Timeline` со стрессом). `portfolio/` — `Portfolio` (деньги, позиции, сделки FIFO). `exec/` — модель исполнения (INV-2, INV-3): `CostConfig`, `fillPrice`, `amounts`, `ExecutionEngine` (`validate`, `processStep` — рыночные заявки, `processNaive` — ФТ-24 только при `naive = true`), `LiquidityBudget` (бюджет ликвидности шага).
- `src/worker/` — `simulation_worker`: gRPC-сервер; сервис (`worker_service.*`) вынесен в `simulation_worker_lib` для тестов. `Ping` отвечает сам, остальные RPC вызывают обработчики из `handlers.hpp` (`Context{bars, calendar}`; каждый в своём `handlers/<имя>.cpp`; `CheckStrategy` готов (parse -> check + lint, конвертеры в `convert/diagnostics.*`, уровень без учёта L001), остальные пока `UNIMPLEMENTED`). Адрес — `--listen`, иначе `$WORKER_ADDRESS`, иначе `0.0.0.0:50051`; бары и календарь — ClickHouse по `$CLICKHOUSE_URL`. В библиотеку автоматически входят `handlers/*.cpp`, `convert/*.cpp` и `clickhouse/*.cpp`. `clickhouse/` — `ClickHouseBarSource` читает свечи по HTTP-интерфейсу ClickHouse (libcurl, только `snapshots_ready`, свечи из сделок — через `candles_from_trades`). `ClickHouseCalendarSource` читает торговый календарь версии данных (`trading_calendar`, только `snapshots_ready`; календарь строит импортёр по акциям, не по индексу IMOEX).
- `src/cli/` — `sim_cli`: CLI для replay (сейчас заглушка).
- `tests/` — GoogleTest: `core_tests` (в том числе тест детерминизма, INV-4, и `tests/build/flags_test.cpp` — проверка `-ffp-contract=off`) и `worker_tests` (Ping через gRPC в процессе; `tests/worker/*_test.cpp`, в том числе `handlers_scaffold_test.cpp`; тесты `ClickHouseBarSource` в `tests/clickhouse/`, интеграционные пропускаются без `CLICKHOUSE_URL`; `tests/money/*_generated_test.cpp` собирается в `worker_tests`, потому что ему нужен protobuf). `tests/support/*.cpp` — общие фикстуры (`fixture1()` — снапшот `fixture-1`), входят в оба набора.
- `testdata/` — входные файлы тестов (программы DSL, эталонный портфель), путь — макрос `CORE_TESTDATA_DIR`.
- `Dockerfile.dev` — образ для сборки и тестов (зависимости из apt); `Dockerfile` — runtime-образ воркера (multi-stage).
- CMake использует только `find_package`, поэтому зависимости можно брать из apt или из vcpkg (`vcpkg.json`).

## Команды
На хосте может не быть компилятора с C++20 — основной путь это Docker. Сборка всегда с `-j2` (мало RAM).

```bash
scripts/dev.sh image   # собрать образ pac-core-dev (один раз)
scripts/dev.sh build   # конфигурация и сборка (пресет debug)
scripts/dev.sh test    # сборка и ctest
scripts/dev.sh shell   # оболочка в контейнере
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

CI (`.github/workflows/ci.yml`): сборка с `-Wall -Wextra -Wpedantic -Werror -ffp-contract=off` (INV-4; `-ffast-math` запрещён, CMake завершается ошибкой), `ctest`, сборка runtime-образа.
