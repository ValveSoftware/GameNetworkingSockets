#!/bin/sh

if [ "$IMAGE" = "alpine" ]
then
	apk update
	apk add bash
fi

exit 0
