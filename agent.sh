#!/bin/bash
#
#  Doozer
#  Copyright (C) 2011 Andreas Öman
#
#  This program is free software: you can redistribute it and/or modify
#  it under the terms of the GNU General Public License as published by
#  the Free Software Foundation, either version 3 of the License, or
#  (at your option) any later version.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program.  If not, see <http://www.gnu.org/licenses/>.
#

set -u


BUILDMASTER=""
AGENT_ID=""
AGENT_SECRET=""
TARGETS=""
BUILDROOT=/var/tmp/buildagent
JARGS=""
JOBSARGS=""
AUTOBUILD_CONFIGURE_EXTRA=""
CMDUSER=`whoami`
TESTMODE="0"
BTRFSROOT=""
FSROOT=""

usage()
{
cat << EOF
usage: $0 OPTIONS

Build agent for the doozer build master

OPTIONS:
   -h      Show this message
   -m      host:port to buildmaster (required)
   -a      Agent ID (required)
   -s      Agent secret (required)
   -t      Comma separated list of targets,
              like "foo,bar,baz" (no spaces) (required)
   -r      Path where to build [${BUILDROOT}]
   -j      Maximum number of jobs for make
   -T      Testmode
   -u      Run commands as user [<unset>]. Required if running as root
   -r      Points to root filesystems
   -b      BTRFS subvolume root
EOF
}



while getopts "m:a:s:t:r:j:hTu:b:r:" OPTION
do
  case $OPTION in
      m)
	  BUILDMASTER="$OPTARG"
	  ;;
      a)
	  AGENT_ID="$OPTARG"
	  ;;
      s)
	  AGENT_SECRET="$OPTARG"
	  ;;
      t)
	  TARGETS="$OPTARG"
	  ;;
      r)
	  BUILDROOT="$OPTARG"
	  ;;
      j)
	  JOBSARGS="--jobs=$OPTARG"
	  JARGS="-j$OPTARG"
	  ;;
      T)
	  TESTMODE=1
	  ;;
      u)
	  CMDUSER="$OPTARG"
	  ;;
      b)
	  BTRFSROOT="$OPTARG"
	  ;;
      r)
	  FSROOT="$OPTARG"
	  ;;
      h)
	  usage
	  exit 0
  esac
done

export AUTOBUILD_CONFIGURE_EXTRA

if [[ $EUID -eq 0 ]] && [[ -z "$CMDUSER" ]]; then
    echo "Running as root without -u is not supported"
    usage
    exit 1
fi

if [[ -z $TESTMODE ]]; then
    if [[ -z $BUILDMASTER ]] || [[ -z $AGENT_ID ]] || [[ -z $AGENT_SECRET ]] || [[ -z $TARGETS ]]; then
	echo "Required option missing"
	usage
	exit 1
    fi
else
    if [[ -z $TARGETS ]]; then
	echo "Required option missing"
	usage
	exit 1
    fi
fi

if [[ -n $BTRFSROOT ]]; then
    which btrfs >/dev/null
    if [ $? -ne 0 ]; then
	echo "btrfs command not available"
	exit 1
    fi
fi

CMDUID=`id -u $CMDUSER`
CMDGID=`id -g $CMDUSER`
USERSPEC="${CMDUID}:${CMDGID}"


send_status() {
    echo "Sending status $1 -- $2"
    [ ${TESTMODE} -eq 1 ] && return

    local msg=`echo $2 | tr " " "+"`
    while ! curl -s "http://${BUILDMASTER}/buildmaster/report?jobid=${JOB_id}&jobsecret=${JOB_jobsecret}&status=$1&msg=${msg}" ; do
	echo "Curl failed with error $? during reporting"
	sleep 3
    done
}

send_artifact() {
    echo "Sending attachment '$1' type=$2 content-type=$3"
    [ ${TESTMODE} -eq 1 ] && return
    md5=`md5sum <$4 | awk '{print $1}'`
    sha1=`sha1sum <$4 | awk '{print $1}'`
    local msg=`echo $1 | tr " " "+"`
    while ! curl -L -X PUT -v --data-binary @$4 -H "Content-Type: $3" "http://${BUILDMASTER}/buildmaster/artifact?jobid=${JOB_id}&jobsecret=${JOB_jobsecret}&name=${msg}&type=$2&md5sum=${md5}&sha1sum=${sha1}" ; do
	echo "Curl failed with error $? when sending attachment: $1"
	sleep 3
    done
}

send_artifact_gzip() {
    echo "Sending attachment '$1' type=$2 content-type=$3 (gzip:ed)"
    [ ${TESTMODE} -eq 1 ] && return
    ZFILE=`mktemp`
    gzip -9 >${ZFILE} <"$4"
    md5=`md5sum <$4 | awk '{print $1}'`
    sha1=`sha1sum <$4 | awk '{print $1}'`
    local msg=`echo $1 | tr " " "+"`
    while ! curl -L -X PUT -v --data-binary @${ZFILE} -H "Content-Type: $3" -H "Content-Encoding: gzip" "http://${BUILDMASTER}/buildmaster/artifact?jobid=${JOB_id}&jobsecret=${JOB_jobsecret}&name=${msg}&type=$2&md5sum=${md5}&sha1sum=${sha1}" ; do
	echo "Curl failed with error $? when sending attachment: $1"
	sleep 3
    done
    rm -f "${ZFILE}"
}

build_fail() {
    send_status failed "$1"
}

build_status() {
    send_status building "$1"
}


write_trampoline_v2() {
    cat >${TRAMPOLINE} <<EOF
#!/bin/bash
set -eu
cd "${CHECKOUTPATH}"
AUTOBUILD_CONFIGURE_EXTRA="${AUTOBUILD_CONFIGURE_EXTRA}"
export AUTOBUILD_CONFIGURE_EXTRA
./Autobuild.sh ${AUTOBUILD_ARGS}
EOF
    chmod 755 ${TRAMPOLINE}
}



write_trampoline_v3() {
    cat >${TRAMPOLINE} <<EOF
#!/bin/bash
set -eu
cd "${CHECKOUTPATH}"
AUTOBUILD_CONFIGURE_EXTRA="${AUTOBUILD_CONFIGURE_EXTRA}"
export AUTOBUILD_CONFIGURE_EXTRA
./Autobuild.sh ${AUTOBUILD_ARGS} -o $1
EOF
    chmod 755 ${TRAMPOLINE}
}


write_deb_trampoline() {
    cat >${TRAMPOLINE} <<EOF
#!/bin/bash
set -eu
artifact() {
    echo "doozer-artifact:\$PWD/\$1:\$2:\$3:\$4"
}


cd "${CHECKOUTPATH}"

BUILD_DEPS=\`awk 'BEGIN {cnt = 1;} /^Build-Depends:/ {split(\$0, line, ":");split(line[2], deps, ",");for (i in deps) {d = deps[i];sub(/^ */, "", d);sub(/ *\$/, "", d);split(d, tokens, " ");packages[cnt] = tokens[1];cnt++;}} END {out = ""; for(i = 1; i <= cnt; i++) {out = out packages[i] " ";} print out; }' debian/control\`
CHANGELOG=debian/changelog
NOW=\`date -R\`
VER=\`git describe | sed "s/\([0-9]*\)\.\([0-9]*\)-\([0-9]*\)-.*/\1.\2.\3/"\`

build() 
{
    echo >\${CHANGELOG} "${JOB_project} (\${VER}) unstable; urgency=low"
    echo >>\${CHANGELOG}
    echo >>\${CHANGELOG} "  * The full changelog can be found at "
    echo >>\${CHANGELOG} "    http://www.lonelycoder.com/showtime/download"
    echo >>\${CHANGELOG}
    echo >>\${CHANGELOG} " -- Andreas Öman <andreas@lonelycoder.com>  \${NOW}"
    
    export JOBSARGS
    export JARGS
    export AUTOBUILD_CONFIGURE_EXTRA
    dpkg-buildpackage -b -us -uc

    for a in ../${JOB_project}*\${VER}*.deb; do
	artifact "\$a" deb application/x-deb \`basename \$a\`
    done

    for a in ../${JOB_project}*\${VER}*.changes; do
	artifact "\$a" changes text/plain \`basename \$a\`
    done
}

clean() 
{
    for a in ../${JOB_project}*\${VER}*.deb; do
	rm -f "\$a"
    done

    for a in ../${JOB_project}*\${VER}*.changes; do
	rm -f "\$a"
    done

    rm -f \${CHANGELOG}
    dh_clean
}

deps() 
{
    if [[ \$EUID -ne 0 ]]; then
	echo "Build dependencies must be installed as root"
	exit 1
    fi
    apt-get -y install \${BUILD_DEPS}
}

buildenv() 
{
    echo \$BUILD_DEPS | shasum | awk '{print \$1}'
}

eval $1
EOF
    chmod 755 ${TRAMPOLINE}
}


cleanup_build()
{
    rm -f ${OFILE}
    if [[ -n "$ROOT" ]]; then
	echo Unmounting ${ROOT}/${BUILDROOT}
	umount ${ROOT}/${BUILDROOT}
	echo done
    fi

    if [[ -d "${BTRFSROOT}/${JOB_target}/tmp" ]]; then
	btrfs subvolume delete "${BTRFSROOT}/${JOB_target}/tmp"
    fi
}

do_build() {
    ROOT=""
    sudo -u ${CMDUSER} mkdir -p ${BUILDROOT}
    CHECKOUTPATH="${BUILDROOT}/repos/${JOB_project}"
    TRAMPOLINE="${BUILDROOT}/trampoline.sh"
    OFILE=`mktemp`
    build_status "GIT checkout"
    echo "================================================================="
    echo "GIT Checkout"
    echo "================================================================="

    cat >${TRAMPOLINE} <<EOF
#!/bin/bash
set -eu

if [ -d "${CHECKOUTPATH}" ]; then
    echo "Updating local copy of repo into ${CHECKOUTPATH} from ${JOB_repo}"
    cd "${CHECKOUTPATH}"
    git remote set-url origin ${JOB_repo}
    git fetch origin
else
    echo "Creating local copy of repo into ${CHECKOUTPATH} from ${JOB_repo}"
    mkdir -p "${BUILDROOT}/repos"
    cd "${BUILDROOT}/repos"
    git clone -n ${JOB_repo} ${CHECKOUTPATH}
fi
cd "${CHECKOUTPATH}"
git checkout -f ${JOB_revision}
find . -maxdepth 1 -name "build.*" -type d -print0 | xargs -0 rm -rf
EOF
    chmod 755 ${TRAMPOLINE}

    sudo -u ${CMDUSER} ${TRAMPOLINE} 2>&1 | tee ${OFILE}
    STATUS=${PIPESTATUS[0]}

    send_artifact "GIT checkout" "checkout" "text/plain; charset=utf-8" ${OFILE}
    rm -f ${OFILE}

    if [ $STATUS -ne 0 ]; then
	build_fail "GIT checkout failed"
	return
    fi

    ARTIFACT_POSTFIX=""
    if [ ! -z ${JOB_postfix} ]; then
	ARTIFACT_POSTFIX="-${JOB_postfix}"
    fi


    # Check how to build


#    if [ -f ${CHECKOUTPATH}/debian/control ]; then
#	TRAMPOLINEWRITE="write_deb_trampoline"
#	export JOBSARGS
#	export JARGS
#	echo "Using debian style autobuild"
#    elif [ -x ${CHECKOUTPATH}/Autobuild.sh ]; then

    if [ -x ${CHECKOUTPATH}/Autobuild.sh ]; then
	AUTOBUILD_ARGS="-t ${JOB_target} ${JARGS}"
	echo "Using ./Autobuild.sh for autobuild"
    else
	build_fail "Autobuild.sh does not exist or is not executable"
	return
    fi

    local auto_build_api_ver=`${CHECKOUTPATH}/Autobuild.sh -v`
    local do_deps=0;
    local do_clean=0;

    case $auto_build_api_ver in 
	2)
	    TRAMPOLINEWRITE="write_trampoline_v2"
	    ;;
	3)
	    TRAMPOLINEWRITE="write_trampoline_v3"
	    do_deps=1
	    do_clean=1
	    ;;
	*)
	    build_fail "Autobuild.sh presents incompatible API version $auto_build_api_ver"
	    cleanup_build
	    return
	    ;;
    esac

    if [[ -d "${BTRFSROOT}/${JOB_target}" ]]; then
	${TRAMPOLINEWRITE} buildenv
	${TRAMPOLINE} 2>&1 | tee ${OFILE}
	STATUS=${PIPESTATUS[0]}
	if [ $STATUS -ne 0 ]; then
	    echo "No buildenv available"
	    buildenv="tmp"
	    btrfs subvolume delete "${BTRFSROOT}/${JOB_target}/${buildenv}"
	else
	    buildenv=`cat ${OFILE}`
	    echo "build environment: $buildenv"
	fi

	ROOT=${BTRFSROOT}/${JOB_target}/${buildenv}

	if [ -d "${BTRFSROOT}/${JOB_target}/${buildenv}" ]; then
	    echo "Reusing existing buildroot: ${ROOT}"
	else
	    btrfs subvolume snapshot "${BTRFSROOT}/${JOB_target}/base" "${ROOT}"
	    if [ $? -ne 0 ]; then
		build_fail "Unable to: btrfs subvolume snapshot ${BTRFSROOT}/${JOB_target}/base ${ROOT}"
		cleanup_build
		return
	    fi

	    echo "Created new buildroot ${ROOT} from ${BTRFSROOT}/${JOB_target}/base"
	fi
    fi
    
    #
    # Link buildroot into target file system
    #

    if [[ -n "$ROOT" ]]; then
	mkdir -p "${ROOT}/${BUILDROOT}"
	mount -B "${BUILDROOT}" "${ROOT}/${BUILDROOT}"
	if [ $? -ne 0 ]; then
	    build_fail "Unable to: mount -B ${BUILDROOT} ${ROOT}/${BUILDROOT}"
	    cleanup_build
	    return
	fi

        #
        # .. and verify that we can chroot
        # 

	chroot "$ROOT" true
	if [ $? -ne 0 ]; then
	    build_fail "Unable to: chroot ${ROOT}"
	    cleanup_build
	    return
	fi

	chroot --userspec=${USERSPEC} "$ROOT" true
	if [ $? -ne 0 ]; then
	    build_fail "Unable to: chroot --userspec=${USERSPEC} ${ROOT}"
	    cleanup_build
	    return
	fi
    fi

    if [ $do_deps -eq 1 ] && [[ -n "$ROOT" ]]; then
	build_status "Installing build deps (buildapi=${auto_build_api_ver})"
	echo "================================================================="
	echo "Build-deps in chroot: ${ROOT:-current}"
	echo "================================================================="

	${TRAMPOLINEWRITE} deps

	chroot  "$ROOT" ${TRAMPOLINE} 2>&1 | tee ${OFILE}
	STATUS=${PIPESTATUS[0]}

	send_artifact_gzip "Build-deps output" "builddeps" "text/plain; charset=utf-8" ${OFILE}

	if [ $STATUS -ne 0 ]; then
	    build_fail "build-deps failed"
	    cleanup_build
	    return
	fi
    fi


    build_status "Building (buildapi=${auto_build_api_ver})"
    echo "================================================================="
    echo "Build in chroot: ${ROOT:-current}"
    echo "  configure extra args: ${AUTOBUILD_CONFIGURE_EXTRA}"
    echo "================================================================="
    ${TRAMPOLINEWRITE} build


    if [[ -n "$ROOT" ]]; then
	echo "chroot to ${ROOT}"
	chroot --userspec=${USERSPEC} "$ROOT" ${TRAMPOLINE} 2>&1 | tee ${OFILE}
	STATUS=${PIPESTATUS[0]}
    else
	sudo -u ${CMDUSER} ${TRAMPOLINE} 2>&1 | tee ${OFILE}
	STATUS=${PIPESTATUS[0]}
    fi

    send_artifact_gzip "Build output" "buildlog" "text/plain; charset=utf-8" ${OFILE}

    if [ $STATUS -ne 0 ]; then
	build_fail "build failed"
	cleanup_build
	return
    fi

    if [ ${JOB_no_output} -ne 1 ]; then
	grep -E "^doozer-artifact:" ${OFILE} | while read line; do
	    localpath="$(echo $line | cut -f2 -d:)"
	    filetype="$(echo $line | cut -f3 -d:)"
	    contenttype="$(echo $line | cut -f4 -d:)"
	    filename="$(echo $line | cut -f5 -d: | sed 's/\(.*\)\.\(.*\)/\1/')"
	    fileending="$(echo $line | cut -f5 -d: | sed 's/\(.*\)\.\(.*\)/\2/')"
	    send_artifact "${filename}${ARTIFACT_POSTFIX}.${fileending}" "${filetype}" "${contenttype}" "${localpath}"
	done

	grep -E "^doozer-artifact-gzip:" ${OFILE} | while read line; do
	    localpath="$(echo $line | cut -f2 -d:)"
	    filetype="$(echo $line | cut -f3 -d:)"
	    contenttype="$(echo $line | cut -f4 -d:)"
	    filename="$(echo $line | cut -f5 -d: | sed 's/\(.*\)\.\(.*\)/\1/')"
	    fileending="$(echo $line | cut -f5 -d: | sed 's/\(.*\)\.\(.*\)/\2/')"
	    send_artifact_gzip "${filename}${ARTIFACT_POSTFIX}.${fileending}" "${filetype}" "${contenttype}" "${localpath}"
	done

	grep -E "^doozer-versioned-artifact:" ${OFILE} | while read line; do
	    localpath="$(echo $line | cut -f2 -d:)"
	    filetype="$(echo $line | cut -f3 -d:)"
	    contenttype="$(echo $line | cut -f4 -d:)"
	    filename="$(echo $line | cut -f5 -d: | sed 's/\(.*\)\.\(.*\)/\1/')"
	    fileending="$(echo $line | cut -f5 -d: | sed 's/\(.*\)\.\(.*\)/\2/')"
	    send_artifact "${filename}.${fileending}" "${filetype}" "${contenttype}" "${localpath}"

	done
    fi


    if [ $do_clean -eq 1 ]; then
	build_status "Cleanup"
	echo "================================================================="
	echo "clean in chroot: ${ROOT:-current}"
	echo "================================================================="
	${TRAMPOLINEWRITE} clean

	if [[ -n "$ROOT" ]]; then
	    chroot  "$ROOT" ${TRAMPOLINE} 2>&1 | tee ${OFILE}
	    STATUS=${PIPESTATUS[0]}
	else
	    sudo -u ${CMDUSER} ${TRAMPOLINE} 2>&1 | tee ${OFILE}
	    STATUS=${PIPESTATUS[0]}
	fi
    fi

    cleanup_build
    send_status done ""
}


if [ ${TESTMODE} -eq 1 ]; then
    JOB_target=${TARGETS}
    JOB_postfix=""
    JOB_no_output=0
    JOB_revision=master
    JOB_repo=/home/andoma/showtime/.git
    JOB_project=showtime
    do_build
    exit 0
fi

probe=`curl -s "http://${BUILDMASTER}/buildmaster/hello?agent=${AGENT_ID}&secret=${AGENT_SECRET}"`
STATUS=$?
if [ $STATUS -ne 0 ]; then
    echo "Unable to contact server ${BUILDMASTER} -- Curl error $STATUS"
    exit 1
fi

if [ "$probe" != "welcome" ]; then
    echo "Server at ${BUILDMASTER} says -- $probe"
    exit 1
fi

echo "We are welcomed by server, entering query loop..."
echo "Maxjobs: $JARGS"
while true; do
    if ! VARS=`curl -s -f "http://${BUILDMASTER}/buildmaster/getjob?agent=${AGENT_ID}&secret=${AGENT_SECRET}&targets=${TARGETS}"`; then
	echo "Curl fail"
	sleep 3
	continue
    fi

    JOB_postfix=""
    for a in ${VARS}; do
	eval JOB_$a
    done

    case ${JOB_type} in
	none)
	    ;;
	build)
	    export REPORT_URL="http://${BUILDMASTER}/buildmaster/report?jobid=${JOB_id}&jobsecret=${JOB_jobsecret}"
	    echo "${VARS}"
	    do_build
	    ;;
	*)
	    sleep 1
	    ;;
    esac

    for a in ${VARS}; do
	unset JOB_`echo $a | sed s/=.*$//` 
    done

    unset REPORT_URL

done


#
# Some random notes for bootstraping BTRFS roots:
#
# copy nosync
# add universe
# apt-get update
# apt-get install build-essential debhelper git ccache language-pack-en
#
# mkdir /home/builder
# useradd -d /home/builder -u 5000 -g 1 -p x builder
# chown builder:daemon /home/builder
