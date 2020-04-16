#pragma once
#include "minecraft/client/encryption_response.hpp"
#include "minecraft/client/handshake.hpp"
#include "minecraft/client/login_start.hpp"
#include "minecraft/hexdump.hpp"
#include "minecraft/net.hpp"
#include "minecraft/security/private_key.hpp"
#include "minecraft/server/encryption_request.hpp"
#include "minecraft/server/login_success.hpp"
#include "read_frame.hpp"
#include "stream.hpp"

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>

namespace minecraft::protocol
{
    struct server_accept_login_params
    {
        void set_server_key(minecraft::security::private_key k) { server_key = std::move(k); }

        void set_server_id(std::string id) { server_encryption_request.server_id = std::move(id); }

        void use_security(bool tf) { use_security_ = tf; }
        bool use_security() const { return use_security_; }

        // inputs
        std::vector< std::uint8_t >      security_token;
        minecraft::security::private_key server_key;
        bool                             use_security_ = true;

        // state
        client::handshake                          client_handshake_frame;
        client::login_start                        client_login_start;
        server::encryption_request                 server_encryption_request;
        client::encryption_response                client_encryption_response;
        server::login_success                      server_login_success;
        std::vector< std::uint8_t >                shared_secret;
        std::function< void(net::mutable_buffer) > on_rx;
        std::function< void(net::const_buffer) >   on_tx;
        std::function< void(error_code) >          on_fail;

        error_code const &log_fail(error_code const &ec)
        {
            if (on_fail)
                on_fail(ec);
            return ec;
        }

        net::mutable_buffer log_rx(net::mutable_buffer buf)
        {
            if (on_rx)
                on_rx(buf);
            return buf;
        }

        net::const_buffer log_tx(net::const_buffer buf)
        {
            if (on_tx)
                on_tx(buf);
            return buf;
        }

        friend auto operator<<(std::ostream &os, server_accept_login_params const &arg) -> std::ostream &
        {
            os << "receive login params :\n";
            os << " security token        : " << hexstring(arg.security_token) << std::endl;
            os << " server key            :\n" << arg.server_key.public_pem() << std::endl;
            os << " use security          : " << arg.use_security() << std::endl;
            os << "client handshake frame :\n" << arg.client_handshake_frame << std::endl;
            os << "client login start     :\n" << arg.client_login_start << std::endl;
            os << "server encryption request :\n" << arg.server_encryption_request << std::endl;
            os << "client encryption response :\n" << arg.client_encryption_response << std::endl;
            os << "shared secret : " << hexstring(arg.shared_secret) << std::endl;
            os << arg.server_login_success;
            return os;
        }
    };

    struct server_accept_op_base
    {
        static boost::uuids::uuid generate_uuid();
    };

    template < class Stream, class DynamicBuffer >
    struct server_accept_op
    : net::coroutine
    , server_accept_op_base
    {
        server_accept_op(Stream &stream, DynamicBuffer buffer, server_accept_login_params &params)
        : stream_(stream)
        , buffer_(buffer)
        , params_(params)
        , tx_buffer_()
        {
            tx_buffer_.reserve(0x10000);
        }

        template < class FrameType >
        static void expect_frame(FrameType &target, net::const_buffer source, error_code &ec)
        {
            auto first = reinterpret_cast< std::uint8_t const * >(source.data());
            auto last  = first + source.size();
            auto which = target.id();
            auto n     = parse2(first, last, which, ec);
            if (!ec.failed())
                if (which != target.id())
                    ec = error::unexpected_packet;
            if (!ec.failed())
            {
                first += n;
                parse(first, last, target, ec);
            }
        }

        template < class Self >
        void operator()(Self &self, error_code ec = {}, std::size_t bytes_transferred = 0)
        {
#include <boost/asio/yield.hpp>
            reenter(this) for (;;)
            {
                yield async_expect_frame(stream_, buffer_, params_.client_handshake_frame, std::move(self));
                if (ec.failed())
                    return self.complete(ec);
                if (params_.client_handshake_frame.validate(ec).failed())
                    return self.complete(ec);

                yield async_expect_frame(stream_, buffer_, params_.client_login_start, std::move(self));
                if (ec.failed())
                    return self.complete(ec);

                if (not params_.use_security())
                {
                    params_.server_login_success.username = params_.client_login_start.name;
                    params_.server_login_success.uuid     = to_string(generate_uuid());
                    goto send_success;
                }

                prepare(params_.server_encryption_request, params_.server_key);
                tx_buffer_.clear();
                compose(params_.server_encryption_request, tx_buffer_);
                yield async_write(stream_, net::buffer(tx_buffer_), std::move(self));
                if (ec.failed())
                    return self.complete(ec);

                yield async_expect_frame(stream_, buffer_, params_.client_encryption_response, std::move(self));
                if (ec.failed())
                    return self.complete(ec);

                params_.shared_secret = params_.client_encryption_response.decrypt_secret(
                    params_.server_key, params_.server_encryption_request.verify_token, ec);

                return self.complete(error::not_implemented);

            send_success:
                tx_buffer_.clear();
                compose(params_.server_login_success, tx_buffer_);
                yield async_write(stream_, net::buffer(tx_buffer_), std::move(self));
                return self.complete(ec);
            }
#include <boost/asio/unyield.hpp>

        }

        Stream &              stream_;
        DynamicBuffer         buffer_;
        server_accept_login_params &params_;
        std::vector< char >   tx_buffer_;
    };

    template < class Stream, class DynamicBuffer, class CompletionHandler >
    auto async_server_accept(Stream &stream, DynamicBuffer buffer,
                             server_accept_login_params &params, CompletionHandler &&handler)
    {
        using op_type = server_accept_op< Stream, DynamicBuffer >;
        return net::async_compose< CompletionHandler, void(error_code) >(
            op_type(stream, buffer, params), handler, stream);
    }

    template < class NextLayer, class CompletionHandler >
    auto
    async_server_accept(stream< NextLayer > &stream, server_accept_login_params &params, CompletionHandler &&handler)
    {
        auto op = [&stream, &params, coro = net::coroutine()](
                      auto &self, error_code ec = {}, std::size_t bytes_transferred = 0) mutable {
#include <boost/asio/yield.hpp>
            reenter(coro) for (;;)
            {
                yield async_expect_frame(stream, params.client_handshake_frame, std::move(self));

                if (not ec.failed())
                    verify(params.client_handshake_frame, ec);

                if (ec.failed())
                    return self.complete(params.log_fail(ec));

                stream.protocol_version(params.client_handshake_frame.protocol_version);

                yield async_expect_frame(stream, params.client_login_start, std::move(self));

                if (not ec.failed())
                    verify(params.client_login_start, ec);

                if (ec.failed())
                    return self.complete(params.log_fail(ec));

                if (params.use_security())
                {
                    //
                    // send encryption request
                    //

                    prepare(params.server_encryption_request, params.server_key);
                    yield stream.async_write_packet(params.server_encryption_request, std::move(self));
                    if (ec.failed())
                        return self.complete(params.log_fail(ec));

                    //
                    // receive encryption response
                    //

                    yield async_expect_frame(stream, params.client_encryption_response, std::move(self));
                    if (ec)
                        return self.complete(params.log_fail(ec));

                    //
                    // decode shared secret
                    //

                    {
                        auto secret = params.client_encryption_response.decrypt_secret(
                            params.server_key, params.server_encryption_request.verify_token, ec);
                        if (ec.failed())
                            return self.complete(params.log_fail(ec));

                        stream.set_encryption(secret);
                    }

                    //
                    // todo : contact minecraft server here for uuid
                    //

                    params.server_login_success.username = params.client_login_start.name;
                    params.server_login_success.uuid     = to_string(server_accept_op_base::generate_uuid());
                }
                else
                {
                    params.server_login_success.username = params.client_login_start.name;
                    params.server_login_success.uuid     = to_string(server_accept_op_base::generate_uuid());
                }

                //
                // send login success
                //

                yield stream.async_write_packet(params.server_login_success, std::move(self));
                return self.complete(ec);
            }
#include <boost/asio/unyield.hpp>
        };

        return net::async_compose< CompletionHandler, void(error_code) >(std::move(op), handler, stream);
    }
}   // namespace minecraft::server