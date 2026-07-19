#!/bin/bash

show() {
	pushd "$1" >> /dev/null
	HASH=$(git rev-parse --short=8 HEAD)
	NAME=$(basename $PWD)
	DATE=$(git log -1 --pretty='%ad' --date=format:'%Y-%m-%d')
	REPO=$(git config --get remote.origin.url)
	REPO=$(sed -E "s,(^git@github.com:)|(^https?://github.com/)|(.git$)|(/$),,g" <<<"$REPO")
	popd >> /dev/null

	printf '\055 %-24s%-10s%-12s%s\n' $NAME $HASH $DATE $REPO
}
list() {
	pushd "$1" >> /dev/null
	for D in ./*; do
		show "$D"
	done
	popd >> /dev/null
}
rule() {
	echo '----------------------------------------------------------------'	
}
tell() {
	echo $1
	rule
}

bump() {
	printf '\n'
}

{
	printf '%-26s%-10s%-12s%s\n' DED-OS HASH DATE REPO
	rule
	show ./
	bump
	
	tell LIBRETRO
	show ./source/ui/libretro-common
	bump
	
	tell CORES
	list ./source/cores/src
	bump
	
	tell COMPARE
	echo https://github.com/REPO/compare/HASH...HEAD
	bump
} | sed 's/\n/ /g'