--
--  Test citext datatype
--

--Column of types “citext”, “ltree” cannot be used as distribution column
-- citext - case insensitive text
CREATE TABLE xl_dc26 (
    product_no integer,
    product_id citext PRIMARY KEY,
    name MONEY,
    purchase_date TIMETZ,
    price numeric
) DISTRIBUTE BY HASH (product_id); --fail

