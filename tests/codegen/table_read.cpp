// Compiled to assembly by check_table_read.cmake: reading a baked table's row by index is
// a few loads and no call; finding one by key is one call into the table.
#include <baker/table.hpp>

#include "move_read.hpp"

extern const baker::Table<views_test::Move> kMoves;

float read_damage_by_index(uint32_t i) {
    return kMoves[i].damage;
}

uint32_t find_by_key(baker::Symbol key) {
    return kMoves.index_of(key).value_or(0);
}
