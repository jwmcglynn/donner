# Donner build report

## Lines of code
```
$ tools/cloc.sh
Lines of source code:   24.3k
Lines of comments:       6.9k
Comment Percentage:     28.2%
```

## Binary size
```
$ tools/binary_size.sh
Total binary size of xml_tool
1.5M	build-binary-size/xml_tool

Saved report to build-binary-size/binary_size_report.html

Top compile units by size:

    FILE SIZE        VM SIZE    
 --------------  -------------- 
  78.0%  1.14Mi  77.8%  1.14Mi    src
    43.0%   502Ki  43.0%   502Ki    [63 Others]
    11.2%   130Ki  11.2%   130Ki    src/svg/svg_circle_element.cc
    10.7%   125Ki  10.7%   125Ki    src/svg/svg_element.cc
     8.9%   103Ki   8.9%   103Ki    src/svg/components/computed_style_component.cc
     5.1%  59.5Ki   5.1%  59.5Ki    src/css/parser/color_parser.cc
     4.5%  52.9Ki   4.5%  52.9Ki    src/svg/svg_pattern_element.cc
     4.0%  46.3Ki   4.0%  46.3Ki    src/svg/xml/xml_parser.cc
     3.6%  42.0Ki   3.6%  42.0Ki    src/css/component_value.cc
     3.4%  40.0Ki   3.4%  40.0Ki    src/css/parser/selector_parser.cc
     3.3%  38.1Ki   3.3%  38.1Ki    src/svg/properties/property_registry.cc
     2.4%  28.6Ki   2.4%  28.6Ki    src/svg/xml/xml_tool.cc
  14.0%   210Ki  14.2%   213Ki    [__LINKEDIT]
   3.6%  53.4Ki   3.5%  53.4Ki    third_party/abseil
    41.4%  22.1Ki  41.4%  22.1Ki    external/abseil-cpp~/absl/strings/internal/charconv_parse.cc
    29.2%  15.6Ki  29.2%  15.6Ki    external/abseil-cpp~/absl/strings/internal/charconv_bigint.cc
    29.1%  15.5Ki  29.1%  15.5Ki    external/abseil-cpp~/absl/strings/charconv.cc
     0.3%     170   0.3%     170    external/abseil-cpp~/absl/strings/internal/memutil.cc
   0.9%  14.0Ki   0.9%  14.0Ki    [__DATA]
   0.9%  13.6Ki   0.9%  13.6Ki    [__DATA_CONST,__const]
   0.9%  12.9Ki   0.9%  12.9Ki    [__TEXT,__cstring]
   0.7%  10.0Ki   0.7%  10.2Ki    [__TEXT]
   0.5%  7.20Ki   0.5%  7.20Ki    [__DATA_CONST]
   0.3%  4.20Ki   0.3%  4.10Ki    [6 Others]
   0.2%  2.57Ki   0.2%  2.57Ki    [__TEXT,__text]
   0.1%  2.18Ki   0.1%  2.18Ki    [__TEXT,__gcc_except_tab]
 100.0%  1.47Mi 100.0%  1.47Mi    TOTAL
```

## Code coverage
```
$ tools/coverage.sh --quiet
Analyzing coverage for: //src/...
Overall coverage rate:
  lines......: 90.7% (16726 of 18439 lines)
  functions......: 87.9% (3057 of 3476 functions)
  branches......: 67.5% (4173 of 6182 branches)
Coverage report saved to coverage-report/index.html
```

