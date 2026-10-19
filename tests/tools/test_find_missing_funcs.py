"""tools/analysis/find_missing_funcs.py on a synthetic guest image and generated sources.

Expected input: a raw dump of the loaded guest image (PE header at offset 0, image base
0x82000000, sections addressed by virtual address) plus a ReXGlue generated directory with
superman_returns_register.cpp (SetFunction(0x..., name)) and superman_returns_recomp.*.cpp
(DEFINE_REX_FUNC bodies with one "// insn" comment per instruction).
"""
import struct

import find_missing_funcs as fmf

BASE = 0x82000000


def build_image():
    """0x400-byte image: .text at RVA 0x100 (size 0x100), .data at RVA 0x200 (size 0x40)."""
    img = bytearray(0x400)
    struct.pack_into("<I", img, 0x3C, 0x40)          # e_lfanew
    img[0x40:0x44] = b"PE\0\0"
    struct.pack_into("<H", img, 0x40 + 6, 2)          # NumberOfSections
    struct.pack_into("<H", img, 0x40 + 20, 0)         # SizeOfOptionalHeader
    table = 0x40 + 24
    for i, (name, rva, size, flags) in enumerate(((b".text", 0x100, 0x100, 0x20000000),
                                                   (b".data", 0x200, 0x40, 0x40000000))):
        s = table + i * 40
        img[s:s + len(name)] = name
        struct.pack_into("<II", img, s + 8, size, rva)
        struct.pack_into("<I", img, s + 36, flags)

    def w(rva, word):
        struct.pack_into(">I", img, rva, word)

    w(0x100, 0x7D8802A6)  # sub_82000100: mflr r12
    w(0x104, 0x4E800020)  #               blr
    w(0x108, 0x38630010)  # uncovered: addi r3,r3,16 (after blr -> entry-like)
    w(0x10C, 0x48000074)  # uncovered: b, previous word is addi -> falls through
    w(0x140, 0x38630020)  # uncovered: preceded by zero padding (only found with --gaps)
    w(0x180, 0x4E800020)  # sub_82000180: blr
    w(0x200, BASE + 0x108)  # data pointer to uncovered entry
    w(0x204, BASE + 0x10C)  # data pointer to fall-through code (rejected)
    w(0x208, BASE + 0x100)  # data pointer to a known function (ignored)
    return bytes(img)


def write_generated(gen):
    gen.mkdir()
    (gen / "superman_returns_register.cpp").write_text(
        "SetFunction(0x82000100, sub_82000100);\nSetFunction(0x82000180, sub_82000180);\n")
    (gen / "superman_returns_recomp.0.cpp").write_text(
        "DEFINE_REX_FUNC(sub_82000100) {\n\t// mflr r12\n\tctx.r12.u64 = ctx.lr;\n"
        "\t// blr\n\treturn;\n}\n\n"
        "DEFINE_REX_FUNC(sub_82000180) {\n\t// blr\n\treturn;\n}\n")


def run(monkeypatch, tmp_path, capsys, *flags):
    image = tmp_path / "image.bin"
    image.write_bytes(build_image())
    write_generated(tmp_path / "gen")
    monkeypatch.setattr(fmf, "GEN", tmp_path / "gen")
    monkeypatch.setattr("sys.argv", ["find_missing_funcs.py", str(image), *flags])
    fmf.main()
    return capsys.readouterr().out


def test_describe_decodes_by_hand():
    # źródło: kodowanie PPC policzone ręcznie, addi = opcode 14, rD=3, rA=3, imm 0xFFF0 = -16
    assert fmf.describe(0x3863FFF0) == "addi r3,r3,-16 (this-adjust thunk)"
    assert fmf.describe(0x7D8802A6) == "mflr r12 (prologue)"  # źródło: stała mflr r12
    assert fmf.describe(0x48000010) == "b ..."  # źródło: opcode 18 = b
    assert fmf.describe(0x60000000) == "60000000"  # źródło: nop ori, brak reguły, zapis hex


def test_pe_sections_on_builtin_image():
    # źródło: wartości wpisane ręcznie do nagłówka w build_image
    assert fmf.pe_sections(build_image()) == [
        (".text", 0x82000100, 0x100, True),
        (".data", 0x82000200, 0x40, False),
    ]


def test_load_functions_sizes(monkeypatch, tmp_path):
    write_generated(tmp_path / "gen")
    monkeypatch.setattr(fmf, "GEN", tmp_path / "gen")
    # źródło: policzone ręcznie, 2 komentarze instrukcji = 8 bajtów, 1 komentarz = 4 bajty
    assert fmf.load_functions() == [
        (0x82000100, 0x82000108, "sub_82000100"),
        (0x82000180, 0x82000184, "sub_82000180"),
    ]


def test_toml_lists_only_entry_like_uncovered_pointer(monkeypatch, tmp_path, capsys):
    out = run(monkeypatch, tmp_path, capsys, "--toml")
    # źródło: policzone ręcznie, jedyny wskaźnik z .data do niepokrytego kodu po blr to 0x82000108
    assert out.splitlines() == ['0x82000108 = { name = "vt_82000108" }']


def test_gaps_adds_padding_preceded_entry(monkeypatch, tmp_path, capsys):
    out = run(monkeypatch, tmp_path, capsys, "--toml", "--gaps")
    # źródło: policzone ręcznie, 0x82000140 poprzedzony zerem w luce 0x82000108-0x82000180
    assert out.splitlines() == ['0x82000108 = { name = "vt_82000108" }',
                                '0x82000140 = { name = "vt_82000140" }']


def test_text_report(monkeypatch, tmp_path, capsys):
    out = run(monkeypatch, tmp_path, capsys)
    # źródło: policzone ręcznie z układu build_image
    assert "functions: 2, uncovered non-zero gaps: 1" in out
    assert "entry points in uncovered code: 1" in out
    assert "rejected (falls through from previous insn): 1" in out
    assert "  82000108  addi r3,r3,16 (this-adjust thunk)" in out  # źródło: 0x38630010 ręcznie
    assert "refs 82000200" in out  # źródło: adres wskaźnika w .data
    assert "  8200010C  b ..." in out and "refs 82000204" in out  # źródło: jw.
    assert "  82000108-82000180 (120 bytes) first=addi r3,r3,16" in out  # źródło: 0x180-0x108=120
