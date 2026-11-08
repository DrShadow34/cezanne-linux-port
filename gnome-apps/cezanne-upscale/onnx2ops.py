"""U^2-Net (rembg's u2net.onnx, BN folded) -> a flat op list for the MDLA
runner, and its float reference in numpy (checked against onnxruntime).

    onnx2ops.py convert u2netp.onnx u2netp-f32.npz   (the package build)
    onnx2ops.py u2netp.onnx u2netp-f32.npz           + compare with onnxruntime

Ops (tensors NHWC float, batch 1, by name):
    conv     in, out, w [co][kh][kw][ci], b, dil, relu
    pool     in, out                 (max 2x2 stride 2, ceil)
    resize   in, out, like           (bilinear, half-pixel centres, to the
                                      size of tensor `like`: _upsample_like)
    concat   ins, out                (channels)
    add      ins, out
    sigmoid  in, out
    combine  ins, out, relu          (seg16.split: the partial convs of one
                                      conv summed, + its bias '<out>.bias')
A conv may read a channel slice of its input: c0 (first channel), ci.
The Shape / Gather / Slice arithmetic of the dynamic resizes is traced back
to the tensor whose shape it takes."""
import json
import sys

import numpy as np


def bilinear(x, size):
    """x [H][W][C] -> [h][w][C], align_corners=False (half-pixel), as
    PyTorch / ONNX Resize linear pytorch_half_pixel"""
    H, W = x.shape[:2]
    h, w = size

    def axis(n_in, n_out):
        if n_out == 1:
            src = np.zeros(1)
        else:
            src = (np.arange(n_out) + 0.5) * n_in / n_out - 0.5
        src = np.clip(src, 0, None)
        i0 = np.minimum(np.floor(src).astype(int), n_in - 1)
        i1 = np.minimum(i0 + 1, n_in - 1)
        f = (src - i0).astype(np.float32)
        return i0, i1, f
    y0, y1, fy = axis(H, h)
    x0, x1, fx = axis(W, w)
    r = x[y0] * (1 - fy)[:, None, None] + x[y1] * fy[:, None, None]
    return r[:, x0] * (1 - fx)[None, :, None] + r[:, x1] * fx[None, :, None]


def maxpool(x):
    H, W, C = x.shape
    h, w = -(-H // 2), -(-W // 2)
    low = -np.inf if np.issubdtype(x.dtype, np.floating) else np.iinfo(x.dtype).min
    p = np.full((2 * h, 2 * w, C), low, x.dtype)
    p[:H, :W] = x
    return p.reshape(h, 2, w, 2, C).max(axis=(1, 3))


def conv(x, w, b, dil):
    """x [H][W][ci], w [co][k][k][ci], same padding (= dil for 3x3)"""
    k = w.shape[1]
    if k == 1:
        return x @ w[:, 0, 0, :].T + b
    H, W, ci = x.shape
    p = dil
    xp = np.zeros((H + 2 * p, W + 2 * p, ci), x.dtype)
    xp[p:p + H, p:p + W] = x
    y = np.zeros((H * W, w.shape[0]), np.float64)
    for ky in range(3):
        for kx in range(3):
            y += xp[ky * dil:ky * dil + H, kx * dil:kx * dil + W].reshape(-1, ci) @ \
                w[:, ky, kx, :].T.astype(np.float64)
    return (y.reshape(H, W, -1) + b).astype(np.float32)


def last_uses(ops):
    """tensor -> index of the last op reading it (resize targets: shape only)"""
    last = {}
    for i, op in enumerate(ops):
        for name in op.get('ins', [op.get('in')]) + ([op['like']] if op['op'] == 'resize' else []):
            last[name] = i
    return last


def forward(ops, weights, x, on_tensor=None, keep=True):
    """float reference: x [H][W][3] (normalised) -> {name: tensor};
    on_tensor(name, value) for each result; keep=False: tensors are freed
    after their last use (the result holds the last one)"""
    t = {ops[0]['in']: x}
    last = None if keep else last_uses(ops)
    for i, op in enumerate(ops):
        k = op['op']
        if k == 'conv':
            x = t[op['in']]
            if 'c0' in op:
                x = x[..., op['c0']:op['c0'] + op['ci']]
            y = conv(x, weights[op['out'] + '.w'], weights[op['out'] + '.b'], op['dil'])
            t[op['out']] = np.maximum(y, 0) if op['relu'] else y
        elif k == 'combine':
            y = sum(t[i] for i in op['ins']) + weights[op['out'] + '.bias']
            t[op['out']] = np.maximum(y, 0) if op['relu'] else y
        elif k == 'pool':
            t[op['out']] = maxpool(t[op['in']])
        elif k == 'resize':
            t[op['out']] = bilinear(t[op['in']], t[op['like']].shape[:2])
        elif k == 'concat':
            t[op['out']] = np.concatenate([t[i] for i in op['ins']], axis=2)
        elif k == 'add':
            t[op['out']] = t[op['ins'][0]] + t[op['ins'][1]]
        elif k == 'sigmoid':
            t[op['out']] = 1 / (1 + np.exp(-t[op['in']]))
        if on_tensor:
            on_tensor(op['out'], t[op['out']])
        if last is not None:
            for name in [n for n in t if last.get(n, -1) <= i and n != op['out']]:
                del t[name]
    return t


def convert(path):
    import onnx
    from onnx import numpy_helper, shape_inference
    model = shape_inference.infer_shapes(onnx.load(path))
    g = model.graph
    shapes = {v.name: [d.dim_value for d in v.type.tensor_type.shape.dim]
              for v in list(g.value_info) + list(g.output) + list(g.input)}
    init = {t.name: numpy_helper.to_array(t) for t in g.initializer}
    consts = {}
    for n in g.node:
        if n.op_type == 'Constant':
            consts[n.output[0]] = True
    inp = [i.name for i in g.input if i.name not in init][0]
    real = {inp}			# NCHW activations
    ops, weights = [], {}
    users = {}
    for n in g.node:
        for i in n.input:
            users.setdefault(i, []).append(n)
    alias = {}				# conv output -> relu output (fused)
    producer = {o: n for n in g.node for o in n.output}

    def shape_sources(name, found):
        """the activations whose Shape feeds a resize's sizes"""
        n = producer.get(name)
        if n is None:
            return found
        if n.op_type == 'Shape':
            found.add(n.input[0])
            return found
        for i in n.input:
            shape_sources(i, found)
        return found
    for n in g.node:
        a = {x.name: onnx.helper.get_attribute_value(x) for x in n.attribute}
        ins = [alias.get(i, i) for i in n.input]
        if n.op_type == 'Conv':
            w = init[n.input[1]].astype(np.float32)
            b = init[n.input[2]].astype(np.float32) if len(n.input) > 2 else np.zeros(w.shape[0], np.float32)
            dil = a.get('dilations', [1, 1])[0]
            assert a.get('group', 1) == 1 and a.get('strides', [1, 1]) == [1, 1], n.name
            assert w.shape[2] in (1, 3) and a.get('pads', [dil] * 4)[0] == (dil if w.shape[2] == 3 else 0), n.name
            out = n.output[0]
            relu = False
            us = users.get(out, [])
            if len(us) == 1 and us[0].op_type == 'Relu':
                relu = True
                alias[out] = us[0].output[0]
                out = us[0].output[0]
            ops.append(dict(op='conv', **{'in': ins[0]}, out=out, dil=dil, relu=relu,
                            ci=int(w.shape[1]), co=int(w.shape[0]), k=int(w.shape[2])))
            weights[out + '.w'] = w.transpose(0, 2, 3, 1)	# [co][kh][kw][ci]
            weights[out + '.b'] = b
            real.add(out)
        elif n.op_type == 'Relu':
            if n.input[0] in alias:		# fused into its conv
                continue
            raise ValueError('lone relu ' + n.name)
        elif n.op_type == 'MaxPool':
            assert a['kernel_shape'] == [2, 2] and a['strides'] == [2, 2], n.name
            ops.append(dict(op='pool', **{'in': ins[0]}, out=n.output[0]))
            real.add(n.output[0])
        elif n.op_type == 'Resize':
            assert a.get('mode') == b'linear' and a.get('coordinate_transformation_mode') in (
                b'pytorch_half_pixel', b'half_pixel'), n.name
            # sizes = N, C of the input and H, W of the target
            srcs = shape_sources(n.input[3] if len(n.input) > 3 and n.input[3] else n.input[2], set())
            srcs = {alias.get(x, x) for x in srcs} - {ins[0]}
            assert len(srcs) == 1 and srcs <= real, (n.name, srcs)
            like = srcs.pop()
            ops.append(dict(op='resize', **{'in': ins[0]}, out=n.output[0], like=alias.get(like, like)))
            real.add(n.output[0])
        elif n.op_type == 'Concat' and all(i in real for i in ins):
            assert a['axis'] == 1, n.name
            ops.append(dict(op='concat', ins=ins, out=n.output[0]))
            real.add(n.output[0])
        elif n.op_type == 'Add' and all(i in real for i in ins):
            ops.append(dict(op='add', ins=ins, out=n.output[0]))
            real.add(n.output[0])
        elif n.op_type == 'Sigmoid':
            ops.append(dict(op='sigmoid', **{'in': ins[0]}, out=n.output[0]))
            real.add(n.output[0])
    outputs = [o.name for o in g.output]
    return inp, ops, weights, outputs


def save(dst, inp, ops, weights, outputs):
    np.savez(dst, ops=np.frombuffer(json.dumps(dict(input=inp, outputs=outputs, ops=ops)).encode(), np.uint8),
             **weights)


def main():
    if sys.argv[1] == 'convert':
        save(sys.argv[3], *convert(sys.argv[2]))
        return
    import onnxruntime as ort
    src, dst = sys.argv[1:3]
    inp, ops, weights, outputs = convert(src)
    print(len(ops), 'ops:', {k: sum(o['op'] == k for o in ops) for k in
                             ('conv', 'pool', 'resize', 'concat', 'add', 'sigmoid')})
    rng = np.random.default_rng(0)
    x = rng.standard_normal((320, 320, 3)).astype(np.float32)
    sess = ort.InferenceSession(src, providers=['CPUExecutionProvider'])
    ref = sess.run(None, {inp: x.transpose(2, 0, 1)[None]})
    t = forward(ops, weights, x)
    for name, r in zip(outputs, ref):
        d = np.abs(t[name] - r[0].transpose(1, 2, 0)).max()
        print(f'{name}: max |numpy - onnxruntime| {d:.2e}')
    save(dst, inp, ops, weights, outputs)


if __name__ == '__main__':
    main()
