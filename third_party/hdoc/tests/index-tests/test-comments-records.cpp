// Copyright 2019-2023 hdoc
// SPDX-License-Identifier: AGPL-3.0-only

#include "tests/TestUtils.hpp"

TEST_CASE("Record with commented member variables") {
  const std::string code = R"(
    /*!
     * @brief foo bar baz
     */
    class Foo {
      public:
        /// the sample rate (as integer 0..100)
        int m_sample_rate;
        /// whether the client is enabled
        bool m_enabled = true;
      private:
        /// the public key to be used in requests
        int m_public_key;
        /// the secret key to be used in requests
        int m_secret_key;
    };
  )";

  hdoc::types::Index index;
  runOverCode(code, index);
  checkIndexSizes(index, 1, 0, 0, 0);

  hdoc::types::RecordSymbol s = index.records.entries.begin()->second;
  CHECK(s.name == "Foo");
  CHECK(s.briefComment == "foo bar baz");
  CHECK(s.docComment == "");
  CHECK(s.ID.str().size() == 16);
  CHECK(s.parentNamespaceID.raw() == 0);
  CHECK(s.vars.size() == 4);
  CHECK(s.templateParams.size() == 0);

  CHECK(s.vars[0].isStatic == false);
  CHECK(s.vars[0].name == "m_sample_rate");
  CHECK(s.vars[0].type.name == "int");
  CHECK(s.vars[0].type.id.raw() == 0);
  CHECK(s.vars[0].defaultValue == "");
  CHECK(s.vars[0].docComment == "the sample rate (as integer 0..100)");
  CHECK(s.vars[0].access == clang::AS_public);

  CHECK(s.vars[1].isStatic == false);
  CHECK(s.vars[1].name == "m_enabled");
  CHECK(s.vars[1].type.name == "bool");
  CHECK(s.vars[1].type.id.raw() == 0);
  CHECK(s.vars[1].defaultValue == "true");
  CHECK(s.vars[1].docComment == "whether the client is enabled");
  CHECK(s.vars[1].access == clang::AS_public);

  CHECK(s.vars[2].isStatic == false);
  CHECK(s.vars[2].name == "m_public_key");
  CHECK(s.vars[2].type.name == "int");
  CHECK(s.vars[2].type.id.raw() == 0);
  CHECK(s.vars[2].defaultValue == "");
  CHECK(s.vars[2].docComment == "the public key to be used in requests");
  CHECK(s.vars[2].access == clang::AS_private);

  CHECK(s.vars[3].isStatic == false);
  CHECK(s.vars[3].name == "m_secret_key");
  CHECK(s.vars[3].type.name == "int");
  CHECK(s.vars[3].type.id.raw() == 0);
  CHECK(s.vars[3].defaultValue == "");
  CHECK(s.vars[3].docComment == "the secret key to be used in requests");
  CHECK(s.vars[3].access == clang::AS_private);
}

TEST_CASE("Record with inline command comments") {
  const std::string code = R"(
    /// @brief Testing if inline command comments, like @a varX, work.
    ///
    /// Let's see if they work in docComments @b makeMeBold.
    class Foo {
      public:
        /// the sample rate (as integer 0..100) @b makeMeBold2
        int m_sample_rate;
      private:
        /// the public key to be used in requests
        int m_public_key;
    };
  )";

  hdoc::types::Index index;
  runOverCode(code, index);
  checkIndexSizes(index, 1, 0, 0, 0);

  hdoc::types::RecordSymbol s = index.records.entries.begin()->second;
  CHECK(s.name == "Foo");
  CHECK(s.briefComment == "Testing if inline command comments, like @a varX, work.");
  CHECK(s.docComment == "Let's see if they work in docComments @b makeMeBold.");
  CHECK(s.ID.str().size() == 16);
  CHECK(s.parentNamespaceID.raw() == 0);
  CHECK(s.vars.size() == 2);
  CHECK(s.templateParams.size() == 0);

  CHECK(s.vars[0].isStatic == false);
  CHECK(s.vars[0].name == "m_sample_rate");
  CHECK(s.vars[0].type.name == "int");
  CHECK(s.vars[0].type.id.raw() == 0);
  CHECK(s.vars[0].defaultValue == "");
  CHECK(s.vars[0].docComment == "the sample rate (as integer 0..100) @b makeMeBold2");
  CHECK(s.vars[0].access == clang::AS_public);

  CHECK(s.vars[1].isStatic == false);
  CHECK(s.vars[1].name == "m_public_key");
  CHECK(s.vars[1].type.name == "int");
  CHECK(s.vars[1].type.id.raw() == 0);
  CHECK(s.vars[1].defaultValue == "");
  CHECK(s.vars[1].docComment == "the public key to be used in requests");
  CHECK(s.vars[1].access == clang::AS_private);
}

TEST_CASE("Record without brief comment") {
  const std::string code = R"raw(
    /**
     * DOM object for a `<path>` element.
     *
     * Use the `d` attribute to define the path.
     *
     * Example path:
     * ```
     * M 40 50 V 250 C 100 100 115 75 190 125
     * ```
     *
     * \htmlonly
     * <svg id="xml_path" width="300" height="300" style="background-color: white">
     *   <style>
     *     #xml_path text { font-size: 16px; font-weight: bold; color: black }
     *     #xml_path path { stroke-width: 2px; stroke: black; fill: none }
     *     #xml_path circle { r: 3px; fill: black }
     *     #xml_path line { stroke-width: 2px; stroke: red; stroke-dasharray: 6,4 }
     *   </style>
     *   <path d="M 40 50 V 250 C 100 100 115 75 190 125" />
     *   <circle cx="40" cy="50" style="fill: red" />
     *   <text x="50" y="53">M 40 50</text>
     *   <polygon points="0,0 5,10 10,0" transform="translate(35,150)" fill="red" />
     *   <circle cx="40" cy="250" />
     *   <text x="50" y="253">V 250</text>
     *   <circle cx="190" cy="125" />
     *   <line x1="40" y1="250" x2="100" y2="100" />
     *   <line x1="115" y1="75" x2="190" y2="125" />
     *   <circle cx="100" cy="100" />
     *   <circle cx="115" cy="75" />
     *   <text x="200" y="128">C 100 100</text>
     *   <text x="200" y="148">115 75</text>
     *   <text x="200" y="168">190 125</text>
     * </svg>
     * \endhtmlonly
     */
    class SVGPathElement {};
)raw";

  hdoc::types::Index index;
  runOverCode(code, index);
  checkIndexSizes(index, 1, 0, 0, 0);

  hdoc::types::RecordSymbol s = index.records.entries.begin()->second;
  CHECK(s.name == "SVGPathElement");
  CHECK(s.briefComment == "");
  CHECK(s.docComment == R"raw(DOM object for a `<path>` element.

Use the `d` attribute to define the path.

Example path:
```
M 40 50 V 250 C 100 100 115 75 190 125
```

 <svg id="xml_path" width="300" height="300" style="background-color: white">
   <style>
     #xml_path text { font-size: 16px; font-weight: bold; color: black }
     #xml_path path { stroke-width: 2px; stroke: black; fill: none }
     #xml_path circle { r: 3px; fill: black }
     #xml_path line { stroke-width: 2px; stroke: red; stroke-dasharray: 6,4 }
   </style>
   <path d="M 40 50 V 250 C 100 100 115 75 190 125" />
   <circle cx="40" cy="50" style="fill: red" />
   <text x="50" y="53">M 40 50</text>
   <polygon points="0,0 5,10 10,0" transform="translate(35,150)" fill="red" />
   <circle cx="40" cy="250" />
   <text x="50" y="253">V 250</text>
   <circle cx="190" cy="125" />
   <line x1="40" y1="250" x2="100" y2="100" />
   <line x1="115" y1="75" x2="190" y2="125" />
   <circle cx="100" cy="100" />
   <circle cx="115" cy="75" />
   <text x="200" y="128">C 100 100</text>
   <text x="200" y="148">115 75</text>
   <text x="200" y="168">190 125</text>
 </svg>)raw");
  CHECK(s.ID.str().size() == 16);
  CHECK(s.parentNamespaceID.raw() == 0);
  CHECK(s.vars.size() == 0);
  CHECK(s.templateParams.size() == 0);
}
