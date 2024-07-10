#!/bin/bash
#
# Usage: ./doxygen_filter_mermaid.sh <md-file>
# 
# Replaces markdown-style mermaid code blocks with `<pre class="mermaid">...</pre>`.
#
# Invoke from Doxygen with the following Doxyfile config:
#
#   FILTER_PATTERNS = *.md=tools/support/doxygen_filter_mermaid.sh
#
sed -E '# Match a range starting with ```mermaid and ending with ```
/^```mermaid$/,/^```$/ {
  # Replace the ```mermaid part with a <pre> tag
  /^```mermaid$/ {
    s/^```mermaid/<pre class="mermaid">/
    # Move to next line line
    n
  }
  # Replace the end marker with </pre>
  /^```$/ {
    s/^```/<\/pre>\n/
    # Move to next line
    n
  }
}' "${1}"
