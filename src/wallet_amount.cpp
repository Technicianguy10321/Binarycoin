#include "wallet.hpp"

#include <charconv>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace bincoin {

Amount parse_bin_amount(const std::string& text) {
    if (text.empty() || text.front() == '-') throw std::invalid_argument("BIN amount must be positive");
    const std::size_t dot = text.find('.');
    if (dot != std::string::npos && text.find('.', dot + 1) != std::string::npos) {
        throw std::invalid_argument("BIN amount has multiple decimal points");
    }
    const std::string whole_text = dot == std::string::npos ? text : text.substr(0, dot);
    std::string fraction_text = dot == std::string::npos ? "" : text.substr(dot + 1);
    if (whole_text.empty() || fraction_text.size() > 8) throw std::invalid_argument("BIN amount supports at most 8 decimals");
    while (fraction_text.size() < 8) fraction_text.push_back('0');
    std::uint64_t whole = 0;
    std::uint64_t fraction = 0;
    const auto [whole_ptr, whole_error] = std::from_chars(whole_text.data(), whole_text.data() + whole_text.size(), whole);
    if (whole_error != std::errc{} || whole_ptr != whole_text.data() + whole_text.size()) throw std::invalid_argument("Invalid BIN amount");
    if (!fraction_text.empty()) {
        const auto [fraction_ptr, fraction_error] = std::from_chars(
            fraction_text.data(), fraction_text.data() + fraction_text.size(), fraction);
        if (fraction_error != std::errc{} || fraction_ptr != fraction_text.data() + fraction_text.size()) {
            throw std::invalid_argument("Invalid BIN fractional amount");
        }
    }
    if (whole > static_cast<std::uint64_t>(MAX_MONEY / BITS_PER_BIN)) throw std::invalid_argument("BIN amount too large");
    const std::uint64_t bits = whole * static_cast<std::uint64_t>(BITS_PER_BIN) + fraction;
    if (bits > static_cast<std::uint64_t>(MAX_MONEY)) throw std::invalid_argument("BIN amount outside money range");
    return static_cast<Amount>(bits);
}

std::string format_bin_amount(const Amount amount) {
    if (!money_range(amount)) throw std::invalid_argument("Amount outside money range");
    std::ostringstream output;
    output << (amount / BITS_PER_BIN) << '.' << std::setw(8) << std::setfill('0') << (amount % BITS_PER_BIN);
    return output.str();
}

} // namespace bincoin
