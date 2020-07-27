---
title: Request Response Message Exchange Pattern
status: published
tags: c++, rotor
---

# Introduction

The plan is to examine request/response pattern in "abstract" actor framework,
and find why it is not so trivial to implement as it might appear at the first.
Later, we'll see how various C++ actor frameworks (CAF, sobjectizer, rotor)
support the pattern.

The Request Response Message Exchange Pattern sounds quite simple: a `client`
asks a `server` to proccess a `request` and return `response` to the client
once it is done.

# Synchronous analogy

This is completely artificial analogy, however it seem useful for further
explanations.

It can be said, that in synchronous request-response can be simply presented as
just a regular function call, where *request* is input parameter to a function,
and the *response* is the return type, i.e.


    struct request_t { ... };
    struct response_t { ... };

    response_t function(const request_t&) {  ... }

The "server" here is the `function` inself, and the "client" is the call-side side.

The semantic of this function says that the function **always** successfully
processes a request and return a result.

Probably, the most modern way to express that a function might fail is to
wrap the response into monad-like wrapper,
like [std::optional](https://en.cppreference.com/w/cpp/utility/optional),
[std::expected](https://github.com/TartanLlama/expected),
[boost::outcome](https://www.boost.org/doc/libs/1_70_0/libs/outcome/doc/html/index.html)
etc.

Probably, the mostly used way to let the caller know that request processing
failed is to throw an *exception*. However, it is not expressible in the modern C++,
so it should be mentioned somewhere in documentation or just assumed; in other
words, from the function signature `response_t function(const request_t&)` never
knows whether it always successfully processes request or sometimes it might
fail and thrown an exception.

# Problems of naive approach in actor framework

Let's realize the simple request-response approach within the imaginary actor
framework: the `on_request` method of server-actor is called with the request
payload, and it returns the `response_t`

    response_t server_t::on_request(request_t &)

The naive implementation of request-response approach follows the
same pattern as in synchronous analogy, when it "forgets" to express that
the request processing might fail. The actor framework responsibility is
to wrap payload (`response_t`, `request_t`) into messages (`message<request_t>`)
and deliver them as *messages* to related actors (client-actor, server-actor).
Later the payload will be unpacked.

The corresponding receiver interface of the client-actor will be like:

    void client_t::on_response(response_t&)

While it looks OK, how will the client-actor be able to distinguish responses
from different requests?

The first solution would be to use some conventions on the request/response
protocol, i.e. inject some synthetic `request_id` into `request_t` and
somewhere into `response_t`, generate and set  it on request-side (client-actor)
and presume that the server-actor will set it back together with the response.
There is a variation of the solution, when the `request_t` is completely
embedded into `response_t`.

While this will definitely work as it will be shown below with `sobjectizer`
framework, this also means that **there is no help from a actor framework**
and the burden of the implementation lies completely on a developer.

The second solution is to let it be managed somehow by an actor framework:
let is suspend client-actor execution until the response will be received.
The "suspending" in the context means that all messages for the client-actor
will be queued until the response will be received.

This will work as soon as everything goes fine. However, as soon as something
goes wrong (i.e. server-actor cannot process the request), the whole client-actor
will stuck: It will not able to process *any* message, because it infinitely
waits the particular one (the response).

In terms of actor-design the client-actor becomes **non-reactive** at the moment
when it stops processing messages, which was a "feature" of the imaginary
naive actor framework to "wait" the `response`.

You might guess, the per-request timeout timer, can resolve the problem. Yes,
indeed, however how in the interface

    void client_t::on_response(response_t&)

it is possible to tell client-actor about timeout trigger? I don't see an
acceptable way to do that. It is paradoxical situation, that a failure
can be detected, but there is no way to react on it.

Anyway, even if there would be a way to notify client-actor about failure,
it still does not completely solves the non-reactivity problem: an actor
becomes reactive only after timeout trigger, and until that it is still
"suspended" waiting `response_t` or timeout trigger.

The root of the problem caused by "forgetfulness" of the server-actor
interface to specify, that it might fail. It's time to review our interfaces,
then.

# No framework support for req/res pattern

Let's summarize the necessary pieces, which are required to implement
request/response pattern in actor-like manner.

First, the server-actor might fail in processing the request, and it
need to tell the framework and client-actor about the failure.

Second, the response should enriched to contain the original `request_id`
to make it possible for client-actor to map the response to the request.
(In other sources it might be named `correlation_id`, which serves the
same purpose).

Third, the original request from the client-actor should also contain
the `request_id`.

Forth, as the original response payload might be missing at all, it
should be wrapped monad-like container (`std::optional`, `std::unique_ptr`
etc.)

So, our basic structures should look like:

    struct request_t { ... };
    struct response_t { ... };
    using request_id_t = std::uint32; /* as as example */

    struct wrapped_request_t {
        request_id_t request_id;
        request_t req;
    };

    enum class request_error_t { SUCCESS, TIMEOUT, FAIL_REASON_1_1, ... };

    struct wrapped_response_t {
        request_error_t request_error;
        request_id_t request_id;
        std::optional<response_t> res;  /* may be it'll contain the payload */
    };

And the corresponding actor interfaces will be:

    wrapped_response_t server_t::on_request(wrapped_request_t& ) { ... }

    void client_t::on_response(wrapped_response_t& ) { ... }

The `FAIL_REASON_1_1` and other error codes are desirable, if the server
wants **fail early** and notify client about that. Otherwise, if server cannot
process request, it silently ignores the request; however client will be notified
only via timeout and it can only guess, what exactly was wrong. In other words,
it is not good practice in general *ignore* wrong requests; react on them is
much better.

So, sending an request from client to server should be like:

    struct client_t {
        ...
        request_id_t last_request_id = 1;
    };

    void client_t::some_method() {
        auto req_id = ++last_request_id;
        auto request = request_t{ ... };
        framework.send(server_address, wrapped_request_t{ req_id, std::move(request) } );
    }

However, the story does not end here, as the timeout-timer part is missing (i.e.
for the case, when server-actor does not answer at all). The needed pieces are:
1) per request timeout timer; 2) when the response arrives in time, the timer
should be cancelled; 3) otherwise, the message with empty payload and timeout-fail
reason should be delivered;  4) if the response still arrives after timeout trigger,
it should be silently  discarded. There is a sense to have this things in dedicated methods.

    /* whatever that is able to identify particular timer instance */
    using timer_id_t = ...;

    struct client_t {
        using timer_map_t = std::unordered_map<timer_id_t, request_id_t>;
        /* reverse mapping */
        using request_map_t = std::unordered_map<request_id_t, timer_id_t>;
        ...
        request_id_t last_request_id = 1;
        timer_map_t timer_map;
        request_map_t request_map;
    };

    void client_t::some_method() {
        auto req_id = ++last_request_id;
        auto request = request_t{ ... };
        framework.send(server_address, wrapped_request_t{ req_id, std::move(request) } );
        /* start timer */
        auto timer_id = timers_framework.start_time(timeout);
        timer_map.emplace(timer_id, req_id);
        request_map.emplace(req_id, timer_id);
    }

    void client_t::on_timer_trigger(timer_id_t timer_id) {
        auto request_id = timer_map[timer_id];
        this->on_response(wrapped_response_t{ request_error_t::TIMEOUT,  request_id });
        timer_map.erase(timer_id);
        request_map.erase(request_id);
    }

    void client_t::on_response_guard(wrapped_response_t& r) {
        if (request_map.count(r.request_id) == 0) {
            /* no timer, means timer already triggered and timeout-response was
            delivered, just discard the response */
            return;
        }
        auto timer_id = request_map[r.request_id];
        timers_framework.cancel(timer_id);
        this->on_response(r); /* actually deliver the response */
        timer_map.erase(timer_id);
        request_map.erase(request_id);
    }

Now, the example is complete. It should be able to handle request-responses
in a robust way. However, there is no actual request processing code, and
a lot of auxiliary code to make it responsible and robust.

The worse thing, if that the boilerplate code have to be repeated for
every request-response pair type. It is discouraging and error-prone way
of development; an developer might end up frustrated with actor-design
at all.

# req/res approach with sobjectizer

The [sobjectizer](https://github.com/Stiffstream/sobjectizer) actor framework
at the moment has version `5.6` and does not help in request-response pattern
usage. So, basically, it is like the request-response sample above without
framework support, with sobjectizer's specifics, of course.

In the example below the "probabilistic pong" (`server` role) is used: i.e. randomly
it successfully answers to ping-requests, and sometimes it just ignores the
requests. The `pinger` (`client` role) should be able to detect the both cases.

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

Output sample:

    ponger::on_ping 1, 0.475312
    pinger::on_timeout
    pinger::on_pong 1, success: 0

Other output sample:

    ponger::on_ping 1, 0.815891
    ponger::on_ping (sending pong back)
    pinger::on_pong 1, success: 1

It should be noted, that request/response pattern *was* supported in sobjectizer
[before](https://sourceforge.net/p/sobjectizer/wiki/so-5.5%20In-depth%20-%20Synchronous%20Interaction/)
version `5.6`, however it was dropped (well, moved to
[sobjectizer-extra](https://sourceforge.net/p/sobjectizer/wiki/About%20so5extra/]), which has
different licensing terms). The request/response was easy as the following like:

    auto r = so_5::request_value<Result,Request>(mbox, timeout, params);

It is convenient; nevertheless, from the explanation sample "How does it work?", the following
sample is available:

    // Waiting and handling the result.
    auto wait_result__ = f__.wait_for(timeout);
    if(std::future_status::ready != wait_result__)
       throw exception_t(...);
    auto r = f__.get();

it suffers the same **non-reactivity taint** as described above, i.e. lack of possibility to
answer other messages, while waiting a response. Hence, you can see "Deadlocks" section
in the documentation, and the developers responsibility to handle the situation.


# req/res approach with CAF

The [C++ actor framework](http://actor-framework.org/) (aka CAF), does support request/response
approach.

    #include <chrono>
    #include <iostream>
    #include <random>
    #include <string>

    #include "caf/all.hpp"
    #include "caf/optional.hpp"
    #include "caf/sec.hpp"

    using std::endl;
    using std::string;
    using namespace std::literals;

    using namespace caf;

    using ping_atom = atom_constant<atom("ping")>;
    using pong_atom = atom_constant<atom("pong")>;

    void ping(event_based_actor *self, actor pong_actor) {
      aout(self) << "ping" << endl;

      self->request(pong_actor, 1s, ping_atom::value)
          .then([=](pong_atom ok) { aout(self) << "pong received" << endl; },
                [=](error err) {
                  aout(self) << "pong was NOT received (timed out?), error code = "
                             << err.code() << endl;
                });
    }

    behavior pong(event_based_actor *self) {
      using generator_t = std::shared_ptr<std::mt19937>;
      using distrbution_t = std::shared_ptr<std::uniform_real_distribution<double>>;

      std::random_device rd;
      auto gen = std::make_shared<typename generator_t::element_type>(rd());
      auto distr = std::make_shared<typename distrbution_t::element_type>();
      return {[=](ping_atom) {
        auto dice = (*distr)(*gen);
        aout(self) << "pong, dice = " << dice << endl;
        if (dice > 0.5) {
          return optional<pong_atom>(pong_atom::value);
        }
        return optional<pong_atom>();
      }};
    }

    void caf_main(actor_system &system) {
      auto pong_actor = system.spawn(pong);
      auto ping_actor = system.spawn(ping, pong_actor);
    }

    CAF_MAIN()

Output sample:

    ping
    pong, dice = 0.571207
    pong received

Another output sample:

    ping
    pong, dice = 0.270214
    pong was NOT received (timed out?), error code = 2


The call `client_actor->request(server_actor, timeout, args..)` returns an intermediate
future-like object, where `then` method can be invoked with forwarded one-shot
actor behaviour. And, yes, there is `await` method too with non-reactive behaviour,
where you can shoot easily yourself with deadlock. So, according to the
[documentation](https://actor-framework.readthedocs.io/en/latest/MessagePassing.html#sending-requests-and-handling-responses)
`then` method is what we need, as it "multiplexes the one-shot handler with the
regular actor behaviour and handles requests as they arrive".

# req/res approach with rotor

The [rotor](https://github.com/basiliscos/cpp-rotor) does support request/response
approach since `v0.04`

    #include <rotor/ev.hpp>
    #include <iostream>
    #include <random>

    namespace payload {
    struct pong_t {};
    struct ping_t {
        using response_t = pong_t;
    };
    } // namespace payload

    namespace message {
    using ping_t = rotor::request_traits_t<payload::ping_t>::request::message_t;
    using pong_t = rotor::request_traits_t<payload::ping_t>::response::message_t;
    } // namespace message

    struct pinger_t : public rotor::actor_base_t {

        using rotor::actor_base_t::actor_base_t;

        void set_ponger_addr(const rotor::address_ptr_t &addr) { ponger_addr = addr; }

        void on_initialize(rotor::message::init_request_t &msg) noexcept override {
            rotor::actor_base_t::on_initialize(msg);
            subscribe(&pinger_t::on_pong);
        }

        void on_start(rotor::message_t<rotor::payload::start_actor_t> &) noexcept override {
            request<payload::ping_t>(ponger_addr).send(rotor::pt::seconds(1));
        }

        void on_pong(message::pong_t &msg) noexcept {
            auto &ec = msg.payload.ec;
            if (!msg.payload.ec) {
                std::cout << "pong received\n";
            } else {
                std::cout << "pong was NOT received: " << ec.message() << "\n";
            }
            supervisor.do_shutdown();
        }

        rotor::address_ptr_t ponger_addr;
    };

    struct ponger_t : public rotor::actor_base_t {
        using generator_t = std::mt19937;
        using distrbution_t = std::uniform_real_distribution<double>;

        std::random_device rd;
        generator_t gen;
        distrbution_t dist;

        ponger_t(rotor::supervisor_t &sup) : rotor::actor_base_t{sup}, gen(rd()) {}

        void on_initialize(rotor::message::init_request_t &msg) noexcept override {
            rotor::actor_base_t::on_initialize(msg);
            subscribe(&ponger_t::on_ping);
        }

        void on_ping(message::ping_t &req) noexcept {
            auto dice = dist(gen);
            std::cout << "pong, dice = " << dice << std::endl;
            if (dice > 0.5) {
                reply_to(req);
            }
        }
    };

    int main() {
        try {
            auto *loop = ev_loop_new(0);
            auto system_context = rotor::ev::system_context_ev_t::ptr_t{new rotor::ev::system_context_ev_t()};
            auto timeout = boost::posix_time::milliseconds{10};
            auto conf = rotor::ev::supervisor_config_ev_t{
                timeout, loop, true, /* let supervisor takes ownership on the loop */
            };
            auto sup = system_context->create_supervisor<rotor::ev::supervisor_ev_t>(conf);

            auto pinger = sup->create_actor<pinger_t>(timeout);
            auto ponger = sup->create_actor<ponger_t>(timeout);
            pinger->set_ponger_addr(ponger->get_address());

            sup->start();
            ev_run(loop);
        } catch (const std::exception &ex) {
            std::cout << "exception : " << ex.what();
        }

        std::cout << "exiting...\n";
        return 0;
    }

Output sample:

    pong, dice = 0.90477
    pong received

Another output sample:

    pong, dice = 0.24427
    pong was NOT received: request timeout


Comparing to [CAF](http://actor-framework.org/), `rotor`'s version is more
verbose in the terms of LOC (lines of code). Partly this is caused by omitted
`main` in `CAF`, while in `rotor` the main cannot be shortened because
it is assumed to work with different loop backends as well as in cooperation
with them and other non-actor loop components; partly because of in `CAF`
the message is hidden from user, while in `rotor` is is exposed outside
due to performance reasons (i.e. allow the payload to be smart-pointer
to have zero-copy); and finally because of `CAFs` intensive usage of
lambdas, which leads to more compact code.

However, it is still what it needed: reactive reactive request-response.

# Request/Response composability

On the top of `request-response` pattern, the **ask pattern** can be developed.
In short, an client-actor makes several of requests, and then, depending on the
results it makes an appropriate action. See
[akka](https://doc.akka.io/docs/akka/current/actors.html#ask-send-and-receive-future)
docs as an example,

However, the **ask pattern** it is a little bit more general: it should be
possible to access to the initial context (message) as well as to all responses
(some of which might fail).

The `sobjectizer` does not offer support request-response patters, so it is out of
comparison. The `sobjectizer-extra` offers `std::future` based solution,
however, as we seen, it not reactive (`while(!fututure.is_ready()){ ... }`)
and as the `std::futures` are not compose-able, the **ask pattern** cannot
be implemented.

As we've seen with `CAF` lambda approach, there are 2 lambdas *per request*
(one is for fail response and another is for success response); each one
captures outer request context and has access to its own response. Nonetheless,
none of the lambdas has access to the contexts of the other requests; in
other words the common context (which can include the original message)
should be *shared* between them, and the code compactness seems to lost.

Here is an example how to compose two ping-pong requests, where any of them
might fail.

    #include <chrono>
    #include <iostream>
    #include <random>
    #include <string>

    #include "caf/all.hpp"
    #include "caf/optional.hpp"
    #include "caf/sec.hpp"

    using std::endl;
    using std::string;
    using namespace std::literals;

    using namespace caf;

    using ping_atom = atom_constant<atom("ping")>;
    using pong_atom = atom_constant<atom("pong")>;

    struct shared_context_t {
      std::size_t pings_left;
      std::size_t pings_success = 0;
      std::size_t pings_error = 0;

      shared_context_t(std::size_t pings_left_) : pings_left{pings_left_} {}

      void output_results() {
        if (pings_left == 0) {
          // unsafe, aout should be used, but how to capture it?
          std::cout << "success: " << pings_success << ", errors: " << pings_error
                    << "\n";
        }
      }
      void record_success() {
        ++pings_success;
        --pings_left;
        output_results();
      }
      void record_fail() {
        ++pings_error;
        --pings_left;
        output_results();
      }
    };

    void ping(event_based_actor *self, actor pong_actor1, actor pong_actor2) {
      aout(self) << "ping" << endl;

      auto context = std::make_shared<shared_context_t>(2);
      self->request(pong_actor1, 1s, ping_atom::value)
          .then([=](pong_atom ok) { context->record_success(); },
                [=](error err) { context->record_fail(); });
      self->request(pong_actor2, 1s, ping_atom::value)
          .then([=](pong_atom ok) { context->record_success(); },
                [=](error err) { context->record_fail(); });
    }

    behavior pong(event_based_actor *self) {
      using generator_t = std::shared_ptr<std::mt19937>;
      using distrbution_t = std::shared_ptr<std::uniform_real_distribution<double>>;

      std::random_device rd;
      auto gen = std::make_shared<typename generator_t::element_type>(rd());
      auto distr = std::make_shared<typename distrbution_t::element_type>();
      return {[=](ping_atom) {
        auto dice = (*distr)(*gen);
        aout(self) << "pong, dice = " << dice << endl;
        if (dice > 0.5) {
          return optional<pong_atom>(pong_atom::value);
        }
        return optional<pong_atom>();
      }};
    }

    void caf_main(actor_system &system) {
      auto pong_actor1 = system.spawn(pong);
      auto pong_actor2 = system.spawn(pong);
      auto ping_actor = system.spawn(ping, pong_actor1, pong_actor2);
    }

    CAF_MAIN()

Output sample:

    ping
    pong, dice = 0.818207
    pong, dice = 0.140753
    success: 1, errors: 1

Another output sample:

    ping
    pong, dice = 0.832334
    pong, dice = 0.744168
    success: 2, errors: 0

I'm not `CAF`s expert, but it seems that in shared context it needs to
be captured the original behaviour to access `aout`, and there is need
to have two methods per each request type (or single composed one
with takes composite monad-like result).

Let's see how it works with `rotor`, however actors' addressing should be
explained first. `Akka` and `CAF` actor frameworks use the `ActorRef`
notion to (globally) identify an actor. It seems that there is one-to-one
matching between `ActorRef` and the actor. In `rotor` address is completely
decoupled from actor, and it can process messages on any address it is
subscribed to. There is "main" (or default) actors address which is used
for main `rotor` mechanics, still it can be subscribed to any address
and process messages on it.

That technique is shown below, when an **ephemeral address** is created
and an unique association between that address and context is created.
Here is a full code:

    #include <rotor/ev.hpp>
    #include <iostream>
    #include <random>
    #include <unordered_map>

    namespace payload {
    struct pong_t {};
    struct ping_t {
        using response_t = pong_t;
    };
    } // namespace payload

    namespace message {
    using ping_t = rotor::request_traits_t<payload::ping_t>::request::message_t;
    using pong_t = rotor::request_traits_t<payload::ping_t>::response::message_t;
    } // namespace message

    struct shared_context_t {
        std::size_t pings_left;
        std::size_t pings_success = 0;
        std::size_t pings_error = 0;
    };

    struct pinger_t : public rotor::actor_base_t {
        using map_t = std::unordered_map<rotor::address_ptr_t, shared_context_t>;

        using rotor::actor_base_t::actor_base_t;

        void set_ponger_addr1(const rotor::address_ptr_t &addr) { ponger_addr1 = addr; }
        void set_ponger_addr2(const rotor::address_ptr_t &addr) { ponger_addr2 = addr; }

        void on_start(rotor::message_t<rotor::payload::start_actor_t> &) noexcept override {
            reply_addr = create_address();
            subscribe(&pinger_t::on_pong, reply_addr);
            request_via<payload::ping_t>(ponger_addr1, reply_addr).send(rotor::pt::seconds(1));
            request_via<payload::ping_t>(ponger_addr2, reply_addr).send(rotor::pt::seconds(1));
            request_map.emplace(reply_addr, shared_context_t{2});
        }

        void on_pong(message::pong_t &msg) noexcept {
            auto &ctx = request_map[msg.address];
            --ctx.pings_left;
            auto &ec = msg.payload.ec;
            if (ec) {
                ++ctx.pings_error;
            } else {
                ++ctx.pings_success;
            }
            if (!ctx.pings_left) {
                std::cout << "success: " << ctx.pings_success << ", errors: " << ctx.pings_error << "\n";
                // optional cleanup
                unsubscribe(&pinger_t::on_pong, reply_addr);
                request_map.erase(msg.address);
                supervisor.do_shutdown();
            }
        }

        map_t request_map;
        rotor::address_ptr_t ponger_addr1;
        rotor::address_ptr_t ponger_addr2;
        rotor::address_ptr_t reply_addr;
    };

    struct ponger_t : public rotor::actor_base_t {
        using generator_t = std::mt19937;
        using distrbution_t = std::uniform_real_distribution<double>;

        std::random_device rd;
        generator_t gen;
        distrbution_t dist;

        ponger_t(rotor::supervisor_t &sup) : rotor::actor_base_t{sup}, gen(rd()) {}

        void on_initialize(rotor::message::init_request_t &msg) noexcept override {
            rotor::actor_base_t::on_initialize(msg);
            subscribe(&ponger_t::on_ping);
        }

        void on_ping(message::ping_t &req) noexcept {
            auto dice = dist(gen);
            std::cout << "pong, dice = " << dice << std::endl;
            if (dice > 0.5) {
                reply_to(req);
            }
        }
    };

    int main() {
        try {
            auto *loop = ev_loop_new(0);
            auto system_context = rotor::ev::system_context_ev_t::ptr_t{new rotor::ev::system_context_ev_t()};
            auto timeout = boost::posix_time::milliseconds{10};
            auto conf = rotor::ev::supervisor_config_ev_t{
                timeout, loop, true, /* let supervisor takes ownership on the loop */
            };
            auto sup = system_context->create_supervisor<rotor::ev::supervisor_ev_t>(conf);

            auto pinger = sup->create_actor<pinger_t>(timeout);
            auto ponger1 = sup->create_actor<ponger_t>(timeout);
            auto ponger2 = sup->create_actor<ponger_t>(timeout);
            pinger->set_ponger_addr1(ponger1->get_address());
            pinger->set_ponger_addr2(ponger2->get_address());

            sup->start();
            ev_run(loop);
        } catch (const std::exception &ex) {
            std::cout << "exception : " << ex.what();
        }

        std::cout << "exiting...\n";
        return 0;
    }

Output sample:

    pong, dice = 0.472509
    pong, dice = 0.305997
    success: 0, errors: 2

Another output sample:

    pong, dice = 0.103796
    pong, dice = 0.8862
    success: 1, errors: 1

Rotor has special support of requests to be replied to custom addresses
(i.e. `request_via` method). The main difference with the `CAF` that instead
of multiple lambdas with additional methods (`record_success` and `record_fail`)
and "gather-them-all" method (`output_results`), with `rotor` there is
just single gather-them-all method (`on_pong`), which actually has exactly
the same signature when as the previous example with `rotor`.

# Conclusion

When you start thinking about possible failures the initially request
response schema abruptly becomes non-trivial. Timeout and other errors
should be handled and without framework support the code quite quickly
becomes cumbersome.

There is still additional requirements, that the provided by a framework
support of request/response pattern did not come of cost of loosing
actor's *reactivity*; for simplicity, you may treat it as
dead-lock avoidance. Another nice-to-have feature would be composability
of the requests.

At the moment `sobjectizer` does not provides request/response pattern,
however in the past it did, however it was *non-reactive*.

Both `CAF` and `rotor` do provide request/response pattern keeping
still actors *reactive*. `CAF` has more compact code; the `rotor's`
code is more verbose. It seems that in `CAF` you should roll
you own composability of requests, i.e. develop context class
and make it shared between different requests handlers. In `rotor`
the composability of requests seems more natural via creating
ephemeral reply addresses, which can associate the linked group
of requests in single place.

# Update

The `sobjectizer` author replied with separate
[article](https://eao197.blogspot.com/2019/10/progc-follow-up-for-basiliscoss-article.html).
, which I recommend to read.

So, it should be updated, that [sobjetizer-extra](https://github.com/Stiffstream/so5extra)
provides support for request-response pattern, but only via a bit
different name (`async_op`, in the case). It is completely
asynchronous and free of dead-locks, i.e. **reactive**.

It is also
[composable](https://github.com/eao197/so5-request-reply-example/blob/master/dev/sample_composability_2/main.cpp),
with the approximately same lines of code as `rotor` example.
The composability is done via lambdas (as in `CAF`), but
the responses are redirected to different `mboxes` (as the
ephemeral addresses in `rotor`).

So, it is possible to get the same result with all considered
frameworks.
