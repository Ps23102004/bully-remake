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
