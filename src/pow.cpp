#include "pow.hpp"

#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace bincoin {
namespace {

const boost::multiprecision::cpp_int& uint256_maximum() {
    static const boost::multiprecision::cpp_int maximum =
        (boost::multiprecision::cpp_int(1) << 256U) - 1;
    return maximum;
}

} // namespace

boost::multiprecision::cpp_int compact_to_target(const std::uint32_t bits) {
    const std::uint32_t exponent = bits >> 24U;
    const std::uint32_t coefficient = bits & 0x007fffffU;

    if ((bits & 0x00800000U) != 0) {
        throw std::invalid_argument("Negative compact targets are invalid");
    }
    if (coefficient == 0) {
        throw std::invalid_argument("Compact target coefficient cannot be zero");
    }

    boost::multiprecision::cpp_int target = coefficient;
    if (exponent <= 3) {
        target >>= 8U * (3U - exponent);
    } else {
        target <<= 8U * (exponent - 3U);
    }

    if (target <= 0 || target > uint256_maximum()) {
        throw std::invalid_argument("Compact target is outside the 256-bit range");
    }

    return target;
}

boost::multiprecision::cpp_int block_proof(const std::uint32_t bits) {
    const boost::multiprecision::cpp_int target = compact_to_target(bits);
    // Bitcoin-style per-block chainwork: floor((2^256 - 1) / (target + 1)) + 1.
    return (uint256_maximum() / (target + 1)) + 1;
}

bool check_proof_of_work(const Hash256& raw_hash, const std::uint32_t bits) {
    boost::multiprecision::cpp_int value = 0;
    // SHA256 digest bytes are interpreted as little-endian for Bitcoin-style PoW.
    for (auto iterator = raw_hash.rbegin(); iterator != raw_hash.rend(); ++iterator) {
        value <<= 8U;
        value += *iterator;
    }
    return value <= compact_to_target(bits);
}

std::string uint256_to_hex(const boost::multiprecision::cpp_int& value) {
    if (value < 0 || value > uint256_maximum()) {
        throw std::invalid_argument("Value is outside the unsigned 256-bit range");
    }
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(64) << value;
    return output.str();
}

std::string target_to_hex(const std::uint32_t bits) {
    return uint256_to_hex(compact_to_target(bits));
}

} // namespace bincoin
