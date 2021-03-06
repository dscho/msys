#!/bin/sh
#
# apropos -- search the whatis database for keywords.
# whatis  -- idem, but match only commands (as whole words).
#
# Copyright (c) 1990, 1991, John W. Eaton.
# Copyright (c) 1994-1999, Andries E. Brouwer.
#
# You may distribute under the terms of the GNU General Public
# License as specified in the README file that comes with the man
# distribution.  
#
# apropos/whatis-1.5m aeb 2003-08-01 (from %version%)
#
# keep old PATH - 000323 - Bryan Henderson
# also look in /var/cache/man - 030801 - aeb

program=`basename $0`

# When man pages in your favorite locale look to grep like binary files
# (and you use GNU grep) you may want to add the 'a' option to *grepopt1.
aproposgrepopt1='i'
aproposgrepopt2=''
whatisgrepopt1='iw'
whatisgrepopt2='^'
grepopt1=$%apropos_or_whatis%grepopt1
grepopt2=$%apropos_or_whatis%grepopt2

if [ $# = 0 ]
then
    echo "usage: $program keyword ..."
    exit 1
fi

# Most systems use a colon as the PATH separator character, but some,
# (notably Microsoft's), insist on a semicolon.  Also, some users insist
# on having spaces within the directory names in their PATH;  (a bad habit
# promoted by Microsoft).  The following `sed' transformations will allow
# us to handle them gracefully.

path_transform='s?%?%25?g;s? ?%20?g;s?%path_separator_char%? ?g'
path_reverse_transform='s?%20? ?g;s?%25?%?g'

manpath=`man --path | sed "$path_transform"`

if [ "$manpath" = "" ]
then
    echo "$program: manpath is null"
    exit 1
fi

args=
for arg in $*; do
    case $arg in
        --version|-V|-v)
	    echo "$program from %version%"
	    exit 0
	    ;;
	--help|-h)
            echo "usage: $program keyword ..."
	    exit 0
	    ;;
	-*)
	    echo "$program: $arg: unknown option"
	    exit 1
	    ;;
	*)
	    args="$args $arg"
    esac
done

while [ "$1" != "" ]
do
    found=0
    for d in /var/cache/man $manpath /usr/lib
    do
        d=`echo $d | sed "$path_reverse_transform"`
        if [ -f "$d/whatis" ]
        then
            if grep -"$grepopt1" "$grepopt2""$1" "$d/whatis"
            then
                found=1
# Some people are satisfied with a single occurrence
# But it is better to give all
#               break
            fi
        fi
    done

    if [ $found = 0 ]
    then
        echo "$1: nothing appropriate"
    fi

    shift
done

exit
