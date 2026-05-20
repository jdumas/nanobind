#include <nanobind/nanobind.h>
#include <nanobind/nb_named_tuple.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

namespace nb = nanobind;

struct Point { int x; float y; };
struct Color { int r; int g; };

// Config exercises per-field defaults.
struct Config { std::string name; int width; int height; };

// OptItem exercises optional<T> fields.
struct OptItem { int id; std::optional<std::string> label; };

// Outer holds a nested NamedTuple (Point) as a field.
struct Outer { Point origin; int weight; };

// Tree is a self-referential NamedTuple: a node value plus a list of children
// of the same type. ``std::vector<Tree>`` is allowed at struct-definition
// time in C++17 because ``std::vector`` only requires its element type to be
// complete at instantiation of certain member templates, not at declaration.
struct Tree { int value; std::vector<Tree> children; };

// DocPoint exercises the helper-API docstring overload (class doc +
// per-field docs through ``.doc("...")``).
struct DocPoint { int x; int y; };

namespace geom {
struct QualPoint { int x; int y; };
} // namespace geom

// Templated NamedTuple regression case: the macros cannot accept ``Foo<int,
// float>`` directly because the comma terminates the variadic preprocessor
// argument list. The supported workarounds are (a) a typedef + ``NB_NAMED_TUPLE``
// and (b) the helper API which does not go through the preprocessor.
template <typename A, typename B> struct Pair { A first; B second; };
using PairIF = Pair<int, float>;
using PairFI = Pair<float, int>;

// Validation regression helpers: non-trailing defaults (#2) and a throwing
// default thunk (#7). Both error paths now surface at the register_named_tuple
// call site rather than at a separate finalize().
struct BadDefaults { int a; int b; };

// Field type whose ``from_cpp`` always raises -- used to exercise the
// exception path when constructing a default value.
struct ThrowOnFromCpp { int x; };

NB_NAMED_TUPLE_CASTER(Point)
NB_NAMED_TUPLE_CASTER(Color)
NB_NAMED_TUPLE_CASTER(Config)
NB_NAMED_TUPLE_CASTER(OptItem)
NB_NAMED_TUPLE_CASTER(Outer)
NB_NAMED_TUPLE_CASTER(Tree)
NB_NAMED_TUPLE_CASTER(DocPoint)
NB_NAMED_TUPLE_CASTER(geom::QualPoint, "QualPoint")
NB_NAMED_TUPLE_CASTER(PairIF)
NB_NAMED_TUPLE_CASTER(PairFI)

NAMESPACE_BEGIN(NB_NAMESPACE)
NAMESPACE_BEGIN(detail)
template <> struct type_caster<ThrowOnFromCpp> {
    NB_TYPE_CASTER(ThrowOnFromCpp, const_name("ThrowOnFromCpp"))
    bool from_python(handle, uint8_t, cleanup_list *) noexcept { return false; }
    static handle from_cpp(const ThrowOnFromCpp &, rv_policy,
                           cleanup_list *) noexcept {
        PyErr_SetString(PyExc_ValueError,
                        "test_named_tuple: from_cpp deliberately failed");
        return handle();
    }
};
NAMESPACE_END(detail)
NAMESPACE_END(NB_NAMESPACE)

struct ThrowField { ThrowOnFromCpp a; int b; };

NB_MODULE(test_named_tuple_ext, m) {
    // Helper API: explicit field declarations.
    nb::register_named_tuple<Point>(m, "Point",
        nb::field<&Point::x>("x"),
        nb::field<&Point::y>("y"));

    // Macro API: same effect with the field list given once.
    NB_NAMED_TUPLE(m, Color, r, g);

    // Per-field defaults via the helper API. ``collections.namedtuple``
    // only supports trailing defaults: ``name`` is required, ``width``
    // defaults to 80, ``height`` defaults to 24.
    nb::register_named_tuple<Config>(m, "Config",
        nb::field<&Config::name>("name"),
        nb::field<&Config::width>("width").default_(80),
        nb::field<&Config::height>("height").default_(24));

    // Optional fields: ``label`` may be ``None``.
    nb::register_named_tuple<OptItem>(m, "OptItem",
        nb::field<&OptItem::id>("id"),
        nb::field<&OptItem::label>("label"));

    // Nested NamedTuple: an ``Outer`` has a ``Point`` field.
    nb::register_named_tuple<Outer>(m, "Outer",
        nb::field<&Outer::origin>("origin"),
        nb::field<&Outer::weight>("weight"));

    // Self-referential NamedTuple: a ``Tree`` has a list of ``Tree``s.
    nb::register_named_tuple<Tree>(m, "Tree",
        nb::field<&Tree::value>("value"),
        nb::field<&Tree::children>("children"));

    // Class + per-field docstrings via the helper API.
    nb::register_named_tuple<DocPoint>(m, "DocPoint",
        "A 2D point with documented fields.",
        nb::field<&DocPoint::x>("x").doc("horizontal coordinate"),
        nb::field<&DocPoint::y>("y").default_(0).doc("vertical coordinate (default 0)"));

    // Qualified C++ type bound via NB_NAMED_TUPLE_NAMED. The stringified
    // C++ name ("geom::QualPoint") is not a valid Python identifier, so the
    // macro accepts an explicit Python name as the third argument.
    NB_NAMED_TUPLE_NAMED(m, geom::QualPoint, "QualPoint", x, y);

    // Templated type bound two ways:
    //  - Via typedef + the macro (PairIF -> Pair<int,float>)
    //  - Via the helper API with an explicit Python name (PairFI ->
    //    Pair<float,int>); this path is preferred when the C++ type alias
    //    name and the desired Python identifier need to differ.
    NB_NAMED_TUPLE(m, PairIF, first, second);
    nb::register_named_tuple<PairFI>(m, "PairFI",
        nb::field<&PairFI::first>("first"),
        nb::field<&PairFI::second>("second"));

    m.def("make_point", [](int x, float y) { return Point{x, y}; });
    m.def("point_x", [](Point p) { return p.x; });
    m.def("point_y", [](Point p) { return p.y; });
    m.def("roundtrip_point", [](Point p) { return p; });

    m.def("make_color", [](int r, int g) { return Color{r, g}; });
    m.def("color_sum", [](Color c) { return c.r + c.g; });
    m.def("roundtrip_color", [](Color c) { return c; });

    m.def("default_config", []() { return Config{"untitled", 80, 24}; });
    m.def("roundtrip_config", [](Config c) { return c; });

    m.def("make_optitem",
          [](int id, std::optional<std::string> label) {
              return OptItem{id, label};
          });
    m.def("roundtrip_optitem", [](OptItem o) { return o; });

    m.def("make_outer", [](int x, float y, int w) {
        return Outer{Point{x, y}, w};
    });
    m.def("roundtrip_outer", [](Outer o) { return o; });

    m.def("tree_leaf", [](int v) { return Tree{v, {}}; });
    m.def("tree_branch", [](int v, std::vector<Tree> children) {
        return Tree{v, std::move(children)};
    });
    m.def("tree_sum", [](Tree t) {
        int total = 0;
        std::vector<Tree> stack;
        stack.push_back(std::move(t));
        while (!stack.empty()) {
            Tree cur = std::move(stack.back());
            stack.pop_back();
            total += cur.value;
            for (auto &child : cur.children)
                stack.push_back(std::move(child));
        }
        return total;
    });

    m.def("roundtrip_docpoint", [](DocPoint p) { return p; });
    m.def("roundtrip_qualpoint", [](geom::QualPoint p) { return p; });
    m.def("roundtrip_pair_if", [](PairIF p) { return p; });
    m.def("roundtrip_pair_fi", [](PairFI p) { return p; });

    // Regression test for fix #2: a non-trailing default must be rejected
    // (surfaced as RuntimeError on the Python side) at the call site --
    // not as a confusing None-placeholder failure from collections.namedtuple.
    m.def("trigger_non_trailing_defaults", [](nb::handle scope) {
        nb::register_named_tuple<BadDefaults>(scope, "BadDefaults_NoTrailing",
            nb::field<&BadDefaults::a>("a").default_(1),
            nb::field<&BadDefaults::b>("b"));
    });

    // Regression test for fix #7: a default-value conversion that raises
    // must surface as a Python exception at the call site (now during
    // construction of the field_t default, eagerly inside register_named_tuple).
    m.def("trigger_throwing_default", [](nb::handle scope) {
        nb::register_named_tuple<ThrowField>(scope, "ThrowField",
            nb::field<&ThrowField::a>("a").default_(ThrowOnFromCpp{}),
            nb::field<&ThrowField::b>("b").default_(0));
    });
}
