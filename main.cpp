// Because it's a simple orderbook - we will build everything in one file only.
// We will expand into multiple files if situation requires it. But I doubt this will cross 1K LOC


#include <iostream>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <print>
#include <iomanip>
#include <string>
#include <string_view>
#include <new>
#include <vector>
#include <algorithm>
#include <numeric>
#include <cassert>
#include <charconv>
#include <chrono>
#include <map>
#include <set>
#include <mutex>
#include <thread>
#include <poll.h>
#include <termios.h>
#include <unistd.h>


constexpr std::int64_t DECIMAL_LIMIT = 3;

//TOGGLE THIS FOR THE LIVE ORDERBOOK TUI
constexpr bool ENABLE_LIVE_DASHBOARD = false;
constexpr std::int64_t PRICE_SCALE = [] {
    std::int64_t scale = 1;
    for (std::int64_t i = 0; i < DECIMAL_LIMIT; ++i) {
        scale *= 10;
    }
    return scale;
}();
struct Order;
using OrderID = size_t;
using Quantity = size_t;
using Timestamp = std::chrono::steady_clock::time_point;

constexpr auto CacheLineSize = std::hardware_destructive_interference_size;

/**
 * instead of a nullptr in typical LL structures - we will use the holy grail number for null
 */
constexpr auto NULL_ORDER_ID = -67;

namespace {
    //because the price is not guaranteed to be a whole number - it could be 192.251, we can store the whole value and decimal separately
    //if the price is 123.456 - base will be 123456, decimal will be 1000 - technically we wont have anything greater than 1000
    //we are doing thing to ensure accuracy in our computations. floating point calculations are not completely deterministic on the big scale
    struct Price {
        std::int64_t _base = 0;
        static constexpr std::int64_t _decimals = PRICE_SCALE;

        Price() = default;

        constexpr explicit Price(const std::int64_t base) : _base(base) {
        }
    };
}

static constexpr Price INVALID{-1};
static constexpr Price MKTORDER{-2};

/**
 * There was no requirement as such for using std::atomic<T> here but I went ahead and did it anyways in case I ever incorporate multithreaded logic in this orderbook
 *
 * Behaves sort of like an arena - we have 1 million order objects allocated in compile time
 */
static std::atomic<OrderID> global_order_id = 0;

/**
 * Helper function for clocking current timestamp (used in adding new orders)
 */
static auto now() { return std::chrono::steady_clock::now(); }

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
    mutable Price _price{};
    mutable Side _side{Side::VOID};
    mutable Quantity _quantity{};

    /**
     *  Timestamp of the order - generated automatically when an order is filled.
     *
     *  Note that because the entire order object pool is allocated before the runtime begins - timestamps initially written are wrong.
     *
     *  On every order - a new timestamp is alloted to ensure determined behavior
     */
    mutable Timestamp _timestamp{std::chrono::steady_clock::now()};

    // The order id of the bid/ask previous to this order in the price level
    OrderID prev = NULL_ORDER_ID;
    // The order id of the bid/ask next to this order in the price level
    OrderID next = NULL_ORDER_ID;
};

using OrderPtr = std::unique_ptr<Order>;

static int compare_prices(const Price &lhs, const Price &rhs) {
    const auto left = static_cast<__int128_t>(lhs._base) * Price::_decimals;
    const auto right = static_cast<__int128_t>(rhs._base) * Price::_decimals;
    return (left > right) - (left < right);
}

namespace {
    struct [[maybe_unused]] cmp_bids {
        bool operator()(const OrderPtr *a, const OrderPtr *b) const {
            const Order &lhs = **a;
            const Order &rhs = **b;
            if (const auto by_price = compare_prices(lhs._price, rhs._price); by_price != 0) {
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
    struct [[maybe_unused]] cmp_asks {
        bool operator()(const OrderPtr *a, const OrderPtr *b) const {
            const Order &lhs = **a;
            const Order &rhs = **b;
            if (const auto by_price = compare_prices(lhs._price, rhs._price); by_price != 0) {
                return by_price < 0;
            }
            if (lhs._timestamp != rhs._timestamp) {
                return lhs._timestamp < rhs._timestamp;
            }
            return lhs._id < rhs._id;
        }
    };
}

constexpr auto BOOK_BUFFER = 1'000'001;
constexpr auto TEN_MILLION1 = 10'000'001;

constexpr auto BITMAPSIZE = (BOOK_BUFFER + 127) / 128;

//these 2 vectors will store the order ids of the HEAD of a doubly linked list
//this doubly linked list is not a struct - it is a design.
//we are also storing the tails of each DLL so we can instantly fetch the last order and append a new order and make that the new tail.

/**
 * stores the order ids of the different bid orders.
 *
 * index - price level (base value)
 *
 * value - order id of the head of the order DLL. NULL_ORDER_ID for no orders
 */
static std::vector<OrderID> bids_(BOOK_BUFFER, NULL_ORDER_ID);

/**
 * stores the order ids of the different bid orders.
 *
 * index - price level (base value)
 *
 * value - order id of the tail of the order DLL. NULL_ORDER_ID for no orders
 */
static std::vector<OrderID> bids_tails(BOOK_BUFFER, NULL_ORDER_ID);

/**
 * stores the order ids of the different ask orders.
 *
 * index - price level (base value)
 *
 * value - order id of the head of the order DLL. NULL_ORDER_ID for no orders
 */
static std::vector<OrderID> asks_(BOOK_BUFFER, NULL_ORDER_ID);

/**
 * stores the order ids of the different ask orders.
 *
 * index - price level (base value)
 *
 * value - order id of the tail of the order DLL. NULL_ORDER_ID for no orders
 */
static std::vector<OrderID> asks_tails(BOOK_BUFFER, NULL_ORDER_ID);

// each struct is allocated in this buffer before runtime - we never delete/alloc anything
static std::vector<Order> orders_(BOOK_BUFFER);

static std::vector<__int128_t> bids_bitmap(BITMAPSIZE);
static std::vector<__int128_t> asks_bitmap(BITMAPSIZE);
static std::vector<__int128_t> orders_bitmap(BITMAPSIZE);

constexpr auto SUMMARY = (BITMAPSIZE + 127) / 128;

static std::vector<__int128_t> asks_summary(SUMMARY);
static std::vector<__int128_t> bids_summary(SUMMARY);

static std::int64_t master_ask_table = 0;
static std::int64_t master_bid_table = 0;
static bool benchmark_mode = false;

/**
 * @brief Returns whether a bid order exists at a certain price
 * @param base the base of the price
 */
static bool has_bid_at(const std::int64_t base) {
    return (bids_bitmap[base / 128] & (static_cast<__int128_t>(1) << (base % 128))) != 0;
}

/**
 * @brief Returns whether an ask order exists at a certain price
 * @param base the base of the price
 */
static bool has_ask_at(const std::int64_t base) {
    return (asks_bitmap[base / 128] & (static_cast<__int128_t>(1) << (base % 128))) != 0;
}

/**
 * @brief Returns whether a valid order exists at a certain order id
 * @param id the OrderID
 */
static bool has_order_at_id(const OrderID id) {
    return (orders_bitmap[id / 128] & (static_cast<__int128_t>(1) << (id % 128))) != 0;
}


// commenting this because this single function cannot do it everywhere. I might have to look for other designs
// template <typename T>
// void turn_on_bit_at(std::vector<T> &bitmap, const size_t index) {
//     bitmap[index / 128] |= (static_cast<__int128_t>(1) << (index % 128));
// }
//
// template <typename T>
// void turn_off_bit_at(std::vector<T> &bitmap, const size_t index) {
//     bitmap[index / 128] &= ~(static_cast<__int128_t>(1) << (index % 128));
// }

static int first_set_bit(const __int128_t val) {
    if (const auto low = static_cast<uint64_t>(val); low != 0) {
        return __builtin_ctzll(low);
    }

    if (const auto high = static_cast<uint64_t>(val >> 64); high != 0) {
        return 64 + __builtin_ctzll(high);
    }

    return -1;
}

static int last_set_bit(const __int128_t val) {
    if (const auto high = static_cast<uint64_t>(val >> 64); high != 0) {
        return 64 + (63 - __builtin_clzll(high));
    }

    if (const auto low = static_cast<uint64_t>(val); low != 0) {
        return 63 - __builtin_clzll(low);
    }

    return -1;
}

static int first_set_bit(const std::int64_t val) {
    if (val == 0) return -1; // Not found
    return __builtin_ctzll(val);
}

static int last_set_bit(const std::int64_t val) {
    if (val == 0) return -1; // Not found
    return 63 - __builtin_clzll(static_cast<std::uint64_t>(val));
}

static OrderID best_bid() {
    const std::int64_t first = last_set_bit(master_bid_table);
    if (first == -1) return static_cast<OrderID>(NULL_ORDER_ID);
    const std::int64_t second = last_set_bit(bids_summary[first]);
    if (second == -1) return static_cast<OrderID>(NULL_ORDER_ID);
    const auto word_index = static_cast<std::size_t>(first * 128 + second);
    const std::int64_t third = last_set_bit(bids_bitmap[word_index]);
    if (third == -1) return static_cast<OrderID>(NULL_ORDER_ID);
    const auto price_index = word_index * 128 + static_cast<std::size_t>(third);
    return bids_[price_index];
}

static OrderID best_ask() {
    const std::int64_t first = first_set_bit(master_ask_table);
    if (first == -1) return static_cast<OrderID>(NULL_ORDER_ID);
    const std::int64_t second = first_set_bit(asks_summary[first]);
    if (second == -1) return static_cast<OrderID>(NULL_ORDER_ID);
    const auto word_index = static_cast<std::size_t>(first * 128 + second);
    const std::int64_t third = first_set_bit(asks_bitmap[word_index]);
    if (third == -1) return static_cast<OrderID>(NULL_ORDER_ID);
    const auto price_index = word_index * 128 + static_cast<std::size_t>(third);
    return asks_[price_index];
}


static std::array<char, 32> format_price(const Price &price) {
    std::array<char, 32> digits{};
    std::array<char, 32> formatted{};
    const auto [digits_end, error] =
            std::to_chars(digits.data(), digits.data() + digits.size() - 1, price._base);
    if (error != std::errc{}) {
        return formatted;
    }

    const auto digit_count = static_cast<std::size_t>(digits_end - digits.data());
    constexpr std::size_t fractional_digits = DECIMAL_LIMIT;
    const auto integer_digits = digit_count > fractional_digits ? digit_count - fractional_digits : 1;
    const auto leading_zeroes = digit_count < fractional_digits + 1
                                    ? fractional_digits + 1 - digit_count
                                    : 0;

    std::size_t output = 0;
    for (std::size_t i = 0; i < integer_digits; ++i) {
        formatted[output++] = i < leading_zeroes ? '0' : digits[i - leading_zeroes];
    }
    formatted[output++] = '.';
    if (leading_zeroes != 0) {
        for (std::size_t i = 0; i < fractional_digits; ++i) {
            formatted[output++] = i < leading_zeroes - 1
                                      ? '0'
                                      : digits[i - (leading_zeroes - 1)];
        }
    } else {
        for (std::size_t i = integer_digits; i < digit_count; ++i) {
            formatted[output++] = digits[i];
        }
    }
    formatted[output] = '\0';
    return formatted;
}

static std::optional<Price> parse_price(const std::string_view text) {
    std::uint64_t base = 0;
    std::size_t fractional_digits = 0;
    bool after_decimal = false;
    bool saw_digit = false;

    for (const char ch: text) {
        if (ch >= '0' && ch <= '9') {
            const auto digit = static_cast<std::uint64_t>(ch - '0');
            if (base > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
                return std::nullopt;
            }
            base = base * 10 + digit;
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

    if (!saw_digit || fractional_digits > static_cast<std::size_t>(DECIMAL_LIMIT)) {
        return std::nullopt;
    }

    for (auto i = fractional_digits; i < static_cast<std::size_t>(DECIMAL_LIMIT); ++i) {
        if (base > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) / 10) {
            return std::nullopt;
        }
        base *= 10;
    }
    if (base == 0 || base > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return std::nullopt;
    }
    if (base >= BOOK_BUFFER) {
        return std::nullopt;
    }

    return Price{static_cast<std::int64_t>(base)};
}

static bool parse_unsigned(const std::string_view text, std::size_t &value) {
    if (text.empty()) {
        return false;
    }
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size();
}

constexpr std::size_t COMMAND_BUFFER_SIZE = 256;

static void set_order_state(const OrderID id, const bool active) {
    auto &word = orders_bitmap.at(id / 128);
    const auto mask = static_cast<__int128_t>(1) << (id % 128);
    if (active) {
        word |= mask;
    } else {
        word &= ~mask;
    }
}

/**
 * This inputs the price base and toggles that entire bid level. You have to be sure not to call this unless ALL orders are cleared for a certain price
 * @param base value of the base price
 * @param active - true if you want to set it LIVE, false otherwise
 */
static void set_bid_state(const std::int64_t base, const bool active) {
    const auto word_index = static_cast<std::size_t>(base / 128);
    const auto bit_index = static_cast<std::size_t>(base % 128);
    const auto summary_index = word_index / 128;
    const auto summary_bit_index = word_index % 128;
    auto &word = bids_bitmap.at(word_index);
    auto &summary = bids_summary.at(summary_index);
    const auto bit_mask = static_cast<__int128_t>(1) << bit_index;
    const auto summary_mask = static_cast<__int128_t>(1) << summary_bit_index;
    const auto master_mask = std::int64_t{1} << summary_index;

    if (active) {
        word |= bit_mask;
        summary |= summary_mask;
        master_bid_table |= master_mask;
    } else {
        word &= ~bit_mask;
        if (word == 0) {
            summary &= ~summary_mask;
            if (summary == 0) {
                master_bid_table &= ~master_mask;
            }
        }
    }
}

/**
 * This inputs the price base and toggles that entire ask level. You have to be sure not to call this unless ALL orders are cleared for a certain price
 * @param base value of the base price
 * @param active - true if you want to set it LIVE, false otherwise
 */
static void set_ask_state(const std::int64_t base, const bool active) {
    const auto word_index = static_cast<std::size_t>(base / 128);
    const auto bit_index = static_cast<std::size_t>(base % 128);
    const auto summary_index = word_index / 128;
    const auto summary_bit_index = word_index % 128;
    auto &word = asks_bitmap.at(word_index);
    auto &summary = asks_summary.at(summary_index);
    const auto bit_mask = static_cast<__int128_t>(1) << bit_index;
    const auto summary_mask = static_cast<__int128_t>(1) << summary_bit_index;
    const auto master_mask = std::int64_t{1} << summary_index;

    if (active) {
        word |= bit_mask;
        summary |= summary_mask;
        master_ask_table |= master_mask;
    } else {
        word &= ~bit_mask;
        if (word == 0) {
            summary &= ~summary_mask;
            if (summary == 0) {
                master_ask_table &= ~master_mask;
            }
        }
    }
}

/**
 * This will evaluate the matching logic - only one bid-ask pair.
 *
 * Exists as a while loop in the main hot path so that it will execute until it cannot match further
 */

static bool evaluate() {
    const auto best_bid_id = best_bid();
    const auto best_ask_id = best_ask();
    if (best_bid_id == NULL_ORDER_ID || best_ask_id == NULL_ORDER_ID) {
        return false;
    }

    auto &bid = orders_[best_bid_id];
    auto &ask = orders_[best_ask_id];
    const auto price_bid = static_cast<std::size_t>(bid._price._base);
    const auto price_ask = static_cast<std::size_t>(ask._price._base);

    if (compare_prices(bid._price, ask._price) < 0) {
        return false;
    }

    const auto traded = std::min(bid._quantity, ask._quantity);
    bid._quantity -= traded;
    ask._quantity -= traded;

    if (bid._quantity == 0) {
        const auto prev_id = bid.prev;
        const auto next_id = bid.next;

        if (prev_id == NULL_ORDER_ID) {
            // update price level head
            bids_[price_bid] = bid.next;
            if (bid.next != NULL_ORDER_ID) {
                // clear new head's prev pointer
                orders_[bid.next].prev = NULL_ORDER_ID;
            } else {
                // if next is also NULL - the entire price level is now empty
                bids_tails[price_bid] = NULL_ORDER_ID;
                set_bid_state(bid._price._base, false);
            }
        }

        if (next_id == NULL_ORDER_ID) {
            // meaning this is the tail - we have to update the tail
            bids_tails[price_bid] = prev_id;
            if (prev_id != NULL_ORDER_ID) {
                orders_[prev_id].next = NULL_ORDER_ID;
            }
        }


        if (prev_id != NULL_ORDER_ID) {
            auto &prev_order = orders_[prev_id];
            prev_order.next = next_id;
        }

        if (next_id != NULL_ORDER_ID) {
            auto &next_order = orders_[next_id];
            next_order.prev = prev_id;
        }

        set_order_state(best_bid_id, false);
    }

    if (ask._quantity == 0) {
        const auto prev_id = ask.prev;
        const auto next_id = ask.next;

        if (prev_id == NULL_ORDER_ID) {
            // update price level head
            asks_[price_ask] = ask.next;
            if (ask.next != NULL_ORDER_ID) {
                // clear new head's prev pointer
                orders_[ask.next].prev = NULL_ORDER_ID;
            } else {
                // if next is also NULL - the entire price level is now empty
                asks_tails[price_ask] = NULL_ORDER_ID;
                set_ask_state(ask._price._base, false);
            }
        }

        if (next_id == NULL_ORDER_ID) {
            // meaning this is the tail - we have to update the tail
            asks_tails[price_ask] = prev_id;
            if (prev_id != NULL_ORDER_ID) {
                orders_[prev_id].next = NULL_ORDER_ID;
            }
        }

        if (prev_id != NULL_ORDER_ID) {
            auto &prev_order = orders_[prev_id];
            prev_order.next = next_id;
        }

        if (next_id != NULL_ORDER_ID) {
            auto &next_order = orders_[next_id];
            next_order.prev = prev_id;
        }


        set_order_state(best_ask_id, false);
    }
    return true;
}


/*
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

 */


static void execute_market_order(Order &incoming) {
    if (incoming._side == Side::BUY) {
        // buy market order consumes asks, starting from the best (lowest) ask
        while (incoming._quantity > 0) {
            const auto best_ask_id = best_ask();
            if (best_ask_id == static_cast<OrderID>(NULL_ORDER_ID)) break;

            auto &resting = orders_[best_ask_id];
            const auto ask_base = static_cast<std::size_t>(resting._price._base);
            const auto traded = std::min(incoming._quantity, resting._quantity);

            incoming._quantity -= traded;
            resting._quantity -= traded;

            if (resting._quantity == 0) {
                // remove resting order from intrusive list
                const auto next_id = resting.next;
                asks_[ask_base] = next_id;

                if (next_id != NULL_ORDER_ID) {
                    orders_[next_id].prev = NULL_ORDER_ID;
                } else {
                    asks_tails[ask_base] = NULL_ORDER_ID;
                    set_ask_state(ask_base, false); // clear bitmap bit if level is empty
                }

                set_order_state(best_ask_id, false);
            }
        }
    } else {
        // sell market order consumes bids, starting from the best (highest) bid
        while (incoming._quantity > 0) {
            const auto best_bid_id = best_bid();
            if (best_bid_id == static_cast<OrderID>(NULL_ORDER_ID)) break;

            auto &resting = orders_[best_bid_id];
            const auto bid_base = static_cast<std::size_t>(resting._price._base);
            const auto traded = std::min(incoming._quantity, resting._quantity);

            incoming._quantity -= traded;
            resting._quantity -= traded;

            if (resting._quantity == 0) {
                // remove resting order from intrusive list
                const auto next_id = resting.next;
                bids_[bid_base] = next_id;

                if (next_id != NULL_ORDER_ID) {
                    orders_[next_id].prev = NULL_ORDER_ID;
                } else {
                    bids_tails[bid_base] = NULL_ORDER_ID;
                    set_bid_state(bid_base, false); // clear bitmap bit if level is empty
                }

                set_order_state(best_bid_id, false);
            }
        }
    }
}


static void add_order(const Order &order_info) {
    if (order_info._quantity == 0) {
        return;
    }

    auto &order = orders_[global_order_id++];

    order._id = global_order_id - 1;
    order._price = order_info._price;
    order._side = order_info._side;
    order._quantity = order_info._quantity;
    order._timestamp = now();

    if (order_info._price._base == MKTORDER._base) {
        execute_market_order(order);
        return;
    }

    const auto id = order._id;

    const auto base = order._price._base;
    if (order._side == Side::BUY) {
        //pehle check if there already exists some orders on the base/level
        if (has_bid_at(base)) {
            //if orders exist - append onto the tail
            const OrderID tail = bids_tails[base];
            orders_[tail].next = id;
            order.next = NULL_ORDER_ID;
            order.prev = tail;
            bids_tails[base] = id;
        } else {
            //this base/level has no orders. This is the first one
            //setting next and prev to NULL in case this was a past order that is being used again
            order.next = NULL_ORDER_ID;
            order.prev = NULL_ORDER_ID;
            bids_[base] = id;
            bids_tails[base] = id;
            set_bid_state(base, true);
        }
    } else if (order._side == Side::SELL) {
        if (has_ask_at(base)) {
            //if orders exist - append onto the tail
            const OrderID tail = asks_tails[base];
            orders_[tail].next = id;
            order.next = NULL_ORDER_ID;
            order.prev = tail;
            asks_tails[base] = id;
        } else {
            //this base/level has no orders. This is the first one
            //setting next and prev to NULL in case this was a past order that is being used again
            order.next = NULL_ORDER_ID;
            order.prev = NULL_ORDER_ID;
            asks_[base] = id;
            asks_tails[base] = id;
            set_ask_state(base, true);
        }
    }

    set_order_state(id, true);

    // execute the matching for best bid and ask order until you can
    while (evaluate()) {
    }
}

static void cancel_order(const OrderID order_id) {
    if (!has_order_at_id(order_id)) {
        return;
    }
    const auto &order = orders_[order_id];
    const auto prev = order.prev;
    const auto next = order.next;

    if (prev != NULL_ORDER_ID) {
        orders_[prev].next = next;
    }

    if (next != NULL_ORDER_ID) {
        orders_[next].prev = prev;
    }

    if (order._side == Side::BUY) {
        if (order.next == NULL_ORDER_ID && order.prev == NULL_ORDER_ID) {
            //only order at this price level
            set_bid_state(order._price._base, false);
        }

        if (prev == NULL_ORDER_ID) {
            //if this order was HEAD of the DLL - change the head
            bids_[order._price._base] = order.next;
        }

        if (next == NULL_ORDER_ID) {
            //order was TAIL of the DLL - change the tail
            bids_tails[order._price._base] = order.prev;
        }
    } else {
        if (order.next == NULL_ORDER_ID && order.prev == NULL_ORDER_ID) {
            //only order at this price level
            set_ask_state(order._price._base, false);
        }

        if (prev == NULL_ORDER_ID) {
            //if this order was HEAD of the DLL - change the head
            asks_[order._price._base] = order.next;
        }

        if (next == NULL_ORDER_ID) {
            //order was TAIL of the DLL - change the tail
            asks_tails[order._price._base] = order.prev;
        }
    }

    set_order_state(order_id, false);
}


static void modify_order(const OrderID order_id,
                         const std::optional<Price> price,
                         const std::optional<Side> side,
                         const std::optional<Quantity> quantity) {
    if (!has_order_at_id(order_id)) {
        std::cerr << "Order " << order_id << " does not exist" << "\n";
        return;
    }

    auto &order = orders_[order_id];

    if (quantity && *quantity == 0) {
        return;
    }


    const Side new_side = side.value_or(order._side);
    const Price new_price = price.value_or(order._price);
    const Quantity new_quantity = quantity.value_or(order._quantity);
    const bool becomes_market = new_price._base == MKTORDER._base;
    const bool priority_lost = (compare_prices(new_price, order._price) != 0) || (new_side != order._side) || (
                                   new_quantity > order._quantity);

    if (becomes_market) {
        cancel_order(order_id);
        order._price = MKTORDER;
        order._side = new_side;
        order._quantity = new_quantity;
        execute_market_order(order);
        return;
    }
    if (priority_lost) {
        // unlinks from old price level & deactivates
        cancel_order(order_id);

        order._price = new_price;
        order._side = new_side;
        order._quantity = new_quantity;
        order._timestamp = now();

        // reactivate order state
        set_order_state(order_id, true);

        // re-insert into the new price level
        const auto base = order._price._base;
        if (order._side == Side::BUY) {
            if (has_bid_at(base)) {
                const OrderID tail = bids_tails[base];
                orders_[tail].next = order_id;
                order.next = NULL_ORDER_ID;
                order.prev = tail;
                bids_tails[base] = order_id;
            } else {
                order.next = NULL_ORDER_ID;
                order.prev = NULL_ORDER_ID;
                bids_[base] = order_id;
                bids_tails[base] = order_id;
                set_bid_state(base, true);
            }
        } else {
            if (has_ask_at(base)) {
                const OrderID tail = asks_tails[base];
                orders_[tail].next = order_id;
                order.next = NULL_ORDER_ID;
                order.prev = tail;
                asks_tails[base] = order_id;
            } else {
                order.next = NULL_ORDER_ID;
                order.prev = NULL_ORDER_ID;
                asks_[base] = order_id;
                asks_tails[base] = order_id;
                set_ask_state(base, true);
            }
        }
    } else {
        order._quantity = new_quantity;
    }
    while (evaluate()) {
    }
}

static Price get_best_bid() {
    const auto bid = best_bid();
    if (bid == static_cast<OrderID>(NULL_ORDER_ID)) {
        return INVALID;
    }
    return orders_[bid]._price;
}

static Price get_best_ask() {
    const auto ask = best_ask();
    if (ask == static_cast<OrderID>(NULL_ORDER_ID)) {
        return INVALID;
    }
    return orders_[ask]._price;
}


using PriceQuantityPair = std::pair<Price, Quantity>;


static void get_top_levels(const size_t depth,
                           std::vector<PriceQuantityPair> &bids_out,
                           std::vector<PriceQuantityPair> &asks_out) {
    bids_out.clear();
    asks_out.clear();

    // walk bids from highest price to lowest price using the hierarchical bitmaps
    std::int64_t master_bids = master_bid_table;
    while (master_bids != 0 && bids_out.size() < depth) {
        const int master_bit = last_set_bit(master_bids);
        if (master_bit < 0) break;

        __int128_t summary_word = bids_summary[master_bit];
        while (summary_word != 0 && bids_out.size() < depth) {
            const int summary_bit = last_set_bit(summary_word);
            if (summary_bit < 0) break;

            const std::size_t word_index = (master_bit * 128) + summary_bit;
            __int128_t bitmap_word = bids_bitmap[word_index];

            while (bitmap_word != 0 && bids_out.size() < depth) {
                const int bit_index = last_set_bit(bitmap_word);
                if (bit_index < 0) break;

                const std::int64_t price_base = (word_index * 128) + bit_index;

                // traverse intrusive linked list at this price level to sum quantities
                Quantity cumulative_qty = 0;
                OrderID current_id = bids_[price_base];
                while (current_id != NULL_ORDER_ID) {
                    const auto &ord = orders_[current_id];
                    cumulative_qty += ord._quantity;
                    current_id = ord.next;
                }

                bids_out.emplace_back(Price{price_base}, cumulative_qty);

                // clear this bit in our local copy to find the next one
                bitmap_word &= ~(static_cast<__int128_t>(1) << bit_index);
            }
            summary_word &= ~(static_cast<__int128_t>(1) << summary_bit);
        }
        master_bids &= ~(std::int64_t{1} << master_bit);
    }

    // walk asks from lowest price to highest price using the hierarchical bitmaps
    std::int64_t master_asks = master_ask_table;
    while (master_asks != 0 && asks_out.size() < depth) {
        const int master_bit = first_set_bit(master_asks);
        if (master_bit < 0) break;

        __int128_t summary_word = asks_summary[master_bit];
        while (summary_word != 0 && asks_out.size() < depth) {
            const int summary_bit = first_set_bit(summary_word);
            if (summary_bit < 0) break;

            const std::size_t word_index = (master_bit * 128) + summary_bit;
            __int128_t bitmap_word = asks_bitmap[word_index];

            while (bitmap_word != 0 && asks_out.size() < depth) {
                const int bit_index = first_set_bit(bitmap_word);
                if (bit_index < 0) break;

                const std::int64_t price_base = (word_index * 128) + bit_index;

                // traverse intrusive linked list at this price level to sum quantities
                Quantity cumulative_qty = 0;
                OrderID current_id = asks_[price_base];
                while (current_id != NULL_ORDER_ID) {
                    const auto &ord = orders_[current_id];
                    cumulative_qty += ord._quantity;
                    current_id = ord.next;
                }

                asks_out.emplace_back(Price{price_base}, cumulative_qty);

                // clear this bit in our local copy to find the next one
                bitmap_word &= ~(static_cast<__int128_t>(1) << bit_index);
            }
            summary_word &= ~(static_cast<__int128_t>(1) << summary_bit);
        }
        master_asks &= ~(std::int64_t{1} << master_bit);
    }
}

namespace {
    struct DashboardSnapshot {
        std::vector<PriceQuantityPair> bids;
        std::vector<PriceQuantityPair> asks;
        std::string input;
        std::string status;
    };
}

static std::mutex dashboard_mutex;
static DashboardSnapshot dashboard_snapshot;

static void publish_dashboard_snapshot() {
    std::lock_guard lock(dashboard_mutex);
    get_top_levels(10, dashboard_snapshot.bids, dashboard_snapshot.asks);
}

static void update_dashboard_input(const std::string_view input) {
    std::lock_guard lock(dashboard_mutex);
    dashboard_snapshot.input = input;
}

static void update_dashboard_status(const std::string_view status) {
    std::lock_guard lock(dashboard_mutex);
    dashboard_snapshot.status = status;
}

static void render_dashboard() {
    DashboardSnapshot snapshot;
    {
        std::lock_guard lock(dashboard_mutex);
        snapshot = dashboard_snapshot;
    }

    const auto max_quantity = [](const auto &levels) {
        Quantity maximum = 0;
        for (const auto &[price, quantity]: levels) {
            maximum = std::max(maximum, quantity);
        }
        return maximum;
    };
    std::cout << "\033[2J\033[H"
            << "\033[1;36m"
            << "  ORDERBOOK  |  LIVE MARKET DEPTH"
            << "\033[0m\n"
            << "\033[1;36m"
            << "  buy|sell <qty> [price] (omit price for market order)\n"
            << "  modify <id> <buy|sell|-1> <qty|-1> <price|-1|-2>\n"
            << "  cancel <id> | gbb | gba | gtl <levels>\n"
            << "  reset | exit  (-1 keeps a modify field; -2 means market)\n"
            << "\033[0m\n";

    const auto render_side = [](const char *heading,
                                const auto &levels,
                                const bool is_ask,
                                const char *color,
                                const Quantity side_max) {
        constexpr std::size_t bar_width = 24;
        std::cout << "  " << color << heading << "\033[0m\n"
                << "  " << std::setw(16) << "PRICE"
                << std::setw(14) << "QUANTITY"
                << "  | VOLUME\n";

        for (std::size_t row = 0; row < levels.size(); ++row) {
            const auto index = is_ask ? levels.size() - 1 - row : row;
            const auto &[price, quantity] = levels[index];
            const auto scaled = static_cast<unsigned __int128>(quantity) * bar_width;
            const auto filled = side_max == 0
                                    ? 0
                                    : std::max<std::size_t>(
                                        1, static_cast<std::size_t>(scaled / side_max));
            const auto bounded_filled = std::min(filled, bar_width);

            const auto formatted_price = format_price(price);
            std::cout << "  " << std::setw(16) << formatted_price.data()
                    << std::setw(14) << quantity
                    << "  | " << color
                    << std::string(bounded_filled, '=')
                    << "\033[2m"
                    << std::string(bar_width - bounded_filled, '-')
                    << "\033[0m\n";
        }
        std::cout << '\n';
    };

    render_side("ASKS / SELL", snapshot.asks, true, "\033[31m", max_quantity(snapshot.asks));
    render_side("BIDS / BUY", snapshot.bids, false, "\033[32m", max_quantity(snapshot.bids));

    std::cout << "  " << snapshot.status << "\n"
            << "  \033[1;36m>\033[0m " << snapshot.input
            << "\033[?25l" << std::flush;
}

namespace {
    class TerminalSession {
    public:
        explicit TerminalSession(const bool enabled) {
            if (!enabled || tcgetattr(STDIN_FILENO, &original_) != 0) {
                return;
            }
            termios raw = original_;
            raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO | ISIG));
            raw.c_cc[VMIN] = 1;
            raw.c_cc[VTIME] = 0;
            if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0) {
                active_ = true;
                std::cout << "\033[?1049h\033[?25l" << std::flush;
            }
        }

        ~TerminalSession() {
            if (active_) {
                tcsetattr(STDIN_FILENO, TCSANOW, &original_);
                std::cout << "\033[0m\033[?25h\033[?1049l" << std::flush;
            }
        }

        [[nodiscard]] bool active() const {
            return active_;
        }

    private:
        termios original_{};
        bool active_ = false;
    };
}

enum class InputResult {
    LINE,
    TOO_LONG,
    END
};

class CommandTimer {
public:
    CommandTimer(std::vector<std::uint64_t> &samples,
                 const bool enabled,
                 bool &capacity_exceeded)
        : samples_(samples),
          enabled_(enabled),
          capacity_exceeded_(capacity_exceeded),
          start_(enabled ? std::chrono::steady_clock::now()
                         : std::chrono::steady_clock::time_point{}) {}

    ~CommandTimer() {
        if (!enabled_) {
            return;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start_).count();
        if (samples_.size() == samples_.capacity()) {
            capacity_exceeded_ = true;
            return;
        }
        samples_.push_back(static_cast<std::uint64_t>(elapsed));
    }

    void discard() {
        enabled_ = false;
    }

private:
    std::vector<std::uint64_t> &samples_;
    bool enabled_;
    bool &capacity_exceeded_;
    std::chrono::steady_clock::time_point start_;
};

static InputResult read_terminal_command(std::array<char, COMMAND_BUFFER_SIZE> &command,
                                         std::size_t &length) {
    length = 0;
    bool too_long = false;
    update_dashboard_input({});

    while (true) {
        pollfd input_fd{STDIN_FILENO, POLLIN, 0};
        const auto poll_result = poll(&input_fd, 1, 100);
        if (poll_result < 0) {
            return InputResult::END;
        }
        if (poll_result == 0) {
            continue;
        }

        char ch = '\0';
        const auto bytes_read = read(STDIN_FILENO, &ch, 1);
        if (bytes_read <= 0) {
            return InputResult::END;
        }
        if (ch == '\n' || ch == '\r') {
            command[length] = '\0';
            update_dashboard_input({});
            return too_long ? InputResult::TOO_LONG : InputResult::LINE;
        }
        if ((ch == 4 && length == 0) || ch == 3) {
            return InputResult::END;
        }
        if (ch == 127 || ch == '\b') {
            if (length != 0 && !too_long) {
                command[--length] = '\0';
                update_dashboard_input(std::string_view(command.data(), length));
            }
            continue;
        }
        if (ch >= 32 && ch < 127) {
            if (length + 1 < command.size()) {
                command[length++] = ch;
                command[length] = '\0';
                update_dashboard_input(std::string_view(command.data(), length));
            } else {
                too_long = true;
            }
        }
    }
}

static InputResult read_batch_command(std::array<char, COMMAND_BUFFER_SIZE> &command,
                                      std::size_t &length) {
    length = 0;
    bool too_long = false;
    char ch = '\0';
    while (std::cin.get(ch)) {
        if (ch == '\n') {
            command[length] = '\0';
            return too_long ? InputResult::TOO_LONG : InputResult::LINE;
        }
        if (length + 1 < command.size()) {
            command[length++] = ch;
        } else {
            too_long = true;
        }
    }
    if (length == 0 && !too_long) {
        return InputResult::END;
    }
    command[length] = '\0';
    return too_long ? InputResult::TOO_LONG : InputResult::LINE;
}

//this function is only for convenience - there wouldn't be any moment in HFT industry when you would want to reset an orderbook in live prod

/**
 * if you are calling this function - you are either very smart or so dumb that you cannot even comprehend the level of your dumbness UwU
 */
static void reset() {
    std::ranges::fill(bids_, NULL_ORDER_ID);
    std::ranges::fill(bids_tails, NULL_ORDER_ID);
    std::ranges::fill(asks_, NULL_ORDER_ID);
    std::ranges::fill(asks_tails, NULL_ORDER_ID);
    std::ranges::fill(bids_bitmap, 0);
    std::ranges::fill(asks_bitmap, 0);
    std::ranges::fill(orders_bitmap, 0);
    std::ranges::fill(bids_summary, 0);
    std::ranges::fill(asks_summary, 0);
    master_bid_table = 0;
    master_ask_table = 0;
    if (!benchmark_mode) {
        std::cout << "EVERYTHING ERASED." << "\n";
    }
}

constexpr std::string_view BUY = "buy";
constexpr std::string_view SELL = "sell";
constexpr std::string_view MODIFY = "modify";
constexpr std::string_view RESET = "reset";
constexpr std::string_view CANCEL = "cancel";
constexpr std::string_view GET_BEST_BID = "get_best_bid";
constexpr std::string_view GET_BEST_ASK = "get_best_ask";
constexpr std::string_view GET_TOP_LEVELS = "gtl";


int main(const int argc, char *argv[]) {
    std::size_t benchmark_capacity = 0;
    if (argc != 1) {
        if (argc != 3 || std::string_view(argv[1]) != "--benchmark-capacity" ||
            !parse_unsigned(argv[2], benchmark_capacity) || benchmark_capacity == 0) {
            std::cerr << "Usage: " << argv[0]
                      << " [--benchmark-capacity <command-count-capacity>]\n";
            return 2;
        }
        benchmark_mode = true;
    }

    std::array<char, COMMAND_BUFFER_SIZE> command{};
    std::array<std::string_view, 5> tokens{};
    std::size_t command_length = 0;
    std::size_t token_count = 0;
    std::vector<std::uint64_t> command_latencies;
    bool benchmark_capacity_exceeded = false;
    if (benchmark_mode) {
        command_latencies.reserve(benchmark_capacity);
    }
    std::vector<PriceQuantityPair> bids_out;
    std::vector<PriceQuantityPair> asks_out;
    bids_out.reserve(10);
    asks_out.reserve(10);
    dashboard_snapshot.bids.reserve(10);
    dashboard_snapshot.asks.reserve(10);
    dashboard_snapshot.input.reserve(COMMAND_BUFFER_SIZE);
    dashboard_snapshot.status.reserve(128);

    const bool interactive = ENABLE_LIVE_DASHBOARD &&
                             isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    TerminalSession terminal(interactive);
    std::atomic render_running{false};
    std::thread renderer;
    if (interactive && terminal.active()) {
        update_dashboard_status("buy/sell <qty> [price] | cancel <id> | gbb | gba | reset | exit");

        publish_dashboard_snapshot();

        render_running.store(true, std::memory_order_release);
        renderer = std::thread([&] {
            while (render_running.load(std::memory_order_acquire)) {
                render_dashboard();
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
    }

    while (true) {
        const auto input_result = interactive && terminal.active()
                                      ? read_terminal_command(command, command_length)
                                      : read_batch_command(command, command_length);
        if (input_result == InputResult::END) {
            break;
        }
        if (input_result == InputResult::TOO_LONG) {
            std::cerr << "Invalid command: input exceeds "
                    << COMMAND_BUFFER_SIZE - 1 << " characters\n";
            continue;
        }

        CommandTimer command_timer(command_latencies, benchmark_mode,
                                   benchmark_capacity_exceeded);
        const std::string_view command_view(command.data(), command_length);
        token_count = 0;
        bool too_many_tokens = false;

        for (std::size_t pos = 0; pos < command_view.size();) {
            while (pos < command_view.size() &&
                   std::isspace(static_cast<unsigned char>(command_view[pos]))) {
                ++pos;
            }

            if (pos == command_view.size()) {
                break;
            }

            const auto start = pos;
            while (pos < command_view.size() &&
                   !std::isspace(static_cast<unsigned char>(command_view[pos]))) {
                ++pos;
            }

            if (token_count == tokens.size()) {
                too_many_tokens = true;
                break;
            }

            tokens[token_count++] = command_view.substr(start, pos - start);
        }

        if (token_count == 0) {
            command_timer.discard();
            continue;
        }


        if (too_many_tokens) {
            std::cerr << "Invalid command: too many arguments\n";
            continue;
        }

        if (tokens[0] == "exit") {
            command_timer.discard();
            if (token_count != 1) {
                continue;
            }
            break;
        }
        if (tokens[0] == CANCEL) {
            std::size_t id = 0;
            if (token_count != 2 || !parse_unsigned(tokens[1], id)) {
                std::cerr << "Invalid cancel command\n";
                continue;
            }

            cancel_order(id);

            if (interactive && terminal.active()) {
                publish_dashboard_snapshot();
            }
        } else if (tokens[0] == GET_BEST_BID || tokens[0] == GET_BEST_ASK) {
            if (token_count != 1) {
                std::cerr << "Invalid best-price query\n";
                continue;
            }
            const Price best = tokens[0] == GET_BEST_BID ? get_best_bid() : get_best_ask();
            if (!benchmark_mode) {
                const auto formatted_price = format_price(best);
                std::array<char, 64> result{};
                const std::string_view prefix =
                        tokens[0] == GET_BEST_BID ? "best_bid=" : "best_ask=";
                auto output = result.data();
                std::copy(prefix.begin(), prefix.end(), output);
                output += prefix.size();
                if (best._base == INVALID._base) {
                    *output++ = '-';
                    *output++ = '1';
                } else {
                    const std::string_view formatted(formatted_price.data());
                    std::copy(formatted.begin(), formatted.end(), output);
                    output += formatted.size();
                }
                *output = '\0';
                if (interactive && terminal.active()) {
                    update_dashboard_status(result.data());
                } else {
                    std::cout << result.data() << '\n';
                }
            }
        } else if (tokens[0] == BUY || tokens[0] == SELL) {
            std::size_t quantity = 0;
            const auto price = token_count == 3 ? parse_price(tokens[2]) : std::optional<Price>{};
            if ((token_count != 2 && token_count != 3) ||
                !parse_unsigned(tokens[1], quantity) ||
                (token_count == 3 && !price)) {
                std::cerr << "Invalid order command\n";
                continue;
            }

            // Valid: buy|sell <quantity> [price]
            // No price token means market order.
            Order temp_order{};
            temp_order._side = (tokens[0] == BUY) ? Side::BUY : Side::SELL;
            temp_order._quantity = quantity;
            temp_order._price = (token_count == 3) ? *price : MKTORDER;

            add_order(temp_order);

            if (interactive && terminal.active()) {
                publish_dashboard_snapshot();
            }
        } else if (tokens[0] == RESET) {
            if (token_count != 1) {
                std::cerr << "Invalid reset command\n";
                continue;
            }

            // Valid: reset

            reset();

            if (interactive && terminal.active()) {
                publish_dashboard_snapshot();
            }
        } else if (tokens[0] == MODIFY) {
            std::size_t id = 0;
            std::size_t quantity = 0;
            if (token_count != 5 ||
                !parse_unsigned(tokens[1], id) ||
                (tokens[2] != "-1" && tokens[2] != BUY && tokens[2] != SELL) ||
                (tokens[3] != "-1" && !parse_unsigned(tokens[3], quantity)) ||
                (tokens[4] != "-1" && tokens[4] != "-2" && !parse_price(tokens[4]))) {
                std::cerr << "Invalid modify command\n";
                continue;
            }

            const auto existing = has_order_at_id(id);
            if (!existing) {
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
            const Price requested_price = new_price.value_or(orders_[id]._price);
            if (requested_price._base < 0 && requested_price._base != MKTORDER._base) {
                continue;
            }

            const Side requested_side = side.value_or(orders_[id]._side);
            if (requested_side == Side::VOID) {
                continue;
            }

            modify_order(id, new_price, side, new_quantity);

            if (interactive && terminal.active()) {
                publish_dashboard_snapshot();
            }
        } else if (tokens[0] == GET_TOP_LEVELS) {
            std::size_t levels = 0;
            if (token_count != 2 || !parse_unsigned(tokens[1], levels) || levels > 10) {
                std::cerr << "Invalid top-level query\n";
                continue;
            }

            get_top_levels(levels, bids_out, asks_out);
            if (!benchmark_mode) {
                for (const auto &[price, quantity]: bids_out) {
                    const auto formatted_price = format_price(price);
                    std::cout << "bid " << formatted_price.data() << ' ' << quantity << '\n';
                }
                for (const auto &[price, quantity]: asks_out) {
                    const auto formatted_price = format_price(price);
                    std::cout << "ask " << formatted_price.data() << ' ' << quantity << '\n';
                }
            }
        } else {
            std::cerr << "Unknown command: " << tokens[0] << '\n';
            continue;
        }

        if (!benchmark_mode) {
            std::cout << "[Parsed] : " << command_view << '\n';
        }
    }

    if (renderer.joinable()) {
        render_running.store(false, std::memory_order_release);
        renderer.join();
    }

    if (benchmark_mode) {
        if (benchmark_capacity_exceeded) {
            std::cerr << "Benchmark sample capacity exceeded; increase --benchmark-capacity\n";
            return 2;
        }
        std::sort(command_latencies.begin(), command_latencies.end());
        const auto total_ns = std::accumulate(command_latencies.begin(),
                                              command_latencies.end(),
                                              std::uint64_t{0});
        const auto percentile = [&](const std::size_t percent) {
            if (command_latencies.empty()) {
                return std::uint64_t{0};
            }
            const auto rank = (command_latencies.size() * percent + 99) / 100;
            return command_latencies[rank - 1];
        };
        std::cout << "Benchmark commands: " << command_latencies.size() << '\n'
                  << "Total command time: " << total_ns << " ns\n";
        if (!command_latencies.empty()) {
            std::cout << "Mean latency: "
                      << total_ns / command_latencies.size() << " ns/command\n";
        }
        std::cout << "p50 latency: " << percentile(50) << " ns\n"
                  << "p90 latency: " << percentile(90) << " ns\n"
                  << "p99 latency: " << percentile(99) << " ns\n";
    }

    return 0;
}
