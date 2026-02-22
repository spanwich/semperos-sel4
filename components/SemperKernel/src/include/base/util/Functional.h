/*
 * Functional.h -- Minimal std::function replacement for -nostdinc++ environments
 *
 * Provides a simple type-erased callable wrapper. Limitations:
 * - Fixed 128-byte inline storage (no heap allocation for small captures)
 * - No copy, move-only
 * - No exceptions
 */

#pragma once

#include <stddef.h>
#include <string.h>

/* Placement new — needed for type-erased storage */
#ifndef _PLACEMENT_NEW_DEFINED
#define _PLACEMENT_NEW_DEFINED
inline void* operator new(size_t, void* p) { return p; }
#endif

namespace std {

/* Forward declaration */
template<typename> class function;

/* Specialization for function types */
template<typename R, typename... Args>
class function<R(Args...)> {
    static const size_t STORAGE_SIZE = 128;

    using invoke_fn = R(*)(void*, Args...);
    using destroy_fn = void(*)(void*);
    using copy_fn = void(*)(void*, const void*);

    alignas(max_align_t) char _storage[STORAGE_SIZE];
    invoke_fn _invoke;
    destroy_fn _destroy;
    copy_fn _copy;

    template<typename F>
    static R invoker(void *storage, Args... args) {
        return (*reinterpret_cast<F*>(storage))(static_cast<Args&&>(args)...);
    }

    template<typename F>
    static void destroyer(void *storage) {
        reinterpret_cast<F*>(storage)->~F();
    }

    template<typename F>
    static void copier(void *dst, const void *src) {
        new(dst) F(*reinterpret_cast<const F*>(src));
    }

public:
    function() : _invoke(nullptr), _destroy(nullptr), _copy(nullptr) {
        memset(_storage, 0, STORAGE_SIZE);
    }

    function(decltype(nullptr)) : function() {}

    template<typename F>
    function(F f) : _invoke(nullptr), _destroy(nullptr), _copy(nullptr) {
        static_assert(sizeof(F) <= STORAGE_SIZE, "Callable too large for inline storage");
        new(_storage) F(static_cast<F&&>(f));
        _invoke = &invoker<F>;
        _destroy = &destroyer<F>;
        _copy = &copier<F>;
    }

    ~function() {
        if (_destroy) _destroy(_storage);
    }

    function(const function &other) : _invoke(other._invoke), _destroy(other._destroy), _copy(other._copy) {
        if (_copy)
            _copy(_storage, other._storage);
        else
            memset(_storage, 0, STORAGE_SIZE);
    }

    function &operator=(const function &other) {
        if (this != &other) {
            if (_destroy) _destroy(_storage);
            _invoke = other._invoke;
            _destroy = other._destroy;
            _copy = other._copy;
            if (_copy)
                _copy(_storage, other._storage);
            else
                memset(_storage, 0, STORAGE_SIZE);
        }
        return *this;
    }

    explicit operator bool() const {
        return _invoke != nullptr;
    }

    R operator()(Args... args) const {
        return _invoke(const_cast<void*>(static_cast<const void*>(_storage)),
                       static_cast<Args&&>(args)...);
    }
};

/* Minimal std::bind / std::placeholders replacement */
namespace placeholders {
    struct _Ph1 {};
    struct _Ph2 {};
    static _Ph1 _1;
    static _Ph2 _2;
}

/* Simple bind for member function with object pointer + 2 placeholders */
template<typename R, typename C, typename A1, typename A2>
struct _BindResult2 {
    using fn_type = R(C::*)(A1, A2);
    fn_type fn;
    C* obj;

    _BindResult2(fn_type f, C* o, placeholders::_Ph1, placeholders::_Ph2)
        : fn(f), obj(o) {}

    R operator()(A1 a1, A2 a2) {
        return (obj->*fn)(a1, a2);
    }
};

template<typename R, typename C, typename A1, typename A2>
_BindResult2<R, C, A1, A2>
bind(R(C::*fn)(A1, A2), C* obj, placeholders::_Ph1 p1, placeholders::_Ph2 p2) {
    return _BindResult2<R, C, A1, A2>(fn, obj, p1, p2);
}

/* std::move / std::forward (minimal) */
template<typename T> struct remove_reference      { typedef T type; };
template<typename T> struct remove_reference<T&>   { typedef T type; };
template<typename T> struct remove_reference<T&&>  { typedef T type; };

template<typename T>
typename remove_reference<T>::type&& move(T&& t) {
    return static_cast<typename remove_reference<T>::type&&>(t);
}

template<typename T>
T&& forward(typename remove_reference<T>::type& t) {
    return static_cast<T&&>(t);
}

} /* namespace std */
