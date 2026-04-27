#!/usr/bin/env bats

@test "bats is alive" {
  result="$(echo hello)"
  [ "$result" = "hello" ]
}
