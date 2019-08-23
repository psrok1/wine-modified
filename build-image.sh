#!/bin/sh
docker build . -t psrok1/wine-modified
docker-squash psrok1/wine-modified -t psrok1/wine-modified -c
