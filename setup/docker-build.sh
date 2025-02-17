#!/bin/sh
cd $(dirname $0)/..
docker rm -f fossil
docker build -t fossil-img .
