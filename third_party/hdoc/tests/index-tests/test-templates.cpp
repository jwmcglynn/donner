// Copyright 2019-2023 hdoc
// SPDX-License-Identifier: AGPL-3.0-only

#include "tests/TestUtils.hpp"

TEST_CASE("Function template declaration") {
  const std::string code = R"(
    template<class T>
    void foo(T& a, T& b);
  )";

  hdoc::types::Index index;
  runOverCode(code, index);
  checkIndexSizes(index, 0, 1, 0, 0);

  hdoc::types::FunctionSymbol f = index.functions.entries.begin()->second;
  CHECK(f.name == "foo");
  CHECK(f.briefComment == "");
  CHECK(f.docComment == "");
  CHECK(f.ID.str().size() == 16);
  CHECK(f.parentNamespaceID.raw() == 0);

  CHECK(f.isRecordMember == false);
  CHECK(f.isConstexpr == false);
  CHECK(f.isConsteval == false);
  CHECK(f.isInline == false);
  CHECK(f.isConst == false);
  CHECK(f.isVolatile == false);
  CHECK(f.isRestrict == false);
  CHECK(f.isVirtual == false);
  CHECK(f.isVariadic == false);
  CHECK(f.isNoExcept == false);
  CHECK(f.hasTrailingReturn == false);
  CHECK(f.isCtorOrDtor == false);

  CHECK(f.access == clang::AS_none);
  CHECK(f.storageClass == clang::SC_None);
  CHECK(f.refQualifier == clang::RQ_None);

  CHECK(f.proto == "template <class T>void foo(T & a, T & b)");
  CHECK(f.returnType.name == "void");
  CHECK(f.returnType.id.raw() == 0);
  CHECK(f.returnTypeDocComment == "");
  CHECK(f.params.size() == 2);

  CHECK(f.params[0].name == "a");
  CHECK(f.params[0].type.name == "T &");
  CHECK(f.params[0].type.id.raw() == 0);
  CHECK(f.params[0].docComment == "");
  CHECK(f.params[0].defaultValue == "");

  CHECK(f.params[1].name == "b");
  CHECK(f.params[1].type.name == "T &");
  CHECK(f.params[1].type.id.raw() == 0);
  CHECK(f.params[1].docComment == "");
  CHECK(f.params[1].defaultValue == "");

  CHECK(f.templateParams.size() == 1);
  CHECK(f.templateParams[0].templateType == hdoc::types::TemplateParam::TemplateType::TemplateTypeParameter);
  CHECK(f.templateParams[0].name == "T");
  CHECK(f.templateParams[0].type == "");
  CHECK(f.templateParams[0].docComment == "");
  CHECK(f.templateParams[0].defaultValue == "");
  CHECK(f.templateParams[0].isParameterPack == false);
  CHECK(f.templateParams[0].isTypename == false);
}

TEST_CASE("Function template definition") {
  const std::string code = R"(
    template<typename T>
    void foo(T& a, T& b) {}
  )";

  hdoc::types::Index index;
  runOverCode(code, index);
  checkIndexSizes(index, 0, 1, 0, 0);

  hdoc::types::FunctionSymbol f = index.functions.entries.begin()->second;
  CHECK(f.name == "foo");
  CHECK(f.briefComment == "");
  CHECK(f.docComment == "");
  CHECK(f.ID.str().size() == 16);
  CHECK(f.parentNamespaceID.raw() == 0);

  CHECK(f.isRecordMember == false);
  CHECK(f.isConstexpr == false);
  CHECK(f.isConsteval == false);
  CHECK(f.isInline == false);
  CHECK(f.isConst == false);
  CHECK(f.isVolatile == false);
  CHECK(f.isRestrict == false);
  CHECK(f.isVirtual == false);
  CHECK(f.isVariadic == false);
  CHECK(f.isNoExcept == false);
  CHECK(f.hasTrailingReturn == false);
  CHECK(f.isCtorOrDtor == false);

  CHECK(f.access == clang::AS_none);
  CHECK(f.storageClass == clang::SC_None);
  CHECK(f.refQualifier == clang::RQ_None);

  CHECK(f.proto == "template <typename T>void foo(T & a, T & b)");
  CHECK(f.returnType.name == "void");
  CHECK(f.returnType.id.raw() == 0);
  CHECK(f.returnTypeDocComment == "");
  CHECK(f.params.size() == 2);

  CHECK(f.params[0].name == "a");
  CHECK(f.params[0].type.name == "T &");
  CHECK(f.params[0].type.id.raw() == 0);
  CHECK(f.params[0].docComment == "");
  CHECK(f.params[0].defaultValue == "");

  CHECK(f.params[1].name == "b");
  CHECK(f.params[1].type.name == "T &");
  CHECK(f.params[1].type.id.raw() == 0);
  CHECK(f.params[1].docComment == "");
  CHECK(f.params[1].defaultValue == "");

  CHECK(f.templateParams.size() == 1);
  CHECK(f.templateParams[0].templateType == hdoc::types::TemplateParam::TemplateType::TemplateTypeParameter);
  CHECK(f.templateParams[0].name == "T");
  CHECK(f.templateParams[0].type == "");
  CHECK(f.templateParams[0].docComment == "");
  CHECK(f.templateParams[0].defaultValue == "");
  CHECK(f.templateParams[0].isParameterPack == false);
  CHECK(f.templateParams[0].isTypename == true);
}

TEST_CASE("Function with variadic template") {
  const std::string code = R"(
    template <typename... Ts>
    void ignore(Ts... ts) {}
  )";

  hdoc::types::Index index;
  runOverCode(code, index);
  checkIndexSizes(index, 0, 1, 0, 0);

  hdoc::types::FunctionSymbol f = index.functions.entries.begin()->second;
  CHECK(f.name == "ignore");
  CHECK(f.briefComment == "");
  CHECK(f.docComment == "");
  CHECK(f.ID.str().size() == 16);
  CHECK(f.parentNamespaceID.raw() == 0);

  CHECK(f.isRecordMember == false);
  CHECK(f.isConstexpr == false);
  CHECK(f.isConsteval == false);
  CHECK(f.isInline == false);
  CHECK(f.isConst == false);
  CHECK(f.isVolatile == false);
  CHECK(f.isRestrict == false);
  CHECK(f.isVirtual == false);
  CHECK(f.isVariadic == false);
  CHECK(f.isNoExcept == false);
  CHECK(f.hasTrailingReturn == false);
  CHECK(f.isCtorOrDtor == false);

  CHECK(f.access == clang::AS_none);
  CHECK(f.storageClass == clang::SC_None);
  CHECK(f.refQualifier == clang::RQ_None);

  CHECK(f.proto == "template <typename... Ts>void ignore(Ts... ts)");
  CHECK(f.returnType.name == "void");
  CHECK(f.returnType.id.raw() == 0);
  CHECK(f.returnTypeDocComment == "");
  CHECK(f.params.size() == 1);

  CHECK(f.params[0].name == "ts");
  CHECK(f.params[0].type.name == "Ts...");
  CHECK(f.params[0].type.id.raw() == 0);
  CHECK(f.params[0].docComment == "");
  CHECK(f.params[0].defaultValue == "");

  CHECK(f.templateParams.size() == 1);
  CHECK(f.templateParams[0].templateType == hdoc::types::TemplateParam::TemplateType::TemplateTypeParameter);
  CHECK(f.templateParams[0].name == "Ts");
  CHECK(f.templateParams[0].type == "");
  CHECK(f.templateParams[0].docComment == "");
  CHECK(f.templateParams[0].defaultValue == "");
  CHECK(f.templateParams[0].isParameterPack == true);
  CHECK(f.templateParams[0].isTypename == true);
}

TEST_CASE("Function specialized template parameter") {
  const std::string code = R"(
    template<class T>
    class Template {};

    struct Foo {
      void Bar(Template<double>&);
    };

    void Foo::Bar(Template<double>&) {}
  )";

  hdoc::types::Index index;
  runOverCode(code, index);
  checkIndexSizes(index, 2, 1, 0, 0);

  std::optional<hdoc::types::RecordSymbol> o1 = findByName(index.records, "Template");
  std::optional<hdoc::types::RecordSymbol> o2 = findByName(index.records, "Foo");

  CHECK(o1);
  CHECK(o2);

  hdoc::types::RecordSymbol s1 = *o1;
  CHECK(s1.name == "Template");
  CHECK(s1.briefComment == "");
  CHECK(s1.docComment == "");
  CHECK(s1.ID.str().size() == 16);
  CHECK(s1.parentNamespaceID.raw() == 0);

  CHECK(s1.type == "class");
  CHECK(s1.proto == "template <class T> class Template");
  CHECK(s1.vars.size() == 0);
  CHECK(s1.methodIDs.size() == 0);
  CHECK(s1.baseRecords.size() == 0);

  CHECK(s1.templateParams.size() == 1);
  CHECK(s1.templateParams[0].templateType == hdoc::types::TemplateParam::TemplateType::TemplateTypeParameter);
  CHECK(s1.templateParams[0].name == "T");
  CHECK(s1.templateParams[0].type == "");
  CHECK(s1.templateParams[0].docComment == "");
  CHECK(s1.templateParams[0].defaultValue == "");
  CHECK(s1.templateParams[0].isParameterPack == false);
  CHECK(s1.templateParams[0].isTypename == false);

  hdoc::types::RecordSymbol s2 = *o2;
  CHECK(s2.name == "Foo");
  CHECK(s2.briefComment == "");
  CHECK(s2.docComment == "");
  CHECK(s2.ID.str().size() == 16);
  CHECK(s2.parentNamespaceID.raw() == 0);

  CHECK(s2.type == "struct");
  CHECK(s2.proto == "struct Foo");
  CHECK(s2.vars.size() == 0);
  CHECK(s2.methodIDs.size() == 1);
  CHECK(s2.baseRecords.size() == 0);
  CHECK(s2.templateParams.size() == 0);

  hdoc::types::FunctionSymbol f = index.functions.entries.begin()->second;
  CHECK(f.name == "Bar");
  CHECK(f.briefComment == "");
  CHECK(f.docComment == "");
  CHECK(f.ID.str().size() == 16);
  CHECK(f.parentNamespaceID == s2.ID);

  CHECK(f.isRecordMember == true);
  CHECK(f.isConstexpr == false);
  CHECK(f.isConsteval == false);
  CHECK(f.isInline == false);
  CHECK(f.isConst == false);
  CHECK(f.isVolatile == false);
  CHECK(f.isRestrict == false);
  CHECK(f.isVirtual == false);
  CHECK(f.isVariadic == false);
  CHECK(f.isNoExcept == false);
  CHECK(f.hasTrailingReturn == false);
  CHECK(f.isCtorOrDtor == false);

  CHECK(f.access == clang::AS_public);
  CHECK(f.storageClass == clang::SC_None);
  CHECK(f.refQualifier == clang::RQ_None);

  CHECK(f.proto == "void Bar(Template<double> &)");
  CHECK(f.returnType.name == "void");
  CHECK(f.returnType.id.raw() == 0);
  CHECK(f.returnTypeDocComment == "");
  CHECK(f.templateParams.size() == 0);

  CHECK(f.params.size() == 1);
  CHECK(f.params[0].name == "");
  CHECK(f.params[0].type.name == "Template<double> &");
  CHECK(f.params[0].type.id == s1.ID);
  CHECK(f.params[0].docComment == "");
  CHECK(f.params[0].defaultValue == "");
}

TEST_CASE("Templated class with templated member variable") {
  const std::string code = R"(
    template <class T>
    struct C {
      T x;
      void bar();
    };
  )";

  hdoc::types::Index index;
  runOverCode(code, index);
  checkIndexSizes(index, 1, 1, 0, 0);

  hdoc::types::RecordSymbol s = index.records.entries.begin()->second;
  CHECK(s.name == "C");
  CHECK(s.briefComment == "");
  CHECK(s.docComment == "");
  CHECK(s.ID.str().size() == 16);
  CHECK(s.parentNamespaceID.raw() == 0);

  CHECK(s.type == "struct");
  CHECK(s.proto == "template <class T> struct C");
  CHECK(s.vars.size() == 1);
  CHECK(s.methodIDs.size() == 1);
  CHECK(s.baseRecords.size() == 0);

  CHECK(s.vars[0].isStatic == false);
  CHECK(s.vars[0].name == "x");
  CHECK(s.vars[0].type.name == "T");
  CHECK(s.vars[0].type.id.raw() == 0);
  CHECK(s.vars[0].defaultValue == "");
  CHECK(s.vars[0].docComment == "");
  CHECK(s.vars[0].access == clang::AS_public);

  CHECK(s.templateParams.size() == 1);
  CHECK(s.templateParams[0].templateType == hdoc::types::TemplateParam::TemplateType::TemplateTypeParameter);
  CHECK(s.templateParams[0].name == "T");
  CHECK(s.templateParams[0].type == "");
  CHECK(s.templateParams[0].docComment == "");
  CHECK(s.templateParams[0].defaultValue == "");
  CHECK(s.templateParams[0].isParameterPack == false);
  CHECK(s.templateParams[0].isTypename == false);

  hdoc::types::FunctionSymbol f = index.functions.entries.begin()->second;
  CHECK(f.name == "bar");
  CHECK(f.briefComment == "");
  CHECK(f.docComment == "");
  CHECK(f.ID.str().size() == 16);
  CHECK(f.parentNamespaceID == s.ID);

  CHECK(f.isRecordMember == true);
  CHECK(f.isConstexpr == false);
  CHECK(f.isConsteval == false);
  CHECK(f.isInline == false);
  CHECK(f.isConst == false);
  CHECK(f.isVolatile == false);
  CHECK(f.isRestrict == false);
  CHECK(f.isVirtual == false);
  CHECK(f.isVariadic == false);
  CHECK(f.isNoExcept == false);
  CHECK(f.hasTrailingReturn == false);
  CHECK(f.isCtorOrDtor == false);

  CHECK(f.access == clang::AS_public);
  CHECK(f.storageClass == clang::SC_None);
  CHECK(f.refQualifier == clang::RQ_None);

  CHECK(f.proto == "void bar()");
  CHECK(f.returnType.name == "void");
  CHECK(f.returnType.id.raw() == 0);
  CHECK(f.returnTypeDocComment == "");
  CHECK(f.params.size() == 0);
}

TEST_CASE("Namespace and templated class") {
  const std::string code = R"(
    namespace ns {
      template<typename T>
      class Foo {};
    }
  )";

  hdoc::types::Index index;
  runOverCode(code, index);
  checkIndexSizes(index, 1, 0, 0, 1);

  hdoc::types::NamespaceSymbol n = index.namespaces.entries.begin()->second;
  CHECK(n.name == "ns");
  CHECK(n.briefComment == "");
  CHECK(n.docComment == "");
  CHECK(n.ID.str().size() == 16);
  CHECK(n.parentNamespaceID.raw() == 0);

  hdoc::types::RecordSymbol s = index.records.entries.begin()->second;
  CHECK(s.name == "Foo");
  CHECK(s.briefComment == "");
  CHECK(s.docComment == "");
  CHECK(s.ID.str().size() == 16);
  CHECK(s.parentNamespaceID == n.ID);

  CHECK(s.type == "class");
  CHECK(s.proto == "template <typename T> class Foo");
  CHECK(s.vars.size() == 0);
  CHECK(s.methodIDs.size() == 0);
  CHECK(s.baseRecords.size() == 0);

  CHECK(s.templateParams.size() == 1);
  CHECK(s.templateParams[0].templateType == hdoc::types::TemplateParam::TemplateType::TemplateTypeParameter);
  CHECK(s.templateParams[0].name == "T");
  CHECK(s.templateParams[0].type == "");
  CHECK(s.templateParams[0].docComment == "");
  CHECK(s.templateParams[0].defaultValue == "");
  CHECK(s.templateParams[0].isParameterPack == false);
  CHECK(s.templateParams[0].isTypename == true);
}

TEST_CASE("Specialized function definition") {
  const std::string code = R"(
    template<class T>
    class Template {
      void Foo();
    };

    template<class T>
    void Template<T>::Foo() {}

    template<>
    void Template<void>::Foo() {}
  )";

  hdoc::types::Index index;
  runOverCode(code, index);
  checkIndexSizes(index, 1, 1, 0, 0);

  hdoc::types::RecordSymbol s = index.records.entries.begin()->second;
  CHECK(s.name == "Template");
  CHECK(s.briefComment == "");
  CHECK(s.docComment == "");
  CHECK(s.ID.str().size() == 16);
  CHECK(s.parentNamespaceID.raw() == 0);

  CHECK(s.type == "class");
  CHECK(s.proto == "template <class T> class Template");
  CHECK(s.vars.size() == 0);
  CHECK(s.methodIDs.size() == 1);
  CHECK(s.baseRecords.size() == 0);

  CHECK(s.templateParams.size() == 1);
  CHECK(s.templateParams[0].templateType == hdoc::types::TemplateParam::TemplateType::TemplateTypeParameter);
  CHECK(s.templateParams[0].name == "T");
  CHECK(s.templateParams[0].type == "");
  CHECK(s.templateParams[0].docComment == "");
  CHECK(s.templateParams[0].defaultValue == "");
  CHECK(s.templateParams[0].isParameterPack == false);
  CHECK(s.templateParams[0].isTypename == false);

  hdoc::types::FunctionSymbol f = index.functions.entries.begin()->second;
  CHECK(f.name == "Foo");
  CHECK(f.briefComment == "");
  CHECK(f.docComment == "");
  CHECK(f.ID.str().size() == 16);
  CHECK(f.parentNamespaceID == s.ID);

  CHECK(f.isRecordMember == true);
  CHECK(f.isConstexpr == false);
  CHECK(f.isConsteval == false);
  CHECK(f.isInline == false);
  CHECK(f.isConst == false);
  CHECK(f.isVolatile == false);
  CHECK(f.isRestrict == false);
  CHECK(f.isVirtual == false);
  CHECK(f.isVariadic == false);
  CHECK(f.isNoExcept == false);
  CHECK(f.hasTrailingReturn == false);
  CHECK(f.isCtorOrDtor == false);

  CHECK(f.access == clang::AS_private);
  CHECK(f.storageClass == clang::SC_None);
  CHECK(f.refQualifier == clang::RQ_None);

  CHECK(f.proto == "void Foo()");
  CHECK(f.returnType.name == "void");
  CHECK(f.returnType.id.raw() == 0);
  CHECK(f.returnTypeDocComment == "");
  CHECK(f.params.size() == 0);
  CHECK(f.templateParams.size() == 0);
}

TEST_CASE("Function that takes specialized template argument has correct TypeIDs") {
  const std::string code = R"(
    template<class T>
    class TemplatedClass {};

    void function(TemplatedClass<double> arg) {}
  )";

  hdoc::types::Index index;
  runOverCode(code, index);
  checkIndexSizes(index, 1, 1, 0, 0);

  hdoc::types::RecordSymbol s = index.records.entries.begin()->second;
  CHECK(s.name == "TemplatedClass");
  CHECK(s.briefComment == "");
  CHECK(s.docComment == "");
  CHECK(s.ID.str().size() == 16);
  CHECK(s.parentNamespaceID.raw() == 0);

  CHECK(s.type == "class");
  CHECK(s.proto == "template <class T> class TemplatedClass");
  CHECK(s.vars.size() == 0);
  CHECK(s.methodIDs.size() == 0);
  CHECK(s.baseRecords.size() == 0);

  CHECK(s.templateParams.size() == 1);
  CHECK(s.templateParams[0].templateType == hdoc::types::TemplateParam::TemplateType::TemplateTypeParameter);
  CHECK(s.templateParams[0].name == "T");
  CHECK(s.templateParams[0].type == "");
  CHECK(s.templateParams[0].docComment == "");
  CHECK(s.templateParams[0].defaultValue == "");
  CHECK(s.templateParams[0].isParameterPack == false);
  CHECK(s.templateParams[0].isTypename == false);

  hdoc::types::FunctionSymbol f = index.functions.entries.begin()->second;
  CHECK(f.name == "function");
  CHECK(f.briefComment == "");
  CHECK(f.docComment == "");
  CHECK(f.ID.str().size() == 16);
  CHECK(f.parentNamespaceID.raw() == 0);

  CHECK(f.isRecordMember == false);
  CHECK(f.isConstexpr == false);
  CHECK(f.isConsteval == false);
  CHECK(f.isInline == false);
  CHECK(f.isConst == false);
  CHECK(f.isVolatile == false);
  CHECK(f.isRestrict == false);
  CHECK(f.isVirtual == false);
  CHECK(f.isVariadic == false);
  CHECK(f.isNoExcept == false);
  CHECK(f.hasTrailingReturn == false);
  CHECK(f.isCtorOrDtor == false);

  CHECK(f.access == clang::AS_none);
  CHECK(f.storageClass == clang::SC_None);
  CHECK(f.refQualifier == clang::RQ_None);

  CHECK(f.proto == "void function(TemplatedClass<double> arg)");
  CHECK(f.returnType.name == "void");
  CHECK(f.returnType.id.raw() == 0);
  CHECK(f.returnTypeDocComment == "");
  CHECK(f.params.size() == 1);
  CHECK(f.templateParams.size() == 0);

  CHECK(f.params.size() == 1);
  CHECK(f.params[0].name == "arg");
  CHECK(f.params[0].type.name == "TemplatedClass<double>");
  CHECK(f.params[0].type.id == s.ID);
  CHECK(f.params[0].docComment == "");
  CHECK(f.params[0].defaultValue == "");
}

TEST_CASE("Function that takes specialized template argument with reference has correct TypeIDs") {
  const std::string code = R"(
    template<class T>
    class TemplatedClass {};

    void function(TemplatedClass<double>& arg) {}
  )";

  hdoc::types::Index index;
  runOverCode(code, index);
  checkIndexSizes(index, 1, 1, 0, 0);

  hdoc::types::RecordSymbol s = index.records.entries.begin()->second;
  CHECK(s.name == "TemplatedClass");
  CHECK(s.briefComment == "");
  CHECK(s.docComment == "");
  CHECK(s.ID.str().size() == 16);
  CHECK(s.parentNamespaceID.raw() == 0);

  CHECK(s.type == "class");
  CHECK(s.proto == "template <class T> class TemplatedClass");
  CHECK(s.vars.size() == 0);
  CHECK(s.methodIDs.size() == 0);
  CHECK(s.baseRecords.size() == 0);

  CHECK(s.templateParams.size() == 1);
  CHECK(s.templateParams[0].templateType == hdoc::types::TemplateParam::TemplateType::TemplateTypeParameter);
  CHECK(s.templateParams[0].name == "T");
  CHECK(s.templateParams[0].type == "");
  CHECK(s.templateParams[0].docComment == "");
  CHECK(s.templateParams[0].defaultValue == "");
  CHECK(s.templateParams[0].isParameterPack == false);
  CHECK(s.templateParams[0].isTypename == false);

  hdoc::types::FunctionSymbol f = index.functions.entries.begin()->second;
  CHECK(f.name == "function");
  CHECK(f.briefComment == "");
  CHECK(f.docComment == "");
  CHECK(f.ID.str().size() == 16);
  CHECK(f.parentNamespaceID.raw() == 0);

  CHECK(f.isRecordMember == false);
  CHECK(f.isConstexpr == false);
  CHECK(f.isConsteval == false);
  CHECK(f.isInline == false);
  CHECK(f.isConst == false);
  CHECK(f.isVolatile == false);
  CHECK(f.isRestrict == false);
  CHECK(f.isVirtual == false);
  CHECK(f.isVariadic == false);
  CHECK(f.isNoExcept == false);
  CHECK(f.hasTrailingReturn == false);
  CHECK(f.isCtorOrDtor == false);

  CHECK(f.access == clang::AS_none);
  CHECK(f.storageClass == clang::SC_None);
  CHECK(f.refQualifier == clang::RQ_None);

  CHECK(f.proto == "void function(TemplatedClass<double> & arg)");
  CHECK(f.returnType.name == "void");
  CHECK(f.returnType.id.raw() == 0);
  CHECK(f.returnTypeDocComment == "");
  CHECK(f.params.size() == 1);
  CHECK(f.templateParams.size() == 0);

  CHECK(f.params.size() == 1);
  CHECK(f.params[0].name == "arg");
  CHECK(f.params[0].type.name == "TemplatedClass<double> &");
  CHECK(f.params[0].type.id == s.ID);
  CHECK(f.params[0].docComment == "");
  CHECK(f.params[0].defaultValue == "");
}

TEST_CASE("Function that takes specialized template argument with pointer has correct TypeIDs") {
  const std::string code = R"(
    template<class T>
    class TemplatedClass {};

    void function(TemplatedClass<double>* arg) {}
  )";

  hdoc::types::Index index;
  runOverCode(code, index);
  checkIndexSizes(index, 1, 1, 0, 0);

  hdoc::types::RecordSymbol s = index.records.entries.begin()->second;
  CHECK(s.name == "TemplatedClass");
  CHECK(s.briefComment == "");
  CHECK(s.docComment == "");
  CHECK(s.ID.str().size() == 16);
  CHECK(s.parentNamespaceID.raw() == 0);

  CHECK(s.type == "class");
  CHECK(s.proto == "template <class T> class TemplatedClass");
  CHECK(s.vars.size() == 0);
  CHECK(s.methodIDs.size() == 0);
  CHECK(s.baseRecords.size() == 0);

  CHECK(s.templateParams.size() == 1);
  CHECK(s.templateParams[0].templateType == hdoc::types::TemplateParam::TemplateType::TemplateTypeParameter);
  CHECK(s.templateParams[0].name == "T");
  CHECK(s.templateParams[0].type == "");
  CHECK(s.templateParams[0].docComment == "");
  CHECK(s.templateParams[0].defaultValue == "");
  CHECK(s.templateParams[0].isParameterPack == false);
  CHECK(s.templateParams[0].isTypename == false);

  hdoc::types::FunctionSymbol f = index.functions.entries.begin()->second;
  CHECK(f.name == "function");
  CHECK(f.briefComment == "");
  CHECK(f.docComment == "");
  CHECK(f.ID.str().size() == 16);
  CHECK(f.parentNamespaceID.raw() == 0);

  CHECK(f.isRecordMember == false);
  CHECK(f.isConstexpr == false);
  CHECK(f.isConsteval == false);
  CHECK(f.isInline == false);
  CHECK(f.isConst == false);
  CHECK(f.isVolatile == false);
  CHECK(f.isRestrict == false);
  CHECK(f.isVirtual == false);
  CHECK(f.isVariadic == false);
  CHECK(f.isNoExcept == false);
  CHECK(f.hasTrailingReturn == false);
  CHECK(f.isCtorOrDtor == false);

  CHECK(f.access == clang::AS_none);
  CHECK(f.storageClass == clang::SC_None);
  CHECK(f.refQualifier == clang::RQ_None);

  CHECK(f.proto == "void function(TemplatedClass<double> * arg)");
  CHECK(f.returnType.name == "void");
  CHECK(f.returnType.id.raw() == 0);
  CHECK(f.returnTypeDocComment == "");
  CHECK(f.params.size() == 1);
  CHECK(f.templateParams.size() == 0);

  CHECK(f.params.size() == 1);
  CHECK(f.params[0].name == "arg");
  CHECK(f.params[0].type.name == "TemplatedClass<double> *");
  CHECK(f.params[0].type.id == s.ID);
  CHECK(f.params[0].docComment == "");
  CHECK(f.params[0].defaultValue == "");
}

// TODO: figure out why there are 3 CXXMethodDecls in this code block
// TEST_CASE("what the fuck") {
//   const std::string code = R"(
//     template<typename T>
//     struct Foo {
//       template<typename R>
//       static int foo();
//     };

//     int a = Foo<int>::foo<float>();
//     int b = Foo<bool>::foo<double>();
//   )";

//   hdoc::types::Index index;
//   runOverCode(code, index);
//   checkIndexSizes(index, 1, 1, 0, 0);
// }
