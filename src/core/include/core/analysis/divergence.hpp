#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "core/dsl/checker.hpp"
#include "core/dsl/interpreter.hpp"
#include "core/engine/journal.hpp"
#include "core/engine/session_engine.hpp"
#include "core/market/timeline.hpp"
#include "core/money/decimal.hpp"
#include "core/portfolio/portfolio.hpp" // Side
#include "core/run/backtest.hpp"        // RunOutput

// Разбор расхождений между действиями человека и алгоритмов (ФТ-18, ФТ-19,
// ФТ-20, `decisions.md`, «Разбор расхождений»). Режим «алгоритм на месте
// человека»: на каждом шаге стратегия видит бары только до этого шага и
// портфель человека на закрытии шага (INV-1). Цена ошибки считается тем же
// движком, что и сессия (INV-3): копия SessionEngine человека на шаге t,
// активные заявки человека сняты, применены заявки алгоритма, затем advance()
// до горизонта. Без часов и случайности, обход только упорядоченных контейнеров
// (INV-4); деньги - Decimal (INV-6).
//
// Действие - подача рыночной или лимитной заявки (замена - новая заявка). Стоп,
// стоп-лимит, тейк-профит, трейлинг-стоп, отмены и заявки с OCO в сравнении не
// участвуют. Действия человека - только принятые движком заявки; заявка,
// поданная и тут же заменённая или снятая на том же шаге, не в счёт - считается
// то, что осталось. Несколько заявок одной стороны на одном шаге по одной
// бумаге - одно действие с суммарным количеством; у шага, где купленное и
// проданное сошлись в ноль, действия нет. Одно и то же лимитное намерение
// правила алгоритма на соседних барах бумаги - одно действие. Окно «момента» W
// = 3 шага шкалы времени.

namespace core {

enum class DivergenceType { Excess, Missed, Size, Timing };

// humanStep / algoStep = -1, если действия на этой стороне нет (тогда и лоты
// 0). Лоты со знаком: покупка +, продажа -. cost = V_алгоритма - V_человека до
// копейки HALF_UP; больше 0 - поступить как алгоритм было бы выгоднее.
// costPercent - cost в процентах (100 = 100 %) от стоимости портфеля человека
// на шаге, на котором считалась цена ошибки (шаг расхождения: у «момента» -
// более поздний из двух шагов); 0, если стоимость портфеля <= 0.
struct Divergence {
  std::string algorithmId;
  int instrument = 0;
  DivergenceType type = DivergenceType::Excess;
  std::int64_t humanStep = -1, algoStep = -1;
  std::int64_t humanLots = 0, algoLots = 0;
  std::int64_t horizonStep = 0;
  Decimal cost;
  double costPercent = 0;
  std::string reason; // `because` правила алгоритма; у «лишнего действия» -
                      // «алгоритм здесь не действовал бы»
};

// Закрытая прибыльная сделка человека (покупка -> продажа по FIFO), вход
// которой алгоритм не сделал. entryStep и exitStep - шаги исполнений, netPnl -
// после издержек.
struct MissedProfitableTrade {
  int instrument = 0;
  std::int64_t entryStep = 0, exitStep = 0, lots = 0;
  Decimal netPnl;
  std::vector<std::string> algorithmIds; // алгоритмы, для которых вход -
                                         // «лишнее действие», в порядке входа
  // Средние цены входа и выхода за штуку: стоимость стороны пары без издержек
  // (RoundTrip::entryValue / exitValue, сумма по сделкам до копейки) / (лоты ×
  // размер лота), 9 знаков HALF_UP. У одной сделки - её цена.
  Decimal entryPrice, exitPrice;
};

// Консенсус алгоритмов. humanAction: действие человека, agreeing из total
// алгоритмов сопоставили его (агрегат «{total - agreeing} из {total} здесь не
// покупали бы»); без humanAction - упущенное действие, agreeing алгоритмов из
// total действовали бы здесь (step - шаг их действия). medianCost - медиана цен
// ошибки по алгоритмам с расхождением (чётное число - среднее двух средних до
// копейки HALF_UP); 0, если расхождений нет. Действия человека, с которыми
// согласились все алгоритмы, в список не попадают.
struct ConsensusItem {
  int instrument = 0;
  std::int64_t step = 0;
  Side side = Side::Buy;
  bool humanAction = false;
  std::int32_t agreeing = 0, total = 0;
  Decimal medianCost;
};

struct AlgorithmInput {
  std::string id;
  dsl::Program program;
  std::vector<dsl::ResolvedParam> params; // результат check() программы
  dsl::ParamValues values; // значения параметров на разбор
};

struct DivergenceResult {
  std::vector<Divergence> divergences; // по алгоритмам (в порядке входа),
                                       // внутри - по шагу, бумаге, типу
  std::vector<MissedProfitableTrade> missed;
  std::vector<ConsensusItem> consensus; // по шагу, бумаге, стороне; сначала
                                        // действия человека, потом упущенные
  std::vector<RunOutput> independent; // независимые портфели: runBacktest
                                      // каждого алгоритма, в порядке входа
};

// human - журнал человека (записи по возрастанию step и seq); lastStep -
// последний шаг пройденного отрезка (конец шкалы или раньше, если сессия
// закончилась досрочно). Разбор идёт только по шагам 0…lastStep: горизонты
// ограничены lastStep, цены ошибки считаются по close(lastStep), независимые
// портфели прогоняются на том же отрезке; бары после lastStep нигде не
// используются (INV-1). Сессия, остановленная банкротством раньше, разбирается
// до остановки. Нарушения - std::invalid_argument: lastStep вне шкалы,
// naive-сессия (ФТ-24 - не про живую игру), больше 5 алгоритмов, повтор id,
// журнал вне порядка или за lastStep, значения параметров вне диапазона.
DivergenceResult
analyzeDivergences(const SessionSpec &spec,
                   std::shared_ptr<const Timeline> timeline,
                   std::span<const JournalEntry> human, std::int64_t lastStep,
                   const std::vector<AlgorithmInput> &algorithms);

} // namespace core
