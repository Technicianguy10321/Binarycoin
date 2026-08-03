#pragma once

#include "hash.hpp"

#include <boost/multiprecision/cpp_int.hpp>
#include <cstdint>
#include <string>

namespace bincoin {

boost::multiprecision::cpp_int compact_to_target(std::uint32_t bits);
std::uint32_t target_to_compact(const boost::multiprecision::cpp_int& target);
boost::multiprecision::cpp_int block_proof(std::uint32_t bits);
bool check_proof_of_work(const Hash256& raw_hash, std::uint32_t bits);
std::string target_to_hex(std::uint32_t bits);
std::string uint256_to_hex(const boost::multiprecision::cpp_int& value);

} // namespace bincoin
