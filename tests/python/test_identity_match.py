"""How a host compares the serial in the USB `hello` with the one in GET /identity."""
import json
from pathlib import Path

import pytest

from dc_device import identity_matches_hello, normalize_serial, serials_match

FIX = Path(__file__).resolve().parents[2] / "tests" / "fixtures" / "p0"


@pytest.mark.parametrize("raw", [
    "AABBCCDDEEFF", "aabbccddeeff", "AA:BB:CC:DD:EE:FF", "aa-bb-cc-dd-ee-ff", "AABB.CCDD.EEFF",
    " AABBCCDDEEFF\n", "aA:bB:Cc:dD:eE:fF",
])
def test_normalize_accepts_common_spellings(raw):
    assert normalize_serial(raw) == "AABBCCDDEEFF"


@pytest.mark.parametrize("raw", [
    "", "   ", "AABBCCDDEEF", "AABBCCDDEEFF0", "GGBBCCDDEEFF", "AA:BB:CC:DD:EE", "camera_board_01",
    None, 123456789012, ["AABBCCDDEEFF"], "AABBCCDDEEFFé",
])
def test_normalize_rejects_what_is_not_a_mac(raw):
    assert normalize_serial(raw) is None


def test_serials_match_needs_two_valid_equal_serials():
    assert serials_match("aa:bb:cc:dd:ee:ff", "AABBCCDDEEFF")
    assert not serials_match("AABBCCDDEEFF", "AABBCCDDEE00")
    # two missing or invalid serials are not "equal"
    assert not serials_match(None, None)
    assert not serials_match("", "")
    assert not serials_match("nope", "nope")


def fix(name):
    return json.loads((FIX / name).read_text())["body"]


def test_real_samples_match():
    ok, why = identity_matches_hello(fix("hello.json"), fix("identity.json"))
    assert ok and why == []


@pytest.mark.parametrize("field,value", [
    ("serial", "AABBCCDDEE00"),
    ("device_id", "other_board"),
    ("type", "robot_car"),
    ("firmware_version", "0.2.0"),
])
def test_any_difference_is_a_mismatch_and_named(field, value):
    hello = fix("hello.json")
    hello[field] = value
    ok, why = identity_matches_hello(hello, fix("identity.json"))
    assert not ok and why == [field]


def test_missing_serial_is_a_mismatch_not_a_pass():
    hello = fix("hello.json")
    del hello["serial"]
    ok, why = identity_matches_hello(hello, fix("identity.json"))
    assert not ok and "serial" in why
    ident = fix("identity.json")
    del ident["serial"]
    ok, why = identity_matches_hello(fix("hello.json"), ident)
    assert not ok and "serial" in why


def test_device_id_is_not_a_serial():
    # camera_board_01 is the same on every camera board; only the serial tells boards apart.
    assert normalize_serial(fix("identity.json")["device_id"]) is None
