---
slug: /ru/sql-reference/data-types/tuple
sidebar_position: 54
sidebar_label: Tuple(T1, T2, ...)
---

# Tuple(T1, T2, ...) {#tuplet1-t2}

Кортеж из элементов любого [типа](index.md#data_types). Элементы кортежа могут быть одного или разных типов.

Кортежи используются для временной группировки столбцов. Столбцы могут группироваться при использовании выражения IN в запросе, а также для указания нескольких формальных параметров лямбда-функций. Подробнее смотрите разделы [Операторы IN](../../sql-reference/data-types/tuple.md), [Функции высшего порядка](../../sql-reference/functions/index.md#higher-order-functions).

Кортежи могут быть результатом запроса. В этом случае, в текстовых форматах кроме JSON, значения выводятся в круглых скобках через запятую. В форматах JSON, кортежи выводятся в виде массивов (в квадратных скобках).

## Создание кортежа {#sozdanie-kortezha}

Кортеж можно создать с помощью функции

``` sql
tuple(T1, T2, ...)
```

Пример создания кортежа:

``` sql
SELECT tuple(1,'a') AS x, toTypeName(x)
```

``` text
┌─x───────┬─toTypeName(tuple(1, 'a'))─┐
│ (1,'a') │ Tuple(UInt8, String)      │
└─────────┴───────────────────────────┘
```

## Особенности работы с типами данных {#osobennosti-raboty-s-tipami-dannykh}

При создании кортежа «на лету» ClickHouse автоматически определяет тип всех аргументов как минимальный из типов, который может сохранить значение аргумента. Если аргумент — [NULL](../../sql-reference/data-types/tuple.md#null-literal), то тип элемента кортежа — [Nullable](nullable.md).

Пример автоматического определения типа данных:

``` sql
SELECT tuple(1,NULL) AS x, toTypeName(x)
```

``` text
┌─x────────┬─toTypeName(tuple(1, NULL))──────┐
│ (1,NULL) │ Tuple(UInt8, Nullable(Nothing)) │
└──────────┴─────────────────────────────────┘
```

## Адресация элементов кортежа {#addressing-tuple-elements}

К элементам кортежа можно обращаться по индексу и по имени:

``` sql
CREATE TABLE named_tuples (`a` Tuple(s String, i Int64)) ENGINE = Memory;

INSERT INTO named_tuples VALUES (('y', 10)), (('x',-10));

SELECT a.s FROM named_tuples;

SELECT a.2 FROM named_tuples;
```

Результат:

``` text
┌─a.s─┐
│ y   │
│ x   │
└─────┘

┌─tupleElement(a, 2)─┐
│                 10 │
│                -10 │
└────────────────────┘
```
