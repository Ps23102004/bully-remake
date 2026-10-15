"""Bully: Scholarship Edition audio: `audio/xact/*.SEA` sound-bank archives -> WAV.

Format (reverse engineered; see docs/re-notes/audio-formats.md):

    u32 sdbk_off, u32 sdbk_len, u32 wbnd_len, u32 (=1), u32 (=4)     5-word header
    ...strings (authoring metadata; never republished)...
    sdbk_off:            'SDBK'  XACT sound bank (cue/sound/track graph; only the header is read here)
    wbnd_off = sdbk_off + sdbk_len:
                         'WBND'  XACT wave bank, version 44 / header version 42

All 322 files satisfy `u32[0] + u32[1] + u32[2] == file size` and carry exactly this pair. The wave bank is the
XACT 2 layout (MonoGame / xactxwb documentation): five (offset, length) segments (bank data, entry metadata, seek
tables, entry names, entry wave data), offsets relative to 'WBND'.

Every one of the 4,512 wave entries is mono MS-ADPCM, 128 samples per 70-byte block. Decoded here to 16-bit PCM
and written as RIFF WAV. Nothing outside the standard library is needed.
"""
from __future__ import annotations

import json
import os
import re
import struct
import wave
from dataclasses import dataclass, field
from pathlib import Path

# standard Microsoft ADPCM coefficient sets (public specification constants)
_COEF1 = (256, 512, 0, 192, 240, 460, 392)
_COEF2 = (0, -256, 0, 64, 0, -208, -232)
_ADAPT = (230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230)

FORMAT_PCM, FORMAT_XMA, FORMAT_ADPCM, FORMAT_WMA = 0, 1, 2, 3


class AudioError(ValueError):
    pass


@dataclass
class WaveEntry:
    index: int
    name: str
    flags: int
    duration: int  # samples
    tag: int  # 0 PCM, 1 XMA, 2 ADPCM, 3 WMA
    channels: int
    sample_rate: int
    block_align: int  # bytes per ADPCM/PCM block, already expanded ((stored + 22) * channels for ADPCM)
    bits16: int
    play_offset: int  # bytes, relative to the wave-data segment
    play_length: int
    loop_offset: int  # samples; XACT loop region, also set on one-shots (not a loop flag)
    loop_length: int

    @property
    def seconds(self) -> float:
        return self.duration / self.sample_rate if self.sample_rate else 0.0


@dataclass
class SeaBank:
    path: str
    bank_name: str
    sdbk_offset: int
    sdbk_length: int
    wbnd_offset: int
    wbnd_length: int
    header: tuple
    entries: list = field(default_factory=list)
    _data: bytes = b""
    _data_start: int = 0

    def raw(self, e: WaveEntry) -> bytes:
        a = self._data_start + e.play_offset
        return self._data[a:a + e.play_length]


def parse_sea(source) -> SeaBank:
    """Parse a .SEA file (path or bytes). Raises AudioError on a structure that does not fit."""
    if isinstance(source, (bytes, bytearray)):
        d, path = bytes(source), "<bytes>"
    else:
        path = str(source)
        d = Path(path).read_bytes()
    if len(d) < 20:
        raise AudioError("file shorter than the 20-byte header")
    hdr = struct.unpack_from("<5I", d, 0)
    sd_off, sd_len, wb_len = hdr[0], hdr[1], hdr[2]
    wb_off = sd_off + sd_len
    if d[sd_off:sd_off + 4] != b"SDBK":
        raise AudioError("no SDBK at u32[0]")
    if d[wb_off:wb_off + 4] != b"WBND":
        raise AudioError("no WBND at u32[0]+u32[1]")
    if wb_off + wb_len > len(d):
        raise AudioError("wave bank runs past end of file")
    return _parse_banks(d, path, hdr, sd_off, sd_len, wb_off, wb_len)


def parse_stream_blob(blob: bytes, path: str = "<stream>") -> SeaBank:
    """One entry of audio/PLAYLIST/*.bin: 'SDBK' at 0, the 'WBND' wave bank at the next sector (2048 in every
    Ambs entry checked), no outer header."""
    if blob[:4] != b"SDBK":
        raise AudioError("stream blob does not start with SDBK")
    wb_off = blob.find(b"WBND", 4)
    if wb_off < 0:
        raise AudioError("no WBND in stream blob")
    return _parse_banks(blob, path, (0, wb_off, len(blob) - wb_off, 0, 0), 0, wb_off, wb_off, len(blob) - wb_off)


def _parse_banks(d, path, hdr, sd_off, sd_len, wb_off, wb_len) -> SeaBank:
    ver, hver = struct.unpack_from("<2I", d, wb_off + 4)
    if ver < 40:
        raise AudioError(f"unsupported WBND version {ver}")
    segs = struct.unpack_from("<10I", d, wb_off + 12)
    bd = wb_off + segs[0]
    _flags, count = struct.unpack_from("<2I", d, bd)
    bank_name = d[bd + 8:bd + 72].split(b"\0")[0].decode("latin1")
    meta_size, name_size, _align, _compact = struct.unpack_from("<4I", d, bd + 72)
    if meta_size < 24:
        raise AudioError(f"entry metadata element size {meta_size} < 24")
    bank = SeaBank(path, bank_name, sd_off, sd_len, wb_off, wb_len, hdr)
    bank._data = d
    bank._data_start = wb_off + segs[8]
    for i in range(count):
        m = struct.unpack_from("<6I", d, wb_off + segs[2] + i * meta_size)
        fd, fmt, po, pl, lo, ll = m
        tag = fmt & 3
        ch = (fmt >> 2) & 7
        sr = (fmt >> 5) & 0x3FFFF
        ba = (fmt >> 23) & 0xFF
        bits16 = fmt >> 31
        if tag == FORMAT_ADPCM:
            ba = (ba + 22) * ch
        name = ""
        if segs[7] and name_size:
            a = wb_off + segs[6] + i * name_size
            name = d[a:a + name_size].split(b"\0")[0].decode("latin1")
        bank.entries.append(WaveEntry(i, name, fd & 0xF, fd >> 4, tag, ch, sr, ba, bits16, po, pl, lo, ll))
    return bank


# ---- SDBK: the XACT 2/3 sound bank (cue -> sound -> track -> clip -> event -> wave) -------------------------

@dataclass
class SdbkCue:
    index: int
    name: str
    flags: int              # 4 in every Bully cue (simple cue that points at a sound)
    sound_offset: int       # bytes from 'SDBK'
    sound: "SdbkSound" = None


@dataclass
class SdbkSound:
    offset: int
    flags: int              # 3 in all 11,075 sounds: bit0 complex (tracks), bit1 RPC list present
    category: int
    volume_byte: int
    pitch: int              # s16
    priority: int
    entry_length: int
    rpc_codes: tuple
    track_volume_byte: int
    clip_offset: int
    filter_a: int
    filter_b: int
    event_type: int         # 4 = PlayWave with effect variation, in every Bully sound
    wave_index: int
    wave_bank: int
    pitch_min: int          # s16 variation range around the sound pitch
    pitch_max: int
    volume_min_byte: int
    volume_max_byte: int
    freq_min: float
    freq_max: float
    q_min: float
    q_max: float

    @property
    def volume_db(self) -> float:
        """XACT volume byte -> dB: 0 = -96 dB .. 255 = +6.0206 dB, linear in dB."""
        return xact_volume_db(self.volume_byte)


@dataclass
class Sdbk:
    bank_name: str
    tool_version: int
    format_version: int
    platform: int
    cues: list
    wave_bank_names: list
    sounds: list


def xact_volume_db(b: int) -> float:
    return (b / 255.0) * (6.0206 + 96.0) - 96.0


def parse_sdbk(bank: "SeaBank") -> Sdbk:
    """Decode the SDBK of a .SEA (layout = the public XACT 3 sound-bank format, FACT/FAudio's reader; tool version 44,
    format 43 in all 322 banks). Bully's banks use ONE shape only: every cue is a simple cue (flags 4) that points at a
    71-byte complex sound with 1 track, 2 RPC codes, 1 clip, 1 event of type 4 (PlayWave, effect variation, 40 bytes).
    Anything else raises AudioError, so a surprise in another bank cannot silently mis-map."""
    d = bank._data
    b0 = bank.sdbk_offset
    if d[b0:b0 + 4] != b"SDBK":
        raise AudioError("no SDBK")
    tool, fmt, _crc = struct.unpack_from("<3H", d, b0 + 4)
    if fmt != 43:
        raise AudioError(f"unsupported SDBK format version {fmt}")
    platform = d[b0 + 0x12]
    n_simple, n_complex, _u1, n_total = struct.unpack_from("<4H", d, b0 + 0x13)
    n_wavebanks = d[b0 + 0x1B]
    n_sounds, name_len, _u2 = struct.unpack_from("<3H", d, b0 + 0x1C)
    (simple_off, complex_off, names_off, _a, var_off, _b, wbn_off, _hash_off, _hashv_off, sounds_off) = \
        struct.unpack_from("<10I", d, b0 + 0x22)
    if n_complex or n_sounds != n_simple:
        raise AudioError(f"complex cues present ({n_complex}) or sounds != cues ({n_sounds} vs {n_simple}); not decoded")
    names = d[b0 + names_off:b0 + names_off + name_len].split(b"\0")
    bank_names = [d[b0 + wbn_off + 64 * i:b0 + wbn_off + 64 * (i + 1)].split(b"\0")[0].decode("latin1")
                  for i in range(n_wavebanks)]
    sounds: dict = {}
    pos = sounds_off
    for _ in range(n_sounds):
        fl, cat, vol, pitch, prio, elen = struct.unpack_from("<BHBhBH", d, b0 + pos)
        base = b0 + pos
        ntr = d[base + 9]
        xl = struct.unpack_from("<H", d, base + 10)[0]
        nrpc = d[base + 12]
        if fl != 3 or ntr != 1 or xl != 2 + 1 + 4 * nrpc or elen != 71:
            raise AudioError(f"sound @{pos}: unexpected shape flags={fl} tracks={ntr} extra={xl} length={elen}")
        rpc = struct.unpack_from(f"<{nrpc}I", d, base + 13)
        q = base + 10 + xl
        tvol = d[q]
        clip, fa, fb = struct.unpack_from("<I2H", d, q + 1)
        ev = b0 + clip + 1
        if d[b0 + clip] != 1:
            raise AudioError(f"sound @{pos}: {d[b0 + clip]} events")
        hdr = struct.unpack_from("<I", d, ev)[0]
        etype = hdr & 0x1F
        if etype != 4:
            raise AudioError(f"sound @{pos}: event type {etype}")
        widx = struct.unpack_from("<H", d, ev + 8)[0]
        wbank = d[ev + 10]
        pmin, pmax = struct.unpack_from("<2h", d, ev + 16)
        vmin, vmax = d[ev + 20], d[ev + 21]
        fmin, fmax, qmin, qmax = struct.unpack_from("<4f", d, ev + 22)
        sounds[pos] = SdbkSound(pos, fl, cat, vol, pitch, prio, elen, tuple(rpc), tvol, clip, fa, fb, etype, widx,
                                wbank, pmin, pmax, vmin, vmax, fmin, fmax, qmin, qmax)
        pos += elen
    cues = []
    for i in range(n_simple):
        flags = d[b0 + simple_off + 5 * i]
        so = struct.unpack_from("<I", d, b0 + simple_off + 5 * i + 1)[0]
        nm = names[i].decode("latin1") if i < len(names) else f"#{i}"
        cues.append(SdbkCue(i, nm, flags, so, sounds.get(so)))
    return Sdbk(bank.bank_name, tool, fmt, platform, cues, bank_names, list(sounds.values()))


def cue_table(bank: "SeaBank") -> dict:
    """Plain-data cue graph of one bank: {cue name: {wave, wave_index, volume_db, pitch_cents, pitch_min, pitch_max}}.
    pitch_cents is the sound's base pitch (XACT pitch units = cents: -1200 = one octave down, pairs with the waves
    named '8va', recorded an octave up to save memory); the event adds a uniform random offset in
    [pitch_min, pitch_max] cents per play."""
    sd = parse_sdbk(bank)
    out = {}
    for c in sd.cues:
        so = c.sound
        out[c.name] = {"wave": bank.entries[so.wave_index].name, "wave_index": so.wave_index,
                       "volume_db": round(so.volume_db, 2), "pitch_cents": so.pitch,
                       "pitch_min": so.pitch_min, "pitch_max": so.pitch_max}
    return out


def resolve_footsteps(install, config: dict, log=print) -> dict:
    """Resolve Config/FootstepSurfaces.json (hand-authored surface -> cue-family rules) against the real cue graphs.

    config["contexts"]: context -> ordered bank list (first bank that has a family wins);
    config["gait_prefix"]: role -> gait -> cue prefix (or an ordered list of prefixes); config["surfaces"]: surface ->
    role -> [family template, ...] in fallback order, template '{G}_CEM_DRY' + gait prefix 'WALK' -> family
    'WALK_CEM_DRY', members WALK_CEM_DRY0, WALK_CEM_DRY1, ...
    Returns the runtime table (also lists unresolved entries instead of failing, so a missing cue is visible)."""
    import re
    banks: dict = {}
    for ctx_banks in config["contexts"].values():
        for b in ctx_banks:
            if b not in banks:
                path = next((p for p in sea_files(install) if p.stem.lower() == b.lower()), None)
                if path is None:
                    raise AudioError(f"bank {b} not found")
                sb = parse_sea(path)
                banks[b] = (sb, cue_table(sb))
    out = {"format": "bully-remake.footstep-cues/1", "contexts": {}, "waves": {}, "unresolved": []}
    for ctx, ctx_banks in config["contexts"].items():
        table = {}
        for surface, roles in config["surfaces"].items():
            table[surface] = {}
            for role, templates in roles.items():
                table[surface][role] = {}
                for gait, prefixes in config["gait_prefix"][role].items():
                    found = None
                    for bank in ctx_banks:
                        cues = banks[bank][1]
                        for tpl in templates:
                            for prefix in ([prefixes] if isinstance(prefixes, str) else prefixes):
                                fam = tpl.replace("{G}", prefix)
                                rx = re.compile("^" + re.escape(fam) + r"(\d+)$")
                                names = sorted((n for n in cues if rx.match(n)), key=lambda n: int(rx.match(n).group(1)))
                                if names:
                                    found = [dict(cue=n, bank=bank, **cues[n]) for n in names]
                                    break
                            if found:
                                break
                        if found:
                            break
                    if found:
                        table[surface][role][gait] = found
                        for c in found:
                            out["waves"].setdefault(c["bank"], [])
                            if c["wave"] not in out["waves"][c["bank"]]:
                                out["waves"][c["bank"]].append(c["wave"])
                    else:
                        out["unresolved"].append(f"{ctx}/{surface}/{role}/{gait}: none of {templates} in {ctx_banks}")
        out["contexts"][ctx] = table
    for u in out["unresolved"]:
        log("unresolved: " + u)
    return out


# ---- audio/PLAYLIST/{Ambs,Cuts,Music,Speech}.bin: streamed sound banks ----------------------------------------

STREAM_BANKS = ("Ambs", "Cuts", "Music", "Speech")


def read_stream_index(bin_path) -> list:
    """'Hash' u32 magic, u32 count, then count x (u32 name hash, u32 byte offset, u32 byte size). Entry i is the i-th
    line of the .lst of the same name (hash function OPEN; the order matches for Ambs: 143 = 143)."""
    with open(bin_path, "rb") as f:
        head = f.read(8)
        if head[:4] != b"Hash":
            raise AudioError("not a PLAYLIST .bin (no 'Hash' magic)")
        n = struct.unpack_from("<I", head, 4)[0]
        raw = f.read(12 * n)
    if len(raw) != 12 * n:
        raise AudioError("truncated index")
    return [struct.unpack_from("<3I", raw, 12 * i) for i in range(n)]


def read_stream_names(lst_path) -> list:
    return [x.strip() for x in Path(lst_path).read_text(errors="replace").splitlines() if x.strip()]


def stream_entry(bin_path, index: list, i: int) -> SeaBank:
    _h, off, size = index[i]
    with open(bin_path, "rb") as f:
        f.seek(off)
        blob = f.read(size)
    return parse_stream_blob(blob, f"{Path(bin_path).name}[{i}]")


def decode_msadpcm(data: bytes, channels: int, block_align: int, max_samples: int | None = None) -> list:
    """Microsoft ADPCM -> interleaved int16 list. Blocks: per channel 7-byte header, then nibbles (high first)."""
    if channels not in (1, 2):
        raise AudioError("only mono/stereo ADPCM")
    out = []
    pos = 0
    n = len(data)
    while pos < n:
        blk = data[pos:pos + block_align]
        pos += block_align
        if len(blk) < 7 * channels:
            break
        # block header is grouped by field: predictor[ch], delta[ch], sample1[ch], sample2[ch] (= 7 bytes per channel)
        preds = blk[:channels]
        deltas = struct.unpack_from(f"<{channels}h", blk, channels)
        s1s = struct.unpack_from(f"<{channels}h", blk, 3 * channels)
        s2s = struct.unpack_from(f"<{channels}h", blk, 5 * channels)
        chs = []
        for c in range(channels):
            if preds[c] > 6:
                raise AudioError("ADPCM predictor index out of range")
            chs.append([preds[c], deltas[c], s1s[c], s2s[c]])
        # output order: sample2 then sample1 of each channel first
        seq = [[chs[c][3], chs[c][2]] for c in range(channels)]
        nib = blk[7 * channels:]
        c = 0
        for byte in nib:
            for v in (byte >> 4, byte & 15):
                st = chs[c]
                pi, delta, s1, s2 = st
                pred = s1 * _COEF1[pi] + s2 * _COEF2[pi]
                pred = -((-pred) >> 8) if pred < 0 else pred >> 8  # C division: truncate toward zero
                sv = v - 16 if v > 7 else v
                x = pred + sv * delta
                x = -32768 if x < -32768 else 32767 if x > 32767 else x
                delta = (_ADAPT[v] * delta) >> 8
                if delta < 16:
                    delta = 16
                st[1], st[2], st[3] = delta, x, s1
                seq[c].append(x)
                c = (c + 1) % channels
        if channels == 1:
            out.extend(seq[0])
        else:
            for a, b in zip(seq[0], seq[1]):
                out.append(a)
                out.append(b)
        if max_samples is not None and len(out) >= max_samples * channels:
            break
    if max_samples is not None:
        out = out[:max_samples * channels]
    return out


def decode_entry(bank: SeaBank, e: WaveEntry) -> list:
    raw = bank.raw(e)
    if e.tag == FORMAT_ADPCM:
        return decode_msadpcm(raw, e.channels, e.block_align, e.duration)
    if e.tag == FORMAT_PCM:
        if e.bits16:
            return list(struct.unpack(f"<{len(raw) // 2}h", raw[:len(raw) // 2 * 2]))
        return [(b - 128) << 8 for b in raw]
    raise AudioError(f"codec tag {e.tag} not supported (XMA/WMA do not occur in Bully's data)")


def write_wav(path, samples, channels: int, rate: int) -> None:
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(struct.pack(f"<{len(samples)}h", *samples))


_SAFE = re.compile(r"[^A-Za-z0-9_.-]+")


def safe_name(s: str) -> str:
    return _SAFE.sub("_", s).strip("_") or "unnamed"


def sea_files(install: Path) -> list:
    return sorted((Path(install) / "audio" / "xact").glob("*.SEA"))


def extract_bank(sea_path, out_dir, names: str | None = None) -> list:
    """Decode every (or the regex-matched) entry of one .SEA to <out_dir>/<bank>/<entry>.wav. Returns manifest rows."""
    bank = parse_sea(sea_path)
    stem = Path(sea_path).stem
    rx = re.compile(names, re.I) if names else None
    rows = []
    for e in bank.entries:
        if rx and not rx.search(e.name):
            continue
        pcm = decode_entry(bank, e)
        rel = Path(stem) / f"{e.index:03d}_{safe_name(e.name)}.wav"
        write_wav(Path(out_dir) / rel, pcm, e.channels, e.sample_rate)
        rows.append({
            "bank": stem, "index": e.index, "name": e.name, "wav": rel.as_posix(),
            "sample_rate": e.sample_rate, "channels": e.channels, "samples": len(pcm) // e.channels,
            "seconds": round(len(pcm) / e.channels / e.sample_rate, 4),
            "loop_start_samples": e.loop_offset, "loop_samples": e.loop_length,
        })
    return rows


def extract_stream_bank(install, stem, out_dir, names: str | None = None) -> list:
    """Decode entries of audio/PLAYLIST/<stem>.bin (names = regex on the .lst name without extension)."""
    pl = Path(install) / "audio" / "PLAYLIST"
    binp = pl / f"{stem}.bin"
    idx = read_stream_index(binp)
    lst = read_stream_names(pl / f"{stem}.lst")
    rx = re.compile(names, re.I) if names else None
    rows = []
    for i, nm in enumerate(lst[:len(idx)]):
        label = Path(nm).stem
        if rx and not rx.search(label):
            continue
        bank = stream_entry(binp, idx, i)
        for e in bank.entries:
            pcm = decode_entry(bank, e)
            rel = Path(stem) / f"{safe_name(label)}.wav"
            write_wav(Path(out_dir) / rel, pcm, e.channels, e.sample_rate)
            rows.append({
                "bank": stem, "index": i, "name": label, "wav": rel.as_posix(), "source": "PLAYLIST",
                "sample_rate": e.sample_rate, "channels": e.channels, "samples": len(pcm) // e.channels,
                "seconds": round(len(pcm) / e.channels / e.sample_rate, 4),
                "loop_start_samples": e.loop_offset, "loop_samples": e.loop_length,
            })
    return rows


def extract(install, out_dir, banks: list | None = None, names: str | None = None, log=print) -> dict:
    """Extract the selected banks (.SEA stems or PLAYLIST stems Ambs/Cuts/Music/Speech, case-insensitive) and write
    <out>/manifest.json (partial extractions keep the rows of banks not redone)."""
    want = {b.lower() for b in banks} if banks else None
    rows = []
    for p in sea_files(install):
        if want and p.stem.lower() not in want:
            continue
        r = extract_bank(p, out_dir, names)
        log(f"{p.stem}: {len(r)} wav")
        rows.extend(r)
    for stem in STREAM_BANKS:
        if want and stem.lower() in want:   # streamed banks are large (2.1 GB): only on request
            r = extract_stream_bank(install, stem, out_dir, names)
            log(f"{stem} (PLAYLIST): {len(r)} wav")
            rows.extend(r)
    Path(out_dir).mkdir(parents=True, exist_ok=True)
    mp = Path(out_dir) / "manifest.json"
    if mp.exists() and (want or names):
        # partial extraction: keep the rows of banks that were not re-extracted
        try:
            old = json.loads(mp.read_text()).get("entries", [])
        except (ValueError, OSError):
            old = []
        redone = {r["bank"] for r in rows}
        rows = [r for r in old if r["bank"] not in redone] + rows
    man = {"format": "bully-remake.audio-manifest/1", "count": len(rows), "entries": rows}
    (Path(out_dir) / "manifest.json").write_text(json.dumps(man, indent=1))
    return man


def survey(install) -> dict:
    """Header-only scan of every .SEA: counts by codec and rate, failures."""
    out = {"files": 0, "entries": 0, "codecs": {}, "failures": []}
    for p in sea_files(install):
        try:
            b = parse_sea(p)
        except (AudioError, struct.error) as ex:
            out["failures"].append((p.name, str(ex)))
            continue
        out["files"] += 1
        for e in b.entries:
            out["entries"] += 1
            k = {0: "pcm", 1: "xma", 2: "adpcm", 3: "wma"}.get(e.tag, str(e.tag))
            out["codecs"][k] = out["codecs"].get(k, 0) + 1
    return out


def survey_streams(install, sample: int = 200) -> dict:
    """Header-only scan of audio/PLAYLIST/*.bin (sampling at most `sample` entries per bank; Speech has ~25k)."""
    pl = Path(install) / "audio" / "PLAYLIST"
    out = {}
    for stem in STREAM_BANKS:
        try:
            idx = read_stream_index(pl / f"{stem}.bin")
            lst = read_stream_names(pl / f"{stem}.lst")
        except (AudioError, OSError) as ex:
            out[stem] = {"error": str(ex)}
            continue
        step = max(1, len(idx) // sample)
        ok, bad, codecs = 0, 0, {}
        for i in range(0, len(idx), step):
            try:
                b = stream_entry(pl / f"{stem}.bin", idx, i)
                ok += 1
                for e in b.entries:
                    k = {0: "pcm", 1: "xma", 2: "adpcm", 3: "wma"}.get(e.tag, str(e.tag))
                    codecs[k] = codecs.get(k, 0) + 1
            except (AudioError, struct.error):
                bad += 1
        out[stem] = {"entries": len(idx), "lst_lines": len(lst), "sampled": ok + bad, "parsed": ok, "unparsed": bad, "codecs": codecs}
    return out
