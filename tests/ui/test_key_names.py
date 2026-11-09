"""key_names.parse: input is the text of the SDK's keybinds.cpp (its kKeyNames
table) and virtual_key.h (the enum's codes); output [(code, name)], one name a
code, the first one in table order."""
import os
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'tools', 'ui'))
import key_names  # noqa: E402

CPP = '''static const std::unordered_map<std::string, VirtualKey> kKeyNames = {
    {"Return", VirtualKey::kReturn},
    {"Enter", VirtualKey::kReturn},
    {"Space", VirtualKey::kSpace},
    {"LMB", VirtualKey::kLButton},
    {"Unknown", VirtualKey::kNotInEnum},
};
'''
H = '''enum class VirtualKey : uint16_t {
  kNone,
  kLButton = 0x01,
  kReturn = 0x0D,  // Enter.
  kSpace = 0x20,
};'''


def test_one_name_a_code_first_wins():
    # source: by hand, Return listed before Enter for 0x0D; kNotInEnum has no code
    assert key_names.parse(CPP, H) == [(0x0D, 'Return'), (0x20, 'Space'), (0x01, 'LMB')]


SDK_CPP = os.path.join(key_names.SDK, 'src', 'ui', 'keybinds.cpp')


@pytest.mark.skipif(not os.path.exists(SDK_CPP), reason='SDK sources not present')
def test_sdk_table():
    names = dict(key_names.sdk_key_names())
    # source: the SDK's keybind defaults (mnk_input_driver.cpp: keybind_a "Semicolon,Space",
    # keybind_lstick_up "W") and Windows VK codes (VK_SPACE 0x20, 'W' 0x57, VK_LBUTTON 0x01)
    assert names[0x20] == 'Space' and names[0x57] == 'W' and names[0x01] == 'LMB'
    assert len(set(names.values())) == len(names)
