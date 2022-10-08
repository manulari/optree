/*
Copyright 2022 MetaOPT Team. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
================================================================================
*/

// Caution: this code uses exceptions. The exception use is local to the
// binding code and the idiomatic way to emit Python exceptions.

#include "include/treespec.h"

namespace optree {

ssize_t PyTreeSpec::num_leaves() const {
    if (traversal.empty()) {
        return 0;
    }
    return traversal.back().num_leaves;
}

ssize_t PyTreeSpec::num_nodes() const { return traversal.size(); }

bool PyTreeSpec::operator==(const PyTreeSpec& other) const {
    if (traversal.size() != other.traversal.size()) {
        return false;
    }
    auto b = other.traversal.begin();
    for (auto a = traversal.begin(); a != traversal.end(); ++a, ++b) {
        if (a->kind != b->kind || a->arity != b->arity ||
            (a->node_data.ptr() == nullptr) != (b->node_data.ptr() == nullptr) ||
            a->custom != b->custom) {
            return false;
        }
        if (a->node_data && a->node_data.not_equal(b->node_data)) {
            return false;
        }
        // We don't need to test equality of num_leaves and num_nodes since they
        // are derivable from the other node data.
    }
    return true;
}

bool PyTreeSpec::operator!=(const PyTreeSpec& other) const { return !(*this == other); }

/*static*/ py::object PyTreeSpec::MakeNode(const PyTreeSpec::Node& node,
                                           absl::Span<py::object> children) {
    if ((ssize_t)children.size() != node.arity) {
        throw std::logic_error("Node arity did not match.");
    }
    switch (node.kind) {
        case PyTreeKind::Leaf:
            throw std::logic_error("MakeNode not implemented for leaves.");

        case PyTreeKind::None:
            return py::none();

        case PyTreeKind::Tuple:
        case PyTreeKind::NamedTuple: {
            py::tuple tuple{node.arity};
            for (ssize_t i = 0; i < node.arity; ++i) {
                tuple[i] = std::move(children[i]);
            }
            if (node.kind == PyTreeKind::NamedTuple) {
                return node.node_data(*tuple);
            } else {
                return std::move(tuple);
            }
        }

        case PyTreeKind::List: {
            py::list list{node.arity};
            for (ssize_t i = 0; i < node.arity; ++i) {
                list[i] = std::move(children[i]);
            }
            return std::move(list);
        }

        case PyTreeKind::Dict: {
            py::dict dict;
            py::list keys = py::reinterpret_borrow<py::list>(node.node_data);
            for (ssize_t i = 0; i < node.arity; ++i) {
                dict[keys[i]] = std::move(children[i]);
            }
            return std::move(dict);
        }

        case PyTreeKind::Custom: {
            py::tuple tuple(node.arity);
            for (ssize_t i = 0; i < node.arity; ++i) {
                tuple[i] = std::move(children[i]);
            }
            return node.custom->from_iterable(node.node_data, tuple);
        }

        default:
            throw std::logic_error("Unreachable code.");
    }
}

/*static*/ PyTreeKind PyTreeSpec::GetKind(const py::handle& handle,
                                          PyTreeTypeRegistry::Registration const** custom) {
    const PyTreeTypeRegistry::Registration* registration =
        PyTreeTypeRegistry::Lookup(handle.get_type());
    if (registration) {
        if (registration->kind == PyTreeKind::Custom) {
            *custom = registration;
        } else {
            *custom = nullptr;
        }
        return registration->kind;
    } else if (py::isinstance<py::tuple>(handle) && py::hasattr(handle, "_fields")) {
        // We can only identify namedtuples heuristically, here by the presence of
        // a _fields attribute.
        return PyTreeKind::NamedTuple;
    } else {
        return PyTreeKind::Leaf;
    }
}

std::string PyTreeSpec::ToString() const {
    std::vector<std::string> agenda;
    for (const Node& node : traversal) {
        if ((ssize_t)agenda.size() < node.arity) {
            throw std::logic_error("Too few elements for container.");
        }

        std::string children = absl::StrJoin(agenda.end() - node.arity, agenda.end(), ", ");
        std::string representation;
        switch (node.kind) {
            case PyTreeKind::Leaf:
                agenda.emplace_back("*");
                continue;

            case PyTreeKind::None:
                representation = "None";
                break;

            case PyTreeKind::Tuple:
                // Tuples with only one element must have a trailing comma.
                if (node.arity == 1) children += ",";
                representation = absl::StrCat("(", children, ")");
                break;

            case PyTreeKind::List:
                representation = absl::StrCat("[", children, "]");
                break;

            case PyTreeKind::Dict: {
                if ((ssize_t)py::len(node.node_data) != node.arity) {
                    throw std::logic_error("Number of keys and entries does not match.");
                }
                representation = "{";
                std::string separator;
                auto child_iter = agenda.end() - node.arity;
                for (const py::handle& key : node.node_data) {
                    absl::StrAppendFormat(
                        &representation, "%s%s: %s", separator, py::repr(key), *child_iter);
                    ++child_iter;
                    separator = ", ";
                }
                representation += "}";
                break;
            }

            case PyTreeKind::NamedTuple: {
                py::object type = node.node_data;
                py::tuple fields = py::reinterpret_borrow<py::tuple>(py::getattr(type, "_fields"));
                if ((ssize_t)py::len(fields) != node.arity) {
                    throw std::logic_error("Number of fields and entries does not match.");
                }
                std::string kind = static_cast<std::string>(py::str(py::getattr(type, "__name__")));
                representation = absl::StrFormat("%s(", kind);
                std::string separator;
                auto child_iter = agenda.end() - node.arity;
                for (const py::handle& field : fields) {
                    absl::StrAppendFormat(
                        &representation,
                        "%s%s=%s",
                        separator,
                        static_cast<std::string>(py::reinterpret_borrow<py::str>(field)),
                        *child_iter);
                    ++child_iter;
                    separator = ", ";
                }
                representation += ")";
                break;
            }

            case PyTreeKind::Custom: {
                py::object type = node.custom->type;
                std::string kind = static_cast<std::string>(py::str(py::getattr(type, "__name__")));
                if (type.is(py::OrderedDict)) {
                    representation = absl::StrFormat("%s([", kind);
                    std::string separator;
                    auto child_iter = agenda.end() - node.arity;
                    for (const py::handle& key : node.node_data) {
                        absl::StrAppendFormat(
                            &representation, "%s(%s, %s)", separator, py::repr(key), *child_iter);
                        ++child_iter;
                        separator = ", ";
                    }
                    representation += "])";
                } else if (type.is(py::DefaultDict)) {
                    if ((ssize_t)py::len(node.node_data) != 2) {
                        throw std::logic_error("Number of auxiliary data mismatch.");
                    }
                    py::tuple aux_data = py::reinterpret_borrow<py::tuple>(node.node_data);
                    py::object factory = aux_data[0];
                    py::tuple keys = py::reinterpret_borrow<py::tuple>(aux_data[1]);
                    if ((ssize_t)py::len(keys) != node.arity) {
                        throw std::logic_error("Number of keys and entries does not match.");
                    }
                    representation = absl::StrFormat("%s(%s, {", kind, py::repr(factory));
                    std::string separator;
                    auto child_iter = agenda.end() - node.arity;
                    for (const py::handle& key : keys) {
                        absl::StrAppendFormat(
                            &representation, "%s%s: %s", separator, py::repr(key), *child_iter);
                        ++child_iter;
                        separator = ", ";
                    }
                    representation += "})";
                } else if (type.is(py::Deque)) {
                    representation = absl::StrFormat("%s([%s])", kind, children);
                } else {
                    std::string data;
                    if (node.node_data) {
                        data = absl::StrFormat("[%s]", py::str(node.node_data));
                    }
                    representation =
                        absl::StrFormat("CustomTreeNode(%s%s, [%s])", kind, data, children);
                }
                break;
            }

            default:
                throw std::logic_error("Unreachable code.");
        }

        agenda.erase(agenda.end() - node.arity, agenda.end());
        agenda.push_back(std::move(representation));
    }
    if (agenda.size() != 1) {
        throw std::logic_error("PyTreeSpec traversal did not yield a singleton.");
    }
    return absl::StrCat("PyTreeSpec(", agenda.back(), ")");
}

py::object PyTreeSpec::ToPicklable() const {
    py::list result;
    for (const auto& node : traversal) {
        result.append(py::make_tuple(static_cast<ssize_t>(node.kind),
                                     node.arity,
                                     node.node_data ? node.node_data : py::none(),
                                     node.custom != nullptr ? node.custom->type : py::none(),
                                     node.num_leaves,
                                     node.num_nodes));
    }
    return std::move(result);
}

/*static*/ PyTreeSpec PyTreeSpec::FromPicklableImpl(py::object picklable) {
    PyTreeSpec tree;
    for (const auto& item : picklable.cast<py::list>()) {
        auto t = item.cast<py::tuple>();
        if (t.size() != 6) {
            throw std::runtime_error("Malformed pickled PyTreeSpec.");
        }
        Node& node = tree.traversal.emplace_back();
        node.kind = static_cast<PyTreeKind>(t[0].cast<ssize_t>());
        node.arity = t[1].cast<ssize_t>();
        switch (node.kind) {
            case PyTreeKind::NamedTuple:
                node.node_data = t[2].cast<py::type>();
                break;
            case PyTreeKind::Dict:
                node.node_data = t[2].cast<py::list>();
                break;
            case PyTreeKind::Custom:
                node.node_data = t[2];
                break;
            default:
                if (!t[2].is_none()) {
                    throw std::runtime_error("Malformed pickled PyTreeSpec.");
                }
                break;
        }
        if (node.kind == PyTreeKind::Custom) {
            node.custom = t[3].is_none() ? nullptr : PyTreeTypeRegistry::Lookup(t[3]);
            if (node.custom == nullptr) {
                throw std::runtime_error(absl::StrCat("Unknown custom type in pickled PyTreeSpec: ",
                                                      static_cast<std::string>(py::repr(t[3])),
                                                      "."));
            }
        } else {
            if (!t[3].is_none()) {
                throw std::runtime_error("Malformed pickled PyTreeSpec.");
            }
        }
        node.num_leaves = t[4].cast<ssize_t>();
        node.num_nodes = t[5].cast<ssize_t>();
    }
    return tree;
}

/*static*/ PyTreeSpec PyTreeSpec::FromPicklable(py::object picklable) {
    return FromPicklableImpl(picklable);
}

}  // namespace optree