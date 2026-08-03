#include "json.hpp"

#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace bincoin {
namespace {

class Parser {
public:
    explicit Parser(const std::string_view input) : input_(input) {}

    Json parse_document() {
        skip_space();
        Json value = parse_value();
        skip_space();
        if (position_ != input_.size()) error("Trailing data after JSON value");
        return value;
    }

private:
    std::string_view input_;
    std::size_t position_{0};

    [[noreturn]] void error(const char* message) const {
        throw std::runtime_error(std::string(message) + " at JSON byte " + std::to_string(position_));
    }

    void skip_space() {
        while (position_ < input_.size()) {
            const char c = input_[position_];
            if (c != ' ' && c != '\n' && c != '\r' && c != '\t') break;
            ++position_;
        }
    }

    bool consume(const char expected) {
        if (position_ < input_.size() && input_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    void expect(const char expected) {
        if (!consume(expected)) error("Unexpected JSON character");
    }

    Json parse_value() {
        skip_space();
        if (position_ >= input_.size()) error("Unexpected end of JSON input");
        const char c = input_[position_];
        if (c == 'n') return parse_literal("null", Json(nullptr));
        if (c == 't') return parse_literal("true", Json(true));
        if (c == 'f') return parse_literal("false", Json(false));
        if (c == '"') return Json(parse_string());
        if (c == '[') return parse_array();
        if (c == '{') return parse_object();
        if (c == '-' || (c >= '0' && c <= '9')) return parse_number();
        error("Invalid JSON value");
    }

    Json parse_literal(const std::string_view literal, Json value) {
        if (input_.substr(position_, literal.size()) != literal) error("Invalid JSON literal");
        position_ += literal.size();
        return value;
    }

    static void append_utf8(std::string& output, const std::uint32_t codepoint) {
        if (codepoint <= 0x7fU) {
            output.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7ffU) {
            output.push_back(static_cast<char>(0xc0U | (codepoint >> 6U)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
        } else if (codepoint <= 0xffffU) {
            output.push_back(static_cast<char>(0xe0U | (codepoint >> 12U)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
        } else if (codepoint <= 0x10ffffU) {
            output.push_back(static_cast<char>(0xf0U | (codepoint >> 18U)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
        } else {
            throw std::runtime_error("Invalid Unicode code point in JSON string");
        }
    }

    std::uint32_t parse_hex4() {
        if (position_ + 4 > input_.size()) error("Incomplete JSON Unicode escape");
        std::uint32_t value = 0;
        for (int index = 0; index < 4; ++index) {
            const char c = input_[position_++];
            value <<= 4U;
            if (c >= '0' && c <= '9') value |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') value |= static_cast<std::uint32_t>(10 + c - 'a');
            else if (c >= 'A' && c <= 'F') value |= static_cast<std::uint32_t>(10 + c - 'A');
            else error("Invalid JSON Unicode escape");
        }
        return value;
    }

    std::string parse_string() {
        expect('"');
        std::string output;
        while (position_ < input_.size()) {
            const char c = input_[position_++];
            if (c == '"') return output;
            if (static_cast<unsigned char>(c) < 0x20U) error("Control character in JSON string");
            if (c != '\\') {
                output.push_back(c);
                continue;
            }
            if (position_ >= input_.size()) error("Incomplete JSON escape");
            const char escaped = input_[position_++];
            switch (escaped) {
                case '"': output.push_back('"'); break;
                case '\\': output.push_back('\\'); break;
                case '/': output.push_back('/'); break;
                case 'b': output.push_back('\b'); break;
                case 'f': output.push_back('\f'); break;
                case 'n': output.push_back('\n'); break;
                case 'r': output.push_back('\r'); break;
                case 't': output.push_back('\t'); break;
                case 'u': {
                    std::uint32_t codepoint = parse_hex4();
                    if (codepoint >= 0xd800U && codepoint <= 0xdbffU) {
                        if (position_ + 2 > input_.size() || input_[position_] != '\\' || input_[position_ + 1] != 'u') {
                            error("Missing low surrogate in JSON string");
                        }
                        position_ += 2;
                        const std::uint32_t low = parse_hex4();
                        if (low < 0xdc00U || low > 0xdfffU) error("Invalid low surrogate in JSON string");
                        codepoint = 0x10000U + ((codepoint - 0xd800U) << 10U) + (low - 0xdc00U);
                    } else if (codepoint >= 0xdc00U && codepoint <= 0xdfffU) {
                        error("Unexpected low surrogate in JSON string");
                    }
                    append_utf8(output, codepoint);
                    break;
                }
                default: error("Invalid JSON escape");
            }
        }
        error("Unterminated JSON string");
    }

    Json parse_array() {
        expect('[');
        Json::Array values;
        skip_space();
        if (consume(']')) return Json(std::move(values));
        while (true) {
            values.push_back(parse_value());
            skip_space();
            if (consume(']')) return Json(std::move(values));
            expect(',');
            skip_space();
        }
    }

    Json parse_object() {
        expect('{');
        Json::Object values;
        skip_space();
        if (consume('}')) return Json(std::move(values));
        while (true) {
            skip_space();
            if (position_ >= input_.size() || input_[position_] != '"') error("JSON object key must be a string");
            std::string key = parse_string();
            skip_space();
            expect(':');
            Json value = parse_value();
            values.insert_or_assign(std::move(key), std::move(value));
            skip_space();
            if (consume('}')) return Json(std::move(values));
            expect(',');
        }
    }

    Json parse_number() {
        const std::size_t begin = position_;
        (void)consume('-');
        if (consume('0')) {
            if (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
                error("Leading zero in JSON number");
            }
        } else {
            if (position_ >= input_.size() || input_[position_] < '1' || input_[position_] > '9') {
                error("Invalid JSON number");
            }
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') ++position_;
        }
        bool floating = false;
        if (consume('.')) {
            floating = true;
            if (position_ >= input_.size() || input_[position_] < '0' || input_[position_] > '9') {
                error("Invalid JSON fraction");
            }
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') ++position_;
        }
        if (position_ < input_.size() && (input_[position_] == 'e' || input_[position_] == 'E')) {
            floating = true;
            ++position_;
            if (position_ < input_.size() && (input_[position_] == '+' || input_[position_] == '-')) ++position_;
            if (position_ >= input_.size() || input_[position_] < '0' || input_[position_] > '9') {
                error("Invalid JSON exponent");
            }
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') ++position_;
        }
        const std::string_view token = input_.substr(begin, position_ - begin);
        if (!floating) {
            std::int64_t value = 0;
            const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
            if (result.ec == std::errc{} && result.ptr == token.data() + token.size()) return Json(value);
        }
        std::string copy(token);
        std::size_t used = 0;
        const double value = std::stod(copy, &used);
        if (used != copy.size() || !std::isfinite(value)) error("Invalid JSON number");
        return Json(value);
    }
};

std::string indentation(const int count) {
    return std::string(static_cast<std::size_t>(count), ' ');
}

} // namespace

Json::Json(const std::uint64_t value) {
    if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        value_ = static_cast<double>(value);
    } else {
        value_ = static_cast<std::int64_t>(value);
    }
}

Json Json::parse(const std::string_view text) { return Parser(text).parse_document(); }

std::string json_escape(const std::string_view text) {
    std::ostringstream output;
    for (const char raw : text) {
        const auto c = static_cast<unsigned char>(raw);
        switch (c) {
            case '"': output << "\\\""; break;
            case '\\': output << "\\\\"; break;
            case '\b': output << "\\b"; break;
            case '\f': output << "\\f"; break;
            case '\n': output << "\\n"; break;
            case '\r': output << "\\r"; break;
            case '\t': output << "\\t"; break;
            default:
                if (c < 0x20U) {
                    output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                           << static_cast<unsigned int>(c) << std::dec;
                } else {
                    output << static_cast<char>(c);
                }
        }
    }
    return output.str();
}

std::string Json::dump(const bool pretty, const int indent) const {
    if (is_null()) return "null";
    if (is_bool()) return as_bool() ? "true" : "false";
    if (is_integer()) return std::to_string(as_integer());
    if (std::holds_alternative<double>(value_)) {
        std::ostringstream output;
        output << std::setprecision(17) << std::get<double>(value_);
        return output.str();
    }
    if (is_string()) return '"' + json_escape(as_string()) + '"';
    if (is_array()) {
        const auto& values = as_array();
        if (values.empty()) return "[]";
        std::ostringstream output;
        output << '[';
        for (std::size_t index = 0; index < values.size(); ++index) {
            if (index != 0) output << ',';
            if (pretty) output << '\n' << indentation(indent + 2);
            output << values[index].dump(pretty, indent + 2);
        }
        if (pretty) output << '\n' << indentation(indent);
        output << ']';
        return output.str();
    }
    const auto& values = as_object();
    if (values.empty()) return "{}";
    std::ostringstream output;
    output << '{';
    std::size_t index = 0;
    for (const auto& [key, value] : values) {
        if (index++ != 0) output << ',';
        if (pretty) output << '\n' << indentation(indent + 2);
        output << '"' << json_escape(key) << '"' << (pretty ? ": " : ":")
               << value.dump(pretty, indent + 2);
    }
    if (pretty) output << '\n' << indentation(indent);
    output << '}';
    return output.str();
}

bool Json::is_null() const noexcept { return std::holds_alternative<std::nullptr_t>(value_); }
bool Json::is_bool() const noexcept { return std::holds_alternative<bool>(value_); }
bool Json::is_integer() const noexcept { return std::holds_alternative<std::int64_t>(value_); }
bool Json::is_number() const noexcept { return is_integer() || std::holds_alternative<double>(value_); }
bool Json::is_string() const noexcept { return std::holds_alternative<std::string>(value_); }
bool Json::is_array() const noexcept { return std::holds_alternative<Array>(value_); }
bool Json::is_object() const noexcept { return std::holds_alternative<Object>(value_); }

bool Json::as_bool() const {
    if (!is_bool()) throw std::runtime_error("JSON value is not a boolean");
    return std::get<bool>(value_);
}

std::int64_t Json::as_integer() const {
    if (is_integer()) return std::get<std::int64_t>(value_);
    if (std::holds_alternative<double>(value_)) return static_cast<std::int64_t>(std::get<double>(value_));
    throw std::runtime_error("JSON value is not an integer");
}

double Json::as_number() const {
    if (is_integer()) return static_cast<double>(std::get<std::int64_t>(value_));
    if (std::holds_alternative<double>(value_)) return std::get<double>(value_);
    throw std::runtime_error("JSON value is not a number");
}

const std::string& Json::as_string() const {
    if (!is_string()) throw std::runtime_error("JSON value is not a string");
    return std::get<std::string>(value_);
}

const Json::Array& Json::as_array() const {
    if (!is_array()) throw std::runtime_error("JSON value is not an array");
    return std::get<Array>(value_);
}

const Json::Object& Json::as_object() const {
    if (!is_object()) throw std::runtime_error("JSON value is not an object");
    return std::get<Object>(value_);
}

Json::Array& Json::as_array() {
    if (!is_array()) throw std::runtime_error("JSON value is not an array");
    return std::get<Array>(value_);
}

Json::Object& Json::as_object() {
    if (!is_object()) throw std::runtime_error("JSON value is not an object");
    return std::get<Object>(value_);
}

const Json& Json::at(const std::string_view key) const {
    const auto& object = as_object();
    const auto iterator = object.find(std::string(key));
    if (iterator == object.end()) throw std::runtime_error("JSON object does not contain key: " + std::string(key));
    return iterator->second;
}

bool Json::contains(const std::string_view key) const {
    if (!is_object()) return false;
    return as_object().contains(std::string(key));
}

} // namespace bincoin
