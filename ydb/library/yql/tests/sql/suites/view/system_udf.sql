/* postgres can not */
/* syntax version 1 */
pragma yt.ViewIsolation = 'true';
USE plato;
SELECT k, s, v FROM Input VIEW system_udf;
