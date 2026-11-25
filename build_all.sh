#!/bin/bash
set -e

cmake --preset desktop
cmake --build --preset desktop

cmake --preset switch1
cmake --build --preset switch1

cmake --preset switch2
cmake --build --preset switch2