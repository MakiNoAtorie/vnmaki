#include <cstdint>
#include <format>
#include <set>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include "engine/tools/inspect.hpp"

namespace vnmaki {

namespace {

std::string scalar_text(const StoreValue& value, bool quote_strings) {
  return std::visit(
      [quote_strings](const auto& item) -> std::string {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, bool>) {
          return item ? "true" : "false";
        } else if constexpr (std::is_same_v<T, std::string>) {
          return quote_strings ? "\"" + item + "\"" : item;
        } else if constexpr (std::is_same_v<T, StoreTableRef>) {
          return std::format("table #{}", item.index);
        } else {
          return std::format("{}", item);
        }
      },
      value);
}

void walk(const StoreSnapshot& snapshot, std::uint32_t index, int depth,
          std::set<std::uint32_t>& expanded, std::vector<StoreRow>& rows) {
  for (const auto& [key, value] : snapshot.tables.at(index).entries) {
    StoreRow row{.depth = depth, .key = scalar_text(key, false)};
    const auto* table = std::get_if<StoreTableRef>(&value);
    if (table == nullptr) {
      row.value = scalar_text(value, true);
      rows.push_back(std::move(row));
      continue;
    }
    row.is_table = true;
    const bool first = expanded.insert(table->index).second;
    if (!first) {
      row.value = std::format("(same table as #{})", table->index);
    }
    rows.push_back(std::move(row));
    if (first) {
      walk(snapshot, table->index, depth + 1, expanded, rows);
    }
  }
}

std::vector<StoreRow> rows_of(const StoreSnapshot& snapshot) {
  std::vector<StoreRow> rows;
  if (!snapshot.tables.empty()) {
    std::set<std::uint32_t> expanded{0};
    walk(snapshot, 0, 0, expanded, rows);
  }
  return rows;
}

}  // namespace

std::vector<StoreRow> inspect_store(const ScriptRuntime& runtime) {
  return rows_of(runtime.store_snapshot());
}

std::vector<StoreRow> inspect_persistent(const ScriptRuntime& runtime) {
  return rows_of(runtime.persistent_snapshot());
}

}  // namespace vnmaki
