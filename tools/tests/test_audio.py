# OWNER: builder-B4 (ambient)
"""Synthetic fixtures for tools/importers/audio.py: a hand-built .SEA (SDBK + XACT2 WBND) with MS-ADPCM data."""
import json
import math
import struct
import wave

import pytest

from tools.importers import audio

C1 = audio._COEF1
C2 = audio._COEF2
ADAPT = audio._ADAPT


def encode_msadpcm_block(samples, pi=0):
    """Reference MS-ADPCM block encoder (mono): returns (bytes, reconstruction). Mirrors the decoder state exactly."""
    s2, s1 = samples[0], samples[1]
    delta = 16
    out = bytearray(struct.pack("<Bhhh", pi, delta, s1, s2))
    recon = [s2, s1]
    nibs = []
    for x in samples[2:]:
        pred = s1 * C1[pi] + s2 * C2[pi]
        pred = -((-pred) >> 8) if pred < 0 else pred >> 8
        q = int(round((x - pred) / delta))
        q = max(-8, min(7, q))
        v = q & 15
        y = max(-32768, min(32767, pred + q * delta))
        delta = max(16, (ADAPT[v] * delta) >> 8)
        s2, s1 = s1, y
        nibs.append(v)
        recon.append(y)
    if len(nibs) % 2:
        nibs.append(0)
    for i in range(0, len(nibs), 2):
        out.append((nibs[i] << 4) | nibs[i + 1])
    return bytes(out), recon


def encode_msadpcm(samples, spb=128):
    data, recon = b"", []
    for i in range(0, len(samples), spb):
        blk = samples[i:i + spb]
        blk = blk + [0] * (spb - len(blk))
        d, r = encode_msadpcm_block(blk)
        data += d
        recon += r
    return data, recon[:len(samples)]


def sine(n, rate=22050, f=440.0, amp=8000):
    return [int(amp * math.sin(2 * math.pi * f * i / rate)) for i in range(n)]


def make_sea(entries, bank="TestBank", sdbk=None):
    """entries: list of (name, samples, rate). Returns SEA bytes (5-word header, SDBK stub, WBND)."""
    waves, meta, names = b"", b"", b""
    for name, samples, rate in entries:
        data, _ = encode_msadpcm(samples)
        stored_ba = 70 - 22
        fmt = 2 | (1 << 2) | (rate << 5) | (stored_ba << 23)
        meta += struct.pack("<6I", len(samples) << 4, fmt, len(waves), len(data), 0, len(samples) - 128)
        waves += data
        names += name.encode().ljust(64, b"\0")
    bank_data = struct.pack("<2I", 0x90000, len(entries)) + bank.encode().ljust(64, b"\0") + struct.pack("<4I", 24, 64, 4, 0) + b"\0" * 8
    hdr_len = 12 + 40
    seg0 = hdr_len
    seg1 = seg0 + len(bank_data)
    seg2 = seg1 + len(meta)
    seg3 = seg2  # no seek table
    seg4 = seg3 + len(names)
    wbnd = (b"WBND" + struct.pack("<2I", 44, 42) +
            struct.pack("<10I", seg0, len(bank_data), seg1, len(meta), seg2, 0, seg3, len(names), seg4, len(waves)) +
            bank_data + meta + names + waves)
    sdbk = sdbk if sdbk is not None else b"SDBK" + b"\0" * 60
    pre = b"AuthoringString\0" + b"C:\\secret\\path\0"
    head_len = 20 + len(pre)
    hdr = struct.pack("<5I", head_len, len(sdbk), len(wbnd), 1, 4)
    return hdr + pre + sdbk + wbnd


def test_header_sum_rule_and_parse():
    sea = make_sea([("StepA", sine(1000), 22050), ("Loop", sine(300), 11025)])
    h = struct.unpack_from("<3I", sea, 0)
    assert sum(h) == len(sea)
    b = audio.parse_sea(sea)
    assert b.bank_name == "TestBank"
    assert [e.name for e in b.entries] == ["StepA", "Loop"]
    e = b.entries[0]
    assert (e.tag, e.channels, e.sample_rate, e.block_align, e.duration) == (2, 1, 22050, 70, 1000)
    assert b.entries[1].sample_rate == 11025
    assert len(b.raw(e)) == e.play_length == 8 * 70


def test_decode_is_bit_exact_with_encoder_reconstruction():
    pcm = sine(1000)
    _, recon = encode_msadpcm(pcm)
    b = audio.parse_sea(make_sea([("S", pcm, 22050)]))
    out = audio.decode_entry(b, b.entries[0])
    assert out == recon
    err = sum(abs(x - y) for x, y in zip(out, pcm)) / len(pcm)
    assert err < 700  # the test encoder restarts its step size each block; mean error stays well under the 8000 amplitude


def test_decode_handles_negative_prediction_rounding():
    # C division truncates toward zero; a floor shift would differ on negative predictions
    pcm = [(-1) ** i * 3000 for i in range(256)]
    _, recon = encode_msadpcm(pcm)
    b = audio.parse_sea(make_sea([("S", pcm, 22050)]))
    assert audio.decode_entry(b, b.entries[0]) == recon


def test_rejects_bad_structure():
    sea = bytearray(make_sea([("S", sine(128), 22050)]))
    bad = bytes(sea[:20]) + bytes(len(sea) - 20)
    with pytest.raises(audio.AudioError):
        audio.parse_sea(bad)
    with pytest.raises(audio.AudioError):
        audio.parse_sea(b"short")
    wb = bytearray(sea)
    off = struct.unpack_from("<2I", wb, 0)
    wb[off[0] + off[1]:off[0] + off[1] + 4] = b"XXXX"
    with pytest.raises(audio.AudioError):
        audio.parse_sea(bytes(wb))


def test_extract_writes_wavs_and_manifest(tmp_path):
    inst = tmp_path / "install"
    (inst / "audio" / "xact").mkdir(parents=True)
    (inst / "audio" / "xact" / "Feet.SEA").write_bytes(make_sea([("Shoe Cement_01", sine(500), 14700), ("Other", sine(256), 22050)]))
    (inst / "audio" / "xact" / "Wind.SEA").write_bytes(make_sea([("WindLoop", sine(1024), 16000)]))
    out = tmp_path / "out"
    man = audio.extract(inst, out, ["feet"], log=lambda m: None)
    assert man["count"] == 2
    row = next(r for r in man["entries"] if r["name"] == "Shoe Cement_01")
    assert row["wav"] == "Feet/000_Shoe_Cement_01.wav" and row["sample_rate"] == 14700 and row["samples"] == 500
    with wave.open(str(out / row["wav"])) as w:
        assert (w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()) == (1, 2, 14700, 500)
    # a partial extraction of another bank keeps the first bank's rows
    man2 = audio.extract(inst, out, ["wind"], log=lambda m: None)
    assert man2["count"] == 3 and {r["bank"] for r in man2["entries"]} == {"Feet", "Wind"}
    assert json.loads((out / "manifest.json").read_text())["count"] == 3


def test_survey_counts_codecs(tmp_path):
    inst = tmp_path / "i"
    (inst / "audio" / "xact").mkdir(parents=True)
    (inst / "audio" / "xact" / "A.SEA").write_bytes(make_sea([("x", sine(128), 22050)] * 3))
    (inst / "audio" / "xact" / "B.SEA").write_bytes(b"junk" * 10)
    s = audio.survey(inst)
    assert s["files"] == 1 and s["entries"] == 3 and s["codecs"] == {"adpcm": 3} and len(s["failures"]) == 1


def encode_msadpcm_stereo(left, right, pi=0):
    """Stereo MS-ADPCM block: header grouped by field, nibbles alternate L (high) / R (low). 128 samples per channel."""
    st = []
    for ch in (left, right):
        st.append({"s2": ch[0], "s1": ch[1], "delta": 16, "x": ch[2:], "rec": [ch[0], ch[1]]})
    out = bytearray(bytes([pi, pi]))
    out += struct.pack("<2h", st[0]["delta"], st[1]["delta"])
    out += struct.pack("<2h", st[0]["s1"], st[1]["s1"])
    out += struct.pack("<2h", st[0]["s2"], st[1]["s2"])
    nibs = []
    for k in range(len(left) - 2):
        for c in range(2):
            t = st[c]
            pred = t["s1"] * C1[pi] + t["s2"] * C2[pi]
            pred = -((-pred) >> 8) if pred < 0 else pred >> 8
            q = max(-8, min(7, int(round((t["x"][k] - pred) / t["delta"]))))
            v = q & 15
            y = max(-32768, min(32767, pred + q * t["delta"]))
            t["delta"] = max(16, (ADAPT[v] * t["delta"]) >> 8)
            t["s2"], t["s1"] = t["s1"], y
            t["rec"].append(y)
            nibs.append(v)
    for i in range(0, len(nibs), 2):
        out.append((nibs[i] << 4) | nibs[i + 1])
    return bytes(out), st[0]["rec"], st[1]["rec"]


def test_stereo_block_header_layout_and_interleave():
    L, R = sine(128, f=300), sine(128, f=700, amp=5000)
    blk, rl, rr = encode_msadpcm_stereo(L, R)
    assert len(blk) == 140
    out = audio.decode_msadpcm(blk, 2, 140)
    assert out[0::2] == rl and out[1::2] == rr


def make_stream_bin(tmp_path, stem, entries):
    """entries: list of (list_name, samples). Writes <stem>.bin ('Hash', count, (hash, offset, size)*) + <stem>.lst."""
    blobs = []
    for name, samples in entries:
        sea = make_sea([(name.split(".")[0], samples, 22050)], bank=name.split(".")[0])
        h = struct.unpack_from("<5I", sea, 0)
        wbnd = sea[h[0] + h[1]:]
        blob = b"SDBK" + b"\0" * 2044 + wbnd           # WBND at the next sector, as in the game's Ambs.bin
        blobs.append(blob + b"\0" * (-len(blob) % 2048))
    table_len = 8 + 12 * len(blobs)
    base = (table_len + 2047) // 2048 * 2048
    index, body, off = b"", b"", base
    for i, b in enumerate(blobs):
        index += struct.pack("<3I", 1000 + i, off, len(b))
        body += b
        off += len(b)
    data = b"Hash" + struct.pack("<I", len(blobs)) + index
    data += b"\0" * (base - len(data)) + body
    (tmp_path / f"{stem}.bin").write_bytes(data)
    (tmp_path / f"{stem}.lst").write_text("\r\n".join(n for n, _ in entries) + "\r\n")


def test_stream_bin_index_blob_and_extract(tmp_path):
    inst = tmp_path / "install"
    pl = inst / "audio" / "PLAYLIST"
    pl.mkdir(parents=True)
    (inst / "audio" / "xact").mkdir()
    make_stream_bin(pl, "Ambs", [("Wind.rsm", sine(1280)), ("Birds01.rsm", sine(640, f=2000))])
    idx = audio.read_stream_index(pl / "Ambs.bin")
    assert len(idx) == 2 and idx[0][1] % 2048 == 0
    names = audio.read_stream_names(pl / "Ambs.lst")
    assert names == ["Wind.rsm", "Birds01.rsm"]
    b = audio.stream_entry(pl / "Ambs.bin", idx, 1)
    assert b.entries[0].name == "Birds01" and b.entries[0].duration == 640
    man = audio.extract(inst, tmp_path / "out", ["ambs"], names="bird", log=lambda m: None)
    assert [r["name"] for r in man["entries"]] == ["Birds01"] and man["entries"][0]["source"] == "PLAYLIST"
    with wave.open(str(tmp_path / "out" / man["entries"][0]["wav"])) as w:
        assert w.getnframes() == 640
    s = audio.survey_streams(inst)
    assert s["Ambs"]["entries"] == 2 and s["Ambs"]["codecs"] == {"adpcm": 2} and "error" in s["Cuts"]


def test_stream_bin_rejects_wrong_magic(tmp_path):
    (tmp_path / "x.bin").write_bytes(b"NoPe" + b"\0" * 64)
    with pytest.raises(audio.AudioError):
        audio.read_stream_index(tmp_path / "x.bin")
    with pytest.raises(audio.AudioError):
        audio.parse_stream_blob(b"RIFF" + b"\0" * 100)


# ---- integration (opt-in: BULLY_INSTALL) ----
import os

INSTALL = os.environ.get("BULLY_INSTALL")
def make_sdbk(cues, bank="TestBank"):
    """cues: list of (cue name, wave index, base pitch cents, pitch_min, pitch_max). Builds the one shape Bully's banks
    use (XACT 3 sound bank, tool 44 / format 43): simple cues -> 71-byte complex sounds, 1 track, 2 RPC codes, 1 clip,
    1 event of type 4 (PlayWave with effect variation, 40 bytes)."""
    n = len(cues)
    names = b"".join(c[0].encode() + b"\0" for c in cues) + b"\0"
    hdr_len, wbn_off = 0x4A, 0x4A + 64
    sounds_off = wbn_off + 64
    sounds = b""
    for i, (_nm, widx, pitch, pmin, pmax) in enumerate(cues):
        base = sounds_off + len(sounds)
        s = struct.pack("<BHBhBH", 3, 1, 180, pitch, 255, 71) + bytes([1]) + struct.pack("<H", 11) + bytes([2]) + struct.pack("<2I", 224, 247)
        s += bytes([180]) + struct.pack("<I2H", base + 30, 24000, 1000)
        ev = struct.pack("<IHBB", 0x20000004, 0, 0xFF, 4) + struct.pack("<HB", widx, 0) + b"\0" * 5 + struct.pack("<2hBB", pmin, pmax, 180, 180)
        ev += struct.pack("<4f", 1000.0, 1000.0, 30.0, 30.0) + b"\0\x30"
        assert len(ev) == 40
        s += bytes([1]) + ev
        assert len(s) == 71, len(s)
        sounds += s
    simple_off = sounds_off + len(sounds)
    simple = b"".join(struct.pack("<BI", 4, sounds_off + 71 * i) for i in range(n))
    names_off = simple_off + len(simple)
    body = bank.encode().ljust(64, b"\0") + bank.encode().ljust(64, b"\0") + sounds + simple + names
    head = b"SDBK" + struct.pack("<3H", 44, 43, 0x1234) + struct.pack("<2I", 1, 2) + bytes([1])
    head += struct.pack("<4H", n, 0, 0, n) + bytes([1]) + struct.pack("<3H", n, len(names), 0)
    head += struct.pack("<10I", simple_off, 0xFFFFFFFF, names_off, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, wbn_off, 0, 0, sounds_off)
    assert len(head) == hdr_len, len(head)
    return head + body


def test_sdbk_cue_graph_parse():
    cues = [("WALK_GRASS0", 1, 0, -300, 200), ("WALK_GRASS1", 0, 0, -300, 200), ("RUN_CEM_PL_DRY0", 1, -900, -100, 100)]
    sea = make_sea([("Shoe_A", sine(300), 22050), ("Shoe_B", sine(300), 22050)], sdbk=make_sdbk(cues))
    b = audio.parse_sea(sea)
    sd = audio.parse_sdbk(b)
    assert (sd.tool_version, sd.format_version) == (44, 43) and sd.wave_bank_names == ["TestBank"]
    assert [c.name for c in sd.cues] == ["WALK_GRASS0", "WALK_GRASS1", "RUN_CEM_PL_DRY0"]
    t = audio.cue_table(b)
    assert t["WALK_GRASS0"]["wave"] == "Shoe_B" and t["WALK_GRASS1"]["wave"] == "Shoe_A"
    assert (t["RUN_CEM_PL_DRY0"]["pitch_cents"], t["RUN_CEM_PL_DRY0"]["pitch_min"], t["RUN_CEM_PL_DRY0"]["pitch_max"]) == (-900, -100, 100)
    assert abs(t["WALK_GRASS0"]["volume_db"] - (-24.0)) < 0.1   # volume byte 180 -> -96 + 180/255 * 102.0206


def test_sdbk_rejects_unknown_shapes():
    bad = bytearray(make_sdbk([("A", 0, 0, 0, 0)]))
    bad[0x4A + 128] = 1            # sound flags 1 instead of 3
    with pytest.raises(audio.AudioError):
        audio.parse_sdbk(audio.parse_sea(make_sea([("S", sine(300), 22050)], sdbk=bytes(bad))))
    with pytest.raises(audio.AudioError):   # event type 1, not the one decoded shape
        bad = bytearray(make_sdbk([("A", 0, 0, 0, 0)]))
        bad[0x4A + 128 + 31] = 0x01
        audio.parse_sdbk(audio.parse_sea(make_sea([("S", sine(300), 22050)], sdbk=bytes(bad))))


def test_footstep_resolution_picks_families_and_falls_back():
    cues = [("WALK_CEM_PL_DRY0", 0, -1200, -300, 200), ("WALK_CEM_PL_DRY1", 1, -1200, -300, 200),
            ("WALK_GRASS0", 1, 0, -300, 200), ("RUN_GRASS0", 0, 300, -300, 200), ("FST_RUN_GRASS0", 1, 500, -300, 200)]
    sea = make_sea([("A", sine(300), 22050), ("B", sine(300), 22050)], bank="FeetTest", sdbk=make_sdbk(cues, "FeetTest"))
    cfg = {"contexts": {"exterior": ["FeetTest"]},
           "gait_prefix": {"player": {"walk": "WALK", "run": "RUN", "sprint": ["FRUN", "FST_RUN"]}},
           "surfaces": {"cement": {"player": ["{G}_CEM_PL_DRY"]}, "grass": {"player": ["{G}_GRASS"]}}}
    import tempfile, pathlib
    with tempfile.TemporaryDirectory() as td:
        (pathlib.Path(td) / "audio" / "xact").mkdir(parents=True)
        (pathlib.Path(td) / "audio" / "xact" / "FeetTest.SEA").write_bytes(sea)
        r = audio.resolve_footsteps(td, cfg, log=lambda m: None)
    ex = r["contexts"]["exterior"]
    assert [c["cue"] for c in ex["cement"]["player"]["walk"]] == ["WALK_CEM_PL_DRY0", "WALK_CEM_PL_DRY1"]
    assert [c["wave"] for c in ex["grass"]["player"]["sprint"]] == ["B"]            # FRUN_GRASS absent -> FST_RUN_GRASS
    assert "cement" in ex and "run" not in ex["cement"]["player"]                    # no RUN_CEM_PL_DRY in this bank
    assert any("cement/player/run" in u for u in r["unresolved"])
    assert r["waves"] == {"FeetTest": ["A", "B"]}


real = pytest.mark.skipif(not INSTALL or not os.path.isdir(os.path.join(INSTALL or "", "audio", "xact")), reason="BULLY_INSTALL not set")


@real
def test_real_install_every_sea_parses_and_sums():
    s = audio.survey(INSTALL)
    assert s["files"] == 322 and s["entries"] == 4512 and s["codecs"] == {"adpcm": 4512} and not s["failures"]
    for p in audio.sea_files(INSTALL):
        d = p.read_bytes()
        assert sum(struct.unpack_from("<3I", d, 0)) == len(d)


@real
def test_real_install_footstep_decodes_to_declared_length():
    b = audio.parse_sea(os.path.join(INSTALL, "audio", "xact", "FeetAll.SEA"))
    e = next(x for x in b.entries if x.name == "Sneaker Walk Cemnt_03")
    pcm = audio.decode_entry(b, e)
    assert len(pcm) == e.duration and max(abs(x) for x in pcm) > 5000


@real
def test_real_install_ambs_stream_school_outdoor_is_stereo_adpcm():
    pl = os.path.join(INSTALL, "audio", "PLAYLIST")
    s = audio.survey_streams(INSTALL, sample=1000)
    assert s["Ambs"]["entries"] == s["Ambs"]["lst_lines"] == 143 and s["Ambs"]["parsed"] == 143
    idx = audio.read_stream_index(os.path.join(pl, "Ambs.bin"))
    names = audio.read_stream_names(os.path.join(pl, "Ambs.lst"))
    b = audio.stream_entry(os.path.join(pl, "Ambs.bin"), idx, names.index("NewSchoolOutdoor_F.rsm"))
    e = b.entries[0]
    assert (e.name, e.channels, e.tag, e.block_align) == ("NewSchoolOutdoor_F", 2, 2, 140)
    pcm = audio.decode_msadpcm(b.raw(e)[:140 * 200], 2, 140)
    assert len(pcm) == 128 * 200 * 2 and max(abs(x) for x in pcm) > 1000


@real
def test_real_install_all_sound_banks_decode_one_shape_and_footsteps_map_to_surfaces():
    total = 0
    for p in audio.sea_files(INSTALL):
        b = audio.parse_sea(p)
        sd = audio.parse_sdbk(b)           # raises on any shape other than the one decoded
        total += len(sd.cues)
        t = audio.cue_table(b)             # dict by name: 2 of the 11,075 cues share a name with another cue of their bank
        assert all(0 <= c["wave_index"] < len(b.entries) for c in t.values())
    assert total == 11075
    feet = audio.cue_table(audio.parse_sea(os.path.join(INSTALL, "audio", "xact", "FeetAll.SEA")))
    assert len(feet) == 459
    # the player's cement cues are the sneaker waves, pitched one octave down; grass is unpitched, three variations
    assert {feet[f"WALK_CEM_PL_DRY{i}"]["wave"] for i in range(4)} == {"Sneaker Walk Cemnt_03", "Sneaker Walk Cemnt_04", "Sneaker Walk Cemnt_06", "Sneaker Walk Cemnt_08"}
    assert feet["WALK_CEM_PL_DRY0"]["pitch_cents"] == -1200 and feet["WALK_GRASS0"]["pitch_cents"] == 0
    assert [feet[f"WALK_GRASS{i}"]["wave"] for i in range(3)] == ["Shoe_Grass_03", "Shoe_Grass_04", "Shoe_Grass_06"]
