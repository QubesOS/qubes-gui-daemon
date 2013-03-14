if [ -O /tmp/qubes-session-env ]; then
while read LINE; do
	TMP=${LINE%%=*}
	[ "x${TMP}" != "x" ] && export "$LINE"
done < /tmp/qubes-session-env
fi
