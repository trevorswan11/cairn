#include "sql/catalog.hh"

#include "sql/schema.hh"
#include "sql/type.hh"

namespace cairn::sql::metadata {

auto sys_tables_schema() -> const schema& {
    static const schema sch{{column{"table_id", type::id_t::BIGINT, false},
                             column{"name", type::id_t::VARCHAR, false},
                             column{"root_page_id", type::id_t::BIGINT, false}}};
    return sch;
}

auto sys_columns_schema() -> const schema& {
    static const schema sch{{column{"composite_id", type::id_t::BIGINT, false},
                             column{"name", type::id_t::VARCHAR, false},
                             column{"type_id", type::id_t::INTEGER, false},
                             column{"nullable", type::id_t::BOOLEAN, false}}};
    return sch;
}

auto sys_indexes_schema() -> const schema& {
    static const schema sch{{column{"composite_id", type::id_t::BIGINT, false},
                             column{"name", type::id_t::VARCHAR, false},
                             column{"column_id", type::id_t::INTEGER, false},
                             column{"root_page_id", type::id_t::BIGINT, false},
                             column{"is_unique", type::id_t::BOOLEAN, false}}};
    return sch;
}

} // namespace cairn::sql::metadata
