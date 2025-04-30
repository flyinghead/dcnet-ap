#!/bin/sh
# dependencies: git-buildpackage debhelper
gbp buildpackage --git-ignore-new -uc -us
