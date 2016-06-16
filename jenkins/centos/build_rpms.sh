#!/bin/bash

set -e

sudo yum install -y epel-release
sudo yum install -y gcc gcc-c++ make yum-utils rpm-build

# add openvstorage repo for dependencies of volumedriver packages
echo '[openvstorage]
name=Open vStorage repo
baseurl=http://yum.openvstorage.org/CentOS/7/x86_64/dists/unstable
enabled=1
gpgcheck=0' | sudo tee /etc/yum.repos.d/openvstorage.repo

## if jenkins copied in volumedriver packages install them,
## else use the packages from the openvstorage repo
P=$(ls -d libovsvolumedriver*.rpm 2>/dev/null || true)
if [ -n "$P" ]
then
  sudo yum install -y libovsvolumedriver*.rpm
else
  sudo yum install -y libovsvolumedriver-devel
fi

## prepare the build env
chown -R jenkins:jenkins blktap
mkdir -p ./blktap/rpm/{BUILD,RPMS,SOURCES,SPECS,SRPMS}

## create tar of our sources for rpmbuild; exclude rpm subdir
tar czf blktap/rpm/SOURCES/blktap-utils-openvstorage-2.0.90.tar.gz --exclude-vcs --exclude=blktap/rpm blktap

cd blktap/rpm

# echo '>>> FETCHING UPSTREAM SOURCES <<<' 
# spectool -g -R SPECS/blktap-utils-openvstorage.spec

echo '>>> INSTALL BUILD DEPENDENCIES <<<'
sudo yum-builddep -y SPECS/blktap-utils-openvstorage.spec

echo '>>> BUILD RPMS <<<'
rpmbuild -ba --define "_topdir ${PWD}" SPECS/blktap-utils-openvstorage.spec

