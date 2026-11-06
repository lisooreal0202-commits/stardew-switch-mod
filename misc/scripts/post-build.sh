#!/bin/bash
set -e

OUT_NSO=${OUT}/${BINARY_NAME}
OUT_NPDM=${OUT}/main.npdm

# Clear older build.
# 2026-10-27: the output directory used to be `deploy/`, which was also an IDE
# workspace root on this machine, so the directory itself could not be removed
# ("Device or resource busy") even though its contents could.  That made
# `rm -rf` delete the payload, then `mkdir` fail with "File exists", and
# `set -e` aborted the whole build -- leaving it empty.  2026-11-06: renamed to
# out-dev/ and out-release/, which nothing else holds open.  Tolerate both
# anyway: contents are cleared, and mkdir -p succeeds either way.
rm -rf ${OUT} || true

# Create out directory.
mkdir -p ${OUT}

# Copy build into out
mv ${NAME}.nso ${OUT_NSO}
mv ${NAME}.npdm ${OUT_NPDM}

# Copy ELF to user path if defined.
if [ ! -z $ELF_EXTRACT ]; then
    cp "$NAME.elf" "$ELF_EXTRACT"
fi

# Release builds leave a marker carrying their own md5.
#
# The packaging step refuses to run without it, which is what stops a dev build
# (diagnostics on) from being shipped by accident: the two are built from the
# same source and are otherwise indistinguishable once copied around.
if [ ! -z $SDV_RELEASE ]; then
    # BINARY_NAME carries a trailing space -- the Makefile sets it as
    # `subsdk8 # comment`, so the space before the # is part of the value.
    # Unquoted echo collapses it; quoting the name fails on the space.
    _bin=$(echo ${BINARY_NAME})
    ( cd "${OUT}" && md5sum "${_bin}" | cut -d' ' -f1 ) > "${OUT}/RELEASE-BUILD"
fi
