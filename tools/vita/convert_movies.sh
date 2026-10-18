#!/bin/bash
# Re-encodes the movies of a mod so the PS Vita can play them. Only the ones
# it can't play are re-encoded; see convert_movies.py for the options.
#
# usage: convert_movies.sh <mod, mods folder or pack> [-o OUT] [--max-height 720]
exec python3 "$(dirname "$0")/convert_movies.py" "$@"
