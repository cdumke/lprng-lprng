#
# -- START --
# preremove.solaris.sh,v 1.1 2001/08/21 20:33:17 root Exp
#
# This is the shell script that does the preremove
echo RUNNING preremove.solaris.sh
echo "Stopping LPD"
kill -INT `ps ${PSHOWALL} | awk '/lpd/{ print $1;}'` >/dev/null 2>&1
exit 0
