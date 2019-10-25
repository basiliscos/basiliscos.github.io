#include <so_5/all.hpp>
#include <optional>
#include <random>
#include <unordered_map>

using request_id_t = std::uint32_t;
using namespace std::literals;

struct ping {};

struct pong {};

struct timeout_signal {
    request_id_t request_id;
};

enum class request_error_t { SUCCESS, TIMEOUT };

struct wrapped_ping {
    request_id_t request_id;
    ping payload;
};

struct wrapped_pong {
    request_id_t request_id;
    request_error_t error_code;
    std::optional<pong> payload;
};

class pinger final : public so_5::agent_t {
    using timer_t = std::unique_ptr<so_5::timer_id_t>;
    using request_map_t = std::unordered_map<request_id_t, timer_t>;

    so_5::mbox_t ponger_;
    request_map_t request_map;
    request_id_t last_request = 0;

    void on_pong(mhood_t<wrapped_pong> cmd) {
        auto &timer = request_map.at(cmd->request_id);
        timer->release();
        request_map.erase(cmd->request_id);
        on_pong_delivery(*cmd);
    }

    void on_pong_delivery(const wrapped_pong &cmd) {
        bool success = cmd.error_code == request_error_t::SUCCESS;
        auto request_id = cmd.request_id;
        std::cout << "pinger::on_pong " << request_id << ", success: " << success << "\n";
        so_deregister_agent_coop_normally();
    }

    void on_timeout(mhood_t<timeout_signal> msg) {
        std::cout << "pinger::on_timeout\n";
        auto request_id = msg->request_id;
        request_map.erase(request_id);
        wrapped_pong cmd{request_id, request_error_t::TIMEOUT};
        on_pong_delivery(cmd);
    }

  public:
    pinger(context_t ctx) : so_5::agent_t{std::move(ctx)} {}

    void set_ponger(const so_5::mbox_t mbox) { ponger_ = mbox; }

    void so_define_agent() override { so_subscribe_self().event(&pinger::on_pong).event(&pinger::on_timeout); }

    void so_evt_start() override {
        auto request_id = ++last_request;
        so_5::send<wrapped_ping>(ponger_, request_id);
        auto timer = so_5::send_periodic<timeout_signal>(*this, so_direct_mbox(), 200ms,
                                                         std::chrono::milliseconds::zero(), request_id);
        auto timer_ptr = std::make_unique<so_5::timer_id_t>(std::move(timer));
        request_map.emplace(request_id, std::move(timer_ptr));
    }
};

class ponger final : public so_5::agent_t {
    const so_5::mbox_t pinger_;
    std::random_device rd;
    std::mt19937 gen;
    std::uniform_real_distribution<> distr;

  public:
    ponger(context_t ctx, so_5::mbox_t pinger) : so_5::agent_t{std::move(ctx)}, pinger_{std::move(pinger)}, gen(rd()) {}

    void so_define_agent() override {
        so_subscribe_self().event([this](mhood_t<wrapped_ping> msg) {
            auto dice_roll = distr(gen);
            std::cout << "ponger::on_ping " << msg->request_id << ", " << dice_roll << "\n";
            if (dice_roll > 0.5) {
                std::cout << "ponger::on_ping (sending pong back)" << std::endl;
                so_5::send<wrapped_pong>(pinger_, msg->request_id, request_error_t::SUCCESS, pong{});
            }
        });
    }
};

int main() {
    so_5::launch([](so_5::environment_t &env) {
        env.introduce_coop([](so_5::coop_t &coop) {
            auto pinger_actor = coop.make_agent<pinger>();
            auto ponger_actor = coop.make_agent<ponger>(pinger_actor->so_direct_mbox());

            pinger_actor->set_ponger(ponger_actor->so_direct_mbox());
        });
    });

    return 0;
}
