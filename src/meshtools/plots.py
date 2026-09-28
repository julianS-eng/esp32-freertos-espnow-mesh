"""Static figures and the animated replay GIF used in the documentation.

Design rules (see the project's data-viz guidelines): one y-axis per chart,
thin 2 px lines, recessive grid, categorical colours assigned to nodes in a
fixed order (never by rank), a single-hue sequential ramp for magnitude,
reserved status colours for liveness states (always with a text label), and
legends plus direct labels so identity never depends on colour alone.
"""

from __future__ import annotations

import math
from collections.abc import Mapping, Sequence
from pathlib import Path

import matplotlib as mpl

mpl.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.animation import FuncAnimation, PillowWriter
from matplotlib.axes import Axes
from matplotlib.colors import LinearSegmentedColormap
from matplotlib.figure import Figure
from matplotlib.patches import Patch

from meshtools.metrics import Activity, MeshMetrics, percentile

# Reference palette (validated: adjacent CVD dE >= 9.1, normal-vision dE >= 19.6).
SURFACE = "#fcfcfb"
TEXT = "#0b0b0b"
TEXT_2 = "#52514e"
GRID = "#e4e3df"
CATEGORICAL = ("#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#008300", "#4a3aa7", "#e34948")
BLUE_RAMP = ("#fcfcfb", "#cde2fb", "#9ec5f4", "#6da7ec", "#3987e5", "#256abf", "#184f95", "#0d366b")
STATUS = {"online": "#0ca30c", "suspect": "#fab219", "offline": "#d03b3b"}


def node_color(index: int) -> str:
    """Colour of the node at position ``index`` in node-id order (fixed, never cycled)."""
    if index >= len(CATEGORICAL):
        raise ValueError("more than 8 series: facet or fold into 'other' instead of cycling colours")
    return CATEGORICAL[index]


def _style(ax: Axes, title: str, xlabel: str, ylabel: str) -> None:
    ax.set_facecolor(SURFACE)
    ax.set_title(title, loc="left", color=TEXT, fontsize=11, fontweight="bold")
    ax.set_xlabel(xlabel, color=TEXT_2)
    ax.set_ylabel(ylabel, color=TEXT_2)
    ax.tick_params(colors=TEXT_2, labelsize=9)
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color(GRID)
    ax.grid(True, color=GRID, linewidth=0.8)
    ax.set_axisbelow(True)


def _figure(w: float, h: float, nrows: int = 1, ncols: int = 1) -> tuple[Figure, list[Axes]]:
    fig, axes = plt.subplots(nrows, ncols, figsize=(w, h), squeeze=False)
    fig.patch.set_facecolor(SURFACE)
    return fig, [a for row in axes for a in row]


def _save(fig: Figure, path: Path) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    fig.tight_layout()
    fig.savefig(path, dpi=120, facecolor=SURFACE)
    plt.close(fig)
    return path


def activity_heatmap(act: Activity, path: Path, subtitle: str = "") -> Path:
    """Frames accepted per node per time bin (sequential single-hue ramp)."""
    counts = act.accepted
    bin_s = act.bin_s
    nodes = sorted(counts)
    fig, (ax,) = _figure(10, 0.5 * len(nodes) + 1.8)
    data = [counts[n] for n in nodes]
    vmax = max((max(row) for row in data if row), default=1)
    cmap = LinearSegmentedColormap.from_list("seq_blue", BLUE_RAMP)
    extent = (0.0, len(data[0]) * bin_s if data else bin_s, len(nodes) - 0.5, -0.5)
    im = ax.imshow(data, aspect="auto", cmap=cmap, vmin=0, vmax=vmax, extent=extent, interpolation="nearest")
    _style(ax, f"Gateway activity: frames accepted per node per {bin_s:.0f} s{subtitle}", "time (s)", "")
    ax.grid(False)
    ax.set_yticks(range(len(nodes)), [f"node {n}" for n in nodes])
    cb = fig.colorbar(im, ax=ax, pad=0.01)
    cb.set_label("frames / bin", color=TEXT_2)
    cb.ax.tick_params(colors=TEXT_2, labelsize=8)
    cb.outline.set_visible(False)
    return _save(fig, path)


def state_timeline(mm: MeshMetrics, path: Path) -> Path:
    """Liveness state of every node over time (status colours + labels)."""
    nodes = sorted(mm.nodes)
    fig, (ax,) = _figure(10, 0.45 * len(nodes) + 1.9)
    t0 = mm.first_ts or 0
    end = (mm.last_ts - t0) / 1000.0
    for row, nid in enumerate(nodes):
        nm = mm.nodes[nid]
        start = ((nm.first_ts or t0) - t0) / 1000.0
        state = "online"
        for tr in nm.transitions:
            t = (tr.ts - t0) / 1000.0
            ax.barh(
                row, t - start, left=start, height=0.6, color=STATUS[state], edgecolor=SURFACE, linewidth=2
            )
            start, state = t, tr.to_state
        ax.barh(row, end - start, left=start, height=0.6, color=STATUS[state], edgecolor=SURFACE, linewidth=2)
    _style(ax, "Node liveness as decided by the gateway", "time (s)", "")
    ax.set_yticks(range(len(nodes)), [f"node {n}" for n in nodes])
    ax.invert_yaxis()
    ax.set_xlim(0, end)
    ax.grid(axis="y", visible=False)
    handles = [Patch(color=c, label=s) for s, c in STATUS.items()]
    ax.legend(handles=handles, loc="upper center", bbox_to_anchor=(0.5, -0.22), ncol=3, frameon=False)
    return _save(fig, path)


def loss_and_retries(mm: MeshMetrics, path: Path) -> Path:
    """Two small multiples: frames needing a retry, and frames lost after all retries (%)."""
    nodes = sorted(mm.nodes)
    retried = []
    lost = []
    for n in nodes:
        nm = mm.nodes[n]
        frames = nm.data + nm.heartbeats
        retried.append(100.0 * nm.retried_ok / frames if frames else 0.0)
        lost.append(100.0 * nm.loss_ratio)
    fig, axes = _figure(10, 3.4, 1, 2)
    labels = [str(n) for n in nodes]
    for ax, vals, title in (
        (axes[0], retried, "Delivered only after a retry (% of frames)"),
        (axes[1], lost, "Lost after all retries (% of frames)"),
    ):
        bars = ax.bar(labels, vals, color=CATEGORICAL[0], width=0.6, edgecolor=SURFACE, linewidth=2)
        _style(ax, title, "node", "%")
        ax.grid(axis="x", visible=False)
        top = max(vals) if vals else 1.0
        ax.set_ylim(0, top * 1.25 if top > 0 else 1.0)
        for b, v in zip(bars, vals, strict=True):
            ax.annotate(
                f"{v:.2f}",
                (b.get_x() + b.get_width() / 2, v),
                ha="center",
                va="bottom",
                fontsize=8,
                color=TEXT_2,
                xytext=(0, 2),
                textcoords="offset points",
            )
    return _save(fig, path)


def _cdf(ax: Axes, series: Mapping[int, Sequence[float]]) -> None:
    for i, (nid, vals) in enumerate(sorted(series.items())):
        if not vals:
            continue
        xs = sorted(vals)
        ys = [(k + 1) / len(xs) for k in range(len(xs))]
        ax.step(xs, ys, where="post", color=node_color(i), linewidth=2, label=f"node {nid}")
    ax.set_ylim(0, 1.02)


def latency_cdfs(mm: MeshMetrics, path: Path) -> Path:
    """CDFs of link RTT and of delivery delay (above best case), per node."""
    fig, axes = _figure(11, 3.8, 1, 2)
    rtt = {n: [v / 1000.0 for v in mm.nodes[n].rtt_us] for n in sorted(mm.nodes)}
    delay = {n: [float(v) for v in mm.nodes[n].delay_ms] for n in sorted(mm.nodes)}
    _cdf(axes[0], rtt)
    _style(axes[0], "Link RTT (node-measured, Karn-safe)", "RTT (ms)", "fraction of frames")
    _cdf(axes[1], delay)
    axes[1].set_xscale("symlog", linthresh=10)
    _style(
        axes[1], "Delivery delay above best case (incl. retries)", "delay (ms, symlog)", "fraction of frames"
    )
    all_rtt = [v for vs in rtt.values() for v in vs]
    all_delay = [v for vs in delay.values() for v in vs]
    for ax, vals in ((axes[0], all_rtt), (axes[1], all_delay)):
        p95 = percentile(vals, 95)
        if not math.isnan(p95):
            ax.axvline(p95, color=TEXT_2, linewidth=1, linestyle=":")
            ax.annotate(
                f"p95 = {p95:.1f} ms",
                (p95, 0.08),
                color=TEXT_2,
                fontsize=8,
                xytext=(4, 0),
                textcoords="offset points",
            )
    axes[1].legend(loc="lower right", frameon=False, fontsize=8)
    return _save(fig, path)


def replay_gif(mm: MeshMetrics, act: Activity, path: Path, frames: int = 60, fps: int = 6) -> Path:
    """Animated replay: rolling per-node activity plus cumulative loss, like the live dashboard."""
    nodes = sorted(act.accepted)
    n_bins = max(1, act.n_bins)
    bin_s = act.bin_s
    fig, axes = _figure(8.0, 4.2, 1, 2)
    ax_act, ax_loss = axes
    total_frames = min(frames, n_bins)
    y_max = max((max(v) for v in act.accepted.values()), default=1) * 1.15 + 1

    def draw(k: int) -> None:
        idx = max(1, round((k + 1) * n_bins / total_frames))
        ax_act.clear()
        ax_loss.clear()
        t = [i * bin_s for i in range(idx)]
        for i, n in enumerate(nodes):
            ax_act.plot(t, act.accepted[n][:idx], color=node_color(i), linewidth=2, label=f"node {n}")
        _style(ax_act, f"Frames accepted per {bin_s:.0f} s", "time (s)", "frames")
        ax_act.set_xlim(0, n_bins * bin_s)
        ax_act.set_ylim(0, y_max)
        ax_act.legend(loc="lower left", fontsize=7, frameon=False, ncol=3)
        loss = []
        for n in nodes:
            acc = sum(act.accepted[n][:idx])
            lost = max(0, sum(act.lost[n][:idx]))
            loss.append(100.0 * lost / (acc + lost) if acc + lost else 0.0)
        bars = ax_loss.bar([str(n) for n in nodes], loss, color=CATEGORICAL[0], width=0.6)
        _style(ax_loss, "Lost after retries (%)", "node", "%")
        ax_loss.grid(axis="x", visible=False)
        ax_loss.set_ylim(0, max(5.0, max(loss) * 1.3))
        for b, v in zip(bars, loss, strict=True):
            ax_loss.annotate(
                f"{v:.1f}",
                (b.get_x() + b.get_width() / 2, v),
                ha="center",
                va="bottom",
                fontsize=7,
                color=TEXT_2,
                xytext=(0, 2),
                textcoords="offset points",
            )
        fig.suptitle(
            f"Replay of the gateway log   t = {idx * bin_s:.0f} s   ({len(mm.nodes)} nodes)",
            color=TEXT,
            fontsize=10,
            x=0.02,
            ha="left",
        )
        fig.tight_layout()

    anim = FuncAnimation(fig, draw, frames=total_frames, interval=1000 // fps)
    path.parent.mkdir(parents=True, exist_ok=True)
    anim.save(str(path), writer=PillowWriter(fps=fps), dpi=80, savefig_kwargs={"facecolor": SURFACE})
    plt.close(fig)
    return path
