gcc --version | head -1
g++ --version | head -1
cmake --version | head -1
git --version
dpkg -l | grep libzmq && ls -la /usr/include/zmq.h
uname -a && cat /etc/os-release
lscpu
free -h
df -h / /home
