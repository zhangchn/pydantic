#pragma once

// PrebuiltValidator - reuse the validator a model/dataclass class already owns
// instead of building a second copy of the same tree for the schema that names
// the class (Rust validators/prebuilt.rs).
#include <memory>
#include <string>
#include <utility>

#include <pybind11/pybind11.h>

#include "../schema_validator.hpp"
#include "../validator.hpp"

namespace pydantic_core {

class PrebuiltValidator : public Validator {
public:
    // `owner` is the SchemaValidator that owns `reused`; holding it is how Rust
    // keeps the reused validator alive (a Py<SchemaValidator> field), and it is
    // the edge the collector sees.  The tree itself is borrowed: an owning C++
    // edge into another validator's tree would be invisible to the collector and
    // would keep whole validator graphs alive past their last Python reference.
    PrebuiltValidator(py::object owner, Validator* reused)
        : owner_(std::move(owner)), reused_(reused) {}

    ValResult<std::shared_ptr<void>> validate(const Input& input, ValidationState& state) override {
        return reused_->validate(input, state);
    }

    ValResult<std::shared_ptr<void>> default_value(ValidationState& state) override {
        return reused_->default_value(state);
    }

    // Everything the parent asks the node it would have built is answered by the
    // reused tree, so a reused validator is invisible except in the repr.
    std::string name() const override { return reused_->name(); }
    std::string display_name() const override { return reused_->display_name(); }
    std::string root_model_inner_name() const override { return reused_->root_model_inner_name(); }
    std::shared_ptr<Validator> inner_validator() const override { return reused_->inner_validator(); }
    const py::object& expected_class() const override { return reused_->expected_class(); }
    bool is_dataclass_validator() const override { return reused_->is_dataclass_validator(); }
    std::string result_dispatch_name() const override { return reused_->result_dispatch_name(); }
    std::string effective_result_name() const override { return reused_->effective_result_name(); }

    std::shared_ptr<Validator> aliased_validator() const override {
        return std::shared_ptr<Validator>(std::shared_ptr<Validator>{}, reused_);  // borrowed
    }

    // Rust's Debug prints the variant, the struct, and the whole SchemaValidator
    // inside it.  The reused tree is left out here: two models that name each
    // other would otherwise print each other forever.
    std::string debug_repr() const override { return "Prebuilt(PrebuiltValidator)"; }

    ValResult<std::shared_ptr<void>> validate_assignment(
        const py::object& obj, const std::string& field_name,
        const py::object& field_value, ValidationState& state) override {
        return reused_->validate_assignment(obj, field_name, field_value, state);
    }

    ValResult<std::shared_ptr<void>> validate_assignment(
        const Input& input, const std::string& field_name, ValidationState& state) override {
        return reused_->validate_assignment(input, field_name, state);
    }

    // Rust impl_py_gc_traverse!(PrebuiltValidator { schema_validator }): report
    // the reused validator itself; the collector walks it into that tree from
    // there, which is what makes two validators referencing each other collectable.
    void visit_refs(RefVisitor visit, void* arg) const override {
        visit_ref(visit, arg, owner_);
    }

private:
    py::object owner_;   // the SchemaValidator that owns reused_; keeps it alive
    Validator* reused_;
};

}  // namespace pydantic_core
