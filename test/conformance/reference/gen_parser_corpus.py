"""Generate the fixed-point parser corpus.

ParseFixed is the boundary where every exchange's decimal strings become
integers, so it is the one function where a silent bug corrupts every number
downstream while looking entirely healthy. It is also small enough to check
exhaustively against an independent implementation, which is what this does.

Each case records what the reference parser does with an input. The C++ test
asserts ParseFixed agrees on BOTH the verdict and the value.

Run:  python3 gen_parser_corpus.py
"""

import json
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import aggregate_ref as R                                    # noqa: E402

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "fixtures")
INT64_MAX = 2**63 - 1

# INT64_MAX is exactly 92233720368.54775807 at 1e8, so the representable range
# ends mid-decimal and the boundary is worth pinning from both sides.
MAX_WHOLE = INT64_MAX // R.SCALE          # 92233720368
MAX_FRAC = INT64_MAX % R.SCALE            # 54775807

CASES = [
    # -- ordinary values, the shapes the three venues actually emit
    "0", "1", "10", "1.5", "0.1", "0.01", "100000.25", "76547.58",
    "0.00000001", "0.00010000", "123456.78901234", "99999.99999999",
    # -- signs
    "-1.5", "-0.00000001", "-0", "+1.5", "+0",
    # -- elided parts
    ".5", "1.", "0.", ".0",
    # -- leading zeros, which several venues do emit
    "007", "0000.5", "00000000.00000001",
    # -- truncation past the 8th decimal, documented behaviour
    "1.234567891", "1.2345678999999", "0.000000019", "0.000000001",
    # -- the exact representable boundary, and one unit past it
    f"{MAX_WHOLE}.{MAX_FRAC:08d}",
    f"{MAX_WHOLE}.{MAX_FRAC + 1:08d}",
    f"{MAX_WHOLE}.99999999",
    f"{MAX_WHOLE}",
    f"{MAX_WHOLE + 1}",
    f"{MAX_WHOLE + 1}.0",
    # -- integer-part overflow well past the boundary
    "99999999999999999999", "123456789012345678901234567890",
    # -- not numbers at all
    "", ".", "-", "+", "-.", "+.", "abc", "1a", "a1", "1.2.3", "1,5",
    " 1", "1 ", " ", "--1", "1-", "1+1", "0x10", "NaN", "inf", "Infinity",
    "null", "true",
    # -- exponent notation: rejected rather than guessed at
    "1e5", "1E5", "1.5e3", "1.5E-3", "1e", "e5",
    # -- unicode and control characters that can appear in a corrupted frame
    "１", "1 ", "1\n", "1\t", "1\0",
]

# Random well-formed decimals across the whole representable range, plus the
# price/quantity magnitudes these venues actually use.
rng = random.Random(20260917)
for _ in range(400):
    whole = rng.choice([
        rng.randint(0, 9),
        rng.randint(0, 200_000),
        rng.randint(0, MAX_WHOLE),
    ])
    frac = rng.randint(0, R.SCALE - 1)
    digits = rng.randint(0, 12)
    text = f"{whole}.{frac:08d}" if digits >= 8 else f"{whole}.{frac // 10 ** (8 - digits):0{digits}d}"
    if digits == 0:
        text = str(whole)
    if rng.random() < 0.2:
        text = "-" + text
    CASES.append(text)


def verdict(text):
    try:
        value = R.parse_e8(text)
    except Exception:
        return {"in": text, "ok": False, "out": None}
    if not (-INT64_MAX - 1 <= value <= INT64_MAX):
        return {"in": text, "ok": False, "out": None}
    return {"in": text, "ok": True, "out": value}


if __name__ == "__main__":
    seen = set()
    cases = []
    for text in CASES:
        if text in seen:
            continue
        seen.add(text)
        cases.append(verdict(text))

    accepted = sum(1 for c in cases if c["ok"])
    doc = {
        "name": "parser_corpus",
        "description": (
            "ParseFixed differential corpus. Verdicts come from the independent "
            "reference parser in aggregate_ref.py; the C++ test asserts "
            "ParseFixed agrees on both the accept/reject decision and the value."
        ),
        "scale": R.SCALE,
        "int64_max_as_decimal": f"{MAX_WHOLE}.{MAX_FRAC:08d}",
        "cases": cases,
    }
    path = os.path.join(OUT, "parser_corpus.json")
    with open(path, "w") as f:
        json.dump(doc, f, indent=2, ensure_ascii=False)
        f.write("\n")
    print(f"{len(cases)} cases -> {os.path.normpath(path)} ({accepted} accepted, "
          f"{len(cases) - accepted} rejected)")
