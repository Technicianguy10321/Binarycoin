#pragma once

#include "transaction.hpp"

#include <string>
#include <vector>

namespace bincoin {

Hash256 merkle_root_raw(const std::vector<Transaction>& transactions);
std::string merkle_root_hex(const std::vector<Transaction>& transactions);

} // namespace bincoin
