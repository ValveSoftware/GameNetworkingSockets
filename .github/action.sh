#!/bin/bash

set -e
set -o pipefail

ACTION="$(basename "$0")"
ACTION="${ACTION%.sh}"

SCRIPT_CANDIDATES=(
	"$PWD/.github/${ACTION}/${IMAGE//\//-}.sh"
	"$PWD/.github/${ACTION}/${IMAGE%/*}.sh"
)

for SCRIPT in "${SCRIPT_CANDIDATES[@]}"; do
	if [[ -f "${SCRIPT}" ]]; then
		bash "${SCRIPT}" | cat
		exit $?
	fi
done

echo "Could not find script in any of these paths:"
for SCRIPT in "${SCRIPT_CANDIDATES[@]}"; do
	echo "   ${SCRIPT}"
done
echo ""
echo "Don't know how to do step '${ACTION}' for ${IMAGE}!"
echo ""
exit 1
