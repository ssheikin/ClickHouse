DROP TABLE IF EXISTS t1;

SET allow_experimental_statistic = 1;
SET allow_statistic_optimize = 1;

CREATE TABLE t1
(
    a Float64 STATISTIC(tdigest),
    b Int64 STATISTIC(tdigest),
    c Int64 STATISTIC(tdigest, uniq),
    pk String,
) Engine = MergeTree() ORDER BY pk
SETTINGS min_bytes_for_wide_part = 0;

SHOW CREATE TABLE t1;

INSERT INTO t1 select number, -number, number/1000, generateUUIDv4() FROM system.numbers LIMIT 10000;
INSERT INTO t1 select 0, 0, 11, generateUUIDv4();

SELECT 'After insert';
SELECT explain FROM (EXPLAIN actions=1 SELECT count(*) FROM t1 WHERE b < 10 and c = 0 and a < 10) WHERE explain LIKE '%Prewhere%' OR explain LIKE '%Filter column%';
SELECT explain FROM (EXPLAIN actions=1 SELECT count(*) FROM t1 WHERE b < 10 and c = 11 and a < 10) WHERE explain LIKE '%Prewhere%' OR explain LIKE '%Filter column%';
OPTIMIZE TABLE t1 FINAL;

SELECT 'After merge';
SELECT explain FROM (EXPLAIN actions=1 SELECT count(*) FROM t1 WHERE b < 10 and c = 0 and a < 10) WHERE explain LIKE '%Prewhere%' OR explain LIKE '%Filter column%';
SELECT explain FROM (EXPLAIN actions=1 SELECT count(*) FROM t1 WHERE b < 10 and c = 11 and a < 10) WHERE explain LIKE '%Prewhere%' OR explain LIKE '%Filter column%';

SELECT 'After modify TDigest';
ALTER TABLE t1 MODIFY STATISTIC c TYPE TDigest;
ALTER TABLE t1 MATERIALIZE STATISTIC c;

SELECT explain FROM (EXPLAIN actions=1 SELECT count(*) FROM t1 WHERE b < 10 and c = 11 and c = 0 and a < 10) WHERE explain LIKE '%Prewhere%' OR explain LIKE '%Filter column%';
SELECT explain FROM (EXPLAIN actions=1 SELECT count(*) FROM t1 WHERE b < 10 and c < -1 and a < 10) WHERE explain LIKE '%Prewhere%' OR explain LIKE '%Filter column%';


ALTER TABLE t1 DROP STATISTIC c;

SELECT 'After drop';
SELECT explain FROM (EXPLAIN actions=1 SELECT count(*) FROM t1 WHERE b < 10 and c = 11 and c = 0 and a < 10) WHERE explain LIKE '%Prewhere%' OR explain LIKE '%Filter column%';
SELECT explain FROM (EXPLAIN actions=1 SELECT count(*) FROM t1 WHERE b < 10 and c < -1 and a < 10) WHERE explain LIKE '%Prewhere%' OR explain LIKE '%Filter column%';

DROP TABLE IF EXISTS t1;
