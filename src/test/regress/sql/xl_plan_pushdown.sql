--ensure that the plans are being pushed to the right subset of nodes
--Sometimes plans get pushed down to more nodes than they really need to.


CREATE TABLE xl_pp (a bigint, b int) DISTRIBUTE BY HASH(a);

INSERT INTO xl_pp SELECT generate_series(1,100), 20;

EXPLAIN VERBOSE SELECT * FROM xl_pp WHERE a = 100;
--This goes to all nodes (incorrect behavior)

EXPLAIN VERBOSE SELECT * FROM xl_pp WHERE a = 100::bigint;
--goes to just one node where 100 is mapped to

--Looks like we test for datatypes for distribution column and constant literals and want them to be of the same type for pushdown to work
--Doesn't look like a necessary restriction.
--100 by default gets typecast-ed to INT
-- Same behavior for SELECT, DELETE and UPDATE right now - (bug) - hence test is failing. 

EXPLAIN VERBOSE INSERT INTO xl_pp (a, b) VALUES (200, 1) ;

EXPLAIN VERBOSE INSERT INTO xl_pp (a, b) VALUES (201::bigint, 1) ;

EXPLAIN VERBOSE UPDATE xl_pp SET b=2 where a=200;

EXPLAIN VERBOSE UPDATE xl_pp SET b=2 where a=200::bigint;

EXPLAIN VERBOSE DELETE FROM xl_pp where a=200;

SELECT * from xl_pp where a=200;

SELECT * from xl_pp where a=200::bigint;

EXPLAIN VERBOSE DELETE FROM xl_pp where a=200;

EXPLAIN VERBOSE DELETE FROM xl_pp where a=200::bigint;

--Testing with MODULO distribution

CREATE TABLE xl_ppm (a INT2, b int) DISTRIBUTE BY MODULO(a);

INSERT INTO xl_ppm SELECT generate_series(1,100), 20;

EXPLAIN VERBOSE SELECT * FROM xl_ppm WHERE a = 100;
--This goes to all nodes (incorrect behavior)

EXPLAIN VERBOSE SELECT * FROM xl_ppm WHERE a = 100::INT2;
--goes to just one node where 100 is mapped to

--Looks like we test for datatypes for distribution column and constant literals and want them to be of the same type for pushdown to work
--Doesn't look like a necessary restriction.
--100 by default gets typecast-ed to INT
-- Same behavior for SELECT, DELETE and UPDATE right now - (bug) - hence test is failing. 

EXPLAIN VERBOSE INSERT INTO xl_ppm (a, b) VALUES (200, 1) ;

EXPLAIN VERBOSE INSERT INTO xl_ppm (a, b) VALUES (201::INT2, 1) ;

EXPLAIN VERBOSE UPDATE xl_ppm SET b=2 where a=200;

EXPLAIN VERBOSE UPDATE xl_ppm SET b=2 where a=200::INT2;

EXPLAIN VERBOSE DELETE FROM xl_ppm where a=200;

SELECT * from xl_ppm where a=200;

SELECT * from xl_ppm where a=200::INT2;

EXPLAIN VERBOSE DELETE FROM xl_ppm where a=200;

EXPLAIN VERBOSE DELETE FROM xl_ppm where a=200::INT2;

DROP TABLE xl_pp;
DROP TABLE xl_ppm;
