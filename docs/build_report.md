# Donner Build Report

Generated with: tools/generate_build_report.py --all --save docs/build_report.md

## Summary

- Platform: Linux aarch64
- Git revision: [df4940839bb7927713658d2dc9924b988410d60a](https://github.com/jwmcglynn/donner/commit/df4940839bb7927713658d2dc9924b988410d60a)
- Working tree: dirty (5 paths)
- Lines of Code: success (3.2s)
- Binary Size: success (231.0s)
- Code Coverage: success (1006.7s)
- Tests: success (596.9s)
- Public Targets: success (1.1s)
- External Dependencies: success (5.7s)

## Local Changes
```
M .github/workflows/deploy_docs.yaml
 M Doxyfile
 M tools/generate_build_report.py
 M tools/generate_build_report_tests.py
?? tools/build_docs.sh
```

## Lines of Code
```
$ tools/cloc.sh
Lines of source code:       84.7k
Lines of comments:          28.6k
Comment percentage:         33.0%
Product lines of code:      47.4k
Test lines of code:         33.0k
```

## Binary Size
Generated with: `tools/binary_size.sh`

Full report: [binary_size_report.html](reports/binary-size/binary_size_report.html)

```
Total binary size of svg_parser_tool
2.2M	build-binary-size/svg_parser_tool

Total binary size of donner-svg
3.8M	build-binary-size/donner-svg
```

### Detailed analysis of `svg_parser_tool`


Saved report to build-binary-size/binary_size_report.html

`bloaty -d compileunits -n 20` output
```
    FILE SIZE        VM SIZE    
 --------------  -------------- 
  86.0%  1.88Mi  83.2%  1001Ki    donner
    24.9%   478Ki  26.9%   268Ki    [100 Others]
    18.1%   348Ki  19.4%   194Ki    donner/svg/parser/SVGParser.cc
     8.9%   171Ki   8.4%  84.1Ki    donner/svg/parser/svg_parser_tool.cc
     5.7%   109Ki   3.7%  37.0Ki    donner/css/CSS.cc
     5.5%   105Ki   5.4%  54.1Ki    donner/svg/SVGDocument.cc
     4.9%  94.2Ki   6.6%  66.1Ki    donner/svg/properties/PropertyParsing.cc
     3.7%  71.7Ki   3.1%  30.9Ki    donner/css/parser/DeclarationListParser.cc
     3.2%  61.7Ki   4.4%  44.3Ki    donner/base/xml/XMLNode.cc
     3.0%  57.4Ki   1.3%  13.4Ki    donner/svg/SVGFETurbulenceElement.cc
     2.6%  49.7Ki   2.4%  24.1Ki    donner/svg/SVGDefsElement.cc
     2.3%  44.6Ki   2.3%  22.8Ki    donner/svg/SVGUseElement.cc
     2.3%  44.3Ki   2.2%  22.3Ki    donner/base/xml/XMLDocument.cc
     2.2%  42.8Ki   2.9%  29.3Ki    donner/base/parser/details/ParserBase.cc
     2.2%  42.1Ki   1.2%  12.2Ki    donner/svg/SVGRectElement.cc
     2.0%  37.9Ki   1.8%  18.3Ki    donner/svg/parser/AttributeParser.cc
     1.8%  35.1Ki   1.8%  18.3Ki    donner/svg/SVGTSpanElement.cc
     1.5%  29.1Ki   1.4%  14.2Ki    donner/svg/SVGGeometryElement.cc
     1.3%  25.8Ki   1.2%  12.2Ki    donner/svg/SVGGraphicsElement.cc
     1.3%  25.7Ki   1.2%  12.5Ki    donner/svg/SVGLinearGradientElement.cc
     1.2%  23.9Ki   1.2%  11.6Ki    donner/svg/SVGSVGElement.cc
     1.2%  22.3Ki   1.1%  10.8Ki    donner/svg/SVGPathElement.cc
   7.5%   166Ki   6.5%  78.3Ki    [590 Others]
   2.5%  55.5Ki   4.6%  55.5Ki    __donner_embedded_kPublicSansMediumOtf
   1.1%  24.5Ki   2.0%  24.5Ki    [section .rodata]
   0.5%  11.1Ki   0.0%       0    [section .symtab]
   0.4% 10.00Ki   0.8%  9.91Ki    _ZN6donner3css12_GLOBAL__N_17kColorsE
   0.4%  8.07Ki   0.7%  8.00Ki    crc_braid_table
   0.3%  6.42Ki   0.4%  5.34Ki    third_party/zlib
    27.4%  1.76Ki  22.4%  1.20Ki    external/zlib+/zutil.c
    19.5%  1.25Ki  21.0%  1.12Ki    external/zlib+/inffast.c
    19.3%  1.24Ki  22.1%  1.18Ki    external/zlib+/adler32.c
    16.0%  1.03Ki  17.2%     940    external/zlib+/inflate.c
    15.1%     990  16.0%     876    external/zlib+/crc32.c
     2.8%     183   1.3%      72    external/zlib+/inftrees.c
   0.3%  6.04Ki   0.3%  4.02Ki    too_large
   0.2%  3.48Ki   0.3%  3.37Ki    _ZN6donner3svg12_GLOBAL__N_111kPropertiesE
   0.2%  3.46Ki   0.3%  3.36Ki    _ZN6donner3svg12_GLOBAL__N_128kValidPresentationAttributesE
   0.1%  2.49Ki   0.0%       0    [section .strtab]
   0.1%  2.05Ki   0.2%  2.00Ki    lenfix
   0.1%  2.00Ki   0.0%       0    [ELF Section Headers]
   0.1%  1.99Ki   0.2%  1.99Ki    [section .rela.plt]
   0.1%  1.61Ki   0.1%  1.36Ki    _ZN6donner3svg10components12_GLOBAL__N_111kPropertiesE
   0.1%  1.51Ki   0.1%     864    _ZNSt8__detail9__variant12__gen_vtableINS0_20__variant_idx_cookieEOZNS0_17_Copy_assign_baseILb0EJN6donner3css5Token5IdentENS6_8FunctionENS6_9AtKeywordENS6_4HashENS6_6StringENS6_9BadStringENS6_3UrlENS6_6BadUrlENS6_5DelimENS6_6NumberENS6_10PercentageENS6_9DimensionENS6_10WhitespaceENS6_3CDOENS6_3CDCENS6_5ColonENS6_9SemicolonENS6_5CommaENS6_13SquareBracketENS6_11ParenthesisENS6_12CurlyBracketENS6_18CloseSquareBracketENS6_16CloseParenthesisENS6_17CloseCurlyBracketENS6_10ErrorTokenENS6_8EofTokenEEEaSERKSX_EUlOT_T0_E_JRKSt7variantIJS7_S8_S9_SA_SB_SC_SD_SE_SF_SG_SH_SI_SJ_SK_SL_SM_SN_SO_SP_SQ_SR_SS_ST_SU_SV_SW_EEEE9_S_vtableE
   0.1%  1.50Ki   0.1%     864    _ZNSt8__detail9__variant12__gen_vtableINS0_20__variant_idx_cookieEOZNS0_15_Copy_ctor_baseILb0EJN6donner3css5Token5IdentENS6_8FunctionENS6_9AtKeywordENS6_4HashENS6_6StringENS6_9BadStringENS6_3UrlENS6_6BadUrlENS6_5DelimENS6_6NumberENS6_10PercentageENS6_9DimensionENS6_10WhitespaceENS6_3CDOENS6_3CDCENS6_5ColonENS6_9SemicolonENS6_5CommaENS6_13SquareBracketENS6_11ParenthesisENS6_12CurlyBracketENS6_18CloseSquareBracketENS6_16CloseParenthesisENS6_17CloseCurlyBracketENS6_10ErrorTokenENS6_8EofTokenEEEC1ERKSX_EUlOT_T0_E_JRKSt7variantIJS7_S8_S9_SA_SB_SC_SD_SE_SF_SG_SH_SI_SJ_SK_SL_SM_SN_SO_SP_SQ_SR_SS_ST_SU_SV_SW_EEEE9_S_vtableE
   0.1%  1.50Ki   0.1%     864    _ZNSt8__detail9__variant12__gen_vtableINS0_20__variant_idx_cookieEOZNS0_17_Move_assign_baseILb0EJN6donner3css5Token5IdentENS6_8FunctionENS6_9AtKeywordENS6_4HashENS6_6StringENS6_9BadStringENS6_3UrlENS6_6BadUrlENS6_5DelimENS6_6NumberENS6_10PercentageENS6_9DimensionENS6_10WhitespaceENS6_3CDOENS6_3CDCENS6_5ColonENS6_9SemicolonENS6_5CommaENS6_13SquareBracketENS6_11ParenthesisENS6_12CurlyBracketENS6_18CloseSquareBracketENS6_16CloseParenthesisENS6_17CloseCurlyBracketENS6_10ErrorTokenENS6_8EofTokenEEEaSEOSX_EUlOT_T0_E_JRSt7variantIJS7_S8_S9_SA_SB_SC_SD_SE_SF_SG_SH_SI_SJ_SK_SL_SM_SN_SO_SP_SQ_SR_SS_ST_SU_SV_SW_EEEE9_S_vtableE
   0.1%  1.50Ki   0.1%     864    _ZNSt8__detail9__variant12__gen_vtableINS0_20__variant_idx_cookieEOZNS0_15_Move_ctor_baseILb0EJN6donner3css5Token5IdentENS6_8FunctionENS6_9AtKeywordENS6_4HashENS6_6StringENS6_9BadStringENS6_3UrlENS6_6BadUrlENS6_5DelimENS6_6NumberENS6_10PercentageENS6_9DimensionENS6_10WhitespaceENS6_3CDOENS6_3CDCENS6_5ColonENS6_9SemicolonENS6_5CommaENS6_13SquareBracketENS6_11ParenthesisENS6_12CurlyBracketENS6_18CloseSquareBracketENS6_16CloseParenthesisENS6_17CloseCurlyBracketENS6_10ErrorTokenENS6_8EofTokenEEEC1EOSX_EUlOT_T0_E_JOSt7variantIJS7_S8_S9_SA_SB_SC_SD_SE_SF_SG_SH_SI_SJ_SK_SL_SM_SN_SO_SP_SQ_SR_SS_ST_SU_SV_SW_EEEE9_S_vtableE
   0.1%  1.44Ki   0.1%     832    _ZNSt8__detail9__variant12__gen_vtableIvOZNS0_16_Variant_storageILb0EJN6donner3css5Token5IdentENS5_8FunctionENS5_9AtKeywordENS5_4HashENS5_6StringENS5_9BadStringENS5_3UrlENS5_6BadUrlENS5_5DelimENS5_6NumberENS5_10PercentageENS5_9DimensionENS5_10WhitespaceENS5_3CDOENS5_3CDCENS5_5ColonENS5_9SemicolonENS5_5CommaENS5_13SquareBracketENS5_11ParenthesisENS5_12CurlyBracketENS5_18CloseSquareBracketENS5_16CloseParenthesisENS5_17CloseCurlyBracketENS5_10ErrorTokenENS5_8EofTokenEEE8_M_resetEvEUlOT_E_JRSt7variantIJS6_S7_S8_S9_SA_SB_SC_SD_SE_SF_SG_SH_SI_SJ_SK_SL_SM_SN_SO_SP_SQ_SR_SS_ST_SU_SV_EEEE9_S_vtableE
 100.0%  2.18Mi 100.0%  1.17Mi    TOTAL
```

![Binary size bar graph](reports/binary-size/binary_size_bargraph.svg)

## Code Coverage
Full report: [coverage-report/index.html](https://jwmcglynn.github.io/donner/reports/coverage/index.html)

```
$ tools/coverage.sh --quiet
Analyzing coverage for: //donner/...
Overall coverage rate:
  lines......: 86.3% (25450 of 29500 lines)
  functions......: 88.4% (5241 of 5926 functions)
  branches......: 77.9% (9984 of 12814 branches)
Coverage report saved to coverage-report/index.html
```

## Tests
```
$ bazel test //donner/...
//donner/base/fonts:woff2_parser_tests                                  SKIPPED
//donner/benchmarks:filter_perf_compare                                 SKIPPED
//donner/benchmarks:render_perf_compare                                 SKIPPED
//donner/svg/renderer/geode:geo_encoder_tests                           SKIPPED
//donner/svg/renderer/geode:geode_device_tests                          SKIPPED
//donner/svg/renderer/geode:geode_shaders_tests                         SKIPPED
//donner/svg/renderer/tests:renderer_ascii_tests                        SKIPPED
//donner/svg/renderer/tests:renderer_geode_golden_tests                 SKIPPED
//donner/svg/renderer/tests:renderer_geode_tests                        SKIPPED
//donner/svg/renderer/tests:text_backend_tests                          SKIPPED
//donner/svg/renderer/tests:text_engine_tests                           SKIPPED
//donner/base:base_lint                                                  PASSED in 0.7s
//donner/base:base_test_utils_lint                                       PASSED in 0.6s
//donner/base:base_tests                                                 PASSED in 16.9s
//donner/base:base_tests_lint                                            PASSED in 0.7s
//donner/base:base_tests_ndebug                                          PASSED in 3.7s
//donner/base:base_tests_ndebug_lint                                     PASSED in 0.7s
//donner/base:base_utils_h_ndebug_lint                                   PASSED in 0.6s
//donner/base:bezier_utils_fuzzer                                        PASSED in 0.2s
//donner/base:bezier_utils_fuzzer_10_seconds                             PASSED in 11.2s
//donner/base:bezier_utils_fuzzer_10_seconds_lint                        PASSED in 3.3s
//donner/base:bezier_utils_fuzzer_lint                                   PASSED in 0.6s
//donner/base:diagnostic_renderer_lint                                   PASSED in 0.6s
//donner/base:failure_signal_handler_lint                                PASSED in 0.6s
//donner/base:path_fuzzer                                                PASSED in 0.2s
//donner/base:path_fuzzer_10_seconds                                     PASSED in 11.2s
//donner/base:path_fuzzer_10_seconds_lint                                PASSED in 0.7s
//donner/base:path_fuzzer_lint                                           PASSED in 0.6s
//donner/base:rcstring_tests_with_exceptions                             PASSED in 0.1s
//donner/base:rcstring_tests_with_exceptions_lint                        PASSED in 0.6s
//donner/base/element:element_lint                                       PASSED in 0.7s
//donner/base/element:element_tests                                      PASSED in 0.1s
//donner/base/element:element_tests_lint                                 PASSED in 0.5s
//donner/base/element:fake_element_lint                                  PASSED in 0.8s
//donner/base/encoding:base64_lint                                       PASSED in 0.6s
//donner/base/encoding:base64_tests                                      PASSED in 0.1s
//donner/base/encoding:base64_tests_lint                                 PASSED in 0.6s
//donner/base/encoding:decompress_fuzzer                                 PASSED in 0.2s
//donner/base/encoding:decompress_fuzzer_10_seconds                      PASSED in 11.2s
//donner/base/encoding:decompress_fuzzer_10_seconds_lint                 PASSED in 0.6s
//donner/base/encoding:decompress_fuzzer_lint                            PASSED in 0.6s
//donner/base/encoding:decompress_lint                                   PASSED in 0.7s
//donner/base/encoding:decompress_tests                                  PASSED in 0.1s
//donner/base/encoding:decompress_tests_lint                             PASSED in 0.7s
//donner/base/encoding:url_decode_lint                                   PASSED in 0.7s
//donner/base/encoding:url_decode_tests                                  PASSED in 0.1s
//donner/base/encoding:url_decode_tests_lint                             PASSED in 0.7s
//donner/base/fonts:fonts_lint                                           PASSED in 0.6s
//donner/base/fonts:fonts_tests                                          PASSED in 0.2s
//donner/base/fonts:fonts_tests_lint                                     PASSED in 3.2s
//donner/base/fonts:woff2_parser_lint                                    PASSED in 0.7s
//donner/base/fonts:woff2_parser_tests_lint                              PASSED in 0.7s
//donner/base/fonts:woff_parser_fuzzer                                   PASSED in 0.2s
//donner/base/fonts:woff_parser_fuzzer_10_seconds                        PASSED in 11.1s
//donner/base/fonts:woff_parser_fuzzer_10_seconds_lint                   PASSED in 0.7s
//donner/base/fonts:woff_parser_fuzzer_lint                              PASSED in 0.6s
//donner/base/parser:line_offsets_lint                                   PASSED in 0.6s
//donner/base/parser:number_parser_fuzzer                                PASSED in 0.2s
//donner/base/parser:number_parser_fuzzer_10_seconds                     PASSED in 11.2s
//donner/base/parser:number_parser_fuzzer_10_seconds_lint                PASSED in 0.6s
//donner/base/parser:number_parser_fuzzer_lint                           PASSED in 0.7s
//donner/base/parser:parser_lint                                         PASSED in 3.3s
//donner/base/parser:parser_tests                                        PASSED in 0.1s
//donner/base/parser:parser_tests_lint                                   PASSED in 0.8s
//donner/base/xml:xml_lint                                               PASSED in 0.7s
//donner/base/xml:xml_parser_fuzzer                                      PASSED in 0.2s
//donner/base/xml:xml_parser_fuzzer_10_seconds                           PASSED in 11.2s
//donner/base/xml:xml_parser_fuzzer_10_seconds_lint                      PASSED in 0.7s
//donner/base/xml:xml_parser_fuzzer_lint                                 PASSED in 0.7s
//donner/base/xml:xml_parser_structured_fuzzer                           PASSED in 0.2s
//donner/base/xml:xml_parser_structured_fuzzer_10_seconds                PASSED in 11.2s
//donner/base/xml:xml_parser_structured_fuzzer_10_seconds_lint           PASSED in 0.6s
//donner/base/xml:xml_parser_structured_fuzzer_lint                      PASSED in 0.6s
//donner/base/xml:xml_qualified_name_lint                                PASSED in 0.7s
//donner/base/xml:xml_tests                                              PASSED in 0.2s
//donner/base/xml:xml_tests_lint                                         PASSED in 0.6s
//donner/base/xml/components:components_lint                             PASSED in 0.6s
//donner/base/xml/components:components_tests                            PASSED in 14.3s
//donner/base/xml/components:components_tests_lint                       PASSED in 0.7s
//donner/benchmarks:css_parse_perf_bench_lint                            PASSED in 0.6s
//donner/benchmarks:skia_filter_perf_bench_lint                          PASSED in 0.5s
//donner/benchmarks:skia_render_perf_bench_lint                          PASSED in 0.6s
//donner/benchmarks:tinyskia_render_perf_bench_lint                      PASSED in 0.7s
//donner/css:core_lint                                                   PASSED in 0.7s
//donner/css:css_lint                                                    PASSED in 0.7s
//donner/css:css_tests                                                   PASSED in 1.0s
//donner/css:css_tests_lint                                              PASSED in 0.7s
//donner/css:selector_test_utils_lint                                    PASSED in 0.7s
//donner/css/parser:anb_microsyntax_parser_fuzzer                        PASSED in 0.2s
//donner/css/parser:anb_microsyntax_parser_fuzzer_10_seconds             PASSED in 11.2s
//donner/css/parser:anb_microsyntax_parser_fuzzer_10_seconds_lint        PASSED in 0.7s
//donner/css/parser:anb_microsyntax_parser_fuzzer_lint                   PASSED in 0.7s
//donner/css/parser:color_parser_fuzzer                                  PASSED in 0.2s
//donner/css/parser:color_parser_fuzzer_10_seconds                       PASSED in 11.2s
//donner/css/parser:color_parser_fuzzer_10_seconds_lint                  PASSED in 0.6s
//donner/css/parser:color_parser_fuzzer_lint                             PASSED in 0.7s
//donner/css/parser:css_parsing_tests                                    PASSED in 0.2s
//donner/css/parser:css_parsing_tests_lint                               PASSED in 0.5s
//donner/css/parser:declaration_list_parser_fuzzer                       PASSED in 0.2s
//donner/css/parser:declaration_list_parser_fuzzer_10_seconds            PASSED in 11.2s
//donner/css/parser:declaration_list_parser_fuzzer_10_seconds_lint       PASSED in 0.6s
//donner/css/parser:declaration_list_parser_fuzzer_lint                  PASSED in 0.6s
//donner/css/parser:parser_lint                                          PASSED in 0.7s
//donner/css/parser:parser_tests                                         PASSED in 0.2s
//donner/css/parser:parser_tests_lint                                    PASSED in 0.7s
//donner/css/parser:selector_parser_fuzzer                               PASSED in 0.2s
//donner/css/parser:selector_parser_fuzzer_10_seconds                    PASSED in 11.2s
//donner/css/parser:selector_parser_fuzzer_10_seconds_lint               PASSED in 0.7s
//donner/css/parser:selector_parser_fuzzer_lint                          PASSED in 0.6s
//donner/css/parser:stylesheet_parser_fuzzer                             PASSED in 0.2s
//donner/css/parser:stylesheet_parser_fuzzer_10_seconds                  PASSED in 11.2s
//donner/css/parser:stylesheet_parser_fuzzer_10_seconds_lint             PASSED in 0.5s
//donner/css/parser:stylesheet_parser_fuzzer_lint                        PASSED in 0.7s
//donner/svg:element_type_lint                                           PASSED in 0.6s
//donner/svg:svg_core_lint                                               PASSED in 0.8s
//donner/svg:svg_document_handle_lint                                    PASSED in 0.6s
//donner/svg:svg_lint                                                    PASSED in 0.6s
//donner/svg/components:components_core_lint                             PASSED in 0.6s
//donner/svg/components:svg_document_context_lint                        PASSED in 0.7s
//donner/svg/components/filter:components_lint                           PASSED in 0.7s
//donner/svg/components/filter:filter_effect_lint                        PASSED in 0.7s
//donner/svg/components/filter:filter_system_lint                        PASSED in 0.7s
//donner/svg/components/filter:filter_system_tests                       PASSED in 0.3s
//donner/svg/components/filter:filter_system_tests_lint                  PASSED in 0.7s
//donner/svg/components/layout:components_lint                           PASSED in 0.7s
//donner/svg/components/layout:layout_system_lint                        PASSED in 0.8s
//donner/svg/components/layout:layout_system_tests                       PASSED in 0.7s
//donner/svg/components/layout:layout_system_tests_lint                  PASSED in 0.6s
//donner/svg/components/paint:components_lint                            PASSED in 0.8s
//donner/svg/components/paint:paint_system_lint                          PASSED in 0.6s
//donner/svg/components/paint:paint_system_tests                         PASSED in 0.6s
//donner/svg/components/paint:paint_system_tests_lint                    PASSED in 0.7s
//donner/svg/components/resources:components_lint                        PASSED in 0.6s
//donner/svg/components/resources:font_resource_lint                     PASSED in 0.7s
//donner/svg/components/resources:resource_manager_context_lint          PASSED in 0.7s
//donner/svg/components/resources/tests:sub_document_cache_tests         PASSED in 0.4s
//donner/svg/components/resources/tests:sub_document_cache_tests_lint    PASSED in 0.6s
//donner/svg/components/shadow:components_lint                           PASSED in 0.7s
//donner/svg/components/shadow:shadow_tree_system_lint                   PASSED in 0.5s
//donner/svg/components/shadow:shadow_tree_system_tests                  PASSED in 0.4s
//donner/svg/components/shadow:shadow_tree_system_tests_lint             PASSED in 0.6s
//donner/svg/components/shape:shape_system_tests                         PASSED in 0.6s
//donner/svg/components/shape:shape_system_tests_lint                    PASSED in 0.6s
//donner/svg/components/style:style_system_tests                         PASSED in 0.5s
//donner/svg/components/style:style_system_tests_lint                    PASSED in 0.6s
//donner/svg/components/text:text_system_tests                           PASSED in 0.6s
//donner/svg/components/text:text_system_tests_lint                      PASSED in 0.6s
//donner/svg/core/tests:core_test_utils_lint                             PASSED in 0.6s
//donner/svg/core/tests:core_tests                                       PASSED in 1.1s
//donner/svg/core/tests:core_tests_lint                                  PASSED in 0.7s
//donner/svg/graph/tests:reference_tests                                 PASSED in 0.1s
//donner/svg/graph/tests:reference_tests_lint                            PASSED in 0.7s
//donner/svg/parser:attribute_parser_lint                                PASSED in 0.7s
//donner/svg/parser:list_parser_fuzzer                                   PASSED in 0.2s
//donner/svg/parser:list_parser_fuzzer_10_seconds                        PASSED in 11.3s
//donner/svg/parser:list_parser_fuzzer_10_seconds_lint                   PASSED in 0.6s
//donner/svg/parser:list_parser_fuzzer_lint                              PASSED in 0.7s
//donner/svg/parser:parser_core_lint                                     PASSED in 0.7s
//donner/svg/parser:parser_details_lint                                  PASSED in 0.7s
//donner/svg/parser:parser_header_lint                                   PASSED in 0.7s
//donner/svg/parser:parser_lint                                          PASSED in 0.8s
//donner/svg/parser:parser_tests                                         PASSED in 0.3s
//donner/svg/parser:parser_tests_lint                                    PASSED in 0.7s
//donner/svg/parser:path_parser_fuzzer                                   PASSED in 0.2s
//donner/svg/parser:path_parser_fuzzer_10_seconds                        PASSED in 11.2s
//donner/svg/parser:path_parser_fuzzer_10_seconds_lint                   PASSED in 0.7s
//donner/svg/parser:path_parser_fuzzer_lint                              PASSED in 0.6s
//donner/svg/parser:svg_parser_fuzzer                                    PASSED in 0.1s
//donner/svg/parser:svg_parser_fuzzer_10_seconds                         PASSED in 11.1s
//donner/svg/parser:svg_parser_fuzzer_10_seconds_lint                    PASSED in 3.1s
//donner/svg/parser:svg_parser_fuzzer_lint                               PASSED in 0.7s
//donner/svg/parser:svg_parser_structured_fuzzer                         PASSED in 0.1s
//donner/svg/parser:svg_parser_structured_fuzzer_10_seconds              PASSED in 11.1s
//donner/svg/parser:svg_parser_structured_fuzzer_10_seconds_lint         PASSED in 0.8s
//donner/svg/parser:svg_parser_structured_fuzzer_lint                    PASSED in 0.5s
//donner/svg/parser:svg_parser_tool_lint                                 PASSED in 0.7s
//donner/svg/parser:transform_parser_fuzzer                              PASSED in 0.2s
//donner/svg/parser:transform_parser_fuzzer_10_seconds                   PASSED in 11.2s
//donner/svg/parser:transform_parser_fuzzer_10_seconds_lint              PASSED in 0.6s
//donner/svg/parser:transform_parser_fuzzer_lint                         PASSED in 0.6s
//donner/svg/properties:properties_lint                                  PASSED in 0.6s
//donner/svg/properties:property_lint                                    PASSED in 0.6s
//donner/svg/properties:property_parsing_lint                            PASSED in 0.6s
//donner/svg/properties/tests:properties_tests                           PASSED in 0.2s
//donner/svg/properties/tests:properties_tests_lint                      PASSED in 0.6s
//donner/svg/properties/tests:property_parsing_tests                     PASSED in 0.5s
//donner/svg/properties/tests:property_parsing_tests_lint                PASSED in 0.7s
//donner/svg/renderer:filter_graph_executor_lint                         PASSED in 0.7s
//donner/svg/renderer:renderer_driver_lint                               PASSED in 0.7s
//donner/svg/renderer:renderer_geode_lint                                PASSED in 0.6s
//donner/svg/renderer:renderer_image_io_lint                             PASSED in 0.7s
//donner/svg/renderer:renderer_interface_lint                            PASSED in 0.6s
//donner/svg/renderer:renderer_skia_lint                                 PASSED in 0.7s
//donner/svg/renderer:renderer_tiny_skia_lint                            PASSED in 0.6s
//donner/svg/renderer:renderer_utils_lint                                PASSED in 0.7s
//donner/svg/renderer:rendering_context_lint                             PASSED in 0.7s
//donner/svg/renderer:stroke_params_lint                                 PASSED in 0.7s
//donner/svg/renderer:terminal_image_viewer_lint                         PASSED in 0.7s
//donner/svg/renderer/geode:geo_encoder_lint                             PASSED in 0.7s
//donner/svg/renderer/geode:geo_encoder_tests_lint                       PASSED in 0.6s
//donner/svg/renderer/geode:geode_device_lint                            PASSED in 0.6s
//donner/svg/renderer/geode:geode_device_tests_lint                      PASSED in 0.6s
//donner/svg/renderer/geode:geode_image_pipeline_lint                    PASSED in 0.6s
//donner/svg/renderer/geode:geode_path_encoder_lint                      PASSED in 0.6s
//donner/svg/renderer/geode:geode_path_encoder_tests                     PASSED in 0.1s
//donner/svg/renderer/geode:geode_path_encoder_tests_lint                PASSED in 0.7s
//donner/svg/renderer/geode:geode_pipeline_lint                          PASSED in 0.8s
//donner/svg/renderer/geode:geode_shaders_lint                           PASSED in 0.6s
//donner/svg/renderer/geode:geode_shaders_tests_lint                     PASSED in 0.7s
//donner/svg/renderer/geode:geode_texture_encoder_lint                   PASSED in 0.6s
//donner/svg/renderer/tests:filter_graph_executor_tests                  PASSED in 0.5s
//donner/svg/renderer/tests:filter_graph_executor_tests_lint             PASSED in 0.7s
//donner/svg/renderer/tests:image_comparison_terminal_preview_tests      PASSED in 0.2s
//donner/svg/renderer/tests:image_comparison_terminal_preview_tests_lint PASSED in 0.6s
//donner/svg/renderer/tests:image_comparison_test_fixture_lint           PASSED in 0.7s
//donner/svg/renderer/tests:renderer_ascii_tests_lint                    PASSED in 3.0s
//donner/svg/renderer/tests:renderer_driver_tests                        PASSED in 0.3s
//donner/svg/renderer/tests:renderer_driver_tests_lint                   PASSED in 0.6s
//donner/svg/renderer/tests:renderer_error_paths_tests                   PASSED in 0.3s
//donner/svg/renderer/tests:renderer_error_paths_tests_lint              PASSED in 0.7s
//donner/svg/renderer/tests:renderer_geode_golden_tests_lint             PASSED in 0.7s
//donner/svg/renderer/tests:renderer_geode_tests_lint                    PASSED in 0.6s
//donner/svg/renderer/tests:renderer_image_test_utils_lint               PASSED in 0.7s
//donner/svg/renderer/tests:renderer_public_api_tests                    PASSED in 0.6s
//donner/svg/renderer/tests:renderer_public_api_tests_impl               PASSED in 0.6s
//donner/svg/renderer/tests:renderer_public_api_tests_impl_lint          PASSED in 0.7s
//donner/svg/renderer/tests:renderer_public_api_tests_skia               PASSED in 2.3s
//donner/svg/renderer/tests:renderer_test_utils_lint                     PASSED in 0.7s
//donner/svg/renderer/tests:renderer_tests                               PASSED in 6.6s
//donner/svg/renderer/tests:renderer_tests_lint                          PASSED in 0.7s
//donner/svg/renderer/tests:resvg_test_suite_impl_lint                   PASSED in 0.7s
//donner/svg/renderer/tests:resvg_test_suite_skia_text                   PASSED in 238.7s
//donner/svg/renderer/tests:resvg_test_suite_skia_text_full              PASSED in 240.8s
//donner/svg/renderer/tests:resvg_test_suite_tiny_skia_text              PASSED in 174.7s
//donner/svg/renderer/tests:resvg_test_suite_tiny_skia_text_full         PASSED in 190.8s
//donner/svg/renderer/tests:terminal_image_viewer_tests                  PASSED in 0.1s
//donner/svg/renderer/tests:terminal_image_viewer_tests_lint             PASSED in 0.6s
//donner/svg/renderer/tests:text_backend_tests_lint                      PASSED in 0.6s
//donner/svg/renderer/tests:text_engine_helpers_tests                    PASSED in 0.3s
//donner/svg/renderer/tests:text_engine_helpers_tests_lint               PASSED in 0.6s
//donner/svg/renderer/tests:text_engine_tests_lint                       PASSED in 0.7s
//donner/svg/resources:font_loader_lint                                  PASSED in 0.7s
//donner/svg/resources:font_manager_lint                                 PASSED in 0.6s
//donner/svg/resources:font_manager_tests                                PASSED in 0.1s
//donner/svg/resources:font_manager_tests_lint                           PASSED in 3.1s
//donner/svg/resources:font_metadata_lint                                PASSED in 0.6s
//donner/svg/resources:image_loader_lint                                 PASSED in 0.7s
//donner/svg/resources:image_resource_lint                               PASSED in 0.7s
//donner/svg/resources:resource_loader_interface_lint                    PASSED in 0.7s
//donner/svg/resources:resource_loader_tests                             PASSED in 1.0s
//donner/svg/resources:resource_loader_tests_lint                        PASSED in 0.6s
//donner/svg/resources:sandboxed_file_resource_loader_lint               PASSED in 0.6s
//donner/svg/resources:url_loader_fuzzer                                 PASSED in 0.2s
//donner/svg/resources:url_loader_fuzzer_10_seconds                      PASSED in 11.2s
//donner/svg/resources:url_loader_fuzzer_10_seconds_lint                 PASSED in 0.7s
//donner/svg/resources:url_loader_fuzzer_lint                            PASSED in 0.7s
//donner/svg/resources:url_loader_lint                                   PASSED in 0.7s
//donner/svg/resources:url_loader_tests                                  PASSED in 0.1s
//donner/svg/resources:url_loader_tests_lint                             PASSED in 0.6s
//donner/svg/tests:parser_test_utils_lint                                PASSED in 0.7s
//donner/svg/tests:svg_tests                                             PASSED in 1.9s
//donner/svg/tests:svg_tests_impl                                        PASSED in 1.9s
//donner/svg/tests:svg_tests_impl_lint                                   PASSED in 0.7s
//donner/svg/tests:svg_tests_skia                                        PASSED in 5.3s
//donner/svg/text:text_backend_full_lint                                 PASSED in 0.6s
//donner/svg/text:text_backend_lint                                      PASSED in 0.7s
//donner/svg/text:text_backend_simple_lint                               PASSED in 0.6s
//donner/svg/text:text_engine_lint                                       PASSED in 0.7s
//donner/svg/text:text_layout_params_lint                                PASSED in 0.6s
//donner/svg/text:text_types_lint                                        PASSED in 0.7s
//donner/svg/tool:donner-svg_lint                                        PASSED in 0.6s
//donner/svg/tool:donner_svg_tool_lib_lint                               PASSED in 0.6s
//donner/svg/tool:donner_svg_tool_tests                                  PASSED in 0.2s
//donner/svg/tool:donner_svg_tool_tests_lint                             PASSED in 3.2s
//donner/svg/tool:donner_svg_tool_utils_lint                             PASSED in 0.6s
//donner/svg/tool:donner_svg_tool_utils_tests                            PASSED in 0.4s
//donner/svg/tool:donner_svg_tool_utils_tests_lint                       PASSED in 0.7s
//donner/svg/renderer/tests:resvg_test_suite                             PASSED in 12.7s
  Stats over 16 runs: max = 12.7s, min = 10.8s, avg = 11.6s, dev = 0.6s
//donner/svg/renderer/tests:resvg_test_suite_impl                        PASSED in 12.7s
  Stats over 16 runs: max = 12.7s, min = 10.8s, avg = 11.6s, dev = 0.6s

Executed 268 out of 279 tests: 268 tests pass and 11 were skipped.
There were tests whose specified size is too big. Use the --test_verbose_timeout_warnings command line option to see which ones these are.
```

## Public Targets
```
$ bazel query 'kind(library, set(//donner/... //:*)) intersect attr(visibility, public, //...)'
//:donner
//donner/base:base
//donner/base:diagnostic_renderer
//donner/base:failure_signal_handler
//donner/css:css
//donner/svg:svg
//donner/svg/parser:parser
//donner/svg/renderer:renderer
//donner/svg/renderer:renderer_driver
//donner/svg/renderer:renderer_geode
//donner/svg/renderer:renderer_interface
//donner/svg/renderer:renderer_tiny_skia
//donner/svg/resources:sandboxed_file_resource_loader
```

## External Dependencies
### Default (tiny-skia)
Generated with: `bazel cquery 'deps(//examples:svg_to_png)'`
Licenses aggregated from: `//third_party/licenses:notice_default` (embed the generated NOTICE.txt for attribution).

- [entt](https://github.com/skypjack/entt) — MIT
- [stb](https://github.com/nothings/stb) — MIT
- [tiny-skia-cpp](https://github.com/jwmcglynn/tiny-skia-cpp) — BSD-3-Clause
- [zlib](https://zlib.net/) — Zlib

### tiny-skia + text-full
Generated with: `bazel cquery 'deps(//examples:svg_to_png)' --config=text-full`
Licenses aggregated from: `//third_party/licenses:notice_text_full` (embed the generated NOTICE.txt for attribution).

- [brotli](https://github.com/google/brotli) — MIT
- [entt](https://github.com/skypjack/entt) — MIT
- [freetype](https://freetype.org/) — FTL
- [harfbuzz](https://github.com/harfbuzz/harfbuzz) — MIT
- [libpng](http://www.libpng.org/pub/png/libpng.html) — libpng-2.0
- [stb](https://github.com/nothings/stb) — MIT
- [tiny-skia-cpp](https://github.com/jwmcglynn/tiny-skia-cpp) — BSD-3-Clause
- [woff2](https://github.com/google/woff2) — MIT
- [zlib](https://zlib.net/) — Zlib

### skia + text-full
Generated with: `bazel cquery 'deps(//examples:svg_to_png)' --config=skia --config=text-full`
Licenses aggregated from: `//third_party/licenses:notice_skia_text_full` (embed the generated NOTICE.txt for attribution).

- [brotli](https://github.com/google/brotli) — MIT
- [entt](https://github.com/skypjack/entt) — MIT
- [freetype](https://freetype.org/) — FTL
- [harfbuzz](https://github.com/harfbuzz/harfbuzz) — MIT
- [libpng](http://www.libpng.org/pub/png/libpng.html) — libpng-2.0
- [skia](https://skia.org/) — BSD-3-Clause
- [stb](https://github.com/nothings/stb) — MIT
- [woff2](https://github.com/google/woff2) — MIT
- [zlib](https://zlib.net/) — Zlib
