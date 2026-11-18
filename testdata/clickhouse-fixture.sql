-- Фикстура ClickHouse для интеграционных тестов worker (ClickHouseIntegration.*).
-- Создаёт базу ttsc и наполняет снапшот ec6b3c97-7036-56cf-ba4d-ec4e919e682d.

CREATE DATABASE IF NOT EXISTS ttsc;

CREATE TABLE IF NOT EXISTS ttsc.snapshots_ready (
    snapshot_id UUID
) ENGINE = MergeTree()
ORDER BY snapshot_id;

CREATE TABLE IF NOT EXISTS ttsc.securities (
    snapshot_id UUID,
    secid String,
    sector String,
    lot_size Int64,
    price_step Decimal(18, 6)
) ENGINE = MergeTree()
ORDER BY (snapshot_id, secid);

CREATE TABLE IF NOT EXISTS ttsc.candles (
    snapshot_id UUID,
    secid LowCardinality(String),
    interval_sec UInt32,
    ts_open DateTime64(3, 'UTC'),
    available_at DateTime64(3, 'UTC'),
    open Decimal(18, 6),
    high Decimal(18, 6),
    low Decimal(18, 6),
    close Decimal(18, 6),
    volume Int64,
    value Decimal(18, 6)
) ENGINE = MergeTree()
ORDER BY (snapshot_id, secid, interval_sec, ts_open);

CREATE TABLE IF NOT EXISTS ttsc.trading_calendar (
    snapshot_id UUID,
    date Date,
    is_trading UInt8
) ENGINE = MergeTree()
ORDER BY (snapshot_id, date);

CREATE TABLE IF NOT EXISTS ttsc.ruonia (
    snapshot_id UUID,
    date Date,
    rate Decimal(18, 6)
) ENGINE = MergeTree()
ORDER BY (snapshot_id, date);

-- Снапшот готов к чтению
INSERT INTO ttsc.snapshots_ready (snapshot_id) VALUES
    ('ec6b3c97-7036-56cf-ba4d-ec4e919e682d');

-- Инструменты: SBER, GAZP, LKOH
INSERT INTO ttsc.securities (snapshot_id, secid, sector, lot_size, price_step) VALUES
    ('ec6b3c97-7036-56cf-ba4d-ec4e919e682d', 'SBER', 'Финансы', 1, 0.01),
    ('ec6b3c97-7036-56cf-ba4d-ec4e919e682d', 'GAZP', 'Энергетика', 10, 0.01),
    ('ec6b3c97-7036-56cf-ba4d-ec4e919e682d', 'LKOH', 'Энергетика', 1, 0.5);

-- Дневные свечи SBER (10 торговых дней 19.10.2026 - 30.10.2026)
-- 00:00 MSK = 21:00 UTC предыдущих суток
INSERT INTO ttsc.candles (snapshot_id, secid, interval_sec, ts_open, available_at, open, high, low, close, volume, value) VALUES
    ('ec6b3c97-7036-56cf-ba4d-ec4e919e682d', 'SBER', 86400, '2026-10-18 21:00:00.000', '2026-10-19 21:00:00.000', 287.49, 291.90, 286.42, 290.70, 36293649, 10497642939.88),
    ('ec6b3c97-7036-56cf-ba4d-ec4e919e682d', 'SBER', 86400, '2026-10-19 21:00:00.000', '2026-10-20 21:00:00.000', 290.70, 292.50, 289.10, 291.80, 25000000, 7295000000.00),
    ('ec6b3c97-7036-56cf-ba4d-ec4e919e682d', 'SBER', 86400, '2026-10-20 21:00:00.000', '2026-10-21 21:00:00.000', 291.80, 293.00, 290.50, 292.10, 24000000, 7010400000.00),
    ('ec6b3c97-7036-56cf-ba4d-ec4e919e682d', 'SBER', 86400, '2026-10-21 21:00:00.000', '2026-10-22 21:00:00.000', 292.10, 294.00, 291.00, 293.50, 26000000, 7631000000.00),
    ('ec6b3c97-7036-56cf-ba4d-ec4e919e682d', 'SBER', 86400, '2026-10-22 21:00:00.000', '2026-10-23 21:00:00.000', 293.50, 295.20, 292.80, 294.90, 27000000, 7962300000.00),
    ('ec6b3c97-7036-56cf-ba4d-ec4e919e682d', 'SBER', 86400, '2026-10-25 21:00:00.000', '2026-10-26 21:00:00.000', 294.90, 296.80, 294.00, 296.10, 28000000, 8290800000.00),
    ('ec6b3c97-7036-56cf-ba4d-ec4e919e682d', 'SBER', 86400, '2026-10-26 21:00:00.000', '2026-10-27 21:00:00.000', 296.10, 297.50, 295.50, 297.00, 23000000, 6831000000.00),
    ('ec6b3c97-7036-56cf-ba4d-ec4e919e682d', 'SBER', 86400, '2026-10-27 21:00:00.000', '2026-10-28 21:00:00.000', 297.00, 299.00, 296.80, 298.50, 29000000, 8656500000.00),
    ('ec6b3c97-7036-56cf-ba4d-ec4e919e682d', 'SBER', 86400, '2026-10-28 21:00:00.000', '2026-10-29 21:00:00.000', 298.50, 300.20, 297.90, 299.80, 31000000, 9293800000.00),
    ('ec6b3c97-7036-56cf-ba4d-ec4e919e682d', 'SBER', 86400, '2026-10-29 21:00:00.000', '2026-10-30 21:00:00.000', 299.80, 301.50, 299.00, 300.90, 30000000, 9027000000.00);

-- Минутные свечи SBER за 28.10.2026 для тестов минутных интервалов и агрегации в 5-минутные
INSERT INTO ttsc.candles (snapshot_id, secid, interval_sec, ts_open, available_at, open, high, low, close, volume, value) VALUES
    ('ec6b3c97-7036-56cf-ba4d-ec4e919e682d', 'SBER', 60, '2026-10-28 03:59:00.000', '2026-10-28 04:00:00.000', 298.20, 298.20, 298.20, 298.20, 4755, 1417941.00),
    ('ec6b3c97-7036-56cf-ba4d-ec4e919e682d', 'SBER', 60, '2026-10-28 04:00:00.000', '2026-10-28 04:01:00.000', 298.21, 298.58, 298.21, 298.58, 5000, 1490000.00),
    ('ec6b3c97-7036-56cf-ba4d-ec4e919e682d', 'SBER', 60, '2026-10-28 04:01:00.000', '2026-10-28 04:02:00.000', 298.58, 298.60, 298.50, 298.55, 4000, 1194000.00),
    ('ec6b3c97-7036-56cf-ba4d-ec4e919e682d', 'SBER', 60, '2026-10-28 04:02:00.000', '2026-10-28 04:03:00.000', 298.55, 298.70, 298.50, 298.65, 3500, 1045000.00),
    ('ec6b3c97-7036-56cf-ba4d-ec4e919e682d', 'SBER', 60, '2026-10-28 04:03:00.000', '2026-10-28 04:04:00.000', 298.65, 298.80, 298.60, 298.75, 3800, 1135000.00),
    ('ec6b3c97-7036-56cf-ba4d-ec4e919e682d', 'SBER', 60, '2026-10-28 04:04:00.000', '2026-10-28 04:05:00.000', 298.75, 298.90, 298.70, 298.85, 4200, 1255000.00),
    ('ec6b3c97-7036-56cf-ba4d-ec4e919e682d', 'SBER', 60, '2026-10-28 04:05:00.000', '2026-10-28 04:06:00.000', 298.85, 299.00, 298.80, 298.95, 4500, 1345000.00),
    ('ec6b3c97-7036-56cf-ba4d-ec4e919e682d', 'SBER', 60, '2026-10-28 04:06:00.000', '2026-10-28 04:07:00.000', 298.95, 299.10, 298.90, 299.05, 4100, 1226000.00),
    ('ec6b3c97-7036-56cf-ba4d-ec4e919e682d', 'SBER', 60, '2026-10-28 04:07:00.000', '2026-10-28 04:08:00.000', 299.05, 299.20, 299.00, 299.15, 3900, 1166000.00),
    ('ec6b3c97-7036-56cf-ba4d-ec4e919e682d', 'SBER', 60, '2026-10-28 04:08:00.000', '2026-10-28 04:09:00.000', 299.15, 299.30, 299.10, 299.25, 4300, 1286000.00);

-- Минутные свечи GAZP за 28.10.2026 для детерминированной выборки
INSERT INTO ttsc.candles (snapshot_id, secid, interval_sec, ts_open, available_at, open, high, low, close, volume, value) VALUES
    ('ec6b3c97-7036-56cf-ba4d-ec4e919e682d', 'GAZP', 60, '2026-10-28 07:00:00.000', '2026-10-28 07:01:00.000', 160.00, 160.50, 159.90, 160.20, 1000, 160200.00),
    ('ec6b3c97-7036-56cf-ba4d-ec4e919e682d', 'GAZP', 60, '2026-10-28 07:01:00.000', '2026-10-28 07:02:00.000', 160.20, 160.80, 160.10, 160.50, 1200, 192600.00);

-- Торговый календарь
INSERT INTO ttsc.trading_calendar (snapshot_id, date, is_trading) VALUES
    ('ec6b3c97-7036-56cf-ba4d-ec4e919e682d', '2026-10-19', 1),
    ('ec6b3c97-7036-56cf-ba4d-ec4e919e682d', '2026-10-20', 1),
    ('ec6b3c97-7036-56cf-ba4d-ec4e919e682d', '2026-10-21', 1),
    ('ec6b3c97-7036-56cf-ba4d-ec4e919e682d', '2026-10-22', 1),
    ('ec6b3c97-7036-56cf-ba4d-ec4e919e682d', '2026-10-23', 1),
    ('ec6b3c97-7036-56cf-ba4d-ec4e919e682d', '2026-10-24', 0),
    ('ec6b3c97-7036-56cf-ba4d-ec4e919e682d', '2026-10-25', 0),
    ('ec6b3c97-7036-56cf-ba4d-ec4e919e682d', '2026-10-26', 1),
    ('ec6b3c97-7036-56cf-ba4d-ec4e919e682d', '2026-10-27', 1),
    ('ec6b3c97-7036-56cf-ba4d-ec4e919e682d', '2026-10-28', 1),
    ('ec6b3c97-7036-56cf-ba4d-ec4e919e682d', '2026-10-29', 1),
    ('ec6b3c97-7036-56cf-ba4d-ec4e919e682d', '2026-10-30', 1);

-- Ставки RUONIA (15% годовых)
INSERT INTO ttsc.ruonia (snapshot_id, date, rate) VALUES
    ('ec6b3c97-7036-56cf-ba4d-ec4e919e682d', '2026-10-19', 15.000000),
    ('ec6b3c97-7036-56cf-ba4d-ec4e919e682d', '2026-10-20', 15.000000),
    ('ec6b3c97-7036-56cf-ba4d-ec4e919e682d', '2026-10-21', 15.000000),
    ('ec6b3c97-7036-56cf-ba4d-ec4e919e682d', '2026-10-22', 15.000000),
    ('ec6b3c97-7036-56cf-ba4d-ec4e919e682d', '2026-10-23', 15.000000),
    ('ec6b3c97-7036-56cf-ba4d-ec4e919e682d', '2026-10-24', 15.000000),
    ('ec6b3c97-7036-56cf-ba4d-ec4e919e682d', '2026-10-25', 15.000000),
    ('ec6b3c97-7036-56cf-ba4d-ec4e919e682d', '2026-10-26', 15.000000),
    ('ec6b3c97-7036-56cf-ba4d-ec4e919e682d', '2026-10-27', 15.000000),
    ('ec6b3c97-7036-56cf-ba4d-ec4e919e682d', '2026-10-28', 15.000000),
    ('ec6b3c97-7036-56cf-ba4d-ec4e919e682d', '2026-10-29', 15.000000),
    ('ec6b3c97-7036-56cf-ba4d-ec4e919e682d', '2026-10-30', 15.000000);
