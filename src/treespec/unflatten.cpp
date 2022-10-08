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

template <typename Span>
py::object PyTreeSpec::UnflattenImpl(Span leaves) const {
    absl::InlinedVector<py::object, 4> agenda;
    auto it = leaves.begin();
    ssize_t leaf_count = 0;
    for (const Node& node : traversal) {
        if ((ssize_t)agenda.size() < node.arity) {
            throw std::logic_error("Too few elements for PyTreeSpec node.");
        }
        switch (node.kind) {
            case PyTreeKind::Leaf:
            case PyTreeKind::None:
                if (it == leaves.end()) {
                    throw std::invalid_argument(
                        absl::StrFormat("Too few leaves for PyTreeSpec; expected %ld, got %ld.",
                                        num_leaves(),
                                        leaf_count));
                }
                agenda.push_back(py::reinterpret_borrow<py::object>(*it));
                ++it;
                ++leaf_count;
                break;

            case PyTreeKind::Tuple:
            case PyTreeKind::NamedTuple:
            case PyTreeKind::List:
            case PyTreeKind::Dict:
            case PyTreeKind::Custom: {
                const ssize_t size = agenda.size();
                absl::Span<py::object> span;
                if (node.arity > 0) {
                    span = absl::Span<py::object>(&agenda[size - node.arity], node.arity);
                }
                py::object out = MakeNode(node, span);
                agenda.resize(size - node.arity);
                agenda.push_back(out);
                break;
            }

            default:
                throw std::logic_error("Unreachable code.");
        }
    }
    if (it != leaves.end()) {
        throw std::invalid_argument(
            absl::StrFormat("Too many leaves for PyTreeSpec; expected %ld.", num_leaves()));
    }
    if (agenda.size() != 1) {
        throw std::logic_error("PyTreeSpec traversal did not yield a singleton.");
    }
    return std::move(agenda.back());
}

py::object PyTreeSpec::Unflatten(py::iterable leaves) const { return UnflattenImpl(leaves); }

py::object PyTreeSpec::Unflatten(absl::Span<const py::object> leaves) const {
    return UnflattenImpl(leaves);
}

}  // namespace optree