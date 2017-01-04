#!/bin/bash

scp ./sipserv.c pi3.local:SIP-Pi/sipserv.c
ssh pi3.local << EOF
cd SIP-Pi 
./sipserv-ctrl.sh stop
make
EOF

