if [ -O /tmp/qubes-session-env ]; then
while read LINE; do
	TMP=${LINE%%=*}
	[ -z ${!TMP} ] && export $LINE
done < /tmp/qubes-session-env
fi
