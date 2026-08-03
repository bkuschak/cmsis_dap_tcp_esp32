#!/usr/bin/env python3
"""
Connect to the cmsis_dap_tcp_esp32 ADC streaming server (CONFIG_ESP_ADC_STREAM_ENABLED)
and plot the incoming measurements live.

Supports both wire formats the firmware can produce (main/adc_stream.c):
  - text:      integer mV, newline-terminated
  - binary16:  16-bit little-endian signed mV

Usage:
    python3 adc_stream_plot.py <host> [--port 4451] [--format auto|text|binary16]
                               [--window 500] [--rate HZ]
                               [--secondary-label LABEL --secondary-slope SLOPE
                                --secondary-offset OFFSET]

If the ADC is reading something other than a voltage directly (e.g. current
via a shunt resistor and gain stage), --secondary-label adds a second y-axis
on the right showing secondary = slope * mv + offset, tracking the primary
(mV) axis's autoscaling.

Example:
    The ADC measures a current sense amplifier. The CSA outputs 66 mV per mA,
    and is biased 65mV above ground. So output (mV) = 66 * mA + 65.
      - slope = 1 / (66 mV/mA) = 1/66 ≈ 0.0151515152 mA/mV
      - offset = −65 mV × slope = −65/66 ≈ −0.9848485 mA

    ./host/adc_stream_plot.py 192.168.0.186 \
        --window 50000 \
        --interval 200 \
        --rate 50 \
        --secondary-label "mA"  \
        --secondary-slope 0.015151515151515152 \
        --secondary-offset -0.9848484848484849
"""

import argparse
import collections
import socket
import struct
import sys
import threading
import time

import matplotlib.pyplot as plt
import matplotlib.animation as animation
import matplotlib.patches as patches


def read_exact(sock, n):
    """Read exactly n bytes from a blocking socket, or return None on EOF."""
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None
        buf.extend(chunk)
    return bytes(buf)


class Receiver:
    """Reads samples from the socket on a background thread and appends them
    to a thread-safe deque that the plot animation callback drains."""

    def __init__(self, host, port, fmt, window):
        self.host = host
        self.port = port
        self.fmt = fmt
        self.samples = collections.deque(maxlen=window)
        self.lock = threading.Lock()
        self.count = 0
        self.connected = False
        self.error = None
        self.sock = None
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)

    def start(self):
        self._thread.start()

    def stop(self):
        self._stop.set()
        if self.sock is not None:
            try:
                self.sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            self.sock.close()
        self._thread.join(timeout=2)

    def _detect_format(self, sock):
        # Peek a few bytes: text mode is ASCII digits/'-'/newline; binary16
        # will very likely contain a byte outside that range.
        sock.settimeout(2)
        first = sock.recv(64, socket.MSG_PEEK)
        sock.settimeout(None)
        if not first:
            return "text"
        for b in first:
            if not (48 <= b <= 57 or b in (ord("-"), ord("\n"), ord("\r"))):
                return "binary16"
        return "text"

    def _run(self):
        try:
            with socket.create_connection((self.host, self.port), timeout=5) as sock:
                self.sock = sock
                sock.settimeout(None)
                fmt = self.fmt
                if fmt == "auto":
                    fmt = self._detect_format(sock)
                print(f"Connected to {self.host}:{self.port}, format={fmt}")
                self.connected = True

                if fmt == "binary16":
                    self._run_binary16(sock)
                else:
                    self._run_text(sock)
        except OSError as e:
            if not self._stop.is_set():
                self.error = str(e)
        finally:
            self.connected = False

    def _run_binary16(self, sock):
        while not self._stop.is_set():
            raw = read_exact(sock, 2)
            if raw is None:
                break
            (mv,) = struct.unpack("<h", raw)
            with self.lock:
                self.samples.append(mv)
                self.count += 1

    def _run_text(self, sock):
        buf = b""
        while not self._stop.is_set():
            chunk = sock.recv(4096)
            if not chunk:
                break
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                line = line.strip()
                if not line:
                    continue
                try:
                    mv = int(line)
                except ValueError:
                    continue
                with self.lock:
                    self.samples.append(mv)
                    self.count += 1

    def snapshot(self):
        with self.lock:
            return list(self.samples), self.count


def main():
    parser = argparse.ArgumentParser(description=__doc__,
            formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("host", help="ESP32 IP address or hostname")
    parser.add_argument("--port", type=int, default=4451,
            help="TCP port (default: 4451, CONFIG_ESP_ADC_STREAM_TCP_PORT default)")
    parser.add_argument("--format", choices=["auto", "text", "binary16"],
            default="auto", help="Wire format (default: auto-detect)")
    parser.add_argument("--window", type=int, default=500,
            help="Number of most recent samples to display (default: 500)")
    parser.add_argument("--rate", type=float, default=None,
            help="Output sample rate in Hz, for a time-based x-axis "
                 "(CONFIG_ESP_ADC_STREAM_SAMPLE_RATE_HZ). If omitted, the "
                 "x-axis is just the sample index.")
    parser.add_argument("--interval", type=int, default=50,
            help="Plot refresh interval in ms (default: 50)")
    parser.add_argument("--secondary-label", default=None,
            help="Label for a secondary y-axis on the right (e.g. "
                 "'Current (mA)'). If given, adds that axis showing "
                 "secondary = slope * mv + offset.")
    parser.add_argument("--secondary-slope", type=float, default=1.0,
            help="Secondary axis conversion slope, secondary units per mV "
                 "(default: 1.0)")
    parser.add_argument("--secondary-offset", type=float, default=0.0,
            help="Secondary axis conversion offset (default: 0.0)")
    args = parser.parse_args()

    if args.secondary_label and args.secondary_slope == 0:
        parser.error("--secondary-slope must be nonzero when "
                "--secondary-label is given")

    receiver = Receiver(args.host, args.port, args.format, args.window)
    receiver.start()

    plt.style.use("dark_background")

    # constrained layout sizes margins from actual label/title extents
    # (roughly fixed pixel widths) rather than the default fixed *fraction*
    # of figure width, which otherwise leaves growing blank margins as the
    # window is widened.
    fig, ax = plt.subplots(layout="constrained")

    # A single-pixel border around the extreme edge of the window. Inset by
    # a couple of pixels rather than 0 -- a rectangle drawn exactly on the
    # canvas boundary gets clipped in half -- but just enough to clear the
    # y-axis labels ("mV"/"mA"), which constrained layout packs right up
    # against the figure edge.
    px = 2
    fig_w_px, fig_h_px = fig.get_size_inches() * fig.dpi
    m_x, m_y = px / fig_w_px, px / fig_h_px
    fig.add_artist(patches.Rectangle((m_x, m_y), 1 - 2 * m_x, 1 - 2 * m_y,
            transform=fig.transFigure, fill=False, edgecolor="white",
            linewidth=72.0 / fig.dpi, clip_on=False, zorder=1000))

    line, = ax.plot([], [], lw=1)
    ax.set_ylabel("mV")
    ax.set_xlabel("time (s)" if args.rate else "sample")
    ax.set_title(f"ADC stream: {args.host}:{args.port}")

    secax = None
    if args.secondary_label:
        # A twinx() (full overlay, independent y-scale) rather than
        # secondary_yaxis() -- the latter only renders a thin tick/label
        # strip and its gridlines don't span the plot, so grid() on it is a
        # no-op visually. Its ylim is kept in sync with the primary axis's
        # (which autoscales every frame) via the same slope/offset used for
        # the tick labels, so grid() on it draws lines at nice round
        # secondary-unit values instead of the primary's mV values.
        slope, offset = args.secondary_slope, args.secondary_offset
        secax = ax.twinx()
        secax.set_ylabel(args.secondary_label)
        secax.grid(True, axis="y", alpha=0.3)
        # twinx() hides its own x-axis (they share the primary's), so
        # vertical lines have to come from the primary axis instead.
        ax.grid(True, axis="x", alpha=0.3)
    else:
        ax.grid(True, alpha=0.3)

    status_text = ax.text(0.01, 0.99, "", transform=ax.transAxes,
            va="top", ha="left", fontsize=9,
            bbox=dict(boxstyle="round", fc="black", alpha=0.7))

    start_time = time.monotonic()

    def update(_frame):
        samples, count = receiver.snapshot()

        if receiver.error:
            status_text.set_text(f"error: {receiver.error}")
            return line, status_text

        if not samples:
            status_text.set_text("waiting for data..." if receiver.connected
                    else "connecting...")
            return line, status_text

        n = len(samples)
        if args.rate:
            # Most recent sample is "now"; older samples are spaced 1/rate
            # apart going backward.
            xs = [(count - n + i) / args.rate for i in range(n)]
        else:
            xs = list(range(count - n, count))

        line.set_data(xs, samples)

        # Auto-scale both axes to the current window, with a little
        # headroom so the trace doesn't hug the plot edges.
        ax.set_xlim(xs[0], xs[-1] if xs[-1] > xs[0] else xs[0] + 1)
        ymin, ymax = min(samples), max(samples)
        pad = max(1, (ymax - ymin) * 0.1)
        ax.set_ylim(ymin - pad, ymax + pad)

        if secax is not None:
            lo = args.secondary_slope * (ymin - pad) + args.secondary_offset
            hi = args.secondary_slope * (ymax + pad) + args.secondary_offset
            secax.set_ylim(min(lo, hi), max(lo, hi))

        elapsed = time.monotonic() - start_time
        rate_est = count / elapsed if elapsed > 0 else 0
        text = (f"samples: {count}  ~{rate_est:.0f}/s  "
                f"last: {samples[-1]} mV  min/max (window): {ymin}/{ymax} mV")
        if secax is not None:
            smin = args.secondary_slope * ymin + args.secondary_offset
            smax = args.secondary_slope * ymax + args.secondary_offset
            smin, smax = min(smin, smax), max(smin, smax)
            text += f"  {smin:.3g}/{smax:.3g} {args.secondary_label}"
        status_text.set_text(text)
        return line, status_text

    ani = animation.FuncAnimation(fig, update, interval=args.interval,
            cache_frame_data=False)

    try:
        plt.show()
    finally:
        receiver.stop()


if __name__ == "__main__":
    main()
