// ─────────────────────────────────────────────────────────────────────────────
// main.cpp — AlgoArena HFT example bot.
//
// This is the file a student starts from. It defines one concrete HFTBot,
// `SpreadCaptureBot`, wires Ctrl-C to a clean shutdown, and runs it.
//
// The example is a deliberately simple SPREAD-CAPTURE / momentum bot so that
// `hft_bot` does something out of the box. Replace the body of on_book() with
// your own edge — that is the whole assignment. Everything below the strategy
// class is plumbing you should not need to touch.
// ─────────────────────────────────────────────────────────────────────────────

#include <atomic>
#include <csignal>
#include <iostream>
#include <string>
#include <unordered_map>

#include "hft_bot.hpp"

// ═════════════════════════════════════════════════════════════════════════════
// EXAMPLE STRATEGY — replace this with your own.
//
// Spread capture with a momentum tilt: on every book update, if the spread is
// wide enough to be worth crossing and price has ticked in one direction since
// the last snapshot, take the near side for a small clip. Flat inventory cap so
// the demo never runs away. ~30 lines of actual logic.
// ═════════════════════════════════════════════════════════════════════════════
class SpreadCaptureBot : public arena::HFTBot {
public:
    using arena::HFTBot::HFTBot;

private:
    void on_book(const std::string& symbol, double bid, double ask, double mid,
                 double microprice, double obi) override {
        // TODO(student): THIS is your edge. microprice/obi are free signal
        // inputs (M6) — but using them costs compute, which costs queue
        // position. Everything below is a placeholder.
        (void)microprice; (void)obi;
        if (bid <= 0.0 || ask <= 0.0) return;          // need a two-sided book

        const double spread   = ask - bid;
        const double last_mid = last_mid_.count(symbol) ? last_mid_[symbol] : mid;
        last_mid_[symbol]     = mid;

        const int    pos    = position_[symbol];
        const int    kClip  = 1;                        // shares per order
        const int    kMaxPos = 5;                        // inventory cap
        const double kEdge  = 0.02;                      // min spread to act on

        if (spread < kEdge) return;                      // too tight to bother

        // Momentum tilt: buy the ask when the mid is rising, sell the bid when
        // it is falling — but only within the inventory cap.
        if (mid > last_mid && pos < kMaxPos) {
            buy_limit(symbol, kClip, ask);               // cross to buy
        } else if (mid < last_mid && pos > -kMaxPos) {
            sell_limit(symbol, kClip, bid);              // cross to sell
        }
    }

    void on_fill(const std::string& side, const std::string& symbol,
                 int quantity, double price, bool maker) override {
        if (side.empty()) return;                        // not our fill
        position_[symbol] += (side == "buy") ? quantity : -quantity;
        std::cerr << "[fill] " << (maker ? "MAKER " : "taker ") << side << " "
                  << quantity << " " << symbol << " @ " << price
                  << "  pos=" << position_[symbol] << "\n";
    }

    void on_session(const std::string& event, const std::string& message) override {
        std::cerr << "[session] " << event << " — " << message << "\n";
        // TODO(student): decide what to do on SESSION_CLOSED (e.g. flatten).
        if (event == "SESSION_CLOSED") {
            for (auto& [sym, qty] : position_) {
                if (qty > 0)      sell_market(sym, qty);
                else if (qty < 0) buy_market(sym, -qty);
            }
        }
    }

    std::unordered_map<std::string, int>    position_;   // symbol -> net shares
    std::unordered_map<std::string, double> last_mid_;   // symbol -> previous mid
};

// ─────────────────────────────────────────────────────────────────────────────
// Plumbing: signal-driven shutdown + entry point. Students need not edit below.
// ─────────────────────────────────────────────────────────────────────────────

namespace {
std::atomic<arena::HFTBot*> g_bot{nullptr};
void handle_signal(int) {
    if (auto* b = g_bot.load()) b->stop();
}
}  // namespace

int main(int argc, char** argv) {
    const arena::ClientConfig cfg = arena::ClientConfig::from_env();
    bool replay = false;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--replay") replay = true;

    SpreadCaptureBot bot(cfg);

    if (replay) {
        // Offline latency benchmark: driven by scripts/latency_replay.py over
        // stdin/stdout. No socket, no signals — just the tape through the bot.
        bot.run_replay(std::cin, std::cout);
        return 0;
    }

    std::cerr << "[main] AlgoArena HFT bot — team=" << cfg.team_id
              << " url=" << cfg.url << "\n";
    g_bot.store(&bot);
    std::signal(SIGINT,  handle_signal);
    std::signal(SIGTERM, handle_signal);

    bot.run();   // blocks until Ctrl-C
    std::cerr << "[main] shutting down\n";
    return 0;
}
