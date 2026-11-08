#!/bin/sh
# Regenerate 0001-cezanne-fork.patch from the dev tree apps/snapshot (the
# upstream tag plus working-tree changes). New files have to be known to git
# first: git -C apps/snapshot add -N <file>
cd "$(dirname "$0")" || exit 1
git -C ../../../apps/snapshot diff > 0001-cezanne-fork.patch &&
	echo "$(grep -c '^diff --git' 0001-cezanne-fork.patch) files in 0001-cezanne-fork.patch"
