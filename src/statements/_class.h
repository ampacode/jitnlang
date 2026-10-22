#pragma once
// Порт statements/class.py — объявление класса.
//
//   class Person
//       public int age = 0
//       public Person(int a)
//           age = a
//       public int getAge()
//           return age
//
// cpp-путь: обычный C++ class с access-секциями.
// eval-путь: класс = шаблон словаря; Person(...) создаёт dict
// с полями и методами (bound_self = экземпляр).
#include "../node.h"
#include "../nvm.h"
#include "../jsgen.h"
#include "assignment.h"
#include "function.h"
#include "model.h" // cpp_type

struct ClassMember {
    std::string access; // public / private / protected
    NodePtr decl;       // DeclarationStatement | FunctionDeclaration
};

class ClassStatement : public Node {
public:
    std::string name;
    std::vector<ClassMember> members;

    ClassStatement(std::string n, std::vector<ClassMember> m)
        : name(std::move(n)), members(std::move(m)) {}

    Emitted javascript(Nvm& vm) override {
        // порт py jsgen: методы -> js-методы, ctor -> constructor
        std::string out = "class " + name + " {\n";
        for (auto& m : members) {
            if (auto f = std::dynamic_pointer_cast<
                    FunctionDeclaration>(m.decl)) {
                std::string mname =
                    f->name == name ? "constructor" : f->name;
                out += "    " + mname + "(" +
                       js::args(vm, f->args) + ") " +
                       js::block(vm, f->statements) + "\n";
            } else if (auto d = std::dynamic_pointer_cast<
                    DeclarationStatement>(m.decl)) {
                out += "    " + d->name + ";\n";
            }
        }
        return Emitted(out + "}");
    }

    Emitted cpp(Nvm& vm) override {
        vm.class_names.insert(name);
        register_abi_fields(vm);
        std::string code = "class " + name + " {\n";
        std::string cur_access;
        int indent = vm.cpp_indent;
        vm.cpp_indent = 1;
        for (auto& m : members) {
            if (m.access != cur_access) {
                cur_access = m.access;
                code += cur_access + ":\n";
            }
            Emitted r = m.decl->cpp(vm);
            code += cpp_pad(vm) + r.getCpp() + ";\n";
        }
        vm.cpp_indent = indent;
        code += "};";
        Emitted e(code);
        e.is_function_declaration = true; // класс — уровень объявлений
        return e;
    }

    // eval: Person() -> новый dict-экземпляр
    NObject eval(Nvm& vm) override;

    // поля -> vm.models[name] + class_data: класс через ABI
    // (JSON-маршалинг, методы перепривязываются при возврате)
    void register_abi_fields(Nvm& vm) {
        for (auto& m : members)
            if (auto d = std::dynamic_pointer_cast<
                    DeclarationStatement>(m.decl))
                vm.models[name].push_back(
                    {ModelStatement::cpp_type(d->type), d->name});
        vm.class_data.insert(name);
    }
};

// фабрика экземпляра — в eval_impl.cpp
FuncPtr make_class_factory(const std::string& name,
                           std::vector<ClassMember> members);
