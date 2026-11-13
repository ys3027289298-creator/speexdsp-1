title() {
  local text="$1"
  printf '\n\033[1;36m============================================================\n'
  printf ' %s\n' "$text"
  printf '============================================================\033[0m\n'
}
pause() {
  local seconds="$1"
  for i in $(seq "$seconds" -1 1); do
    printf '\r\033[1;33m--- next segment in %ds ---\033[0m' "$i"
    sleep 1
  done
  printf '\r%*s\r' 40 ""
}
