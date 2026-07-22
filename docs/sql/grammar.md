# Cairn SQL v1 Grammar Reference

This document outlines the official v1 SQL grammar subset supported by Cairn.

## Keywords

The parser treats the following keywords as reserved and case-insensitive:

- **DML/QL**: `SELECT`, `FROM`, `WHERE`, `GROUP`, `BY`, `HAVING`
- **DDL**: `CREATE`, `DROP`, `ALTER`, `TABLE`, `INDEX`, `ADD`, `COLUMN`, `ON`
- **Types**: `BOOLEAN`, `TINYINT`, `SMALLINT`, `INT`, `INTEGER`, `BIGINT`, `FLOAT`, `DOUBLE`, `VARCHAR`, `DATETIME`
- **Expressions & Operators**: `AS`, `NULL`, `NOT`, `AND`, `OR`, `DISTINCT`, `TRUE`, `FALSE`
- **Aggregates**: `COUNT`, `SUM`, `AVG`, `MIN`, `MAX`

---

## Data Types

Cairn supports the following SQL data types for table columns:

- `BOOLEAN`
- `TINYINT`
- `SMALLINT`
- `INT` or `INTEGER`
- `BIGINT`
- `FLOAT`
- `DOUBLE`
- `VARCHAR`
- `DATETIME`

---

## Identifiers

Cairn's identifiers name tables, columns, indexes, and aliases:
| Flavor | Description | Example |
|:--|:--|:--|
|Unquoted | Standard alphanumeric starting with a letter or underscore, excluding reserved keywords | `users`, `created_at` |
|Backticked | Enclosed in backticks | `` `select` `` |
|Double Quoted | Enclosed in double quotes | `"users"` |
|Qualified | Prefixed by a table name and a dot | `users.id` |

## Literals
- **Boolean**: `TRUE` or `FALSE`.
- **Null**: `NULL`.
- **String**: Enclosed in single quotes (e.g., `'hello'`, `'it\'s a string'`). Backslash `\` is used to escape single quotes.
- **Numeric**: Integers or floating-point numbers, optionally signed, optionally in scientific notation (e.g., `123`, `-456`, `3.14159`, `.5`, `1.2e-3`, `4E+5`).

---

## EBNF Grammar

### Statements

```ebnf
Statement ::= SelectStatement
            | CreateTableStatement
            | DropTableStatement
            | AlterTableStatement
            | CreateIndexStatement
            | DropIndexStatement
```

#### `SELECT` Statement

```ebnf
SelectStatement ::= "SELECT" SelectList
                    "FROM" Identifier [ ["AS"] TableAlias ]
                    [ "WHERE" Expression ]
                    [ "GROUP" "BY" GroupByExprList ]
                    [ "HAVING" Expression ]

SelectList      ::= SelectItem { "," SelectItem }
SelectItem      ::= "*" | Expression
TableAlias      ::= Identifier
GroupByExprList ::= Expression { "," Expression }
```

#### `CREATE TABLE` Statement

```ebnf
CreateTableStatement ::= "CREATE" "TABLE" Identifier "(" ColumnDefList ")"
ColumnDefList        ::= ColumnDef { "," ColumnDef }
ColumnDef            ::= Identifier DataType [ [ "NOT" ] "NULL" ]
DataType             ::= "BOOLEAN" | "TINYINT" | "SMALLINT" | "INT" | "INTEGER"
                       | "BIGINT" | "FLOAT" | "DOUBLE" | "VARCHAR" | "DATETIME"
```

#### `DROP TABLE` Statement

```ebnf
DropTableStatement ::= "DROP" "TABLE" Identifier
```

#### `ALTER TABLE` Statement

```ebnf
AlterTableStatement ::= "ALTER" "TABLE" Identifier AddOrDropColumn
AddOrDropColumn     ::= "ADD" [ "COLUMN" ] ColumnDef
                      | "DROP" [ "COLUMN" ] Identifier
```

#### `CREATE INDEX` Statement

```ebnf
CreateIndexStatement ::= "CREATE" "INDEX" Identifier "ON" Identifier "(" IndexColumnList ")"
IndexColumnList      ::= Identifier { "," Identifier }
```

#### `DROP INDEX` Statement

```ebnf
DropIndexStatement ::= "DROP" "INDEX" Identifier "ON" Identifier
```

---

### Expressions & Operators

Expressions support operator precedence, logical operations, arithmetic, and aggregates:

```ebnf
Expression   ::= PrimaryExpr { BinaryOp PrimaryExpr }

PrimaryExpr  ::= Literal
               | QualifiedIdentifier
               | AggregateExpr
               | "(" Expression ")"

QualifiedIdentifier ::= [ Identifier "." ] Identifier

BinaryOp     ::= "+" | "-" | "*" | "/"
               | "=" | "!=" | "<>" | "<" | ">" | "<=" | ">="
               | "AND" | "OR"

Literal      ::= "TRUE" | "FALSE" | "NULL" | StringLiteral | NumericLiteral
```

#### Aggregate Expressions

```ebnf
AggregateExpr ::= "COUNT" "(" "*" ")"
                | AggFuncName "(" [ "DISTINCT" ] Expression ")"

AggFuncName   ::= "COUNT" | "SUM" | "AVG" | "MIN" | "MAX"
```
