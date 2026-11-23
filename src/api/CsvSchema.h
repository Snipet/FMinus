// src/api/CsvSchema.h: the module a CSV file's header and contents suggest, as FMinus source, for
// `fminus data.csv --emit fm` (docs/tables.md, "The schema, written for you"). The file is read by the runtime's own
// reader (runtime/Csv.cpp), so the module reads what csv_read will.
#pragma once

#include <string>
#include <string_view>

namespace fminus
{
    // The FMinus text of a module of one field a column of the CSV `text` (named `fileName`, its fields split by
    // `delimiter`), and the enumerations its fields need: a column of integers an int, of other numbers a float (a
    // double when a value has more significant digits than a float keeps, or is past a float's range), of true and
    // false a bool, of at most 16 distinct words that can be an enumeration's constants an enumeration, and any other
    // text an int with [[csv::category]]; each name that is no field's own given [[csv::name("...")]]; each field's
    // range, its empty cells or its categories' count a comment. False, with `error` set, for a text that is no CSV.
    bool csvSchema(std::string_view text, std::string_view fileName, char delimiter, std::string& out,
                   std::string& error);
} // namespace fminus
