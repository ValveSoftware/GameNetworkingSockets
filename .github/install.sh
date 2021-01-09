#!/bin/bash

INSTALL_SCRIPT_CANDIDATES=(
	"$PWD/.travis/install-${IMAGE//\//-}.sh"
	"$PWD/.travis/install-${IMAGE%/*}.sh"
)

for INSTALL_SCRIPT in "${INSTALL_SCRIPT_CANDIDATES[@]}"; do
	if [[ -f "${INSTALL_SCRIPT}" ]]; then
		bash "${INSTALL_SCRIPT}"
		exit $?
	fi
done

echo "Could not find install script in any of these paths:"
for INSTALL_SCRIPT in "${INSTALL_SCRIPT_CANDIDATES[@]}"; do
	echo "   ${INSTALL_SCRIPT}"
done
echo ""
echo "Don't know how to install packages for ${IMAGE}!"
echo ""
exit 1
