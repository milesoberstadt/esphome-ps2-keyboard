#!/usr/bin/env python3
"""Focused validation tests for the PS/2 component schema helpers."""

from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "components"))

from ps2_keyboard import (  # noqa: E402
    validate_bytes,
    validate_key,
    validate_keys,
    validate_text,
)
from voluptuous import Invalid  # noqa: E402


def expect_invalid(call, *args):
    try:
        call(*args)
    except Invalid:
        return
    raise AssertionError(f"expected invalid value for {args!r}")


def main():
    if validate_keys(["A"] * 32) != "+".join(["A"] * 32):
        raise AssertionError("32-key combination should be accepted")
    expect_invalid(validate_keys, ["A"] * 33)
    if validate_keys(["CTRL", "+"]) != "CTRL++":
        raise AssertionError("terminal physical plus should be accepted")
    if validate_keys(["A"] * 31 + ["+"]) != "+".join(["A"] * 31 + ["+"]):
        raise AssertionError("32 keys including a physical plus should be accepted")
    if validate_keys("CTRL++") != "CTRL++":
        raise AssertionError("terminal physical plus string should be accepted")
    expect_invalid(validate_keys, "CTRL+")
    expect_invalid(validate_keys, "CTRL+ALT+")
    expect_invalid(validate_keys, ["CTRL", "+", "ALT"])
    expect_invalid(validate_keys, "A" * 65)

    if validate_text("a" * 1024) != "a" * 1024:
        raise AssertionError("1024-byte text should be accepted")
    expect_invalid(validate_text, "a" * 1025)
    if validate_key("A" * 64) != "A" * 64:
        raise AssertionError("64-byte key should be accepted")
    expect_invalid(validate_key, "A" * 65)
    if len(validate_bytes([0] * 4096)) != 4096:
        raise AssertionError("4096-byte raw list should be accepted")
    expect_invalid(validate_bytes, [0] * 4097)
    combination_1024 = "+".join(["A" * 32] * 31 + ["A"])
    combination_1025 = "+".join(["A" * 32] * 31 + ["AA"])
    if len(combination_1024) != 1024 or validate_keys(combination_1024) != combination_1024:
        raise AssertionError("1024-byte combination should be accepted")
    expect_invalid(validate_keys, combination_1025)

    print("schema_test: all checks passed")


if __name__ == "__main__":
    main()
