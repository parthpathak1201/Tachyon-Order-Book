// Because it's a simple orderbook - we will build everything in one file only.
// We will expand into multiple files if situation requires it. But I doubt this will cross 1K LOC


#include <iostream>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <new>
#include <vector>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <map>
#include <set>

struct Order;
using OrderID = size_t;
using Quantity = size_t;
using Timestamp = std::chrono::steady_clock::time_point;

constexpr auto CacheLineSize = std::hardware_destructive_interference_size;


namespace {
    //because the currency is not guaranteed to be a whole number - it could be 192.251, we can store the whole value and decimal separately
    //if the price is 123.456 - base will be 123456, decimal will be 1000 - technically we wont have anything greater than 1000
    //we are doing thing to ensure accuracy in our computations. floating point calculations are not completely deterministic on the big scale
    struct Price {
        std::int64_t _base = 0;
        std::int64_t _decimals = 1;

        Price() = default;

        constexpr Price(const std::int64_t base, const std::int64_t decimals) : _base(base), _decimals(decimals) {
        }
    };
}

static constexpr Price INVALID{-1, -1};
static constexpr Price MKTORDER{-2, -2};

// will keep on increasing
static std::atomic<OrderID> global_order_id = 0;


namespace {
    //because we are doing the simple stuff - only buy and sell (for now lol)
    enum class Side {
        BUY,
        SELL,
        VOID
    };
}

struct alignas(CacheLineSize) Order {
    OrderID _id{};
    Price _price{};
    Side _side{Side::VOID};
    Quantity _quantity{};
    Timestamp _timestamp{std::chrono::steady_clock::now()};
};

using OrderPtr = std::unique_ptr<Order>;

static int compare_prices(const Price &lhs, const Price &rhs) {
    const auto left = static_cast<__int128_t>(lhs._base) * rhs._decimals;
    const auto right = static_cast<__int128_t>(rhs._base) * lhs._decimals;
    return (left > right) - (left < right);
}

namespace {
    struct cmp_bids {
        bool operator()(const OrderPtr *a, const OrderPtr *b) const {
            const Order &lhs = **a;
            const Order &rhs = **b;
            const auto by_price = compare_prices(lhs._price, rhs._price);
            if (by_price != 0) {
                return by_price > 0;
            }
            if (lhs._timestamp != rhs._timestamp) {
                return lhs._timestamp < rhs._timestamp;
            }
            return lhs._id < rhs._id;
        }
    };
}

namespace {
    struct cmp_asks {
        bool operator()(const OrderPtr *a, const OrderPtr *b) const {
            const Order &lhs = **a;
            const Order &rhs = **b;
            const auto by_price = compare_prices(lhs._price, rhs._price);
            if (by_price != 0) {
                return by_price < 0;
            }
            if (lhs._timestamp != rhs._timestamp) {
                return lhs._timestamp < rhs._timestamp;
            }
            return lhs._id < rhs._id;
        }
    };
}


static std::set<OrderPtr *, cmp_bids> bids_;
static std::set<OrderPtr *, cmp_asks> asks_;


static std::map<OrderID, OrderPtr> orders_;


static std::string format_price(const Price &price) {
    auto digits = std::to_string(price._base);
    std::size_t fractional_digits = 0;
    for (auto scale = price._decimals; scale > 1; scale /= 10) {
        ++fractional_digits;
    }
    if (fractional_digits == 0) {
        return digits;
    }

    if (digits.size() <= fractional_digits) {
        return "0." + std::string(fractional_digits - digits.size(), '0') + digits;
    }
    digits.insert(digits.size() - fractional_digits, 1, '.');
    return digits;
}

static std::optional<Price> parse_price(std::string_view text) {
    std::string digits;
    digits.reserve(text.size());
    std::size_t fractional_digits = 0;
    bool after_decimal = false;
    bool saw_digit = false;

    for (const char ch: text) {
        if (ch >= '0' && ch <= '9') {
            digits.push_back(ch);
            saw_digit = true;
            if (after_decimal) {
                ++fractional_digits;
            }
        } else if (ch == '.' && !after_decimal) {
            after_decimal = true;
        } else {
            return std::nullopt;
        }
    }

    if (!saw_digit || fractional_digits > 18) {
        return std::nullopt;
    }

    std::uint64_t base = 0;
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), base);
    if (error != std::errc{} || end != digits.data() + digits.size() ||
        base == 0 || base > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return std::nullopt;
    }

    std::int64_t scale = 1;
    for (std::size_t i = 0; i < fractional_digits; ++i) {
        scale *= 10;
    }

    return Price{static_cast<std::int64_t>(base), scale};
}

static bool parse_unsigned(const std::string_view text, std::size_t &value) {
    if (text.empty()) {
        return false;
    }
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size();
}

static bool evaluate() {
    if (bids_.empty() || asks_.empty()) {
        return false;
    }
    auto *bid_ref = *bids_.begin();
    auto *ask_ref = *asks_.begin();
    Order &bid = **bid_ref;
    Order &ask = **ask_ref;
    if (compare_prices(bid._price, ask._price) < 0) {
        return false;
    }

    const auto bid_id = bid._id;
    const auto ask_id = ask._id;
    const auto traded = std::min(bid._quantity, ask._quantity);
    bid._quantity -= traded;
    ask._quantity -= traded;

    if (bid._quantity == 0) {
        bids_.erase(bid_ref);
        orders_.erase(bid_id);
    }
    if (ask._quantity == 0) {
        asks_.erase(ask_ref);
        orders_.erase(ask_id);
    }
    return true;
}

static void execute_market_order(Order &incoming) {
    const auto consume = [&](auto &opposite) {
        while (incoming._quantity > 0 && !opposite.empty()) {
            auto *resting_ref = *opposite.begin();
            Order &resting = **resting_ref;
            const auto traded = std::min(incoming._quantity, resting._quantity);
            incoming._quantity -= traded;
            resting._quantity -= traded;
            if (resting._quantity == 0) {
                const auto resting_id = resting._id;
                opposite.erase(resting_ref);
                orders_.erase(resting_id);
            }
        }
    };

    if (incoming._side == Side::BUY) {
        consume(asks_);
    } else {
        consume(bids_);
    }
}

static void add_order(OrderPtr &order) {
    if (order->_quantity == 0) {
        return;
    }

    order->_id = global_order_id++;
    if (order->_price._base == MKTORDER._base &&
        order->_price._decimals == MKTORDER._decimals) {
        execute_market_order(*order);
        return;
    }

    const auto id = order->_id;

    auto [it, inserted] = orders_.emplace(id, std::move(order));
    if (!inserted) {
        return;
    }

    OrderPtr *const order_ref = &it->second;
    if (it->second->_side == Side::BUY) {
        bids_.insert(order_ref);
    } else if (it->second->_side == Side::SELL) {
        asks_.insert(order_ref);
    } else {
        orders_.erase(it);
        return;
    }

    while (evaluate()) {
    }
}

static void cancel_order(const OrderID order_id) {
    const auto it = orders_.find(order_id);
    if (it == orders_.end()) {
        //std::cerr << "Order " << order_id << " does not exist" << std::endl;
        return;
    }

    if (it->second->_side == Side::BUY) {
        bids_.erase(&it->second);
    } else {
        asks_.erase(&it->second);
    }
    orders_.erase(it);

    // std::cerr << "Order " << order_id << " canceled" << std::endl;
}


static void modify_order(const OrderID order_id,
                         const std::optional<Price> price,
                         const std::optional<Side> side,
                         const std::optional<Quantity> quantity) {
    const auto it = orders_.find(order_id);
    if (it == orders_.end()) {
        //std::cerr << "Order " << order_id << " does not exist" << "\n";
        return;
    }

    if (quantity && *quantity == 0) {
        return;
    }

    Order &order = *it->second;
    const Side new_side = side.value_or(order._side);
    const Price new_price = price.value_or(order._price);
    const Quantity new_quantity = quantity.value_or(order._quantity);
    const bool becomes_market =
            new_price._base == MKTORDER._base && new_price._decimals == MKTORDER._decimals;

    if (order._side == Side::BUY) {
        bids_.erase(&it->second);
    } else {
        asks_.erase(&it->second);
    }

    if (new_side != order._side || compare_prices(new_price, order._price) != 0 ||
        new_quantity > order._quantity) {
        order._timestamp = std::chrono::steady_clock::now();
    }
    order._price = new_price;
    order._side = new_side;
    order._quantity = new_quantity;

    if (becomes_market) {
        execute_market_order(order);
        orders_.erase(it);
        return;
    }

    if (new_side == Side::BUY) {
        bids_.insert(&it->second);
    } else {
        asks_.insert(&it->second);
    }
    while (evaluate()) {
    }
}

static Price get_best_bid() {
    if (bids_.empty()) {
        return INVALID;
    }
    return (*bids_.begin())->get()->_price;
}

static Price get_best_ask() {
    if (asks_.empty()) {
        return INVALID;
    }
    return (*asks_.begin())->get()->_price;
}


using PriceQuantityPair = std::pair<Price, Quantity>;


static void get_top_levels(const size_t depth,
                           std::vector<PriceQuantityPair> &bids_out,
                           std::vector<PriceQuantityPair> &asks_out) {
    bids_out.clear();
    asks_out.clear();


    {
        Price current_price{-1, -1};
        Quantity cumulative_qty = 0;

        for (const auto *order_ptr_ref: bids_) {
            const Order &order = **order_ptr_ref;

            if (current_price._base == -1 || compare_prices(order._price, current_price) != 0) {
                if (current_price._base != -1) {
                    bids_out.emplace_back(current_price, cumulative_qty);
                    if (bids_out.size() >= depth) break;
                }
                current_price = order._price;
                cumulative_qty = order._quantity;
            } else {
                cumulative_qty += order._quantity;
            }
        }
        if (current_price._base != -1 && bids_out.size() < depth) {
            bids_out.emplace_back(current_price, cumulative_qty);
        }
    }


    {
        Price current_price{-1, -1};
        Quantity cumulative_qty = 0;

        for (const auto *order_ptr_ref: asks_) {
            const Order &order = **order_ptr_ref;

            if (current_price._base == -1 || compare_prices(order._price, current_price) != 0) {
                if (current_price._base != -1) {
                    asks_out.emplace_back(current_price, cumulative_qty);
                    if (asks_out.size() >= depth) break;
                }
                current_price = order._price;
                cumulative_qty = order._quantity;
            } else {
                cumulative_qty += order._quantity;
            }
        }
        if (current_price._base != -1 && asks_out.size() < depth) {
            asks_out.emplace_back(current_price, cumulative_qty);
        }
    }
}

static void reset() {
    asks_ = {};
    bids_ = {};
    orders_.clear();
    //std::cout << "EVERYTHING ERASED." << "\n";
}

constexpr std::string BUY = "buy";
constexpr std::string SELL = "sell";
constexpr std::string MODIFY = "modify";
constexpr std::string RESET = "reset";
constexpr std::string CANCEL = "cancel";
constexpr std::string GET_BEST_BID = "gbb";
constexpr std::string GET_BEST_ASK = "gba";
constexpr std::string GET_TOP_LEVELS = "gtl";


static void print_order_book() {
    std::cout << "______________________________________" << "\n";
    std::cout << "SELL ORDERS ->" << "\n";

    for (const auto order: asks_) {
        std::print("{} {} {}\n", order->get()->_id, order->get()->_quantity,
                   format_price(order->get()->_price));
    }
    std::cout << "______________________________________" << "\n";
    std::cout << "BUY ORDERS ->" << "\n";

    for (const auto order: bids_) {
        std::print("{} {} {}\n", order->get()->_id, order->get()->_quantity,
                   format_price(order->get()->_price));
    }
    std::cout << "______________________________________" << "\n";
}

int main() {
    //excluding the metadata overhead + null terminator - we have 231 bytes worth of space for commands - HIGHLY unlikely to do another heap alloc
    std::string command;
    std::vector<std::string> tokens; //at max 5 tokens
    tokens.reserve(5);
    command.reserve(CacheLineSize);
    std::vector<PriceQuantityPair> bids_out(10);
    std::vector<PriceQuantityPair> asks_out(10);
    //usage:
    /*
     * for buy/sell
     * <side> <qty> <price (or leave empty for market orders)>
     *
     * reset -
     * reset
     *
     * modifying -
     *
     * modify <order_id(REQUIRED EXISTING ORDER)> <side(-1 for no change)> <qty(-1 for no change)> <price(-2 for turning it into a market order, -1 for no change)>
     */


    while (std::getline(std::cin, command)) {
        tokens.clear();
        bool too_many_tokens = false;

        for (std::size_t pos = 0; pos < command.size();) {
            while (pos < command.size() &&
                   std::isspace(static_cast<unsigned char>(command[pos]))) {
                ++pos;
            }

            if (pos == command.size()) {
                break;
            }

            const auto start = pos;
            while (pos < command.size() &&
                   !std::isspace(static_cast<unsigned char>(command[pos]))) {
                ++pos;
            }

            if (tokens.size() == 5) {
                too_many_tokens = true;
                break;
            }

            tokens.emplace_back(command.data() + start, pos - start);
        }

        if (tokens.empty()) {
            continue;
        }


        if (too_many_tokens) {
            continue;
        }

        if (tokens[0] == "exit") {
            if (tokens.size() != 1) {
                continue;
            }
            break;
        }
        if (tokens[0] == CANCEL) {
            std::size_t id = 0;
            if (tokens.size() != 2 || !parse_unsigned(tokens[1], id)) {
                continue;
            }
            cancel_order(id);
        } else if (tokens[0] == GET_BEST_BID || tokens[0] == GET_BEST_ASK) {
            if (tokens.size() != 1) {
                continue;
            }
            const Price best = tokens[0] == GET_BEST_BID ? get_best_bid() : get_best_ask();

            std::print("{}={}\n", tokens[0] == GET_BEST_BID ? "best_bid" : "best_ask",
                       best._base == INVALID._base && best._decimals == INVALID._decimals ? "-1" : format_price(best));
        } else if (tokens[0] == BUY || tokens[0] == SELL) {
            std::size_t quantity = 0;
            const auto price = tokens.size() == 3 ? parse_price(tokens[2]) : std::optional<Price>{};
            if ((tokens.size() != 2 && tokens.size() != 3) ||
                !parse_unsigned(tokens[1], quantity) ||
                (tokens.size() == 3 && !price)) {
                continue;
            }

            // Valid: buy|sell <quantity> [price]
            // No price token means market order.
            auto order = std::make_unique<Order>();
            if (tokens[0] == BUY) {
                order->_side = Side::BUY;
            } else {
                order->_side = Side::SELL;
            }

            order->_quantity = quantity;
            if (tokens.size() == 3) {
                order->_price = *price;
            } else {
                order->_price = MKTORDER;
            }

            add_order(order);
        } else if (tokens[0] == RESET) {
            if (tokens.size() != 1) {
                continue;
            }

            // Valid: reset

            reset();
        } else if (tokens[0] == MODIFY) {
            std::size_t id = 0;
            std::size_t quantity = 0;
            if (tokens.size() != 5 ||
                !parse_unsigned(tokens[1], id) ||
                (tokens[2] != "-1" && tokens[2] != BUY && tokens[2] != SELL) ||
                (tokens[3] != "-1" && !parse_unsigned(tokens[3], quantity)) ||
                (tokens[4] != "-1" && tokens[4] != "-2" && !parse_price(tokens[4]))) {
                continue;
            }

            const auto existing = orders_.find(id);
            if (existing == orders_.end()) {
                modify_order(id, std::nullopt, std::nullopt, std::nullopt);
                continue;
            }

            std::optional<Side> side;
            if (tokens[2] != "-1") {
                side = tokens[2] == BUY ? Side::BUY : Side::SELL;
            }
            std::optional<Quantity> new_quantity;
            if (tokens[3] != "-1") {
                new_quantity = quantity;
                if (quantity == 0) {
                    continue;
                }
            }
            std::optional<Price> new_price;
            if (tokens[4] == "-2") {
                new_price = MKTORDER;
            } else if (tokens[4] != "-1") {
                new_price = parse_price(tokens[4]);
            }

            // -1 preserves a field, price -2 converts it into a market order.
            const Price requested_price = new_price.value_or(existing->second->_price);
            if (requested_price._base < 0 &&
                !(requested_price._base == MKTORDER._base &&
                  requested_price._decimals == MKTORDER._decimals)) {
                continue;
            }

            const Side requested_side = side.value_or(existing->second->_side);
            if (requested_side == Side::VOID) {
                continue;
            }

            modify_order(id, new_price, side, new_quantity);
        } else if (tokens[0] == GET_TOP_LEVELS) {
            if (tokens.size() != 2) {
                continue;
            }
            const auto levels = stoi(tokens[1]);
            if (levels > 10) {
                continue;
            }

            get_top_levels(levels, bids_out, asks_out);
            //std::cerr << "Unknown command: " << tokens[0] << '\n';
            continue;
        }

        // std::cout << "[Parsed] : " << command << '\n';
        // print_order_book();
    }

    return 0;
}
