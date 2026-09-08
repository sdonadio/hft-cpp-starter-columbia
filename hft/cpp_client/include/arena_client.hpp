// ─────────────────────────────────────────────────────────────────────────────
// arena_client.hpp — AlgoArena HFT client transport layer.
//
// ArenaClient owns the WebSocket connection to the EXISTING Python exchange and
// speaks the SAME JSON-over-WebSocket protocol as the Python bots (see
// shared/messages.py). It:
//
//   * reads TEAM_ID / EXCHANGE_HOST / EXCHANGE_PORT / EXCHANGE_URL / ARENA_TOKEN
//     from the environment (defaults: localhost:8765, empty token);
//   * opens the socket and sends the Handshake JSON (role="trader");
//   * parses every inbound frame by its "type" discriminator and calls a
//     virtual handler;
//   * maintains a tiny local order-book view (best bid / best ask / mid) built
//     from BookSnapshot messages;
//   * exposes send helpers place_limit / place_market / cancel that serialize
//     the exact PlaceOrder / CancelOrder JSON the Python engine expects;
//   * gates trading on SESSION_OPEN and reconnects automatically on disconnect.
//
// This file is PLUMBING. Students should not need to edit it — the strategy
// surface lives in hft_bot.hpp. The one hook worth knowing here is
// on_book_snapshot(), which stamps the tick-arrival clock used for the
// tick-to-order latency measurement (the graded axis of the course).
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <ixwebsocket/IXWebSocket.h>

namespace arena {

// A single symbol's top-of-book, distilled from a BookSnapshot.
struct BookView {
    std::string symbol;
    double best_bid = 0.0;   // highest resting bid price (0 if book empty)
    double best_ask = 0.0;   // lowest resting ask price  (0 if book empty)
    double mid      = 0.0;   // exchange-reported mid_price
    double spread   = 0.0;   // exchange-reported spread
    double ref      = 0.0;   // venue mark (ref_price); exists even one-sided
    double microprice = 0.0; // depth-weighted touch (signal input, M6)
    double obi        = 0.0;  // order-book imbalance in [-1,1] (+bids heavy)
    std::string asset_type = "equity";

    bool has_bid() const { return best_bid > 0.0; }
    bool has_ask() const { return best_ask > 0.0; }
};

// Connection / runtime configuration, resolved from the environment.
struct ClientConfig {
    std::string team_id;   // TEAM_ID            (default "hft_alpha")
    std::string url;       // EXCHANGE_URL, else ws://EXCHANGE_HOST:EXCHANGE_PORT
    std::string token;     // ARENA_TOKEN        (default "")
    int level = 1;         // handshake unlock level

    static ClientConfig from_env();
};

// ─────────────────────────────────────────────────────────────────────────────
// ArenaClient — WebSocket transport + protocol codec.
//
// Subclass it and override the on_* handlers (HFTBot does exactly this). All
// handlers are invoked on IXWebSocket's receive thread; keep them fast and do
// not block. The send helpers are thread-safe.
// ─────────────────────────────────────────────────────────────────────────────
class ArenaClient {
public:
    using clock      = std::chrono::steady_clock;
    using time_point = clock::time_point;

    explicit ArenaClient(ClientConfig cfg);
    virtual ~ArenaClient();

    ArenaClient(const ArenaClient&)            = delete;
    ArenaClient& operator=(const ArenaClient&) = delete;

    // Connect and block forever, servicing the socket and reconnecting on drop.
    // Returns only if stop() is called from another thread.
    void run();

    // Offline benchmark mode: read newline-delimited BookSnapshot JSON from
    // `in`, feed each to the strategy, and write one JSON line per input to
    // `out` (the orders it produced, or [] for none). Latency is emitted to
    // stderr as `LAT <symbol> <micros>`. This is the interface the Python
    // harness scripts/latency_replay.py drives (--cmd ./hft_bot --replay).
    void run_replay(std::istream& in, std::ostream& out);

    // Ask run() to return; safe to call from a signal handler or another thread.
    void stop();

    // ── Send helpers — serialize the exact wire JSON the Python engine parses ──

    // Rest / cross a limit order. Mirrors shared.messages.PlaceOrder with
    // order_type="limit".
    void place_limit(const std::string& symbol, const std::string& side,
                     int quantity, double price);

    // Send a market order (price is ignored by the venue). order_type="market".
    void place_market(const std::string& symbol, const std::string& side,
                      int quantity);

    // Cancel a resting order. Mirrors shared.messages.CancelOrder.
    void cancel(const std::string& order_id, const std::string& symbol);

    // ── Accessors ──────────────────────────────────────────────────────────
    const std::string& team_id()      const { return cfg_.team_id; }
    bool               session_open() const { return session_open_.load(); }

    // Latest top-of-book for a symbol; returns false if we have never seen one.
    bool book(const std::string& symbol, BookView& out) const;

protected:
    // ── Virtual protocol handlers — overridden by HFTBot ─────────────────────
    // Default implementations do nothing (except book bookkeeping, which the
    // base class always performs before dispatching on_book_snapshot()).

    // A BookSnapshot arrived. `recv_time` is stamped the instant the frame was
    // decoded — pass it back into an order send to measure tick-to-order
    // latency. The BookView is already merged into the local book cache.
    virtual void on_book_snapshot(const BookView& /*book*/,
                                  time_point /*recv_time*/) {}

    // A confirmed trade (ours or a counterparty's). side/qty/price are the
    // fill from OUR perspective when we are a party; side is "" otherwise.
    // maker == our resting order was hit (rebate); !maker == we crossed (fee).
    virtual void on_fill(const std::string& /*side*/, const std::string& /*symbol*/,
                         int /*quantity*/, double /*price*/, bool /*maker*/) {}

    // Authoritative portfolio state pushed by the exchange.
    virtual void on_portfolio(double /*cash*/, double /*net_worth*/,
                              double /*realized_pnl*/, double /*unrealized_pnl*/) {}

    // SESSION_OPEN / SESSION_CLOSED / SHOCK / CALENDAR / … — the raw event name.
    virtual void on_session(const std::string& /*event*/, const std::string& /*message*/) {}

    // An OrderAck: the venue accepted (and possibly filled) an order.
    // queue_ahead = shares that will fill before ours at our price level;
    // level_qty = total resting at that level. Both 0 if we fully filled or
    // queue feedback is off. Front of the queue is queue_ahead == 0.
    virtual void on_order_ack(const std::string& /*order_id*/, const std::string& /*symbol*/,
                              const std::string& /*side*/, double /*price*/, int /*quantity*/,
                              int /*queue_ahead*/, int /*level_qty*/) {}

    // A QueueUpdate: our resting order's FIFO standing moved (a fill/cancel
    // ahead advanced us; a reprice sent us to the back). Owner-only.
    virtual void on_queue_update(const std::string& /*order_id*/, const std::string& /*symbol*/,
                                 const std::string& /*side*/, double /*price*/,
                                 int /*queue_ahead*/, int /*level_qty*/) {}

    // An ErrorMsg: the venue rejected something.
    virtual void on_error(const std::string& /*code*/, const std::string& /*message*/) {}

    // Latency instrumentation: record one tick-to-order sample (microseconds).
    // The base class prints a running summary to stderr; override to customise.
    virtual void record_latency(const std::string& symbol, long long micros);

private:
    void connect_handlers();               // wire IXWebSocket callbacks
    void send_handshake();                 // emit the Handshake JSON on open
    void dispatch(const std::string& raw); // decode one inbound frame
    void send_raw(const std::string& json);

    ClientConfig      cfg_;
    ix::WebSocket     ws_;
    std::atomic<bool> session_open_{false};
    std::atomic<bool> running_{false};

    // Offline replay: when set, send_raw() buffers orders here instead of
    // hitting the socket, so run_replay() can emit them per input line.
    bool                     replay_mode_ = false;
    std::vector<std::string> replay_orders_;

    mutable std::mutex        book_mtx_;
    std::map<std::string, BookView> books_;

    // Latency histogram (coarse microsecond buckets) + running stats.
    std::mutex        lat_mtx_;
    long long         lat_count_ = 0;
    long long         lat_sum_   = 0;
    long long         lat_min_   = 0;
    long long         lat_max_   = 0;
};

}  // namespace arena
