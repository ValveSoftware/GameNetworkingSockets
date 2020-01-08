#!/bin/sh

if grep -q "Alpine Linux" /etc/os-release &>/dev/null; then
	apk update
	apk add bash
fi

exit 0
