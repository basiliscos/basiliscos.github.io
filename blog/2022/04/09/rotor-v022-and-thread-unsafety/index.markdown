---
status: published
title: 'rotor v0.22 and thread unsafety'
tags: c++ 
---

There is not so well explained `BUILD_THREAD_UNSAFE`
[rotor](https://github.com/basiliscos/cpp-rotor/) build option. Technically
it means, that [boost's intrusive ptr](https://www.boost.org/doc/libs/1_78_0/libs/smart_ptr/doc/html/smart_ptr.html#intrusive_ptr)
is used in thread-unsafe manner, i.e. the underlying reference counter
is not atomic. Accordingly, all objects (in our case messages and actors from
`rotor`) cannot be accessed from different threads concurrently.

It should be explicitly mentioned, that rotor's cross-thread messaging facility
also cannot be used, otherwise there is notorious UB (undefined behavior).

Practically that usually means, that you are building single-threaded
application, most likely a service with asio/ev backend.

Why you might need that feature? **Performance** is the answer, i.e.
when you need rotor supervising/messaging facilities in single-threaded app.
According to my measurements, with the thread-unsafety you'll get **~30.8**
millions of messages per second instead of **~23.5** with the feauture disabled,
i.e. ~30% of performance boost for free.

The question arises, then, how to stop that single threaded application?
With the thread safety it can be done via launching additional thread, which
monitors some *atomic flag*, and, once it detects that it is set, it sends
shutdown signal to the root supervisor. The *atomic flag* is set externally,
i.e. in signal handler (NB: you cannot send shutdown message within
signal handler as all memory allocations are prohibited).

    ...
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = [](int) { shutdown_flag = true; };
    auto r = sigaction(SIGINT, &action, nullptr);
    ...
    auto console_thread = std::thread([&] {
        while (!shutdown_flag) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        supervisor->shutdown();
    });

    supervisor->start();
    io_context.run();
    console_thread.join();

However, this is no longer possible with thread-unsafety option. What
can be done instead, is periodical flag checking from the rotor
thread itself, i.e. using timers from root supervisor. I found that
I use that feature frequently, so I decided to include it in 
[rotor](https://github.com/basiliscos/cpp-rotor/): in supervisor
builder there is needed to specify reference to the shutdown_flag
and the frequency of checking it, i.e.:

    rth::system_context_thread_t ctx;
    auto timeout = r::pt::milliseconds{100};
    auto sup = ctx.create_supervisor<rth::supervisor_thread_t>()
                   .timeout(timeout)
                   .shutdown_flag(shutdown_flag, timeout / 2)
                   .finish();

(You still need to have to set it externally, like in the example
above with `sigaction` call). When it detects the flag is set to
`true` it shuts self down.

The full example of usage can be seen at
[examples/thread/sha512.cpp](https://github.com/basiliscos/cpp-rotor/blob/master/examples/thread/sha512.cpp)


`BUILD_THREAD_UNSAFE` is turned off by default. You should explicitly turn
it on if you are knowning what you are doing.
