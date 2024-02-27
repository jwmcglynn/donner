#!/bin/bash
#
# Usage: ./doxygen_filter_mermaid.sh <md-file>
# 
# Replaces markdown-style mermaid code blocks with `<pre class="mermaid>...</pre>`.
#
# Invoke from Doxygen with the following Doxyfile config:
#
#   FILTER_PATTERNS = *.md=tools/support/doxygen_filter_mermaid.sh
#
sed -E '/^```mermaid$/,/^```$/ {
/^```mermaid$/ {
    s/^```mermaid/<pre class="mermaid">/
    n
}
/^```$/ {
    s/^```/<\/pre>/
    n
}
}' "${1}"
