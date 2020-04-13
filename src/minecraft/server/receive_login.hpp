#pragma once
#include "minecraft/client/encryption_response.hpp"
#include "minecraft/client/handshake.hpp"
#include "minecraft/client/login_start.hpp"
#include "minecraft/net.hpp"
#include "gateway/hexdump.hpp"
#include "minecraft/read_frame.hpp"
#include "minecraft/security/private_key.hpp"
#include "minecraft/server/encryption_request.hpp"
#include "minecraft/server/login_success.hpp"

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>

namespace minecraft::server
{
    struct receive_login_params
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
        client::handshake           client_handshake_frame;
        client::login_start         client_login_start;
        server::encryption_request  server_encryption_request;
        client::encryption_response client_encryption_response;
        server::login_success       server_login_success;
        std::vector< std::uint8_t > shared_secret;

        friend auto operator<<(std::ostream &os, receive_login_params const &arg) -> std::ostream &
        {
            os << "receive login params :\n";
            os << " security token        : " << gateway::hexstring(arg.security_token) << std::endl;
            os << " server key            :\n" << arg.server_key.public_pem() << std::endl;
            os << " use security          : " << arg.use_security() << std::endl;
            os << "client handshake frame :\n" << arg.client_handshake_frame << std::endl;
            os << "client login start     :\n" << arg.client_login_start << std::endl;
            os << "server encryption request :\n" << arg.server_encryption_request << std::endl;
            os << "client encryption response :\n" << arg.client_encryption_response << std::endl;
            os << "shared secret : " << gateway::hexstring(arg.shared_secret) << std::endl;
            os << arg.server_login_success;
            return os;
        }
    };

    struct login_op_base
    {
        static boost::uuids::uuid generate_uuid();
    };

    template < class Stream, class DynamicBuffer >
    struct receive_login_op
    : net::coroutine
    , login_op_base
    {
        receive_login_op(Stream &stream, DynamicBuffer buffer, receive_login_params &params)
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
                encode(params_.server_encryption_request, std::back_inserter(tx_buffer_));
                yield async_write(stream_, net::buffer(tx_buffer_), std::move(self));
                if (ec.failed())
                    return self.complete(ec);

                yield async_expect_frame(stream_, buffer_, params_.client_encryption_response, std::move(self));
                if (ec.failed())
                    return self.complete(ec);

                params_.shared_secret =
                    params_.client_encryption_response.decrypt_secret(params_.server_key, params_.security_token, ec);

                return self.complete(error::not_implemented);

            send_success:
                tx_buffer_.clear();
                encode(params_.server_login_success, tx_buffer_);
                yield async_write(stream_, net::buffer(tx_buffer_), std::move(self));
                return self.complete(ec);
            }
#include <boost/asio/unyield.hpp>

        }   // namespace minecraft::server

        Stream &              stream_;
        DynamicBuffer         buffer_;
        receive_login_params &params_;
        std::vector< char >   tx_buffer_;
    };   // namespace minecraft::server

    template < class Stream, class DynamicBuffer, class CompletionHandler >
    auto
    async_receive_login(Stream &stream, DynamicBuffer buffer, receive_login_params &params, CompletionHandler &&handler)
    {
        using op_type = receive_login_op< Stream, DynamicBuffer >;
        return net::async_compose< CompletionHandler, void(error_code) >(
            op_type(stream, buffer, params), handler, stream);
    }
}   // namespace minecraft::server