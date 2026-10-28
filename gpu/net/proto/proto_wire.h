// SPDX-License-Identifier: GPL-2.0-or-later
// Protocol Buffers wire format for the shadNet messages (gpu/net/proto/gen_proto.py writes the
// classes on top of it): the encoding is
// https://protobuf.dev/programming-guides/encoding/, written here from that page so that the
// online module needs neither libprotobuf nor abseil (Apache-2.0, kb #850). Every length and count
// read from the network is checked against what is left of the buffer before anything is
// allocated.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ProtoWire {

enum class WireType : std::uint8_t {
    Varint = 0,
    Fixed64 = 1,
    Len = 2,
    StartGroup = 3,
    EndGroup = 4,
    Fixed32 = 5,
};

// Field numbers are 29 bits (encoding guide, "Message Structure").
inline constexpr std::uint32_t MAX_FIELD_NUMBER = (1u << 29) - 1;
// Elements of one repeated field in one message: far above what a shadNet reply carries (rooms,
// members, scores), so that a crafted reply of tiny elements cannot make the decoder build
// millions of objects out of an 8 MiB packet.
inline constexpr std::size_t MAX_REPEATED_ELEMENTS = 1u << 16;
// protobuf's default recursion limit (io::CodedInputStream), for nested unknown groups.
inline constexpr std::size_t MAX_GROUP_DEPTH = 100;

class Reader {
public:
    explicit Reader(std::string_view data)
        : m_p_cursor(reinterpret_cast<const std::uint8_t*>(data.data())),
          m_p_end(m_p_cursor + data.size()) {}

    bool is_at_end() const {
        return m_p_cursor == m_p_end;
    }

    // At most 10 bytes; bits past the 64th are dropped, as protobuf's own parsers do.
    bool read_varint(std::uint64_t& out_value) {
        std::uint64_t value = 0;
        for (int i_byte = 0; i_byte < 10; ++i_byte) {
            if (m_p_cursor == m_p_end) {
                return false;
            }
            const std::uint8_t byte = *m_p_cursor++;
            value |= static_cast<std::uint64_t>(byte & 0x7f) << (7 * i_byte);
            if ((byte & 0x80) == 0) {
                out_value = value;
                return true;
            }
        }
        return false;
    }

    bool read_tag(std::uint32_t& out_field, WireType& out_type) {
        std::uint64_t tag = 0;
        if (!read_varint(tag) || tag > 0xffffffffu) {
            return false;
        }
        const auto field = static_cast<std::uint32_t>(tag >> 3);
        const auto type = static_cast<std::uint8_t>(tag & 7);
        if (field == 0 || field > MAX_FIELD_NUMBER || type > 5) {
            return false;
        }
        out_field = field;
        out_type = static_cast<WireType>(type);
        return true;
    }

    // The bytes stay in the caller's buffer: the length is checked against what remains first.
    bool read_len(std::string_view& out_bytes) {
        std::uint64_t cb_value = 0;
        if (!read_varint(cb_value) || cb_value > static_cast<std::uint64_t>(m_p_end - m_p_cursor)) {
            return false;
        }
        out_bytes = std::string_view(reinterpret_cast<const char*>(m_p_cursor),
                                     static_cast<std::size_t>(cb_value));
        m_p_cursor += cb_value;
        return true;
    }

    // Skips the field whose tag was just read. A group (wire types 3 and 4, proto2's, never sent
    // for this proto3 schema but skipped by protobuf's parsers) is walked to its matching end with
    // a stack instead of recursion, at most as deep as protobuf's default recursion limit.
    bool skip(std::uint32_t field, WireType type) {
        if (type == WireType::EndGroup) {
            return false;
        }
        if (type != WireType::StartGroup) {
            return skip_value(type);
        }
        std::uint32_t open_groups[MAX_GROUP_DEPTH];
        std::size_t c_open = 0;
        open_groups[c_open++] = field;
        while (c_open > 0) {
            std::uint32_t inner_field = 0;
            WireType inner_type{};
            if (!read_tag(inner_field, inner_type)) {
                return false;
            }
            if (inner_type == WireType::StartGroup) {
                if (c_open == MAX_GROUP_DEPTH) {
                    return false;
                }
                open_groups[c_open++] = inner_field;
            } else if (inner_type == WireType::EndGroup) {
                if (open_groups[--c_open] != inner_field) {
                    return false;
                }
            } else if (!skip_value(inner_type)) {
                return false;
            }
        }
        return true;
    }

private:
    bool skip_value(WireType type) {
        std::uint64_t ignored = 0;
        std::string_view ignored_bytes;
        switch (type) {
        case WireType::Varint:
            return read_varint(ignored);
        case WireType::Fixed64:
            return skip_bytes(8);
        case WireType::Len:
            return read_len(ignored_bytes);
        case WireType::Fixed32:
            return skip_bytes(4);
        case WireType::StartGroup:
        case WireType::EndGroup:
            return false;
        }
        return false;
    }

    bool skip_bytes(std::size_t cb_skip) {
        if (cb_skip > static_cast<std::size_t>(m_p_end - m_p_cursor)) {
            return false;
        }
        m_p_cursor += cb_skip;
        return true;
    }

    const std::uint8_t* m_p_cursor;
    const std::uint8_t* m_p_end;
};

inline void append_varint(std::string& out, std::uint64_t value) {
    while (value >= 0x80) {
        out.push_back(static_cast<char>((value & 0x7f) | 0x80));
        value >>= 7;
    }
    out.push_back(static_cast<char>(value));
}

inline void append_tag(std::string& out, std::uint32_t field, WireType type) {
    append_varint(out, (static_cast<std::uint64_t>(field) << 3) | static_cast<std::uint8_t>(type));
}

inline void append_len(std::string& out, std::uint32_t field, std::string_view bytes) {
    append_tag(out, field, WireType::Len);
    append_varint(out, bytes.size());
    out.append(bytes);
}

// int32 and enum values are sign-extended to 64 bits, so a negative one takes ten bytes
// (encoding guide, "Signed Integers").
inline std::uint64_t widen_int32(std::int32_t value) {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(value));
}

// proto3 `string` fields must hold UTF-8 (language guide, "Scalar Value Types"); protobuf's own
// parser refuses a message whose string is not, and so does this one. Overlong forms, surrogates
// and code points above U+10FFFF are invalid.
inline bool is_valid_utf8(std::string_view text) {
    const auto* p_byte = reinterpret_cast<const std::uint8_t*>(text.data());
    const auto* p_end = p_byte + text.size();
    while (p_byte < p_end) {
        const std::uint8_t lead = *p_byte;
        if (lead < 0x80) {
            ++p_byte;
            continue;
        }
        std::size_t c_continuation = 0;
        std::uint32_t code_point = 0;
        std::uint32_t min_code_point = 0;
        if ((lead & 0xe0) == 0xc0) {
            c_continuation = 1;
            code_point = lead & 0x1f;
            min_code_point = 0x80;
        } else if ((lead & 0xf0) == 0xe0) {
            c_continuation = 2;
            code_point = lead & 0x0f;
            min_code_point = 0x800;
        } else if ((lead & 0xf8) == 0xf0) {
            c_continuation = 3;
            code_point = lead & 0x07;
            min_code_point = 0x10000;
        } else {
            return false;
        }
        if (c_continuation >= static_cast<std::size_t>(p_end - p_byte)) {
            return false;
        }
        for (std::size_t i = 1; i <= c_continuation; ++i) {
            if ((p_byte[i] & 0xc0) != 0x80) {
                return false;
            }
            code_point = (code_point << 6) | (p_byte[i] & 0x3f);
        }
        if (code_point < min_code_point || code_point > 0x10ffff ||
            (code_point >= 0xd800 && code_point <= 0xdfff)) {
            return false;
        }
        p_byte += c_continuation + 1;
    }
    return true;
}

} // namespace ProtoWire
