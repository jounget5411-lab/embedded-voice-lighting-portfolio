# prep_train_export_wclh_k5.py
import argparse, glob, json, os
import numpy as np, pandas as pd
from sklearn.model_selection import train_test_split
from sklearn.linear_model import LogisticRegression, Perceptron
from sklearn.metrics import classification_report, confusion_matrix

# ===== 5버전 특징 정의 (A보드 패킷 순서와 반드시 동일) =====
FEATS_K5 = [
    "rms_u8","vpp_u8","zcr_u8",
    "p1100_u8","p1200_u8","p2200_u8","p2300_u8","p800_u8"
]
CLASSES = ["silence","warm","cool","low","high"]  # 0..4

def load_csvs(patterns, feats):
    files = []
    for p in patterns: files += glob.glob(p)
    if not files: raise FileNotFoundError(patterns)
    dfs = []
    for f in files:
        df = pd.read_csv(f)
        # CSV에 pc_time_ms, seq가 있어도 무시하고 feats+label만 사용
        miss = [c for c in feats+["label"] if c not in df.columns]
        if miss: raise ValueError(f"{f} missing {miss}")
        df = df[feats+["label"]].copy()
        for c in feats: df[c] = np.clip(df[c].astype(np.float32), 0, 255)
        df["label"] = df["label"].astype(int)
        dfs.append(df)
    return pd.concat(dfs, ignore_index=True)

def relabel_with_vad(df, vad_thresh=12, top_pct=0.3, pos_classes=(1,2,3,4)):
    """
    pos_classes 각 클래스에서:
      1) rms < vad_thresh 이면 0(silence)로 격하
      2) 남은 샘플 중 RMS 상위 top_pct만 유지, 나머지는 0으로 격하
    """
    out = df.copy()
    for cls in pos_classes:
        idx = out["label"]==cls
        if not idx.any(): continue
        small = idx & (out["rms_u8"] < vad_thresh)
        out.loc[small, "label"] = 0
        idx2 = out["label"]==cls
        if idx2.sum() == 0: continue
        thr = np.quantile(out.loc[idx2, "rms_u8"], 1.0 - top_pct)
        keep = idx2 & (out["rms_u8"] >= thr)
        out.loc[idx2 & ~keep, "label"] = 0
    return out

def balance_dataset(df, mode="undersample0", seed=0):
    """
    undersample0 : 클래스0(silence)만, 양성(1..4) 최대크기에 맞춰 다운샘플
    undersample_all : 모든 클래스를 최소크기에 맞춰 다운샘플
    none : 그대로
    """
    if mode not in ["undersample0","undersample_all","none"]: return df
    if mode == "none": return df
    np.random.seed(seed)
    parts = {c: df[df.label==c] for c in df.label.unique()}
    if mode == "undersample0":
        n_pos = max(len(parts.get(c, pd.DataFrame())) for c in [1,2,3,4])
        d0 = parts.get(0, pd.DataFrame())
        if len(d0) > n_pos:
            d0 = d0.sample(n_pos, random_state=seed)
        parts[0] = d0
        out = pd.concat(parts.values(), ignore_index=True)
    else:  # undersample_all
        n_min = min(len(v) for v in parts.values())
        out = pd.concat([v.sample(n_min, random_state=seed) for v in parts.values()],
                        ignore_index=True)
    return out.sample(frac=1.0, random_state=seed)

def stack_temporal(df, feats, k=3):
    """k=3이면 [t-1,t,t+1] 스택. 가장자리는 복제 패딩."""
    X = df[feats].values.astype(np.float32)
    y = df["label"].values.astype(int)
    if k<=1:
        return X, y, feats
    pad = k//2
    Xpad = np.pad(X, ((pad,pad),(0,0)), mode="edge")
    chunks = []
    for t in range(len(X)):
        sl = Xpad[t:t+k].reshape(-1)  # (len(feats) * k)
        chunks.append(sl)
    Xs = np.vstack(chunks)
    feat_names = [f"{c}_t{dt}" for dt in range(-pad,pad+1) for c in feats]
    return Xs, y, feat_names

def to_c_array(name, arr, ctype):
    arr = np.asarray(arr)
    if arr.ndim==1:
        body = ", ".join(str(int(v)) for v in arr)
        shape = f"[{arr.shape[0]}]"
    else:
        rows = []
        for r in arr:
            rows.append(", ".join(str(int(v)) for v in r))
        body = ",\n  ".join(rows)
        shape = "".join(f"[{d}]" for d in arr.shape)
    return f"const {ctype} {name}{shape} = {{\n  {body}\n}};\n"

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--csv", nargs="+", required=True, help="record_k5로 모은 CSV 경로(글롭 OK)")
    ap.add_argument("--apply_vad", type=int, default=1, help="1: VAD 격하 적용, 0: 미적용")
    ap.add_argument("--vad_thresh", type=int, default=13)
    ap.add_argument("--top_pct", type=float, default=0.30)
    ap.add_argument("--balance", choices=["undersample0","undersample_all","none"], default="undersample0")
    ap.add_argument("--stack", type=int, default=3)
    ap.add_argument("--model", choices=["logistic","perceptron"], default="logistic")
    ap.add_argument("--sx", type=int, default=9)
    ap.add_argument("--sw", type=float, default=32.0)
    ap.add_argument("--test_size", type=float, default=0.2)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--class_weight", choices=["none","balanced"], default="balanced",
                    help="scikit-learn 분류기 class_weight")
    ap.add_argument("--out", default="weights_wclh_k5.h")
    args = ap.parse_args()

    feats = FEATS_K5

    df = load_csvs(args.csv, feats)
    if args.apply_vad:
        df = relabel_with_vad(df, args.vad_thresh, args.top_pct, pos_classes=(1,2,3,4))
    df = balance_dataset(df, args.balance, args.seed)

    if args.stack<=1:
        X = df[feats].values.astype(np.float32)
        feat_names = feats
    else:
        X, y, feat_names = stack_temporal(df, feats, args.stack)
        df = pd.DataFrame(X, columns=feat_names).assign(label=df["label"].values)
    y = df["label"].values.astype(int)

    Xtr, Xte, ytr, yte = train_test_split(
        df[feat_names].values.astype(np.float32), y,
        test_size=args.test_size, stratify=y, random_state=args.seed
    )

    mu = Xtr.mean(axis=0); sigma = Xtr.std(axis=0) + 1e-6
    Xtr_n = (Xtr - mu)/sigma
    Xte_n = (Xte - mu)/sigma

    cw = None if args.class_weight=="none" else "balanced"
    if args.model=="logistic":
        clf = LogisticRegression(max_iter=600, solver="lbfgs", class_weight=cw, multi_class="auto")
    else:
        clf = Perceptron(max_iter=2000, early_stopping=True, validation_fraction=0.1, class_weight=cw)

    clf.fit(Xtr_n, ytr)
    ypr = clf.predict(Xte_n)

    print("\n=== After relabel/balance/stack ===")
    print("Report:\n", classification_report(yte, ypr, digits=4))
    print("CM:\n", confusion_matrix(yte, ypr))

    # ---- 정수화/내보내기 ----
    Sx = int(args.sx); Sw = float(args.sw)
    Wf = clf.coef_.astype(np.float32)     # shape: [N_CLASS, N_FEAT]
    bf = clf.intercept_.astype(np.float32)
    W_int8 = np.clip(np.round(Wf * Sw), -128, 127).astype(np.int8)
    B_int  = np.round(bf * Sw).astype(np.int32)
    MEAN   = np.round(mu).astype(np.int16)
    SCALE  = np.round((2**Sx) / sigma).astype(np.int16)

    N_CLASS, N_FEAT = W_int8.shape
    with open(args.out, "w", encoding="utf-8") as f:
        f.write("#pragma once\n\n")
        f.write(f"#define N_CLASS {N_CLASS}\n\n")
        f.write(f"#define N_FEAT {N_FEAT}\n\n")
        f.write(f"#define Sx {Sx}\n\n")
        f.write("// class order per meta.json\n\n")
        f.write(to_c_array("W", W_int8, "int8_t"))
        f.write("\n")
        f.write(to_c_array("B", B_int, "int32_t"))
        f.write("\n")
        f.write(to_c_array("MEAN", MEAN, "int16_t"))
        f.write("\n")
        f.write(to_c_array("SCALE", SCALE, "int16_t"))
        f.write("\n#define MODEL_VER 0x0201\n")

    meta = {
        "feature_order": feat_names,       # 스택 적용 시: feats × [t-1,t,t+1]
        "base_features": ["rms_u8","vpp_u8","zcr_u8"],
        "freq_bins": [1100,1200,2200,2300,800],
        "class_order": CLASSES,
        "Sx": Sx, "Sw": Sw,
        "relabel": {"vad_thresh": args.vad_thresh, "top_pct": args.top_pct, "pos_classes": [1,2,3,4] if args.apply_vad else None},
        "stack": args.stack,
        "note": "xi=((x-MEAN)*SCALE)>>Sx; score=W*xi+B; argmax"
    }
    meta_path = os.path.splitext(args.out)[0]+".meta.json"
    with open(meta_path,"w",encoding="utf-8") as f:
        json.dump(meta, f, ensure_ascii=False, indent=2)

    print(f"\nExported: {args.out}")
    print(f"Meta: {meta_path}")
    print(f"N_CLASS={N_CLASS}, N_FEAT={N_FEAT}  (stack={args.stack} → {len(FEATS_K5)}*{args.stack})")

if __name__ == "__main__":
    main()
