// ─────────────────────────────────────────────────────────────────────────────
// arena_client.cpp — implementation of the AlgoArena HFT transport layer.
//
// Every outbound message is a JSON object whose shape matches a Pydantic model
// in shared/messages.py exactly (the exchange calls parse_message() on it).
// Every inbound frame is dispatched by its "type" discriminator to a virtual
// handler. See arena_client.hpp for the class contract.
// ─────────────────────────────────────────────────────────────────────────────

#include "arena_client.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <thread>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace arena {

namespace {

// Read an environment variable, falling back to `def` when unset/empty.
std::string env_or(const char* key, const std::string& def) {
    const char* v = std::getenv(key);
    if (v == nullptr || *v == '\0') return def;
    return std::string(v);
}

}  // namespace

// ── ClientConfig ─────────────────────────────────────────────────────────────

ClientConfig ClientConfig::from_env() {
    ClientConfig c;
    c.team_id = env_or("TEAM_ID", "hft_alpha");
    c.token   = env_or("ARENA_TOKEN", "");

    // EXCHANGE_URL wins (hosted TLS play, e.g. wss://feed.arena.example.edu),
    // otherwise assemble ws://HOST:PORT — the same precedence as trader/config.py.
    const std::string url = env_or("EXCHANGE_URL", "");
    if (!url.empty()) {
        c.url = url;
    } else {
        const std::string host = env_or("EXCHANGE_HOST", "localhost");
        const std::string port = env_or("EXCHANGE_PORT", "8765");
        c.url = "ws://" + host + ":" + port;
    }

    const std::string lvl = env_or("ARENA_LEVEL", "1");
    try { c.level = std::stoi(lvl); } catch (...) { c.level = 1; }
    return c;
}

// ── ArenaClient ──────────────────────────────────────────────────────────────

ArenaClient::ArenaClient(ClientConfig cfg) : cfg_(std::move(cfg)) {
    ws_.setUrl(cfg_.url);
    // Keepalive so a silent exchange restart is detected promptly.
    ws_.setPingInterval(15);
    // IXWebSocket auto-reconnects with backoff; cap it so we retry briskly.
    ws_.setMaxWaitBetweenReconnectionRetries(3000);  // ms
    connect_handlers();
}

ArenaClient::~ArenaClient() {
    ws_.stop();
}

void ArenaClient::connect_handlers() {
    ws_.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
        switch (msg->type) {
            case ix::WebSocketMessageType::Open:
                std::cerr << "[arena] connected to " << cfg_.url
                          << " as " << cfg_.team_id << " (trader)\n";
                session_open_.store(false);  // re-gate on every (re)connect
                send_handshake();
                break;

            case ix::WebSocketMessageType::Message:
                dispatch(msg->str);
                break;

            case ix::WebSocketMessageType::Close:
                std::cerr << "[arena] connection closed ("
                          << msg->closeInfo.code << ": "
                          << msg->closeInfo.reason << ") — reconnecting\n";
                session_open_.store(false);
                break;

            case ix::WebSocketMessageType::Error:
                std::cerr << "[arena] socket error: "
                          << msg->errorInfo.reason << " — retrying\n";
                break;

            default:
                break;
        }
    });
}

void ArenaClient::send_handshake() {
    // Mirrors shared.messages.Handshake:
    //   {"type":"handshake","team_id":..,"role":"trader","level":N,"token":..}
    json hs = {
        {"type",    "handshake"},
        {"team_id", cfg_.team_id},
        {"role",    "trader"},
        {"level",   cfg_.level},
        {"token",   cfg_.token},
    };
    send_raw(hs.dump());
}

void ArenaClient::send_raw(const std::string& payload) {
    if (replay_mode_) {
        // Offline benchmark: capture the order instead of sending it.
        replay_orders_.push_back(payload);
        return;
    }
    // IXWebSocket queues the frame; safe to call from any thread. It silently
    // drops if the socket is not open, which mirrors the Python bot's
    // "connection closed" tolerance.
    ws_.sendText(payload);
}

void ArenaClient::run_replay(std::istream& in, std::ostream& out) {
    // No socket, no session gating — feed the tape straight through the
    // strategy and time each tick->order path. One output line per input.
    replay_mode_ = true;
    session_open_.store(true);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) { out << "[]\n"; out.flush(); continue; }
        replay_orders_.clear();
        dispatch(line);                 // stamps recv, runs on_book, buffers orders + LAT
        out << "[";
        for (size_t i = 0; i < replay_orders_.size(); ++i) {
            if (i) out << ",";
            out << replay_orders_[i];
        }
        out << "]\n";
        out.flush();                    // the harness times until this arrives
    }
}

void ArenaClient::run() {
    running_.store(true);
    ws_.start();  // spins up the background receive/reconnect thread
    // Block until stop() is requested; the socket thread does the real work.
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ws_.stop();
}

void ArenaClient::stop() {
    running_.store(false);
}

// ── Inbound dispatch ─────────────────────────────────────────────────────────

void ArenaClient::dispatch(const std::string& raw) {
    // Stamp the arrival clock BEFORE any parsing so the latency measurement
    // captures the whole tick-to-order path the student controls.
    const time_point recv_time = clock::now();

    json m;
    try {
        m = json::parse(raw);
    } catch (const std::exception& e) {
        std::cerr << "[arena] bad JSON frame: " << e.what() << "\n";
        return;
    }

    const std::string type = m.value("type", "");

    if (type == "book_snapshot") {
        BookView bv;
        bv.symbol     = m.value("symbol", "");
        bv.mid        = m.value("mid_price", 0.0);
        bv.spread     = m.value("spread", 0.0);
        bv.ref        = m.value("ref_price", 0.0);
        bv.microprice = m.value("microprice", 0.0);
        bv.obi        = m.value("obi", 0.0);
        bv.asset_type = m.value("asset_type", "equity");
        // bids/asks are [[price, qty], ...]; best is index 0 on each side.
        if (m.contains("bids") && m["bids"].is_array() && !m["bids"].empty())
            bv.best_bid = m["bids"][0][0].get<double>();
        if (m.contains("asks") && m["asks"].is_array() && !m["asks"].empty())
            bv.best_ask = m["asks"][0][0].get<double>();

        {
            std::lock_guard<std::mutex> lk(book_mtx_);
            books_[bv.symbol] = bv;
        }
        on_book_snapshot(bv, recv_time);

    } else if (type == "trade_execution") {
        const std::string sym    = m.value("symbol", "");
        const int         qty    = m.value("quantity", 0);
        const double      price  = m.value("price", 0.0);
        const std::string buyer  = m.value("buyer_id", "");
        const std::string seller = m.value("seller_id", "");
        std::string side;
        if (buyer == cfg_.team_id)       side = "buy";
        else if (seller == cfg_.team_id) side = "sell";
        // maker == our resting order was hit (we earned the rebate); taker ==
        // we crossed the spread (we paid the fee).
        const bool maker = (m.value("maker_id", "") == cfg_.team_id);
        on_fill(side, sym, qty, price, maker);

    } else if (type == "portfolio_update") {
        on_portfolio(m.value("cash", 0.0), m.value("net_worth", 0.0),
                     m.value("realized_pnl", 0.0), m.value("unrealized_pnl", 0.0));

    } else if (type == "session_event") {
        const std::string ev  = m.value("event", "");
        const std::string txt = m.value("message", "");
        if (ev == "SESSION_OPEN" || ev == "SESSION_PREOPEN") {
            session_open_.store(true);
        } else if (ev == "SESSION_CLOSED") {
            session_open_.store(false);
        }
        on_session(ev, txt);

    } else if (type == "order_ack") {
        on_order_ack(m.value("order_id", ""), m.value("symbol", ""),
                     m.value("side", ""), m.value("price", 0.0),
                     m.value("quantity", 0),
                     m.value("queue_ahead", 0), m.value("level_qty", 0));

    } else if (type == "queue_update") {
        on_queue_update(m.value("order_id", ""), m.value("symbol", ""),
                        m.value("side", ""), m.value("price", 0.0),
                        m.value("queue_ahead", 0), m.value("level_qty", 0));

    } else if (type == "error") {
        on_error(m.value("code", ""), m.value("message", ""));

    }
    // leaderboard / command_ack and any other types are ignored here.
}

// ── Send helpers — exact PlaceOrder / CancelOrder JSON ───────────────────────

void ArenaClient::place_limit(const std::string& symbol, const std::string& side,
                              int quantity, double price) {
    // Mirrors shared.messages.PlaceOrder (order_type="limit"). stop_price is
    // optional in the schema, so we omit it for wire compatibility.
    json o = {
        {"type",       "place_order"},
        {"team_id",    cfg_.team_id},
        {"symbol",     symbol},
        {"side",       side},
        {"order_type", "limit"},
        {"price",      price},
        {"quantity",   quantity},
    };
    send_raw(o.dump());
}

void ArenaClient::place_market(const std::string& symbol, const std::string& side,
                               int quantity) {
    // price is ignored by the venue for market orders, but the schema requires
    // the field, so we send 0.0 (as the Python trader does).
    json o = {
        {"type",       "place_order"},
        {"team_id",    cfg_.team_id},
        {"symbol",     symbol},
        {"side",       side},
        {"order_type", "market"},
        {"price",      0.0},
        {"quantity",   quantity},
    };
    send_raw(o.dump());
}

void ArenaClient::cancel(const std::string& order_id, const std::string& symbol) {
    json c = {
        {"type",     "cancel_order"},
        {"team_id",  cfg_.team_id},
        {"order_id", order_id},
        {"symbol",   symbol},
    };
    send_raw(c.dump());
}

// ── Book accessor ────────────────────────────────────────────────────────────

bool ArenaClient::book(const std::string& symbol, BookView& out) const {
    std::lock_guard<std::mutex> lk(book_mtx_);
    auto it = books_.find(symbol);
    if (it == books_.end()) return false;
    out = it->second;
    return true;
}

// ── Latency instrumentation ──────────────────────────────────────────────────

void ArenaClient::record_latency(const std::string& symbol, long long micros) {
    std::lock_guard<std::mutex> lk(lat_mtx_);
    if (lat_count_ == 0) {
        lat_min_ = lat_max_ = micros;
    } else {
        lat_min_ = std::min(lat_min_, micros);
        lat_max_ = std::max(lat_max_, micros);
    }
    lat_sum_ += micros;
    ++lat_count_;

    // Canonical latency line the Python harness parses (scripts/latency_report
    // .py): `LAT <symbol> <micros>`. Every 50 samples also print a running
    // summary so the tail is visible without a full log.
    std::cerr << "LAT " << symbol << " " << micros << "\n";
    if (lat_count_ % 50 == 0) {
        std::cerr << "[latency] n=" << lat_count_
                  << " avg=" << (lat_sum_ / lat_count_)
                  << " min=" << lat_min_
                  << " max=" << lat_max_ << " us\n";
    }
}

}  // namespace arena
