"""Generate the demo deck's videos (no audio) and animate frames. Needs numpy, matplotlib, ffmpeg.

    python3 make_media.py        # skips anything that already exists
"""
import os
import subprocess

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

W, H, FPS = 1280, 720, 30
os.makedirs("videos", exist_ok=True)
os.makedirs("frames", exist_ok=True)


def encoder(path):
    """ffmpeg reading raw RGB frames on stdin, writing H.264 that plays everywhere."""
    return subprocess.Popen(
        ["ffmpeg", "-loglevel", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", f"{W}x{H}",
         "-r", str(FPS), "-i", "-", "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "20",
         "-movflags", "+faststart", path],
        stdin=subprocess.PIPE)


def poster(name, at=0.5):
    subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-ss", str(at), "-i", f"videos/{name}.mp4",
                    "-frames:v", "1", f"videos/{name}-poster.png"], check=True)


def waves(path, seconds=10):
    """Two point sources interfering: bright fringes to circle with the pen."""
    y, x = np.mgrid[0:H, 0:W].astype(np.float32)
    srcs = [(W * 0.38, H * 0.5), (W * 0.62, H * 0.5)]
    r = [np.hypot(x - sx, y - sy) + 1 for sx, sy in srcs]
    cmap = plt.get_cmap("twilight_shifted")
    p = encoder(path)
    for f in range(seconds * FPS):
        t = f / FPS
        z = sum(np.sin(0.09 * ri - 2 * np.pi * 0.8 * t) / np.sqrt(ri / 40) for ri in r)
        rgb = (cmap(np.clip(z * 0.35 + 0.5, 0, 1))[..., :3] * 255).astype(np.uint8)
        p.stdin.write(rgb.tobytes())
    p.stdin.close(); p.wait()


def vortex_pair(path, seconds=12):
    """Unequal counter-rotating vortex pair (Lamb-Oseen cores) stirring two clouds of tracers."""
    rng = np.random.default_rng(3)
    gam = np.array([1.0, -0.6])                    # unequal strengths: the pair curves
    pos = np.array([[-0.5, 0.0], [0.5, 0.0]])
    core = 0.12
    n = 9000
    tr = np.concatenate([pos[0] + rng.normal(0, 0.42, (n, 2)), pos[1] + rng.normal(0, 0.42, (n, 2))])
    color = np.r_[np.zeros(n), np.ones(n)]

    def vel(pts, vort, g):
        v = np.zeros_like(pts)
        for (vx, vy), gi in zip(vort, g):
            dx, dy = pts[:, 0] - vx, pts[:, 1] - vy
            r2 = dx * dx + dy * dy + 1e-9
            k = gi / (2 * np.pi * r2) * (1 - np.exp(-r2 / core ** 2))
            v[:, 0] += -k * dy
            v[:, 1] += k * dx
        return v

    fig = plt.figure(figsize=(W / 100, H / 100), dpi=100, facecolor="#0b0c24")
    ax = fig.add_axes([0, 0, 1, 1]); ax.set_facecolor("#0b0c24")
    p = encoder(path)
    dt = 0.05
    for f in range(seconds * FPS):
        for _ in range(2):                         # RK2 for tracers and vortices
            k1t, k1v = vel(tr, pos, gam), np.array([vel(pos[[i]], pos[[1 - i]], gam[[1 - i]])[0] for i in (0, 1)])
            tm, vm = tr + 0.5 * dt * k1t, pos + 0.5 * dt * k1v
            tr = tr + dt * vel(tm, vm, gam)
            pos = pos + dt * np.array([vel(vm[[i]], vm[[1 - i]], gam[[1 - i]])[0] for i in (0, 1)])
        cx, cy = pos.mean(axis=0)
        ax.clear(); ax.set_facecolor("#0b0c24"); ax.set_axis_off()
        ax.scatter(tr[:, 0], tr[:, 1], c=color, cmap="cool", s=1.8, alpha=0.6, linewidths=0)
        ax.set_xlim(cx - 2.4, cx + 2.4); ax.set_ylim(cy - 1.35, cy + 1.35)
        fig.canvas.draw()
        p.stdin.write(np.asarray(fig.canvas.buffer_rgba())[..., :3].tobytes())
    p.stdin.close(); p.wait(); plt.close(fig)


def turing(path, seconds=12):
    """Gray-Scott reaction-diffusion: coral-like patterns growing from a few seeds."""
    h, w = H // 2, W // 2                       # simulate at half size, upscale when encoding
    rng = np.random.default_rng(7)
    u = np.ones((h, w), np.float32); v = np.zeros((h, w), np.float32)
    for _ in range(14):
        y, x = rng.integers(20, h - 20), rng.integers(20, w - 20)
        u[y - 6:y + 6, x - 6:x + 6] = 0.5; v[y - 6:y + 6, x - 6:x + 6] = 0.25
    f, k, du, dv = 0.0545, 0.062, 0.16, 0.08    # "coral" regime: grows to fill the frame

    def lap(z):
        return np.roll(z, 1, 0) + np.roll(z, -1, 0) + np.roll(z, 1, 1) + np.roll(z, -1, 1) - 4 * z

    cmap = plt.get_cmap("magma")
    p = encoder(path)
    for frame in range(seconds * FPS):
        for _ in range(28):
            uvv = u * v * v
            u += du * lap(u) - uvv + f * (1 - u)
            v += dv * lap(v) + uvv - (f + k) * v
        img = (cmap(np.clip(v * 2.4, 0, 1))[..., :3] * 255).astype(np.uint8)
        p.stdin.write(img.repeat(2, axis=0).repeat(2, axis=1).tobytes())
    p.stdin.close(); p.wait()


def fourier_frames(n=48):
    """Epicycles drawing a square wave from its first four odd harmonics (for animate)."""
    harmonics = [1, 3, 5, 7]
    ts = np.linspace(0, 2 * np.pi, 400)
    for f in range(n):
        t = 2 * np.pi * f / n
        fig = plt.figure(figsize=(8, 3.6), dpi=110, facecolor="white")
        ax = fig.add_axes([0, 0, 1, 1]); ax.set_axis_off(); ax.set_aspect("equal")
        x, y = 0.0, 0.0
        for k in harmonics:
            r = 4 / (k * np.pi)
            ax.add_patch(plt.Circle((x, y), r, fill=False, color="#7B4DFF", alpha=0.35, lw=1.2))
            nx, ny = x + r * np.cos(k * t), y + r * np.sin(k * t)
            ax.plot([x, nx], [y, ny], color="#23246B", lw=1.6)
            x, y = nx, ny
        # traced wave to the right, newest sample at the left edge
        past = t - ts[ts <= 2 * np.pi]
        wave = sum(4 / (k * np.pi) * np.sin(k * past) for k in harmonics)
        xs = 2.2 + (t - past) * 0.75
        ax.plot(xs, wave, color="#3D7BFF", lw=2.2)
        ax.plot([x, 2.2], [y, y], color="#23246B", lw=0.8, ls="--")
        ax.plot(2.2, y, "o", color="#3D7BFF", ms=5)
        ax.set_xlim(-1.6, 7.0); ax.set_ylim(-1.6, 1.6)
        fig.savefig(f"frames/fourier-{f}.png"); plt.close(fig)


for name, make in [("waves", waves), ("vortex_pair", vortex_pair), ("turing", turing)]:
    if not os.path.exists(f"videos/{name}.mp4"):
        print("rendering", name); make(f"videos/{name}.mp4")
    poster(name, at=9 if name == "turing" else 0.5)   # the pattern has grown by then
if not os.path.exists("frames/fourier-47.png"):
    print("rendering fourier frames"); fourier_frames()
