#!/bin/bash
# build-kmod.sh - build kmod-xsight for the running kernel from the
# kernel-rpmtree spec and its patches, on this host.  It only builds: it
# installs nothing and loads nothing.
#
# kernel-rpmtree builds kmods in a container (build-kernel-in-ubi.sh) and
# tardis1 has no container engine, so this is rpmbuild with the macros that
# script passes.  It is how kmod-64k-xsight 2.0.1-4 (the RX refill retry
# patch) was built and tested on 2026-10-08.
#
# Needs rpm-build (installed on tardis1 that day) and the running kernel's
# -devel and -modules-core packages.
RPMTREE=${RPMTREE:-/home/serviceadmin/snitm/kernel/kernel-rpmtree}
KMODVER=${KMODVER:-2.0.1}
REL=$(uname -r)
case $REL in
*+64k) PG=64k ;;
*+16k) PG=16k ;;
*)     PG= ;;
esac
KV=${REL%+*}				# rpm's kversion has no +64k / +16k
T=${T:-/var/tmp/zc-run-logs/kmod-xsight-${PG:-4k}-build}
command -v rpmbuild > /dev/null || { echo "no rpmbuild: dnf install rpm-build"; exit 1; }
rpm -q kernel${PG:+-$PG}-devel-$KV kernel${PG:+-$PG}-modules-core-$KV || { echo "the running kernel's -devel and -modules-core must be installed"; exit 1; }
git -C $RPMTREE log --oneline -1 -- xsight
grep -E '^(Release|Patch)' $RPMTREE/xsight/xsight.spec | head -8

rm -rf $T; mkdir -p $T/{SPECS,SOURCES,BUILD,RPMS,SRPMS,BUILDROOT}
cp $RPMTREE/xsight/xsight.spec $T/SPECS/
for f in $RPMTREE/xsight/*; do case $f in *.spec) ;; *) cp $f $T/SOURCES/ ;; esac; done
A=(--define "_topdir $T" --define "kversion $KV" --define "kvermaj $(cut -d. -f1,2 <<< $KV)"
   --define "buildnumber 1" --define "kmodver $KMODVER")
[ -n "$PG" ] && A+=(--with "kernel$PG")
url=$(rpmspec -P "${A[@]}" $T/SPECS/xsight.spec 2>/dev/null | awk '/^Source0?:/{print $2; exit}')
echo "Source0 -> $url"
curl -sSfL -o $T/SOURCES/$(basename $url) "$url" || { echo "cannot fetch the tarball"; exit 1; }
rpmbuild -bb "${A[@]}" $T/SPECS/xsight.spec > $T/build.log 2>&1 || { echo "rpmbuild failed:"; tail -20 $T/build.log; exit 1; }

R=$(find $T/RPMS -name 'kmod-*xsight-[0-9]*.rpm' ! -name '*debuginfo*')
echo "built: $R"
# Verify by the artifact, not by the exit status.
mkdir -p $T/x && (cd $T/x && rpm2cpio $R | cpio -idm --quiet)
K=$(find $T/x -name xeu.ko)
echo "vermagic: $(modinfo -F vermagic $K)   (running: $REL)"
echo "retry patch in xeu.ko: $(nm $K | grep -c xeu_rx_refill_timer)   rx_ip_align parameter: $(modinfo $K | grep -c 'parm: *rx_ip_align')"
rpm -qp --requires $R | grep -i kernel
echo "loaded now: srcversion $(cat /sys/module/xeu/srcversion), retry patch $(grep -c xeu_rx_refill_timer /proc/kallsyms); built: srcversion $(modinfo -F srcversion $K)"
