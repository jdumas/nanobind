/*
    nanobind/nb_named_tuple.h: bind C++ structs as Python NamedTuple classes

    Copyright (c) 2026 Wenzel Jakob

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE file.

    This header is opt-in: it is NOT pulled in by <nanobind/nanobind.h>. Users
    must include it explicitly when they want to expose C++ structs as Python
    NamedTuples.

    Typing information (field types, defaults, Optional, nesting) is recorded
    by stubgen via the sentinel attribute __nb_named_tuple__ set on each
    registered class -- it does not live in runtime __annotations__.
*/

#pragma once

#include <nanobind/nanobind.h>

NAMESPACE_BEGIN(NB_NAMESPACE)
NAMESPACE_BEGIN(detail)

/// Sentinel attribute set on every Python class registered via
/// register_named_tuple. Stubgen looks for this name to identify
/// NamedTuple-bound types reliably.
inline constexpr const char *named_tuple_sentinel_attr = "__nb_named_tuple__";

/// Attribute on every NamedTuple-bound class storing per-field type strings
/// and (optional) per-field docstrings for stubgen consumption. Format:
/// [(field_name, type_str, doc_or_None), ...] in _fields order.
inline constexpr const char *named_tuple_fields_attr = "__nb_named_tuple_fields__";

/// Per-T storage for the registered Python class plus its reader/writer
/// trampoline arrays. C++17 inline variable templates give single-definition
/// semantics across translation units, so the binding TU's write is visible
/// to caster TUs without any runtime lookup table.
template <typename T> struct named_tuple_state {
    using reader_fn = handle (*)(const T &, rv_policy, cleanup_list *);
    using writer_fn = bool (*)(T &, handle, uint8_t, cleanup_list *);
    handle cls;
    size_t n_fields = 0;
    const reader_fn *readers = nullptr;
    const writer_fn *writers = nullptr;
};

template <typename T> inline named_tuple_state<T> named_tuple_data{};

/// Process-wide Python dict mapping mangled typeid name -> NamedTuple class.
/// Used by the descr-to-string walker to resolve nested NamedTuple field
/// types. Lazily initialized; the dict leaks at process exit, matching
/// nanobind's overall stance that extension modules set
/// Py_MOD_MULTIPLE_INTERPRETERS_NOT_SUPPORTED.
inline PyObject *named_tuple_registry() {
    static PyObject *value = nullptr;
    if (!value)
        value = PyDict_New();
    return value;
}

/// Append a type_info's qualified Python name to parts. Resolution order:
/// (1) NamedTuple registry (typeid mangled name keyed) for nested NamedTuples,
/// (2) nanobind's regular type registry for class_<>-bound types,
/// (3) the mangled C++ name as a deterministic fallback.
inline void nt_append_pyname(list &parts, const std::type_info *t) {
    PyObject *reg = named_tuple_registry();
    if (reg) {
        // PyDict_GetItemString returns a borrowed reference; null on miss.
        PyObject *found = PyDict_GetItemString(reg, t->name());
        if (found) {
            handle h(found);
            parts.append(h.attr("__module__"));
            parts.append(str("."));
            parts.append(h.attr("__qualname__"));
            return;
        }
    }
    PyObject *py = nb_type_lookup(t);
    if (py) {
        handle h(py);
        parts.append(h.attr("__module__"));
        parts.append(str("."));
        parts.append(h.attr("__qualname__"));
        return;
    }
    parts.append(str(t->name()));
}

/// Walk a descr<N, Ts...> and build the Python type string used for
/// NamedTuple field annotations. '%' markers substitute the qualified Python
/// class name of the corresponding type; '@' io-name blocks emit only the
/// output variant (the segment after the middle '@').
template <size_t N, typename... Ts>
inline str descr_to_field_type_string(const descr<N, Ts...> &d) {
    const std::type_info *types[sizeof...(Ts) + 1] = { nullptr };
    if constexpr (sizeof...(Ts) > 0)
        d.put_types(types);

    list parts;
    size_t ti = 0;
    size_t lit_start = 0;
    auto flush_lit = [&](size_t end) {
        if (end > lit_start)
            parts.append(str(&d.text[lit_start], end - lit_start));
    };
    for (size_t i = 0; i < N; ++i) {
        char c = d.text[i];
        if (c == '%') {
            flush_lit(i);
            nt_append_pyname(parts, types[ti++]);
            lit_start = i + 1;
        } else if (c == '@') {
            flush_lit(i);
            // io_name block: skip input variant (advancing ti through any
            // embedded '%') then emit only the output variant.
            ++i;
            while (i < N && d.text[i] != '@') {
                if (d.text[i] == '%') ++ti;
                ++i;
            }
            if (i < N) ++i; // step past middle '@'
            size_t out_start = i;
            while (i < N && d.text[i] != '@') {
                if (d.text[i] == '%') {
                    if (i > out_start)
                        parts.append(str(&d.text[out_start], i - out_start));
                    nt_append_pyname(parts, types[ti++]);
                    out_start = i + 1;
                }
                ++i;
            }
            if (i > out_start)
                parts.append(str(&d.text[out_start], i - out_start));
            // outer ++i moves past the trailing '@'
            lit_start = i + 1;
        }
    }
    flush_lit(N);
    return borrow<str>(str("").attr("join")(parts));
}

/// Member-pointer trait used by field_t to recover the class type C and
/// field type F from a non-type template parameter of type F C::*.
template <typename> struct nt_member_traits;
template <typename C, typename F> struct nt_member_traits<F C::*> {
    using class_type = C;
    using field_type = F;
};

/// Non-templated base holding the runtime metadata of a field. Allows
/// register_named_tuple to collect heterogeneous field_t<Member>... into a
/// single-type array of base pointers for iteration.
struct field_base {
    const char *name_;
    PyObject *default_value_ = nullptr;
    const char *doc_ = nullptr;

    explicit constexpr field_base(const char *name) : name_(name) { }

    field_base(const field_base &) = delete;
    field_base &operator=(const field_base &) = delete;
    field_base(field_base &&o) noexcept
        : name_(o.name_), default_value_(o.default_value_), doc_(o.doc_) {
        o.default_value_ = nullptr;
    }
    field_base &operator=(field_base &&) = delete;
    ~field_base() { Py_XDECREF(default_value_); }
};

/// Per-field declaration value built by nanobind::field<Member>(name). The
/// member pointer is a non-type template parameter so the reader/writer
/// trampolines are template-instantiated per field rather than holding
/// std::function captures or void* capture buffers.
///
/// Move-only: default_value_ is an owning PyObject* set via the .default_()
/// chained setter and released on destruction (unless transferred into the
/// registration call).
template <auto Member>
struct field_t : field_base {
    using class_type = typename nt_member_traits<decltype(Member)>::class_type;
    using field_type = typename nt_member_traits<decltype(Member)>::field_type;

    explicit constexpr field_t(const char *name) : field_base(name) { }

    /// Attach a default value supplied as a Python object (handle or object).
    field_t &&default_(handle v) && {
        Py_XDECREF(default_value_);
        Py_XINCREF(v.ptr());
        default_value_ = v.ptr();
        return static_cast<field_t &&>(*this);
    }

    /// Attach a default value supplied as a C++ value of the field type.
    /// Conversion happens eagerly so that any error surfaces at the call
    /// site rather than at a later stage.
    template <typename V,
              std::enable_if_t<!std::is_base_of_v<handle, std::decay_t<V>>, int> = 0>
    field_t &&default_(V &&v) && {
        handle h = make_caster<field_type>::from_cpp(
            (field_type) std::forward<V>(v), rv_policy::copy, nullptr);
        if (!h.is_valid())
            raise_python_error();
        Py_XDECREF(default_value_);
        default_value_ = h.ptr();
        return static_cast<field_t &&>(*this);
    }

    /// Attach a per-field docstring (lifetime managed by caller, typically a
    /// string literal).
    field_t &&doc(const char *d) && {
        doc_ = d;
        return static_cast<field_t &&>(*this);
    }
};

/// Reader trampoline: convert c.*Member to a Python object via make_caster.
template <auto Member, typename T>
static handle nt_read(const T &c, rv_policy policy, cleanup_list *cl) noexcept {
    using F = typename nt_member_traits<decltype(Member)>::field_type;
    return make_caster<F>::from_cpp(c.*Member, policy, cl);
}

/// Writer trampoline: convert a Python object into c.*Member.
template <auto Member, typename T>
static bool nt_write(T &c, handle src, uint8_t flags, cleanup_list *cl) noexcept {
    using F = typename nt_member_traits<decltype(Member)>::field_type;
    make_caster<F> caster;
    if (!caster.from_python(src, flags_for_local_caster<F>(flags), cl))
        return false;
    c.*Member = caster.operator cast_t<F>();
    return true;
}

/// Append (name, type_str, doc_or_None) to fields_meta for the field
/// identified by Member. Instantiated per <Member> so the field type F is
/// available via make_caster<F>::Name.
template <auto Member>
inline void nt_append_field_meta(list &fields_meta, const field_base &f) {
    using F = typename nt_member_traits<decltype(Member)>::field_type;
    str ty = descr_to_field_type_string(make_caster<F>::Name);
    object doc_obj = f.doc_ ? object(str(f.doc_)) : object(none());
    ::nanobind::tuple entry = make_tuple(str(f.name_), ty, doc_obj);
    if (PyList_Append(fields_meta.ptr(), entry.ptr()))
        raise_python_error();
}

/// Generic type caster shared by every C++ struct exposed via
/// register_named_tuple. Users opt a type T into this caster with
/// NB_NAMED_TUPLE_CASTER(T) (or the NB_NAMED_TUPLE convenience macro which
/// expands to it).
template <typename T> struct named_tuple_caster {
    NB_TYPE_CASTER(T, const_name<T>())

    bool from_python(handle src, uint8_t flags, cleanup_list *cleanup) noexcept {
        auto &data = named_tuple_data<T>;
        if (!data.cls.is_valid())
            return false;

        // seq_get_with_size may return a borrowed view (temp == src) or a
        // freshly built tuple that we own; wrap the latter in steal so it is
        // released even if a writer trampoline leaves an unexpected exception.
        PyObject *temp_raw = nullptr;
        PyObject **items = seq_get_with_size(src.ptr(), data.n_fields, &temp_raw);
        object temp = steal(temp_raw);
        if (!items)
            return false;

        for (size_t i = 0; i < data.n_fields; ++i) {
            if (!data.writers[i](value, handle(items[i]), flags, cleanup))
                return false;
        }
        return true;
    }

    template <typename T_>
    static handle from_cpp(T_ &&src, rv_policy policy, cleanup_list *cleanup) noexcept {
        auto &data = named_tuple_data<T>;
        if (!data.cls.is_valid()) {
            PyErr_SetString(PyExc_RuntimeError,
                "nanobind: NamedTuple type was used before its module-level "
                "registration ran. Make sure the binding code is reached "
                "before any C++ -> Python conversion.");
            return handle();
        }

        object args = steal(PyTuple_New((Py_ssize_t) data.n_fields));
        if (!args.is_valid())
            return handle();

        const T &ref = static_cast<const T &>(src);
        for (size_t i = 0; i < data.n_fields; ++i) {
            object item = steal(data.readers[i](ref, policy, cleanup));
            if (!item.is_valid())
                return handle();
            NB_TUPLE_SET_ITEM(args.ptr(), (Py_ssize_t) i, item.release().ptr());
        }
        return PyObject_Call(data.cls.ptr(), args.ptr(), nullptr);
    }
};

NAMESPACE_END(detail)


/// Build a field declaration value: nb::field<&T::x>("x"). The member
/// pointer is a non-type template parameter; the name is a runtime
/// const char* (typically a string literal in .rodata).
template <auto Member>
inline detail::field_t<Member> field(const char *name) {
    return detail::field_t<Member>(name);
}

/// Eagerly register a NamedTuple-bound C++ struct as a Python class. All
/// metadata (class doc, field names, defaults, docs) is known at the call
/// site -- there is no two-phase init. Errors raise via nanobind's standard
/// exception channel.
///
/// Overload taking only the field pack (no class docstring). Forwards to
/// the cls_doc-bearing overload with nullptr.
template <typename T, auto... Members>
inline void register_named_tuple(handle scope, const char *cls_name,
                                 detail::field_t<Members>... fields) {
    register_named_tuple<T>(scope, cls_name, (const char *) nullptr,
                            static_cast<detail::field_t<Members> &&>(fields)...);
}

template <typename T, auto... Members>
inline void register_named_tuple(handle scope, const char *cls_name,
                                 const char *cls_doc,
                                 detail::field_t<Members>... fields) {
    static_assert(sizeof...(Members) > 0,
                  "nanobind::register_named_tuple<T>: at least one field is "
                  "required.");

    detail::field_base *field_ptrs[] = { static_cast<detail::field_base *>(&fields)... };
    constexpr size_t n = sizeof...(Members);

    // Validate per-field default placement before contacting Python.
    // collections.namedtuple only supports trailing defaults; raise a clear
    // error rather than letting Python report it as a synthetic None mismatch.
    const char *first_default = nullptr;
    const char *missing_default_after = nullptr;
    for (size_t i = 0; i < n; ++i) {
        if (field_ptrs[i]->default_value_) {
            if (!first_default)
                first_default = field_ptrs[i]->name_;
        } else if (first_default) {
            missing_default_after = field_ptrs[i]->name_;
            break;
        }
    }
    if (missing_default_after)
        raise("nanobind::register_named_tuple<T>(\"%s\"): field '%s' has no "
              "default but follows field '%s' which does -- "
              "collections.namedtuple only allows trailing defaults.",
              cls_name, missing_default_after, first_default);

    object collections = module_::import_("collections");
    object factory = collections.attr("namedtuple");

    list field_list;
    for (size_t i = 0; i < n; ++i)
        field_list.append(str(field_ptrs[i]->name_));

    object cls;
    if (first_default) {
        list defaults_list;
        for (size_t i = 0; i < n; ++i) {
            if (field_ptrs[i]->default_value_)
                defaults_list.append(handle(field_ptrs[i]->default_value_));
        }
        dict kw;
        kw["defaults"] = tuple(defaults_list);
        cls = factory(str(cls_name), field_list, **kw);
    } else {
        cls = factory(str(cls_name), field_list);
    }
    cls.attr(detail::named_tuple_sentinel_attr) = bool_(true);

    // collections.namedtuple infers __module__ from the calling C frame,
    // which for nanobind bindings is _frozen_importlib. Override it with the
    // scope's real module name so stubgen and type checkers see the
    // canonical path.
    if (PyObject *mod_name = PyObject_GetAttrString(scope.ptr(), "__name__")) {
        cls.attr("__module__") = steal<object>(mod_name);
    } else {
        PyErr_Clear();
    }

    if (cls_doc)
        cls.attr("__doc__") = str(cls_doc);

    // Per-field docstrings: each field is a property on the namedtuple
    // subclass; setting __doc__ on the property is the canonical way.
    for (size_t i = 0; i < n; ++i) {
        if (!field_ptrs[i]->doc_)
            continue;
        object f = cls.attr(field_ptrs[i]->name_);
        f.attr("__doc__") = str(field_ptrs[i]->doc_);
    }

    scope.attr(cls_name) = cls;

    // Make T discoverable in the registry *before* resolving field-type
    // strings, so a NamedTuple that references itself resolves correctly.
    PyObject *reg = detail::named_tuple_registry();
    if (reg)
        PyDict_SetItemString(reg, typeid(T).name(), cls.ptr());

    // Build __nb_named_tuple_fields__ = [(name, type_str, doc), ...] for
    // stubgen to consume. Per-field type strings come from make_caster<F>::Name
    // which is a descr<N, Ts...> that may have a different N per field, so we
    // resolve each one via a fold expression that instantiates the helper per
    // <Member>.
    list fields_meta;
    (detail::nt_append_field_meta<Members>(fields_meta, fields), ...);
    cls.attr(detail::named_tuple_fields_attr) = fields_meta;

    // Publish trampolines and class handle to the per-T inline-variable
    // state. The static arrays live for the process lifetime; C++17 inline
    // variable templates guarantee a single definition across TUs.
    using R = handle (*)(const T &, rv_policy, detail::cleanup_list *);
    using W = bool (*)(T &, handle, uint8_t, detail::cleanup_list *);
    static const R readers[] = { &detail::nt_read<Members, T>... };
    static const W writers[] = { &detail::nt_write<Members, T>... };
    auto &data = detail::named_tuple_data<T>;
    data.cls = handle(cls);
    data.n_fields = n;
    data.readers = readers;
    data.writers = writers;
}

NAMESPACE_END(NB_NAMESPACE)

// ---------------------------------------------------------------------------
// File-scope macro: opt a C++ struct into the named-tuple type caster.
//
// Two forms are supported:
//   NB_NAMED_TUPLE_CASTER(Type)
//       Uses ``#Type`` as the Python class name literal baked into the
//       caster's ``Name`` descr. Works when the C++ identifier matches the
//       Python identifier.
//   NB_NAMED_TUPLE_CASTER(Type, "PyName")
//       Uses the explicit literal. Required when ``Type`` is qualified
//       (``geom::QualPoint``) or otherwise not a valid Python identifier.
//
// The override shadows the ``Name = const_name<T>()`` inherited from
// named_tuple_caster<T>, so nb_func_render_signature copies the literal
// verbatim instead of emitting a '%' placeholder that requires a runtime
// registry lookup.
// ---------------------------------------------------------------------------
#define NB_NAMED_TUPLE_CASTER_DISPATCH_(_1, _2, N, ...) NB_NAMED_TUPLE_CASTER_##N
#define NB_NAMED_TUPLE_CASTER_1(Type) NB_NAMED_TUPLE_CASTER_2(Type, #Type)
#define NB_NAMED_TUPLE_CASTER_2(Type, PyName)                                  \
    namespace nanobind { namespace detail {                                    \
    template <> struct type_caster<Type> : named_tuple_caster<Type> {          \
        static constexpr auto Name = ::nanobind::detail::const_name(PyName);   \
    }; } }
#define NB_NAMED_TUPLE_CASTER(...)                                             \
    NB_NT_EXPAND(NB_NAMED_TUPLE_CASTER_DISPATCH_(__VA_ARGS__, 2, 1)(__VA_ARGS__))

// ---------------------------------------------------------------------------
// In-module macros: declare a NamedTuple binding in one call. Expand to
// register_named_tuple<T>(scope, name, nullptr, nb::field<&T::n>(#n)...).
// Support up to 16 fields; use register_named_tuple directly for more.
// ---------------------------------------------------------------------------

#define NB_NT_EXPAND(x) x
#define NB_NT_CAT_(a, b) a##b
#define NB_NT_CAT(a, b) NB_NT_CAT_(a, b)

#define NB_NT_NARG_(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13,    \
                    _14, _15, _16, N, ...) N
#define NB_NT_NARG(...) NB_NT_EXPAND(NB_NT_NARG_(                              \
    __VA_ARGS__, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1))

#define NB_NT_F1(T, x)        ::nanobind::field<&T::x>(#x)
#define NB_NT_F2(T, x, ...)   ::nanobind::field<&T::x>(#x), NB_NT_EXPAND(NB_NT_F1(T, __VA_ARGS__))
#define NB_NT_F3(T, x, ...)   ::nanobind::field<&T::x>(#x), NB_NT_EXPAND(NB_NT_F2(T, __VA_ARGS__))
#define NB_NT_F4(T, x, ...)   ::nanobind::field<&T::x>(#x), NB_NT_EXPAND(NB_NT_F3(T, __VA_ARGS__))
#define NB_NT_F5(T, x, ...)   ::nanobind::field<&T::x>(#x), NB_NT_EXPAND(NB_NT_F4(T, __VA_ARGS__))
#define NB_NT_F6(T, x, ...)   ::nanobind::field<&T::x>(#x), NB_NT_EXPAND(NB_NT_F5(T, __VA_ARGS__))
#define NB_NT_F7(T, x, ...)   ::nanobind::field<&T::x>(#x), NB_NT_EXPAND(NB_NT_F6(T, __VA_ARGS__))
#define NB_NT_F8(T, x, ...)   ::nanobind::field<&T::x>(#x), NB_NT_EXPAND(NB_NT_F7(T, __VA_ARGS__))
#define NB_NT_F9(T, x, ...)   ::nanobind::field<&T::x>(#x), NB_NT_EXPAND(NB_NT_F8(T, __VA_ARGS__))
#define NB_NT_F10(T, x, ...)  ::nanobind::field<&T::x>(#x), NB_NT_EXPAND(NB_NT_F9(T, __VA_ARGS__))
#define NB_NT_F11(T, x, ...)  ::nanobind::field<&T::x>(#x), NB_NT_EXPAND(NB_NT_F10(T, __VA_ARGS__))
#define NB_NT_F12(T, x, ...)  ::nanobind::field<&T::x>(#x), NB_NT_EXPAND(NB_NT_F11(T, __VA_ARGS__))
#define NB_NT_F13(T, x, ...)  ::nanobind::field<&T::x>(#x), NB_NT_EXPAND(NB_NT_F12(T, __VA_ARGS__))
#define NB_NT_F14(T, x, ...)  ::nanobind::field<&T::x>(#x), NB_NT_EXPAND(NB_NT_F13(T, __VA_ARGS__))
#define NB_NT_F15(T, x, ...)  ::nanobind::field<&T::x>(#x), NB_NT_EXPAND(NB_NT_F14(T, __VA_ARGS__))
#define NB_NT_F16(T, x, ...)  ::nanobind::field<&T::x>(#x), NB_NT_EXPAND(NB_NT_F15(T, __VA_ARGS__))

#define NB_NT_FIELDS(T, ...)                                                   \
    NB_NT_EXPAND(NB_NT_CAT(NB_NT_F, NB_NT_NARG(__VA_ARGS__))(T, __VA_ARGS__))

// NB_NAMED_TUPLE stringifies Type and uses the result as the Python class
// name. This only works when Type is itself a valid Python identifier;
// qualified names (geom::Point) produce strings that collections.namedtuple
// rejects. Use NB_NAMED_TUPLE_NAMED to provide an explicit Python identifier
// in that case (or call register_named_tuple directly).
#define NB_NAMED_TUPLE(scope, Type, ...)                                       \
    ::nanobind::register_named_tuple<Type>((scope), #Type, (const char *) nullptr, \
        NB_NT_FIELDS(Type, __VA_ARGS__))

// Variant of NB_NAMED_TUPLE that accepts an explicit Python identifier for
// the bound class. Use for C++ types whose name is not a valid Python
// identifier (e.g. geom::Point). The macro form does not carry docstrings
// or per-field defaults; call register_named_tuple directly when those are
// needed.
#define NB_NAMED_TUPLE_NAMED(scope, Type, PyName, ...)                         \
    ::nanobind::register_named_tuple<Type>((scope), PyName, (const char *) nullptr, \
        NB_NT_FIELDS(Type, __VA_ARGS__))
