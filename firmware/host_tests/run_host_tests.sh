#!/bin/bash
# HachimoDock port host logic tests (ported from firmware/tests).
# Pure C asserts compiled with the workspace's w64devkit gcc (AGENTS.md).
set -e
cd "$(dirname "$0")"
GCC="D:/esp-mo/mingw/w64devkit/bin/gcc.exe"
[ -x "$GCC" ] || GCC=gcc
PET=../main/pet
COMMON="-std=c11 -Wall -I$PET -I../main -DMOSAICO_HOST_SIMULATION=1"

run() {
  local name=$1; shift
  "$GCC" $COMMON "$name.c" "$@" -lm -o "$name.exe"
  ./"$name.exe"
  echo "PASS $name"
}

run p4_input_logic_test "$PET/pet_p4_input_core.c"
run p4_game_logic_test "$PET/pet_p4_game.c"
run p4_conversation_test "$PET/pet_p4_conversation.c" "$PET/pet_p4_view.c"
run p4_widget_data_test "$PET/pet_p4_widget_data.c" "$PET/host_only/cJSON.c"
echo ALL_HOST_TESTS_PASS
