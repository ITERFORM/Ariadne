#!/bin/bash

# Script to generate API documentation from code comments using Doxygen
#
# Layout (migrated 2026-11-19): doxygen writes the reference into
#   docs/api_reference/api/   (HTML_OUTPUT=api, config = Doxyfile at the repo root)
# and the hand-written landing page stays at
#   docs/api_reference/index.html
# which is ABOVE the output, so generation and hand-written copy can never
# overwrite each other. The old pre-migration layout was docs/api_reference/html/
# with the landing written INTO the output directory.

set -e  # Exit immediately if a command exits with a non-zero status

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}Generating API documentation with Doxygen...${NC}"

# Check if doxygen is installed
if ! command -v doxygen &> /dev/null; then
    echo -e "${RED}Error: doxygen is not installed${NC}" >&2
    echo "Please install doxygen to generate documentation:"
    echo "  Ubuntu/Debian: sudo apt-get install doxygen"
    echo "  macOS: brew install doxygen"
    echo "  CentOS/RHEL: sudo yum install doxygen"
    exit 1
fi

# Go to project root directory where Doxyfile is located
cd "$(dirname "$0")/.."

mkdir -p docs/api_reference

# Generate documentation using the Doxyfile
echo -e "${YELLOW}Running doxygen to generate API documentation...${NC}"
doxygen Doxyfile 2>&1 | tee docs/api_reference/build.log

# Doxygen 1.18 links to two doc-group index pages it never emits: "functions__.html"
# and "functions_func__.html" are the empty-name group, i.e. the undocumented
# members, and the intended target is always the plain group index. Measured on the
# 2026-11-19 build: 52 of 73,229 local refs pointed at those two names. Emit the
# alias as a meta redirect so the reference carries no dead links; only these two
# files are written, no generated page is rewritten.
alias() {  # alias <missing.html> <target.html>
  local f="docs/api_reference/api/$1"
  [ -f "$f" ] || cat > "$f" <<EOF
<!DOCTYPE html>
<html><head><meta http-equiv="refresh" content="0; url=$2"></head>
<body><p>Group index moved to <a href="$2">$2</a>.</p></body></html>
EOF
}
alias functions__.html      functions_func.html
alias functions_func__.html functions_func.html

# Drop the pre-migration layout if it is still on disk.
if [ -d "docs/api_reference/html" ]; then
    echo -e "${YELLOW}Removing the pre-migration docs/api_reference/html/ layout...${NC}"
    rm -rf docs/api_reference/html
fi

# Check if documentation was generated successfully (new layout: api/ + landing)
GENERATED="docs/api_reference/api/index.html"   # doxygen's own main page
LANDING="docs/api_reference/index.html"         # hand-written site landing

if [ -f "${GENERATED}" ] && [ -s "${GENERATED}" ] && [ -f "${LANDING}" ]; then
    PAGES=$(find docs/api_reference/api -name '*.html' | wc -l | tr -d ' ')
    WARNINGS=$(grep -ci warning docs/api_reference/build.log || true)
    echo -e "${GREEN}✅ API documentation generated successfully (${PAGES} pages, ${WARNINGS} doxygen warnings)!${NC}"
    echo -e "${GREEN}📁 Landing page:   docs/api_reference/index.html${NC}"
    echo -e "${GREEN}📁 API reference:  docs/api_reference/api/${NC}"
    echo ""
    echo -e "${BLUE}Generated documentation includes:${NC}"
    echo "  • Symbol pages for everything in include/, src/, tools/, bench/"
    echo "  • Class hierarchy and alphabetical symbol index"
    echo "  • In-page client-side search (searchdata.js ships with the output; no php)"
    echo "  • Site dark theme via docs/api_reference/assets/ariadne-docs.css (HTML_EXTRA_STYLESHEET)"
    echo ""
    echo -e "${BLUE}To view it, serve docs/ statically and open the landing:${NC}"
    echo "  docs/api_reference/index.html"
    echo "  python3 -m http.server 8080 --directory docs   # then http://localhost:8080/api_reference/"
else
    [ -f "${GENERATED}" ] || echo -e "${RED}❌ ${GENERATED} is missing${NC}" >&2
    [ -f "${LANDING}" ] || echo -e "${RED}❌ Landing page ${LANDING} is missing; the site route needs it${NC}" >&2
    exit 1
fi
