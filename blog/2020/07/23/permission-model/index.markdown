---
status: published
title: C++ permission model
tags: c++
---


# Abstract

The problems of `public`, `protected` and `private` access are considered.
The generic templated `access` approach  with the on-demand specialization
for a consumer is proposed; it's advantages and drawbacks are discussed.
The synthetic solution satisfactory enough is proposed in the conclusion.

# The problem

Out of the box C++ offers "classical" class access model: `public` properties
(fields and methods) are accessible from anywhere, `protected` properties
are accessible to descendant classes only, and finally `private` properties
which permit access to class itself only.

Additionally it is possible to declare `friend` class (which might be templated)
to provide maximum access to all properties (i.e. the same as `private`). This
allows to access to the internals of a class to a **related** class.

For example, if there is an HTTP-library with `Request` and `Connection` classes
and `Request` class would like to access `Connection` internals, this can be done
as:

    class Request;  /* forward declare */

    enum class Shutdown { read, write, both };
    class Connection {
        public:
            virtual void handle() { ... }
        private:
            void shutdown(Shutdown how) { ...;  }
            int skt;
            friend class Request;
    };

    class Request {
        public:
            virtual void process() {
                ...;
                /* I know what I'm doing */
                conn->shutdown(Shutdown::both);
            }
        protected:
          Connection* conn;
    };

Now let's assume that there is an descendant

    class HttpRequest: public Request {
        public:
            virtual void process() override {
                conn->shutdown(Shutdown::both); // Oops!
            }
    };

Alas, there is no way in C++ to access to `Connection::shutdown` from it.
To overcome this, with the current access model, there are the possibilities.
*First*, it is possible to declare `HttpRequest` as a friend in the `Connection`.
Whilst this will certainly work, the solution has strict limitation, that it
applicable only for single library (project) to code of which you have access.
Otherwise, *it does not scales at all*.

The *second* possibility if is to "expose" private connection from the `Request`
class to all it's descendants, like:

    class Request {
        protected:
            void connection_shutdown(Shutdown how) {  conn->shutdown(how); }
            int& connection_socket() {  conn->skt; }
            const int& connection_socket() const {  conn->skt; }
            Connection* conn;
    };

This approach is better, because it scales to all descendant classes which can
be located in different libraries. However, the price is quite high as there
is need to provide access to all properties apriori even if some properties
will not be needed. The more serious drawback is that the approach is limited
to class inheritance; in other words, if there is need to access private
properties of `Connection` not from `Request`'s descendants, e.g. for tests.

Somebody might become disappointed at all and try to make everything **public by
default**. This scales well and covers all abovedescribed issues though brings
a new ones: the boundary between stable *public* API interface and *private*
implementation details of a class is blurred and completion suggestions in
an IDE can be overloaded with too many variants. In other words the proposed
approach is *too permissive*.

Semantically identical would be to write simple **accessors for all private
properties**; it just brings an *illusion* of the interface/implementation
separation since a class author already exposed all class internals outside.

Let's summarize the requirements for the private properties (aka implementation
details):

 - they should scale outside of a library

 - they should be accessible outside of class hierarchy

 - they should not "pollute" the class public API, i.e. somehow be not
available by default, still be accessible

# The possible solutions

It consists of two pieces, the first one is to declare possibility to access
the private fields of a class, e.g.:

    // my_library.h
    namespace my::library {
        class Connection {
            public:
                virtual void handle() { ... }
                template<typename T...> auto& access() noexcept;
            private:
                void shutdown(Shutdown how) { ...;  }
                int skt;
        };
    }

The second piece is actually provide full access specialization in the
target place, e.g. :

    // my_app.cpp
    namespace to {
        struct skt{}; // matches private field
    }

    namespace my::library {
        auto& Connection::access<to::skt>() noexcept { return skt; }
    }

    // namespace does not matter
    class HttpRequest: public Request {
        public:
            virtual void process() override {
                auto& s = conn->access<to::skt>();  // voila!
                shutdown(s, SHUT_RDWR);
            }
    };

In other words, in the source class the generic templated accessor
is defined, and in the place, where the access is needed, the specialization
is provided as the actual access to the required fields.

The solution meets all requirements, however it still has it's own drawbacks.
First, there is need of duplication of `const` and `non-const` access, i.e.

    class Connection {
        public:
            virtual void handle() { ... }
            template<typename T...> auto& access() noexcept;
            template<typename T...> auto& access() const noexcept;
    };

    ...

    namespace my::library {
        auto& Connection::access<to::skt>() noexcept       { return skt; }
        auto& Connection::access<to::skt>() const noexcept { return skt; }
    }

Although, you don't have to provide `const` and `non-const` access if you need
only one.

The second drawback, that to let the approach work *for methods*, especially
those, which return type can't be `auto&` (e.g. `void` or `int`). To overcome
it the `access` should be rewritten as:

    class Connection {
        public:
            template<typename T, typename... Args> T access(Args...);
    }

    namespace my::library {
        void Connection::access<void, Shutdown>(Shutdown how) {
            return shutdown(how);
        }
    }

    class HttpRequest: public Request {
        public:
            virtual void process() override {
                conn->access<void, Shutdown>(Shutdown::both);
            }
    };

Another problem arises: if there are two or more private methods with identical
signatures (return and arguments types), the artificial tag should be introduced
again, i.e.

    class Connection {
        template<typename T, typename Tag, typename... Args> T access(Args...);
    };

    namespace to {
        struct skt{};
        struct shutdown{};
    }

    namespace my::library {
        int& Connection::access<int&, to::skt>() { return skt; }
        void Connection::access<void, to::shutdown, Shutdown>(Shutdown how) {
            shutdown(how);
        }
    }

    ...
    conn->access<void, to::shutdown>(Shutdown::both); // voila!

The variadic `Args...` template parameter dos not force to duplicate the original
arguments; it can have even add unrelated types to "inject" new methods with additional
logic into the `Connection` class. For example:

    namespace to {
        struct fun{}
    }

    namespace my::library {
        void Connection::access<void, to::fun>() {
            Shutdown how = std::rand() > 1000 ? Shutdown::read ? Shutdown::write;
            shutdown(how);
        }
    }

It is known, that methods might have optional `noexcept` specification in addition
to `const`. So, for the sake of generality, all four access cases should be
provided, i.e.:

    class Connection {
    public:
    template<typename T, typename Tag, typename... Args> T access(Args...);
    template<typename T, typename Tag, typename... Args> T access(Args...) const;
    template<typename T, typename Tag, typename... Args> T access(Args...) noexcept;
    template<typename T, typename Tag, typename... Args> T access(Args...) const noexcept;
    };

Alas, it was not the last problem with the approach: there is a problem with inheritance,
e.g.:

    class Connection {
        template<typename T> auto& access();
    private:
        int skt;
    };

    enum class SslState { /* whatever */};

    class SslConnection:public Connection {
    public:
        template<typename T> auto& access();
    private:
        SslState state;
    };

    namespace to {
        struct skt{};
        struct state{};
    }

    namespace my::library {
        auto& Connection::access<to::skt>() { return skt; }
        auto& SslConnection::access<to::state>() { return state; }
    }

However, as soon as try to access to parent property via child class, i.e.:

    SslConnection* conn = ...;
    auto& skt = conn->access<to::skt>(); // oops!

It cannot resolve access to socket via `SslConnection` because there is no
`to::skt` specialization for `SslConnection`; there is on in it's parent class,
but in accordance with C++ rules a compiler does not see it. The solution
is to cast to the base class:

    SslConnection* conn = ...;
    auto& skt = static_cast<Connection*>(conn)->access<to::skt>();

This becomes even more unhandy when an object is stored behind smart pointer.

Let's enumerate key points:

 - accessors multiplication due to `const` and `noexcept` variants

 - not so handy access for private methods (too verbose due to multiple template
params), although "injection" of own accessor-methods seems an advantage

 - too clumpsy syntax to access private proreties in class hierarchy

# Conclusion

The proposed solution is far from perfect. I found the following golden ratio
for my projects on the implementation details access topic:

 - if the property is stable enough or it is the part of class interface,
then public accessor should be written for it. It would be desirable for
read only access, i.e. the accessor should be just a getter. For example,
the `address` property in `actor_base` in
[rotor](https://github.com/basiliscos/cpp-rotor/blob/master/include/rotor/actor_base.h).

 - otherwise, if implementation details might be usable in descendants,
make them `private`

 - provide generic templated accessor (`template<typename T> auto& access()`)
but for properties only; no access to private methods, as I don't see possible
use cases now. This point might be different for different projects.

The described approach is applied in to be released soon
[rotor](https://github.com/basiliscos/cpp-rotor) `v0.09`.
