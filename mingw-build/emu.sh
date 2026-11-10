#!/bin/bash
case "$1" in
  *file_to_c.exe) shift; exec ${FILE_TO_C_HOST:-file_to_c} "$@";;
  *) exec wine "$@";;
esac
