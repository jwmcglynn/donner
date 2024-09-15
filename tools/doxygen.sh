#!/bin/bash -e
cd "${0%/*}/.."

doxygen Doxyfile

echo "Documentation generated in generated-doxygen/html/index.html"
 