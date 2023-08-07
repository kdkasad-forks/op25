#!/bin/sh

# op25 install script for MacOS

if [ ! -d op25/gr-op25 ]; then
	echo ====== error, op25 top level directories not found
	echo ====== you must change to the op25 top level directory
	echo ====== before running this script
	exit
fi

# Install dependencies
brew install gnuradio librtlsdr uhd hackrf itpp libpcap orc cmake swig gnuplot libsndfile spdlog cppunit pybind11
python3 -m pip install numpy waitress requests

# Tell op25 to use Python 3
echo "/usr/bin/python3" > op25/gr-op25_repeater/apps/op25_python

# Run build process
rm -rf build
mkdir build
cd build
cmake ../         2>&1 | tee cmake.log
make              2>&1 | tee cmake.log
sudo make install 2>&1 | tee install.log
