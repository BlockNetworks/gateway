#pragma once

#include <string>
#include <cstdint>
#include <wise_enum.h>
#include "minecraft/incomplete.hpp"
#include "minecraft/parse_error.hpp"

namespace minecraft {


    template<class Iter>
    auto
    parse(Iter first, Iter last, std::uint16_t &target) -> Iter
    {
        assert(std::distance(first, last) >= 2);

        auto convert = [](char c) {
            return static_cast<std::uint16_t>(std::uint16_t(static_cast<std::uint8_t>(c)));
        };
        auto h = convert(*first++) << 8;
        auto l = convert(*first++);
        target = h | l;
        return first;
    }

    template<class I1, class I2>
    auto
    parse(I1 first, I2 last, std::int32_t &result) -> I1
    {
        std::uint32_t accumulator = 0;
        auto success = [&] {
            result = accumulator;
            return first;
        };

        int shift = 0;
        while (first != last)
        {
            auto x = std::uint32_t(*first++);
            accumulator |= (x & 0x7fu) << shift;
            shift += 7;
            if (not(x & 0x80u))
                return success();
            if (shift > 28)
                throw make_error_code(error::invalid_varint);
        }
        throw incomplete();
    }

    template<class Iter, class Enum>
    static auto
    parse(Iter first, Iter last, Enum &target) ->
    std::enable_if_t<std::is_enum_v<Enum>, Iter>
    {
        using utype = std::underlying_type_t<Enum>;
        auto accum = utype();
        first = parse(first, last, accum);
        auto check = [&accum] {
            for (auto e : wise_enum::range<Enum>)
                if (static_cast<utype>(e.value) == accum)
                    return true;
            return false;
        };
        if (!check())
            throw make_error_code(error::invalid_enum);
        target = static_cast<Enum>(accum);
        return first;
    }

    template<class Iter>
    auto
    parse(Iter first, Iter last, std::string &target, std::size_t char_limit = 32767) -> Iter
    {
        auto size = std::int32_t();
        first = parse(first, last, size);

        const auto byte_limit = (char_limit * 4) + 3;
        if (size > byte_limit)
            throw make_error_code(error::invalid_string);

        target.reserve(size);
        while (first != last && size)
        {
            --size;
            target.push_back(*first++);
        }

        if (size)
            throw incomplete();

        return first;
    }

    template<class Iter>
    auto parse(Iter first, Iter last, std::vector<std::uint8_t>& target, std::int32_t byte_limit = 65536)
    {
        auto size = std::int32_t();
        first = parse(first, last, size);

        if (size > byte_limit)
            throw make_error_code(error::invalid_array);

        target.reserve(size);
        target.clear();
        while (first != last && size)
        {
            --size;
            target.push_back(*first++);
        }

        if (size)
            throw incomplete();

        return first;
    }


}