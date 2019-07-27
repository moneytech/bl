#!/bin/bash

echo Running blc setup...
realpath() {
    OURPWD=$PWD
    cd "$(dirname "$1")"
    LINK=$(readlink "$(basename "$1")")
    while [ "$LINK" ]; do
      cd "$(dirname "$LINK")"
      LINK=$(readlink "$(basename "$1")")
    done
    REALPATH="$PWD/$(basename "$1")"
    cd "$OURPWD"
    echo "$REALPATH"
}

WDIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"
echo working directory: $WDIR
cd $WDIR

CONFIG_FILE="../etc/bl.conf"
STATUS=0

echo "- Looking for bl APIs"
LIB_DIR="../lib/bl/api"
if [ -d "$LIB_DIR" ]; then
    LIB_DIR=$(realpath $LIB_DIR)
    echo "  FOUND - $LIB_DIR"
else
    echo "  error: Cannot find bl APIs. You can try to set correct path manually in etc/bl.conf file."
    $STATUS=1
fi

echo "- Looking for ld"
LINKER_EXEC=$(which ld run)
if [ -z "$LINKER_EXEC" ]; then
    echo "  error: Cannot find ld linker. You can try to set correct path manually in etc/bl.conf file."
    $STATUS=1
else
    echo "  FOUND - $LINKER_EXEC"
fi

echo "- Looking for C runtime objects"
BIN_DIR="/usr/bin"
if [ -d "$BIN_DIR" ]; then
    echo "  FOUND - $BIN_DIR"
else
    echo "  error: Cannot find '$BIN_DIR'. You can try to set correct path manually in etc/bl.conf file."
    $STATUS=1
fi

CLIB_DIR="/usr/lib/x86_64-linux-gnu"
if [ -d "$CLIB_DIR" ]; then
    echo "  FOUND - $CLIB_DIR"
else
    echo "  error: Cannot find '$CLIB_DIR'. You can try to set correct path manually in etc/bl.conf file."
    $STATUS=1
fi

CRT1_O="/usr/lib/x86_64-linux-gnu/crt1.o"
if [ -e "$CRT1_O" ]; then
    echo "  FOUND - $CRT1_O"
else
    echo "  error: Cannot find '$CRT1_O'. You can try to set correct path manually in etc/bl.conf file."
    $STATUS=1
fi

CRTI_O="/usr/lib/x86_64-linux-gnu/crti.o"
if [ -e "$CRTI_O" ]; then
    echo "  FOUND - $CRTI_O"
else
    echo "  error: Cannot find '$CRTI_O'. You can try to set correct path manually in etc/bl.conf file."
    $STATUS=1
fi

CRTN_O="/usr/lib/x86_64-linux-gnu/crtn.o"
if [ -e "$CRTN_O" ]; then
    echo "  FOUND - $CRTN_O"
else
    echo "  error: Cannot find '$CRTN_O'. You can try to set correct path manually in etc/bl.conf file."
    $STATUS=1
fi

LDLIB="/lib64/ld-linux-x86-64.so.2"
if [ -e "$LDLIB" ]; then
    echo "  FOUND - $LDLIB"
else
    echo "  error: Cannot find '$LDLIB'. You can try to set correct path manually in etc/bl.conf file."
    $STATUS=1
fi

LINKER_OPT="$LDLIB $CRT1_O $CRTI_O $CRTN_O -L$CLIB_DIR -L$BIN_DIR -lc -lm"

rm -f $CONFIG_FILE
mkdir -p ../etc
printf "/*\n * blc config file\n */\n\n" >> $CONFIG_FILE
echo LIB_DIR \"$LIB_DIR\" >> $CONFIG_FILE
echo LINKER_EXEC \"$LINKER_EXEC\" >> $CONFIG_FILE
echo LINKER_OPT \"$LINKER_OPT\" >> $CONFIG_FILE

if [ $STATUS -eq 0 ]; then
    CONFIG_FILE=$(realpath $CONFIG_FILE)
    echo Configuration finnished without errors and written to $CONFIG_FILE file.
else
    echo Configuration finnished with errors.
fi


exit $STATUS
