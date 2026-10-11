# log_features_serial_5bin.py
import argparse, csv, time, sys
from datetime import datetime
import serial
import numpy as np
import os

STX, ETX = 0xAA, 0x55

def parse_bins(bins_str, n_bins_expected=5):
    if not bins_str:
        # 5-bin 기본값(예시): 프로젝트에서 고른 값 넣어두기
        # 필요 없으면 반드시 --bins 로 넘겨줘
        return [1100,1200,2200,2300,800]
    freqs = [int(x.strip()) for x in bins_str.split(",") if x.strip()]
    if len(freqs) != n_bins_expected:
        raise ValueError(f"--bins 개수({len(freqs)})가 5가 아님")
    return freqs

def read_packet(ser, payload_len):
    while True:
        b = ser.read(1)
        if not b:
            return None
        if b[0] == 0xAA:
            break

    hdr = ser.read(2)
    if len(hdr) < 2:
        return None
    seq, N = hdr[0], hdr[1]
    if N != payload_len:
        return None

    payload = ser.read(N)
    footer  = ser.read(2)
    if len(payload) != N or len(footer) < 2:
        return None
    ck, etx = footer[0], footer[1]
    if etx != 0x55:
        return None

    calc = seq ^ N ^ int(np.bitwise_xor.reduce(np.frombuffer(payload, dtype=np.uint8)))
    if calc != ck:
        return None

    return seq, payload

def main():
    ap = argparse.ArgumentParser(description="Log 5-bin features from A-board to CSV")
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--outfile", default="log_5bin.csv")
    ap.add_argument("--bins", default="", help="예: 1100,1200,2200,2300,800 (5개)")
    ap.add_argument("--seconds", type=float, default=None)
    ap.add_argument("--max_frames", type=int, default=None)
    ap.add_argument("--print", action="store_true")
    args = ap.parse_args()

    freqs = parse_bins(args.bins, 5)
    feat_names = ["rms_u8","vpp_u8","zcr_u8"] + [f"p{f}_u8" for f in freqs]

    os.makedirs(os.path.dirname(args.outfile) or ".", exist_ok=True)
    payload_len = 3 + 5  # 8

    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.2)
        ser.reset_input_buffer()
    except Exception as e:
        print(f"[ERROR] serial open fail: {e}")
        sys.exit(1)

    with open(args.outfile, "w", newline="", encoding="utf-8") as fp:
        writer = csv.writer(fp)
        header = ["time_iso","t_rel","seq","lost"] + feat_names
        writer.writerow(header)

        t0 = time.time()
        last_seq = None
        frames = 0

        print(f"[START] port={args.port}, save={args.outfile}, bins={freqs}")
        try:
            while True:
                if args.seconds is not None and (time.time() - t0) >= args.seconds:
                    break
                if args.max_frames is not None and frames >= args.max_frames:
                    break

                pkt = read_packet(ser, payload_len)
                if pkt is None:
                    continue

                seq, payload = pkt
                vals = list(payload)
                now = time.time()
                time_iso = datetime.fromtimestamp(now).isoformat(timespec="milliseconds")
                t_rel = now - t0

                lost = 0
                if last_seq is not None:
                    diff = (seq - last_seq) & 0xFF
                    if diff > 1:
                        lost = diff - 1
                last_seq = seq

                row = [time_iso, f"{t_rel:.6f}", seq, lost] + vals
                writer.writerow(row)
                frames += 1

                if args.print:
                    print(row)
        except KeyboardInterrupt:
            pass
        finally:
            ser.close()
            print(f"[DONE] frames={frames}, saved={args.outfile}")

if __name__ == "__main__":
    main()
