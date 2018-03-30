/*
 * Copyright (c) 2014 Anders Wang Kristensen
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef __UJSON_HPP__
#define __UJSON_HPP__

#include <algorithm>
#include <cstdint>
#include <exception>
#include <iosfwd>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ujson {

#ifdef _LIBCPP_VERSION
#define UJSON_SHORT_STRING_OPTIMIZATION // libc++
#ifdef __LP64__
enum { sso_max_length = 22 }; // 22 bytes on 64 bit
#else
enum { sso_max_length = 10 }; // 10 bytes on 32 bit
#endif
#elif defined _CPPLIB_VER
#define UJSON_SHORT_STRING_OPTIMIZATION // msvc + dinkumware stl
enum { sso_max_length = 15 }; // 15 bytes on both 32/64 bit
#elif defined __GLIBCXX__
#define UJSON_REF_COUNTED_STRING // libstdc++
#else
#error Unrecognized STL library.
#endif

// vs2013 ctp 1 supports noexcept but rest don't
#if defined _MSC_VER && _MSC_FULL_VER != 180021114
#define noexcept
#endif

enum class value_type { null, boolean, number, string, array, object };

class value;
class string_view;

using string = std::string;
using array = std::vector<value>;
using name_value_pair = std::pair<std::string, value>;
using object = std::vector<name_value_pair>;

enum class validate_utf8 { no, yes };

class value final {
public:
    // construct null value
    value() noexcept;

    // construct boolean value
    value(bool b) noexcept;

    // construct double value; throws if not finite
    value(double d);

    // construct 32 bit signed int value
    value(std::int32_t d) noexcept;

    // construct 32 bit unsigned int value
    value(std::uint32_t d) noexcept;

    // construct string value; throws if invalid utf-8
    value(string const &str, validate_utf8 validate = validate_utf8::yes);
    value(string &&str, validate_utf8 validate = validate_utf8::yes);

    // construct string value; if len==0 ptr must be zero terminated
    value(const char *ptr, std::size_t len = 0,
          validate_utf8 validate = validate_utf8::yes);

    // construct array value
    value(array const &a);
    value(array &&a);

    // construct object value
    value(object const &o, validate_utf8 validate = validate_utf8::yes);
    value(object &&o, validate_utf8 validate = validate_utf8::yes);

    value(value const &) noexcept;
    value(value &&) noexcept;
    value(const void *) = delete;

    // construct from array<T> if T is convertible to value
    template <typename T>
    value(const std::vector<T> &a,
          const typename std::enable_if<
              std::is_convertible<T, value>::value>::type *p = nullptr);

    // construct from array<T> if suitable to_json(T) method exists
    template <typename T>
    value(const std::vector<T> &a,
          const typename std::enable_if<std::is_same<
              value, decltype(to_json(std::declval<T>()))>::value>::type *p =
              nullptr);

    // construct from map<string,T> if T is convertible to value
    template <typename T>
    value(const std::map<std::string, T> &o,
          const typename std::enable_if<
              std::is_convertible<T, value>::value>::type *p = nullptr);

    // construct from map<string,T> if suitable to_json(T) method exists
    template <typename T>
    value(const std::map<std::string, T> &o,
          const typename std::enable_if<std::is_same<
              value, decltype(to_json(std::declval<T>()))>::value>::type *p =
              nullptr);

    value &operator=(bool) noexcept;
    value &operator=(double); // throws if not finite
    value &operator=(std::int32_t) noexcept;
    value &operator=(std::uint32_t) noexcept;
    value &operator=(string const &); // throws if invalid utf-8
    value &operator=(string &&);      // throws if invalid utf-8
    value &operator=(const char *);   // throws if invalid utf-8
    value &operator=(array const &);
    value &operator=(array &&);
    value &operator=(object const &);
    value &operator=(object &&);
    value &operator=(value const &) noexcept;
    value &operator=(value &&) noexcept;
    value &operator=(const void *) = delete;

    ~value();

    bool is_null() const noexcept;
    bool is_boolean() const noexcept;
    bool is_number() const noexcept;
    bool is_string() const noexcept;
    bool is_array() const noexcept;
    bool is_object() const noexcept;

    value_type type() const noexcept;

    void swap(value &other) noexcept;

private:

    friend bool operator==(const value &lhs, const value &rhs);
    friend bool operator!=(const value &lhs, const value &rhs);

    // all casts throw if value has wrong type

    // contained bool
    friend bool bool_cast(value const &v);

    // contained bool (moved from value will be null)
    friend bool bool_cast(value &&v);

    // contained double
    friend double double_cast(value const &v);

    // contained double (moved from value will be null)
    friend double double_cast(value &&v);

    // contained double cast to int32
    // throws integer_overflow if number is out of range
    friend std::int32_t int32_cast(value const &v);

    // contained double cast to int32 (moved from value will be null)
    // throws integer_overflow if number is out of range
    friend std::int32_t int32_cast(value &&v);

    // contained double cast to uint32
    // throws integer_overflow if number is out of range
    friend std::uint32_t uint32_cast(value const &v);

    // contained double cast to uint32 (moved from value will be null)
    // throws integer_overflow if number is out of range
    friend std::uint32_t uint32_cast(value &&v);

    // const reference to contained string
    friend string_view string_cast(value const &v);

    // contained string or copy if shared (moved from value will be null)
    friend string string_cast(value &&v);

    // const reference to contained array
    friend array const &array_cast(value const &v);

    // contained array or copy if shared (moved from value will be null)
    friend array array_cast(value &&v);

    // const reference to contained object
    friend object const &object_cast(value const &v);

    // contained object or copy if shared (moved from value will be null)
    friend object object_cast(value &&v);

    struct impl_t {
        virtual ~impl_t() = 0;
        virtual value_type type() const noexcept = 0;
        virtual void clone(char *storage) const noexcept = 0;
        virtual bool equals(const impl_t *ptr) const noexcept = 0;
    };

    struct null_impl_t : impl_t {
        null_impl_t() noexcept;
        value_type type() const noexcept override;
        void clone(char *storage) const noexcept override;
        bool equals(const impl_t *ptr) const noexcept override;
    };

    struct boolean_impl_t : impl_t {
        boolean_impl_t(bool b) noexcept;
        value_type type() const noexcept override;
        void clone(char *storage) const noexcept override;
        bool equals(const impl_t *ptr) const noexcept override;
        bool boolean;
    };

    struct number_impl_t : impl_t {
        number_impl_t(double n) noexcept;
        value_type type() const noexcept override;
        void clone(char *storage) const noexcept override;
        bool equals(const impl_t *ptr) const noexcept override;
        double number;
    };

#ifdef UJSON_SHORT_STRING_OPTIMIZATION

    struct short_string_impl_t : impl_t {
        short_string_impl_t(const char *ptr, std::size_t len);
        short_string_impl_t(const string &s);
        value_type type() const noexcept override;
        void clone(char *storage) const noexcept override;
        bool equals(const impl_t *ptr) const noexcept override;
        char buffer[sso_max_length + 1];
        std::uint8_t length;
    };

    struct long_string_impl_t : impl_t {
        long_string_impl_t(string s);
        long_string_impl_t(std::shared_ptr<string> const &p);
        value_type type() const noexcept override;
        void clone(char *storage) const noexcept override;
        bool equals(const impl_t *ptr) const noexcept override;
        std::shared_ptr<string> ptr;
    };

#elif defined UJSON_REF_COUNTED_STRING

    struct string_impl_t : impl_t {
        string_impl_t(string s);
        value_type type() const noexcept override;
        void clone(char *storage) const noexcept override;
        bool equals(const impl_t *ptr) const noexcept override;
        string str;
    };

#endif

    struct array_impl_t : impl_t {
        array_impl_t(array a);
        array_impl_t(const std::shared_ptr<array> &p);
        value_type type() const noexcept override;
        void clone(char *storage) const noexcept override;
        bool equals(const impl_t *ptr) const noexcept override;
        std::shared_ptr<array> ptr;
    };
    struct object_impl_t : impl_t {
        object_impl_t(object o, validate_utf8 validate);
        object_impl_t(const std::shared_ptr<object> &p);
        value_type type() const noexcept override;
        void clone(char *storage) const noexcept override;
        bool equals(const impl_t *ptr) const noexcept override;
        std::shared_ptr<object> ptr;
    };

    // cast m_storage to impl_t*
    const impl_t *impl() const noexcept;

    // destroy object in m_storage
    void destroy() noexcept;

    static bool is_valid_utf8(const char *start, const char *end) noexcept;

public:
#define UJSON_MAX(a, b) (((a) > (b)) ? (a) : (b))

#ifdef UJSON_SHORT_STRING_OPTIMIZATION
    static const std::size_t storage_size = UJSON_MAX(
        sizeof(null_impl_t),
        UJSON_MAX(
            sizeof(boolean_impl_t),
            UJSON_MAX(
                sizeof(number_impl_t),
                UJSON_MAX(
                    sizeof(array_impl_t),
                    UJSON_MAX(sizeof(object_impl_t),
                              UJSON_MAX(sizeof(short_string_impl_t),
                                        sizeof(long_string_impl_t)))))));
#elif defined UJSON_REF_COUNTED_STRING
    static const std::size_t storage_size = UJSON_MAX(
        sizeof(null_impl_t),
        UJSON_MAX(sizeof(boolean_impl_t),
                  UJSON_MAX(sizeof(number_impl_t),
                            UJSON_MAX(sizeof(array_impl_t),
                                      UJSON_MAX(sizeof(object_impl_t),
                                                sizeof(string_impl_t))))));
#endif
#undef UJSON_MAX
    char m_storage[storage_size];
};

void swap(value &lhs, value &rhs) noexcept;

inline bool operator<(name_value_pair const &lhs,
                      name_value_pair const &rhs) {
    return lhs.first < rhs.first;
}

extern const value null;

class string_view {
public:
    string_view(const char *ptr, std::size_t len);

    const char *c_str() const;
    std::size_t length() const;

    operator std::string() const;

    const char *begin() const;
    const char *cbegin() const;
    const char *end() const;
    const char *cend() const;

    friend bool operator==(string_view const &lhs, string_view const &rhs);

private:
    const char *m_ptr;
    std::size_t m_length;
};

enum class character_encoding { ascii, utf8 };

struct to_string_options {
    int indent_amount;           // 0 means no insigficant whitespace
    character_encoding encoding; // ascii with utf-8 escaped \uXXXX or utf-8
};

const to_string_options indented_utf8 = { 4, character_encoding::utf8 };
const to_string_options indented_ascii = { 4, character_encoding::ascii };
const to_string_options compact_utf8 = { 0, character_encoding::utf8 };
const to_string_options compact_ascii = { 0, character_encoding::ascii };

// convert value to string using specified string options
std::string to_string(value const &v,
                      const to_string_options &opts = indented_utf8);
std::ostream &operator<<(std::ostream &stream, value const &v);

// parse buffer into value; if len==0 buffer must be zero terminated
// throws if buffer is not valid JSON
value parse(const char *buffer, std::size_t len = 0);
value parse(const std::string &buffer);

enum class error_code {
    bad_cast,        // value has wrong type for cast
    bad_number,      // number not finite (NaN/inf not supported by JSON)
    bad_string,      // invalid utf-8 string
    invalid_syntax,  // error parsing JSON
    integer_overflow // number is outside valid range for integer cast
};

class exception final : public std::exception {
public:
    exception(error_code code, int line = -1);

    error_code get_error_code() const;

    // line number if thrown during parsing or -1
    int get_line() const;

    const char *what() const noexcept override;

private:
    mutable std::string m_what;
    error_code m_error_code;
    int m_line;
};

// find first value with given name; returns obj.end() if not found
object::const_iterator find(object const &obj, char const *name);
object::iterator find(object &obj, char const *name);

// find first value with given name; throws std::out_of_range if not found
object::const_iterator at(object const &obj, char const *name);
object::iterator at(object &obj, char const *name);

}

// ---------------------------------------------------------------------------
// implementation

#include <cassert>
#include <cmath>
#include <cstring>

namespace ujson {

inline value::value() noexcept { new (m_storage) null_impl_t{}; }

inline value::value(bool b) noexcept { new (m_storage) boolean_impl_t{ b }; }

inline value::value(double d) {
    if (!std::isfinite(d))
        throw exception(error_code::bad_number);
    new (m_storage) number_impl_t{ d };
}

inline value::value(std::int32_t i) noexcept : value(double(i)) {}

inline value::value(std::uint32_t i) noexcept : value(double(i)) {}

inline value::value(string const &s, validate_utf8 validate) {

    if (validate == validate_utf8::yes &&
        !is_valid_utf8(s.data(), s.data() + s.length()))
        throw exception(error_code::bad_string);

#ifdef UJSON_SHORT_STRING_OPTIMIZATION
    auto length = s.length();
    if (length <= sso_max_length)
        new (m_storage) short_string_impl_t{ s.c_str(), length };
    else
        new (m_storage) long_string_impl_t{ s };
#elif defined UJSON_REF_COUNTED_STRING
    new (m_storage) string_impl_t{ std::move(s) };
#endif
}

inline value::value(string &&s, validate_utf8 validate) {

    if (validate == validate_utf8::yes &&
        !is_valid_utf8(s.data(), s.data() + s.length()))
        throw exception(error_code::bad_string);

#ifdef UJSON_SHORT_STRING_OPTIMIZATION
    auto length = s.length();
    if (length <= sso_max_length)
        new (m_storage)short_string_impl_t{ s.c_str(), length };
    else
        new (m_storage)long_string_impl_t{ std::move(s) };
#elif defined UJSON_REF_COUNTED_STRING
    new (m_storage)string_impl_t{ std::move(s) };
#endif
}
inline value::value(const char *ptr, std::size_t length,
                    validate_utf8 validate) {

    if (!length)
        length = std::strlen(ptr);

    if (validate == validate_utf8::yes && !is_valid_utf8(ptr, ptr + length))
        throw exception(error_code::bad_string);

#ifdef UJSON_SHORT_STRING_OPTIMIZATION
    if (length <= sso_max_length)
        new (m_storage) short_string_impl_t{ ptr, length };
    else
        new (m_storage) long_string_impl_t{ std::string(ptr, ptr + length) };
#elif defined UJSON_REF_COUNTED_STRING
    new (m_storage) string_impl_t{ std::string(ptr, ptr + length) };
#endif
}

inline value::value(array const &a) { new (m_storage) array_impl_t{ a }; }

inline value::value(array &&a) { new (m_storage)array_impl_t{ std::move(a) }; }

inline value::value(object const &o, validate_utf8 validate) {
    new (m_storage) object_impl_t{ o, validate };
}

inline value::value(object &&o, validate_utf8 validate) {
    new (m_storage)object_impl_t{ std::move(o), validate };
}

inline value::value(value const &rhs) noexcept {
    rhs.impl()->clone(m_storage);
}

inline value::value(value &&rhs) noexcept {
    std::memcpy(m_storage, rhs.m_storage, storage_size);
    new (rhs.m_storage)null_impl_t{};
}

template <typename T>
inline value::value(const std::vector<T> &a,
                    const typename std::enable_if<
                        std::is_convertible<T, value>::value>::type *p) {
    array tmp(a.begin(), a.end());
    new (m_storage) array_impl_t{ std::move(tmp) };
}

template <typename T>
inline value::value(
    const std::vector<T> &a,
    const typename std::enable_if<std::is_same<
        value, decltype(to_json(std::declval<T>()))>::value>::type *
        p) {
    array tmp;
    tmp.reserve(a.size());
    for (auto const &v : a)
        tmp.push_back(to_json(v));
    new (m_storage) array_impl_t{ std::move(tmp) };
}

template <typename T>
inline value::value(const std::map<std::string, T> &o,
                    const typename std::enable_if<
                        std::is_convertible<T, value>::value>::type *p) {
    object tmp(o.begin(), o.end());
    new (m_storage) object_impl_t{ std::move(tmp), validate_utf8::yes };
}

template <typename T>
inline value::value(
    const std::map<std::string, T> &o,
    const typename std::enable_if<std::is_same<
        value, decltype(to_json(std::declval<T>()))>::value>::type *
        p) {
    object tmp;
    for (auto it = o.begin(); it != o.end(); ++it)
        tmp.push_back({ it->first, to_json(it->second) });
    new (m_storage) object_impl_t{ std::move(tmp), validate_utf8::yes };
}

inline value &value::operator=(bool b) noexcept {
    destroy();
    new (m_storage) boolean_impl_t{ b };
    return *this;
}

inline value &value::operator=(double d) {
    if (!std::isfinite(d))
        throw exception(error_code::bad_number);
    destroy();
    new (m_storage) number_impl_t{ d };
    return *this;
}

inline value &value::operator=(std::int32_t i) noexcept {
    *this = double(i);
    return *this;
}

inline value &value::operator=(std::uint32_t i) noexcept {
    *this = double(i);
    return *this;
}

inline value &value::operator=(string const &s) {

    if (!is_valid_utf8(s.data(), s.data() + s.length()))
        throw exception(error_code::bad_string);

#ifdef UJSON_SHORT_STRING_OPTIMIZATION
    auto length = s.length();
    if (length <= sso_max_length) {
        destroy();
        new (m_storage) short_string_impl_t{ s.c_str(), length };
    } else {
        char storage[storage_size];
        new (storage) long_string_impl_t{ s }; // may throw
        destroy();
        std::memcpy(m_storage, storage, storage_size);
    }
#elif defined UJSON_REF_COUNTED_STRING
    destroy();
    new (m_storage) string_impl_t{ s };
#endif

    return *this;
}

inline value &value::operator=(string &&s) {

    if (!is_valid_utf8(s.data(), s.data() + s.length()))
        throw exception(error_code::bad_string);

#ifdef UJSON_SHORT_STRING_OPTIMIZATION
    auto length = s.length();
    if (length <= sso_max_length) {
        destroy();
        new (m_storage)short_string_impl_t{ s.c_str(), length };
    }
    else {
        char storage[storage_size];
        new (storage)long_string_impl_t{ std::move(s) }; // may throw
        destroy();
        std::memcpy(m_storage, storage, storage_size);
    }
#elif defined UJSON_REF_COUNTED_STRING
    destroy();
    new (m_storage)string_impl_t{ std::move(s) };
#endif

    return *this;
}

inline value &value::operator=(const char *p) {
    *this = std::string(p);
    return *this;
}

inline value &value::operator=(array const &a) {
    char storage[storage_size];
    new (storage) array_impl_t{ a }; // may throw
    destroy();
    std::memcpy(m_storage, storage, storage_size);
    return *this;
}

inline value &value::operator=(array &&a) {
    char storage[storage_size];
    new (storage)array_impl_t{ std::move(a) }; // may throw
    destroy();
    std::memcpy(m_storage, storage, storage_size);
    return *this;
}

inline value &value::operator=(object const &o) {
    char storage[storage_size];
    new (storage) object_impl_t{ o, validate_utf8::yes }; // may throw
    destroy();
    std::memcpy(m_storage, storage, storage_size);
    return *this;
}

inline value &value::operator=(object &&o) {
    char storage[storage_size];
    new (storage)object_impl_t{ std::move(o), validate_utf8::yes }; // may throw
    destroy();
    std::memcpy(m_storage, storage, storage_size);
    return *this;
}

inline value &value::operator=(value const &rhs) noexcept{
	if ( this == &rhs )
		return *this;
    destroy();
    rhs.impl()->clone(m_storage);
    return *this;
}

inline value &value::operator=(value &&rhs) noexcept {
	if ( this == &rhs )
		return *this;
    destroy();
    std::memcpy(m_storage, rhs.m_storage, storage_size);
    new (rhs.m_storage)null_impl_t{};
    return *this;
}

inline value::~value() { destroy(); }

inline bool value::is_null() const noexcept {
    return type() == value_type::null;
}

inline bool value::is_boolean() const noexcept {
    return type() == value_type::boolean;
}
inline bool value::is_number() const noexcept {
    return type() == value_type::number;
}
inline bool value::is_string() const noexcept {
    return type() == value_type::string;
}
inline bool value::is_array() const noexcept {
    return type() == value_type::array;
}
inline bool value::is_object() const noexcept {
    return type() == value_type::object;
}

inline value_type value::type() const noexcept { return impl()->type(); }

inline void value::swap(value &other) noexcept {
    char tmp[storage_size];
    std::memcpy(tmp, m_storage, storage_size);
    std::memcpy(m_storage, other.m_storage, storage_size);
    std::memcpy(other.m_storage, tmp, storage_size);
}

inline void value::destroy() noexcept { impl()->~impl_t(); }

inline const value::impl_t *value::impl() const noexcept {
    return reinterpret_cast<const impl_t *>(m_storage);
}

inline void swap(value &lhs, value &rhs) noexcept { lhs.swap(rhs); }

inline bool operator==(const value &lhs, const value &rhs) {

    if (typeid(*lhs.impl()) != typeid(*rhs.impl()))
        return false;

    return lhs.impl()->equals(rhs.impl());
}

inline bool operator!=(const value &lhs, const value &rhs) {
    return !(lhs == rhs);
}

// --------------------------------------------------------------------------

inline string_view::string_view(const char *ptr, std::size_t len)
    : m_ptr(ptr), m_length(len) {}

inline const char *string_view::c_str() const { return m_ptr; }

inline std::size_t string_view::length() const { return m_length; }

inline string_view::operator std::string() const {
    return std::string(m_ptr, m_ptr + m_length);
}

inline const char *string_view::begin() const { return m_ptr; }

inline const char *string_view::cbegin() const { return m_ptr; }

inline const char *string_view::end() const { return m_ptr + m_length; }

inline const char *string_view::cend() const { return m_ptr + m_length; }

inline bool operator==(string_view const &lhs, string_view const &rhs) {
    return lhs.m_ptr == rhs.m_ptr && lhs.m_length == rhs.m_length;
}

// --------------------------------------------------------------------------

inline bool bool_cast(value const &v) {
    auto ptr = dynamic_cast<const value::boolean_impl_t *>(v.impl());
    if (ptr)
        return ptr->boolean;
    throw exception(error_code::bad_cast);
}

inline bool bool_cast(value &&v) {
    auto ptr = dynamic_cast<const value::boolean_impl_t *>(v.impl());
    if (!ptr)
        throw exception(error_code::bad_cast);
    bool tmp = ptr->boolean;
    v = null;
    return tmp;
}

inline double double_cast(value const &v) {
    auto ptr = dynamic_cast<const value::number_impl_t *>(v.impl());
    if (ptr)
        return ptr->number;
    throw exception(error_code::bad_cast);
}

inline double double_cast(value &&v) {
    auto ptr = dynamic_cast<const value::number_impl_t *>(v.impl());
    if (!ptr)
        throw exception(error_code::bad_cast);
    double tmp = ptr->number;
    v = null;
    return tmp;
}

inline std::int32_t int32_cast(value const &v) {
    auto ptr = dynamic_cast<const value::number_impl_t *>(v.impl());
    if (!ptr)
        throw exception(error_code::bad_cast);
    if (ptr->number < std::numeric_limits<std::int32_t>::min() ||
        ptr->number > std::numeric_limits<std::int32_t>::max())
        throw exception(error_code::integer_overflow);
    return std::int32_t(ptr->number);
}

inline std::int32_t int32_cast(value &&v) {
    auto ptr = dynamic_cast<const value::number_impl_t *>(v.impl());
    if (!ptr)
        throw exception(error_code::bad_cast);
    if (ptr->number < std::numeric_limits<std::int32_t>::min() ||
        ptr->number > std::numeric_limits<std::int32_t>::max())
        throw exception(error_code::integer_overflow);
    std::int32_t tmp = std::int32_t(ptr->number);
    v = null;
    return tmp;
}

inline std::uint32_t uint32_cast(value const &v) {
    auto ptr = dynamic_cast<const value::number_impl_t *>(v.impl());
    if (!ptr)
        throw exception(error_code::bad_cast);
    if (ptr->number < std::numeric_limits<std::uint32_t>::min() ||
        ptr->number > std::numeric_limits<std::uint32_t>::max())
        throw exception(error_code::integer_overflow);
    return std::uint32_t(ptr->number);
}

inline std::uint32_t uint32_cast(value &&v) {
    auto ptr = dynamic_cast<const value::number_impl_t *>(v.impl());
    if (!ptr)
        throw exception(error_code::bad_cast);
    if (ptr->number < std::numeric_limits<std::uint32_t>::min() ||
        ptr->number > std::numeric_limits<std::uint32_t>::max())
        throw exception(error_code::integer_overflow);
    std::uint32_t tmp = std::uint32_t(ptr->number);
    v = null;
    return tmp;
}

inline string_view string_cast(value const &v) {
#ifdef UJSON_SHORT_STRING_OPTIMIZATION
    auto short_impl =
        dynamic_cast<const value::short_string_impl_t *>(v.impl());
    if (short_impl)
        return { short_impl->buffer, short_impl->length };
    auto long_impl =
        dynamic_cast<const value::long_string_impl_t *>(v.impl());
    if (long_impl)
        return { long_impl->ptr->c_str(), long_impl->ptr->length() };
#elif defined UJSON_REF_COUNTED_STRING
    auto impl = dynamic_cast<const value::string_impl_t *>(v.impl());
    if (impl)
        return { impl->str.c_str(), impl->str.length() };
#endif
    throw exception(error_code::bad_cast);
}

inline string string_cast(value &&v) {
#ifdef UJSON_SHORT_STRING_OPTIMIZATION
    auto short_impl =
        dynamic_cast<const value::short_string_impl_t *>(v.impl());
    if (short_impl) {
        std::string tmp(short_impl->buffer,
                        short_impl->buffer + short_impl->length);
        v = null;
        return tmp;
    }
    auto long_impl =
        dynamic_cast<const value::long_string_impl_t *>(v.impl());
    if (!long_impl)
        throw exception(error_code::bad_cast);

    if (long_impl->ptr.use_count() == 1) {
        auto tmp = std::move(*long_impl->ptr);
        v = null;
        return tmp;
    } else {
        auto copy = *long_impl->ptr;
        v = null;
        return copy;
    }
#elif defined UJSON_REF_COUNTED_STRING
    auto impl = dynamic_cast<const value::string_impl_t *>(v.impl());
    if (!impl)
        throw exception(error_code::bad_cast);
    auto tmp = std::move(impl->str);
    v = null;
    return tmp;
#endif
}

inline array const &array_cast(value const &v) {
    auto impl = dynamic_cast<const value::array_impl_t *>(v.impl());
    if (impl)
        return *impl->ptr;
    throw exception(error_code::bad_cast);
}

inline array array_cast(value &&v) {
    auto impl = dynamic_cast<const value::array_impl_t *>(v.impl());
    if (!impl)
        throw exception(error_code::bad_cast);
    if (impl->ptr.use_count() == 1) {
        auto tmp = std::move(*impl->ptr);
        v = null;
        return tmp;
    } else {
        auto copy = *impl->ptr;
        v = null;
        return copy;
    }
}

inline object const &object_cast(value const &v) {
    auto impl = dynamic_cast<const value::object_impl_t *>(v.impl());
    if (impl)
        return *impl->ptr;
    throw exception(error_code::bad_cast);
}

inline object object_cast(value &&v) {
    auto impl = dynamic_cast<const value::object_impl_t *>(v.impl());
    if (!impl)
        throw exception(error_code::bad_cast);
    if (impl->ptr.use_count() == 1) {
        auto tmp = std::move(*impl->ptr);
        v = null;
        return tmp;
    } else {
        auto copy = *impl->ptr;
        v = null;
        return copy;
    }
}

inline object::const_iterator find(object const &obj, char const *name) {
    assert(std::is_sorted(obj.begin(), obj.end()));
    object::const_iterator it = std::lower_bound(obj.begin(), obj.end(), name,
                            [](name_value_pair const &lhs, char const *rhs) {
        return lhs.first < rhs;
    });
	if ( it == obj.end() || it->first != name )
		return obj.end();
	return it;
}

inline object::iterator find(object &obj, char const *name) {
    assert(std::is_sorted(obj.begin(), obj.end()));
    object::iterator it = std::lower_bound(obj.begin(), obj.end(), name,
                            [](name_value_pair const &lhs, char const *rhs) {
        return lhs.first < rhs;
    });
	if ( it == obj.end() || it->first != name )
		return obj.end();
	return it;
}

inline object::const_iterator at(object const &obj, char const *name) {
    assert(std::is_sorted(obj.begin(), obj.end()));
    auto it = find(obj, name);
    if (it == obj.end())
        throw std::out_of_range("name not found");
    return it;
}

inline object::iterator at(object &obj, char const *name) {
    assert(std::is_sorted(obj.begin(), obj.end()));
    auto it = find(obj, name);
    if (it == obj.end())
        throw std::out_of_range("name not found");
    return it;
}

// --------------------------------------------------------------------------

inline value::impl_t::~impl_t() {}

// null
inline value::null_impl_t::null_impl_t() noexcept {}

inline value_type value::null_impl_t::type() const noexcept {
    return value_type::null;
}

inline void value::null_impl_t::clone(char *storage) const noexcept {
    new (storage) null_impl_t{};
}

inline bool value::null_impl_t::equals(const impl_t *) const noexcept {
    return true;
}

// boolean

inline value::boolean_impl_t::boolean_impl_t(bool b) noexcept : boolean(b) {}

inline value_type value::boolean_impl_t::type() const noexcept {
    return value_type::boolean;
}

inline void value::boolean_impl_t::clone(char *storage) const noexcept {
    new (storage) boolean_impl_t{ boolean };
}

inline bool value::boolean_impl_t::equals(const impl_t *base) const noexcept {
    const boolean_impl_t *ptr = static_cast<const boolean_impl_t *>(base);
    return ptr->boolean == boolean;
}

// number

inline value::number_impl_t::number_impl_t(double n) noexcept : number(n) {}

inline value_type value::number_impl_t::type() const noexcept {
    return value_type::number;
}

inline void value::number_impl_t::clone(char *storage) const noexcept {
    new (storage) number_impl_t{ number };
}

inline bool value::number_impl_t::equals(const impl_t *base) const noexcept {
    const number_impl_t *ptr = static_cast<const number_impl_t *>(base);
    return ptr->number == number;
}

#ifdef UJSON_SHORT_STRING_OPTIMIZATION

// short string

inline value::short_string_impl_t::short_string_impl_t(const char *ptr,
                                                       std::size_t len) {
    assert(len <= sso_max_length);
    std::memcpy(buffer, ptr, len + 1);
    length = static_cast<std::uint8_t>(len);
}

inline value::short_string_impl_t::short_string_impl_t(const string &s)
    : short_string_impl_t(s.c_str(), s.length()) {}

inline value_type value::short_string_impl_t::type() const noexcept {
    return value_type::string;
}

inline void value::short_string_impl_t::clone(char *storage) const noexcept {
    new (storage) short_string_impl_t{ buffer, length };
}

inline bool value::short_string_impl_t::equals(const impl_t *base) const
    noexcept {
    auto ptr = static_cast<const short_string_impl_t *>(base);
    if (length != ptr->length)
        return false;
    return std::memcmp(ptr->buffer, buffer, length) == 0;
}

// long string

inline value::long_string_impl_t::long_string_impl_t(string s) {
    ptr = std::make_shared<string>(std::move(s));
}

inline value::long_string_impl_t::long_string_impl_t(
    std::shared_ptr<string> const &p)
    : ptr(std::move(p)) {}

inline value_type value::long_string_impl_t::type() const noexcept {
    return value_type::string;
}

inline void value::long_string_impl_t::clone(char *storage) const noexcept {
    new (storage) long_string_impl_t{ ptr };
}

inline bool value::long_string_impl_t::equals(const impl_t *base) const
    noexcept {
    auto derived = static_cast<const long_string_impl_t *>(base);
    return *derived->ptr == *ptr;
}

#elif defined UJSON_REF_COUNTED_STRING

inline value::string_impl_t::string_impl_t(string s) : str(std::move(s)) {}

inline value_type value::string_impl_t::type() const noexcept {
    return value_type::string;
}

inline void value::string_impl_t::clone(char *storage) const noexcept {
    new (storage) string_impl_t{ str };
}

inline bool value::string_impl_t::equals(const impl_t *base) const noexcept {
    auto derived = static_cast<const string_impl_t *>(base);
    return derived->str == str;
}

#endif

// array

inline value::array_impl_t::array_impl_t(array a) {
    ptr = std::make_shared<array>(std::move(a));
}

inline value::array_impl_t::array_impl_t(const std::shared_ptr<array> &p)
    : ptr(p) {}

inline value_type value::array_impl_t::type() const noexcept {
    return value_type::array;
}

inline void value::array_impl_t::clone(char *storage) const noexcept {
    new (storage) array_impl_t{ ptr };
}

inline bool value::array_impl_t::equals(const impl_t *base) const noexcept {
    const array_impl_t *derived = static_cast<const array_impl_t *>(base);
    return *derived->ptr == *ptr;
}

// object

inline value::object_impl_t::object_impl_t(object o, validate_utf8 validate) {
    if (validate == validate_utf8::yes) {
        for (auto const &p : o) {
            if (!value::is_valid_utf8(p.first.c_str(),
                p.first.c_str() + p.first.length()))
                throw exception(error_code::bad_string);
        }
    }
    
    std::stable_sort(o.begin(), o.end());
    ptr = std::make_shared<object>(std::move(o));
}

inline value::object_impl_t::object_impl_t(const std::shared_ptr<object> &p)
    : ptr(p) {}

inline value_type value::object_impl_t::type() const noexcept {
    return value_type::object;
}

inline void value::object_impl_t::clone(char *storage) const noexcept {
    new (storage) object_impl_t{ ptr };
}

inline bool value::object_impl_t::equals(const impl_t *base) const noexcept {
    const object_impl_t *derived = static_cast<const object_impl_t *>(base);
    return *derived->ptr == *ptr;
}

// @FDVALVE>> Added accessors to fetch a value, without using exceptions

inline int get_int32( const object &obj, const char *name, int default_value = 0 )
{
	object::const_iterator it = find( obj, name );
	if ( it == obj.end() )
		return default_value;
	return int32_cast( it->second );
}

inline double get_double( const object &obj, const char *name, double default_value = 0.0 )
{
	object::const_iterator it = find( obj, name );
	if ( it == obj.end() )
		return default_value;
	return ujson::double_cast( it->second );
}

inline bool get_bool( const object &obj, const char *name )
{
	object::const_iterator it = find( obj, name );
	if ( it == obj.end() )
		return false;
	if ( it->second.is_number() )
		return ujson::int32_cast( it->second ) != 0;
	return ujson::bool_cast( it->second );
}

inline const char *get_string( const object &obj, const char *name, const char *default_value = nullptr )
{
	object::const_iterator it = find( obj, name );
	if ( it == obj.end() )
		return default_value;
	return ujson::string_cast( it->second ).c_str();
}

inline const object *get_object( const object &obj, const char *name )
{
	object::const_iterator it = find( obj, name );
	if ( it == obj.end() )
		return nullptr;
	return &ujson::object_cast( it->second );
}

inline const array *get_array( const object &obj, const char *name )
{
	object::const_iterator it = find( obj, name );
	if ( it == obj.end() )
		return nullptr;
	return &ujson::array_cast( it->second );
}

// << @FDVALVE

}

#ifdef noexcept
#undef noexcept
#endif

#endif //__UJSON_HPP__
