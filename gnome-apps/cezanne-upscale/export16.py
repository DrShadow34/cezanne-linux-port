"""Quantise x4plus to int16 (quant16) and write what the MDLA runner
(x4plus_mdla.py) needs: per layer the packed weights (MDLA int16 order,
output channels padded to 16), int32 bias, Q31 multiplier / shift and the
PReLU stage parameters, as one .npz.
  export16.py <model base (.param / .bin)> calibration.json out.npz"""
import json, sys
import numpy as np
import esrgan, quant16
from mdlacmd import pack_conv_weights16, wblock

net = esrgan.load(sys.argv[1])
cal = {k: tuple(v) for k, v in json.load(open(sys.argv[2])).items()}
L = quant16.quantise(net, cal)
meta, arrays = [], {}
for i, (name, l) in enumerate(L.items()):
    co, k, _, ci = l.wq.shape
    cop = -(-co // 16) * 16
    wq = np.zeros((cop, k, k, ci), np.int64); wq[:co] = l.wq
    bq = np.zeros(cop, np.int64); bq[:co] = l.bq
    assert np.abs(wq).max() <= 32767
    arrays[f'w{i}'] = np.frombuffer(pack_conv_weights16(wq, wblock(k, -(-ci // 2) * 2, 2)), np.uint8)
    arrays[f'b{i}'] = bq.astype('<i4')
    meta.append(dict(name=name, K=k, CI=ci, CO=cop, mult=l.mult, sh=l.sh,
                     prelu=list(l.prelu) if l.prelu else None))
arrays['meta'] = np.frombuffer(json.dumps(meta).encode(), np.uint8)
np.savez(sys.argv[3], **arrays)
print(len(meta), 'layers,', sum(a.nbytes for k, a in arrays.items() if k[0] == 'w') / 2 ** 20, 'MiB weights')
